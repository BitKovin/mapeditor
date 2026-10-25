// rBSP export (REQUIREMENTS В§25, DESIGN В§12): EXR lightmaps, convex collision pieces, the compiler.
#include "core/bsp_compile.h"
#include "core/jpeg.h"
#include "core/albedo.h"
#include "core/parallel.h"
#include <tuple>
#include <filesystem>
#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <array>
#include "core/exr.h"
#include "core/lightmap_fit.h"
#include "core/lightmap_pack.h"
#include "core/lighting_classes.h"
#include "doctest.h"
#include "test_util.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <thread>
#include "core/router.h"
#include "core/bsp_export.h"
#include "core/scene.h"
#include "core/sky.h"
#include "core/textures.h"

using namespace core;
using namespace testutil;

namespace {

double pieceVolume(const ConvexPiece& p) {
    double v = 0;
    const auto w = convexWindings(p.sides);
    for (size_t i = 0; i < w.size(); ++i) {
        if (w[i].empty()) continue;
        Vec3 s;
        for (size_t k = 0; k + 2 < w[i].size(); ++k) s = s + (w[i][k + 1] - w[i][0]).cross(w[i][k + 2] - w[i][0]);
        v += 0.5 * s.dot(p.sides[i].normal) * w[i][0].dot(p.sides[i].normal) / 3.0;
    }
    return v;
}
double totalVolume(const std::vector<ConvexPiece>& ps) { double v = 0; for (const auto& p : ps) v += pieceVolume(p); return v; }

Entity ent(const std::string& id, const std::string& cls, std::vector<Brush> brushes = {}, Vec3 origin = {}) {
    Entity e;
    e.id = id;
    e.classname = cls;
    e.brushes = std::move(brushes);
    e.origin = origin;
    return e;
}

struct Compiled {
    Document doc;
    std::vector<uint8_t> bytes;
    BspOutput out;
    BspView view;
};

BspInput inputFor(const Document& doc, std::shared_ptr<SolidSet>& solids) {
    BspInput in;
    const DocumentLoader loader = [](const std::string& p) -> const Document& { throw std::runtime_error("no prefab " + p); };
    in.entities = flattenForCompile(doc, loader).entities;
    in.surfaces = collectSurfaces(doc, {});
    in.classes = LightingClasses::defaults();
    solids = std::make_shared<SolidSet>(collectSolids(doc, {}, [](const std::string& c) { return c == "worldspawn" || c == "func_group" || c == "func_detail"; }));
    in.solids = solids.get();
    return in;
}

// Two closed rooms side by side (x 0..16 and 17..33), 4 m high, 16 m deep, a solid wall between them; 0.5 m thick shells.
Document twoRooms(bool door = false) {
    std::vector<Brush> b;
    int n = 0;
    auto box = [&](Vec3 a, Vec3 c) { b.push_back(Brush::makeBox("b" + std::to_string(n++), a, c, "dev/wall")); };
    box({-0.5, -0.5, -0.5}, {33.5, 0, 16.5});    // floor
    box({-0.5, 4, -0.5}, {33.5, 4.5, 16.5});     // ceiling
    box({-0.5, 0, -0.5}, {33.5, 4, 0});          // z = 0 wall
    box({-0.5, 0, 16}, {33.5, 4, 16.5});         // z = 16 wall
    box({-0.5, 0, 0}, {0, 4, 16});               // x = 0 wall
    box({33, 0, 0}, {33.5, 4, 16});              // x = 33 wall
    if (door) { box({16, 0, 0}, {17, 4, 7}); box({16, 0, 9}, {17, 4, 16}); box({16, 3, 7}, {17, 4, 9}); }
    else box({16, 0, 0}, {17, 4, 16});           // dividing wall
    Document d;
    d.entities.push_back(ent("w", "worldspawn", b));
    d.entities.push_back(ent("p", "info_player_start", {}, {8, 1, 8}));
    return d;
}

int clusterAt(const BspView& v, const Vec3& editorPoint) {
    const auto nodes = v.lump<rbsp::Node>(rbsp::kNodes);
    const auto planes = v.lump<rbsp::Plane>(rbsp::kPlanes);
    const auto leafs = v.lump<rbsp::Leaf>(rbsp::kLeafs);
    const Vec3 q = toQuake(editorPoint);
    int i = 0;
    while (i >= 0) {
        const auto& n = nodes[static_cast<size_t>(i)];
        const auto& p = planes[static_cast<size_t>(n.plane)];
        const double d = q.x * p.normal[0] + q.y * p.normal[1] + q.z * p.normal[2] - p.dist;
        i = d >= 0 ? n.children[0] : n.children[1];
    }
    return leafs[static_cast<size_t>(-i - 1)].cluster;
}

bool sees(const BspView& v, int a, int b) {
    const auto vis = v.lump<uint8_t>(rbsp::kVisibility);
    int32_t bytesPer = 0;
    std::memcpy(&bytesPer, vis.data() + 4, 4);
    return (vis[8 + static_cast<size_t>(a) * bytesPer + b / 8] >> (b % 8)) & 1;
}

}  // namespace

TEST_CASE("exr: half float RGB with ZIP compression round-trips and is a standard EXR") {
    const int w = 37, h = 21;  // not a multiple of the 16 line blocks
    std::vector<float> rgb(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0; i < rgb.size(); ++i) rgb[i] = static_cast<float>((i % 97) * 0.173 + (i % 5 == 0 ? 120.5 : 0));
    const auto bytes = encodeExr(w, h, rgb.data());
    REQUIRE(bytes.size() > 8);
    CHECK(bytes[0] == 0x76); CHECK(bytes[1] == 0x2f); CHECK(bytes[2] == 0x31); CHECK(bytes[3] == 0x01);
    CHECK(bytes.size() < rgb.size() * 2);  // compressed below the raw half size
    ExrImage img;
    std::string err;
    REQUIRE(decodeExr(bytes.data(), bytes.size(), img, &err));
    CHECK(img.width == w);
    CHECK(img.height == h);
    double worst = 0;
    for (size_t i = 0; i < rgb.size(); ++i) worst = std::max(worst, std::fabs(double(img.rgb[i]) - rgb[i]) / std::max(1.0, double(rgb[i])));
    CHECK(worst < 1e-3);  // half precision
    CHECK_FALSE(decodeExr(bytes.data(), 6, img, &err));
}

TEST_CASE("bsp convex pieces: a box is one piece, an L brush splits into pieces of the same volume, subtraction removes the cutter") {
    const Brush box = Brush::makeBox("a", {0, 0, 0}, {3, 3, 3});
    const auto one = convexPieces(box);
    REQUIRE(one.size() == 1);
    CHECK(one[0].sides.size() == 6);
    CHECK(totalVolume(one) == doctest::Approx(27));

    const Brush l = lPrism("l");
    const auto ls = convexPieces(l);
    CHECK(ls.size() >= 2);
    CHECK(totalVolume(ls) == doctest::Approx(3));
    for (const auto& p : ls) CHECK(pieceVolume(p) > 0);

    const auto cutter = convexPieces(Brush::makeBox("c", {1, 1, 1}, {2, 2, 2}));
    REQUIRE(cutter.size() == 1);
    const auto hollow = subtractConvex(one[0], cutter[0]);
    CHECK(totalVolume(hollow) == doctest::Approx(26));
    CHECK(hollow.size() <= 6);
    const auto apart = convexPieces(Brush::makeBox("d", {5, 5, 5}, {6, 6, 6}));
    const auto untouched = subtractConvex(one[0], apart[0]);
    REQUIRE(untouched.size() == 1);
    CHECK(pieceVolume(untouched[0]) == doctest::Approx(27));
}

TEST_CASE("bsp convex pieces: a brush of solids glued back to back keeps all of its volume, whatever face comes first") {
    // three boxes glued where they touch: the glued faces stay in the brush, back to back (facing opposite ways on one plane)
    const Brush glued = gluedBrush(gluedBrush(Brush::makeBox("a", {3, 2, 0}, {4, 3, 1}), Brush::makeBox("b", {2, 1, 0}, {3, 3, 1})),
                                   Brush::makeBox("c", {2, 0, 0}, {4, 1, 1}));
    REQUIRE(glued.signedVolume() == doctest::Approx(5));
    for (size_t r = 0; r < glued.faces.size(); ++r) {
        Brush b = glued;
        std::rotate(b.faces.begin(), b.faces.begin() + static_cast<std::ptrdiff_t>(r), b.faces.end());
        INFO("first face ", r);
        CHECK(totalVolume(convexPieces(b)) == doctest::Approx(5));
    }
}

TEST_CASE("bsp compile: header, lumps, models, entities in Quake units and axes") {
    Document d = twoRooms();
    Brush door = Brush::makeBox("door1", {16, 0, 7}, {17, 3, 9}, "dev/door");
    d.entities.push_back(ent("d", "func_door", {door}));
    std::shared_ptr<SolidSet> solids;
    BspInput in = inputFor(d, solids);
    in.settings.visCell = 2;
    const BspOutput out = compileBsp(in);
    BspView v;
    std::string err;
    REQUIRE(v.open(out.bsp, &err));
    CHECK(std::string(v.header.ident, 4) == "rBSP");
    CHECK(v.header.version == 2);
    for (const auto& l : v.header.lumps) CHECK(l.offset % 4 == 0);

    const auto models = v.lump<rbsp::Model>(rbsp::kModels);
    REQUIRE(models.size() == 2);
    CHECK(models[0].numSurfaces > 0);
    CHECK(models[1].numSurfaces == 6);   // the door box
    CHECK(models[1].numBrushes == 1);
    // bounds in Quake units: x 0..33 m -> about -16..1072, editor y (up) 0..4 m -> Quake z
    CHECK(models[0].mins[0] == doctest::Approx(-16).epsilon(0.01));
    CHECK(models[0].maxs[2] == doctest::Approx(4.5 * 32).epsilon(0.01));
    CHECK(models[0].mins[1] == doctest::Approx(-16.5 * 32).epsilon(0.01));  // Quake y = -editor z

    const auto surfaces = v.lump<rbsp::Surface>(rbsp::kSurfaces);
    const auto verts = v.lump<rbsp::DrawVert>(rbsp::kDrawVerts);
    const auto idx = v.lump<int32_t>(rbsp::kDrawIndexes);
    const auto shaders = v.lump<rbsp::Shader>(rbsp::kShaders);
    std::set<std::string> names;
    for (const auto& s : shaders) names.insert(s.name);
    CHECK(names.count("textures/dev/wall") == 1);
    CHECK(names.count("textures/dev/door") == 1);
    for (const auto& s : surfaces) {
        CHECK(s.type == rbsp::kSurfacePolygon);
        CHECK(s.lightmapStyles[0] == rbsp::kNoStyle);  // nothing baked in this test
        CHECK(s.lightmapNum[0] == -1);
        REQUIRE(s.numIndexes % 3 == 0);
        // the winding is clockwise seen from the front: (b - a) x (c - a) points against the normal
        const auto& a = verts[static_cast<size_t>(s.firstVert + idx[static_cast<size_t>(s.firstIndex)])].xyz;
        const auto& b = verts[static_cast<size_t>(s.firstVert + idx[static_cast<size_t>(s.firstIndex) + 1])].xyz;
        const auto& c = verts[static_cast<size_t>(s.firstVert + idx[static_cast<size_t>(s.firstIndex) + 2])].xyz;
        const Vec3 e1{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        CHECK(e1.cross(e2).dot(Vec3{s.normal[0], s.normal[1], s.normal[2]}) < 0);
    }
    // buried faces (the wall faces inside the floor, ...) were dropped
    CHECK(out.stats.droppedSurfaces > 0);

    const std::string ents = v.entities();
    CHECK(ents.find("\"classname\" \"worldspawn\"") != std::string::npos);
    CHECK(ents.find("\"model\" \"*1\"") != std::string::npos);
    CHECK(ents.find("\"origin\" \"8 1 8\"") != std::string::npos);  // the engine's own space: metres, Y up, as in the editor
    CHECK(ents.find("func_door") != std::string::npos);
}

TEST_CASE("bsp compile: a face partly under another brush is kept (only faces buried everywhere are dropped)") {
    // a floor slab with a wall standing across its middle: the floor's top face has its centre under the wall, both sides of it are open
    Document d;
    d.entities.push_back(ent("w", "worldspawn", {Brush::makeBox("floor", {0, -0.5, 0}, {10, 0, 10}, "dev/floor"),
                                                 Brush::makeBox("wall", {0, 0, 3}, {10, 4, 7}, "dev/wall")}));
    std::shared_ptr<SolidSet> solids;
    BspInput in = inputFor(d, solids);
    const BspOutput out = compileBsp(in);
    BspView v;
    REQUIRE(v.open(out.bsp));
    const auto shaders = v.lump<rbsp::Shader>(rbsp::kShaders);
    int floorTops = 0, wallBottoms = 0;
    for (const auto& s : v.lump<rbsp::Surface>(rbsp::kSurfaces)) {
        const std::string name = shaders[static_cast<size_t>(s.shader)].name;
        if (name == "textures/dev/floor" && s.normal[2] > 0.99f) ++floorTops;
        if (name == "textures/dev/wall" && s.normal[2] < -0.99f) ++wallBottoms;
    }
    CHECK(floorTops == 1);     // the open strips beside the wall need it
    CHECK(wallBottoms == 0);   // lies on the floor: never seen
    CHECK(out.stats.droppedSurfaces >= 1);
}

TEST_CASE("bsp visibility: closed rooms do not see each other, a doorway lets them, open space sees far") {
    for (const bool door : {false, true}) {
        CAPTURE(door);
        std::shared_ptr<SolidSet> solids;
        BspInput in = inputFor(twoRooms(door), solids);
        in.settings.visCell = 2;
        in.settings.visRays = 512;
        const BspOutput out = compileBsp(in);
        BspView v;
        REQUIRE(v.open(out.bsp));
        CHECK(out.stats.clusters > 0);
        const int a = clusterAt(v, {4, 2, 4}), a2 = clusterAt(v, {12, 2, 12}), b = clusterAt(v, {29, 2, 12});
        REQUIRE(a >= 0);
        REQUIRE(b >= 0);
        CHECK(sees(v, a, a2));
        CHECK(sees(v, a2, a));
        CHECK(sees(v, a, b) == door);
        CHECK(sees(v, b, a) == door);
    }
}

TEST_CASE("bsp lightmaps and light grid: base pages as baked, style layers re-packed, grid header and palette") {
    Document d = twoRooms(true);
    std::shared_ptr<SolidSet> solids;
    BspInput in = inputFor(d, solids);
    in.settings.lightmapJpeg = false;  // this test is about the style layers
    in.settings.visibility = false;
    LightmapSettings ls;
    ls.texelsPerMeter = 2;
    ls.atlasSize = 256;
    LightmapLayout layout = buildLightmapLayout(in.surfaces, in.classes, ls);
    REQUIRE(!layout.faces.empty());
    // style 5 touches the first face only
    ls.faceStyles[layout.faces[0].key()] = {5};
    layout = buildLightmapLayout(in.surfaces, in.classes, ls);
    int styledChart = -1;
    for (size_t c = 0; c < layout.charts.size(); ++c) if (layout.charts[c].styles.size() == 2) styledChart = static_cast<int>(c);
    REQUIRE(styledChart >= 0);
    in.layout = &layout;
    for (int style : {0, 5}) {
        LightmapPages p;
        p.atlas = 256;
        p.pageCount = layout.pageCount;
        for (int k = 0; k < p.pageCount; ++k) {
            p.rgb.push_back(std::vector<float>(256 * 256 * 3, style == 0 ? 0.5f : 2.0f));
            p.valid.push_back(std::vector<uint8_t>(256 * 256, 1));
        }
        in.pages[style] = p;
    }
    // a 3 x 2 x 4 light grid (editor axes), each probe's ambient red = its index / 100
    LightvolGrid g;
    g.origin = {1, 1, 1};
    g.cell = 2;
    g.nx = 3; g.ny = 2; g.nz = 4;
    g.probes.resize(g.nx * g.ny * g.nz);
    for (size_t i = 0; i < g.probes.size(); ++i) { g.probes[i].valid = true; g.probes[i].ambient = {i / 100.0, 0, 0}; g.probes[i].dir = {0, 1, 0}; }
    in.lightvol[0] = g;
    const BspOutput out = compileBsp(in);
    BspView v;
    REQUIRE(v.open(out.bsp));

    REQUIRE(out.lightmaps.size() == static_cast<size_t>(layout.pageCount) + 1);  // one dense page for style 5
    CHECK(out.lightmaps[0].rgb[0] == 0.5f);
    const auto& ch = layout.charts[static_cast<size_t>(styledChart)];
    const auto surfaces = v.lump<rbsp::Surface>(rbsp::kSurfaces);
    bool found = false;
    for (const auto& s : surfaces) {
        if (s.lightmapStyles[1] != 5) continue;
        found = true;
        CHECK(s.lightmapStyles[0] == 0);
        CHECK(s.lightmapNum[0] == ch.page);
        CHECK(s.lightmapNum[1] == layout.pageCount);
        const int x = s.lightmapCorner[1][0], y = s.lightmapCorner[1][1];
        CHECK(out.lightmaps[static_cast<size_t>(s.lightmapNum[1])].rgb[(static_cast<size_t>(y) * 256 + x) * 3] == 2.0f);
        CHECK(s.lightmapStyles[2] == rbsp::kNoStyle);
    }
    CHECK(found);
    for (const auto& s : surfaces) if (s.lightmapStyles[0] == 0) CHECK(s.lightmapNum[0] >= 0);

    // grid: header in Quake units / axes; Quake (x, y, z) = editor (x, -z, y)
    const auto arr = v.lump<uint8_t>(rbsp::kLightArray);
    REQUIRE(arr.size() == sizeof(rbsp::LightGridHeader) + g.count() * 2);
    rbsp::LightGridHeader h{};
    std::memcpy(&h, arr.data(), sizeof(h));
    CHECK(h.dims[0] == 3); CHECK(h.dims[1] == 4); CHECK(h.dims[2] == 2);
    CHECK(h.cellSize[0] == doctest::Approx(64));
    CHECK(h.origin[0] == doctest::Approx(32));
    CHECK(h.origin[1] == doctest::Approx(-(1 + 3 * 2) * 32));
    CHECK(h.origin[2] == doctest::Approx(32));
    const auto palette = v.lump<rbsp::GridPoint>(rbsp::kLightGrid);
    // Quake point (qx, qy, qz) is the editor probe (qx, qz, nz - 1 - qy)
    for (int qz = 0; qz < 2; ++qz)
        for (int qy = 0; qy < 4; ++qy)
            for (int qx = 0; qx < 3; ++qx) {
                uint16_t pi = 0;
                std::memcpy(&pi, arr.data() + sizeof(h) + ((static_cast<size_t>(qz) * 4 + qy) * 3 + qx) * 2, 2);
                REQUIRE(pi < palette.size());
                const size_t ei = g.index(qx, qz, 4 - 1 - qy);
                CHECK(palette[pi].ambient[0][0] == lightvolToByte(ei / 100.0));
                CHECK(palette[pi].styles[0] == 0);
                CHECK(palette[pi].styles[1] == rbsp::kNoStyle);
                CHECK(palette[pi].latLong[0] == 0);  // editor up = Quake +Z: lat 0
            }
    CHECK(v.entities().find("\"gridsize\" \"64 64 64\"") != std::string::npos);
}

TEST_CASE("bsp export (router): export_bsp bakes, writes <name>.bsp and <name>/lm_NNNN.exr, reports progress and the result") {
    TempDir dir;
    json light = entityJson("lamp", "light_point", {}, {0, 2, 0});
    light["keyvalues"] = {{"brightness", "8"}, {"radius", "20"}};
    const json doc = docJson({entityJson("w", "worldspawn", {Brush::makeBox("floor", {-4, -1, -4}, {4, 0, 4}), Brush::makeBox("wall", {-4, 0, -4}, {4, 3, -3.5})}),
                              light, entityJson("p", "info_player_start", {}, {1, 0.5, 1})});
    dir.write("maps/room.map.json", doc);
    Scene scene(dir.path);
    scene.loadGameProfile();
    scene.openDocument("maps/room.map.json");
    LightingService::Options opt;
    opt.live.maxPasses = 2;
    opt.live.useGpu = false;
    opt.live.denoiseIndirect = false;
    opt.live.giScale = 1;
    opt.lightvol.useGpu = false;
    opt.lightvol.gridSize = 2;
    opt.lightvol.normals = 8;
    opt.lightvol.giSamples = 4;
    Router router(scene, {}, opt);

    const RouterOutput bad = router.onMessage(R"({"type":"export_bsp","path":"../outside.bsp","requestId":1})");
    REQUIRE(bad.toSender.size() == 1);
    CHECK(json::parse(bad.toSender[0])["ok"] == true);  // accepted: the path is checked on the export thread, the result says why it failed
    json result;
    for (int i = 0; i < 600 && result.is_null(); ++i) {
        for (const auto& m : router.onTick().toAll) if (json::parse(m)["type"] == "export_result") result = json::parse(m);
        if (result.is_null()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(!result.is_null());
    CHECK(result["ok"] == false);
    CHECK(result["error"].get<std::string>().find("assets") != std::string::npos);

    const RouterOutput ok = router.onMessage(R"({"type":"export_bsp","path":"maps/room.bsp","requestId":2})");
    CHECK(json::parse(ok.toSender[0])["ok"] == true);
    const RouterOutput busy = router.onMessage(R"({"type":"export_bsp","path":"maps/room.bsp","requestId":3})");
    CHECK(json::parse(busy.toSender[0])["ok"] == false);  // one at a time
    result = json();
    bool progress = false;
    for (int i = 0; i < 3000 && result.is_null(); ++i) {
        for (const auto& m : router.onTick().toAll) {
            const json j = json::parse(m);
            progress |= j["type"] == "export_progress";
            if (j["type"] == "export_result") result = j;
        }
        if (result.is_null()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(!result.is_null());
    INFO(result.dump());
    CHECK(progress);
    REQUIRE(result["ok"] == true);
    CHECK(result["bsp"] == "maps/room.bsp");
    CHECK(result["lightmaps"] == "maps/room");
    CHECK(result["stats"]["lightmapPages"].get<int>() >= 1);
    CHECK(result["stats"]["gridPoints"].get<int>() > 0);

    std::ifstream f(dir.path / "maps/room.bsp", std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    BspView v;
    REQUIRE(v.open(bytes));
    ExrImage page;
    std::string err;
    REQUIRE(readExr((dir.path / "maps/room/lm_0000.exr").string(), page, &err));
    CHECK(page.width == opt.layout.atlasSize);
    // the floor under the lamp is lit: find its surface and read the page at its first vertex's lightmap coordinate
    const auto surfaces = v.lump<rbsp::Surface>(rbsp::kSurfaces);
    const auto verts = v.lump<rbsp::DrawVert>(rbsp::kDrawVerts);
    double best = 0;
    for (const auto& s : surfaces) {
        if (s.lightmapStyles[0] != 0 || s.normal[2] < 0.9) continue;  // the floor's top faces Quake +Z
        Vec2 c{0, 0};
        for (int k = 0; k < s.numVerts; ++k) { c.u += verts[static_cast<size_t>(s.firstVert + k)].lightmap[0][0]; c.v += verts[static_cast<size_t>(s.firstVert + k)].lightmap[0][1]; }
        const int x = static_cast<int>(c.u / s.numVerts * page.width), y = static_cast<int>(c.v / s.numVerts * page.height);
        best = std::max(best, double(page.rgb[(static_cast<size_t>(y) * page.width + x) * 3]));
    }
    CHECK(best > 0.1);
    CHECK(v.entities().find("light_point") != std::string::npos);
    CHECK(v.lump<rbsp::GridPoint>(rbsp::kLightGrid).size() > 0);
}

TEST_CASE("bsp lightmaps: 8 slots in at most 4 UV sets (slots 3..7 share one placement, each slot has its own pages)") {
    std::shared_ptr<SolidSet> solids;
    BspInput in = inputFor(twoRooms(true), solids);
    in.settings.visibility = false;
    LightmapSettings ls;
    ls.texelsPerMeter = 2;
    ls.atlasSize = 256;
    LightmapLayout layout = buildLightmapLayout(in.surfaces, in.classes, ls);
    REQUIRE(layout.faces.size() > 2);
    ls.faceStyles[layout.faces[0].key()] = {1, 2, 3, 4, 5, 6, 7};  // 8 slots
    ls.faceStyles[layout.faces[1].key()] = {3, 9};                  // 3 slots
    layout = buildLightmapLayout(in.surfaces, in.classes, ls);
    in.layout = &layout;
    for (int style : {0, 1, 2, 3, 4, 5, 6, 7, 9}) {
        LightmapPages p;
        p.atlas = 256;
        p.pageCount = layout.pageCount;
        for (int k = 0; k < p.pageCount; ++k) p.rgb.push_back(std::vector<float>(256 * 256 * 3, float(style) + 0.5f));
        in.pages[style] = p;
    }
    const BspOutput out = compileBsp(in);
    BspView v;
    REQUIRE(v.open(out.bsp));
    const auto surfaces = v.lump<rbsp::Surface>(rbsp::kSurfaces);
    const auto verts = v.lump<rbsp::DrawVert>(rbsp::kDrawVerts);
    bool eight = false, three = false;
    for (const auto& s : surfaces) {
        int used = 0;
        while (used < rbsp::kStyles && s.lightmapStyles[used] != rbsp::kNoStyle) ++used;
        if (used == 8) {
            eight = true;
            std::set<int> pages;
            for (int k = 0; k < 8; ++k) {
                CHECK(s.lightmapStyles[k] == k);
                pages.insert(s.lightmapNum[k]);
                // the page holds that slot's style layer at the slot's corner
                const auto& pg = out.lightmaps[static_cast<size_t>(s.lightmapNum[k])].rgb;
                CHECK(pg[(static_cast<size_t>(s.lightmapCorner[k][1]) * 256 + s.lightmapCorner[k][0]) * 3] == doctest::Approx(k + 0.5));
            }
            CHECK(pages.size() == 8);
            for (int k = 4; k < 8; ++k) { CHECK(s.lightmapCorner[k][0] == s.lightmapCorner[3][0]); CHECK(s.lightmapCorner[k][1] == s.lightmapCorner[3][1]); }
            // at most 4 distinct UV sets per vertex: slots 3..7 carry the same coordinates
            for (int i = 0; i < s.numVerts; ++i) {
                const auto& dv = verts[static_cast<size_t>(s.firstVert + i)];
                for (int k = 4; k < 8; ++k) { CHECK(dv.lightmap[k][0] == dv.lightmap[3][0]); CHECK(dv.lightmap[k][1] == dv.lightmap[3][1]); }
            }
        }
        if (used == 3) { three = true; CHECK(s.lightmapStyles[1] == 3); CHECK(s.lightmapStyles[2] == 9); }
    }
    CHECK(eight);
    CHECK(three);
}

TEST_CASE("bsp contents: rules from the game config on top of Quake 3's defaults; class rules set brush contents") {
    CHECK(globMatch("common/clip", "Common/Clip"));
    CHECK(globMatch("*water*", "liquids/water_dark"));
    CHECK(globMatch("trigger_*", "trigger_multiple"));
    CHECK_FALSE(globMatch("trigger_*", "func_trigger"));
    CHECK(globMatch("a?c", "abc"));
    const BspContentRules def = BspContentRules::defaults();
    CHECK(def.forTexture("dev/wall").contents == 1);
    CHECK(def.forTexture("common/clip").contents == 0x10000u);
    CHECK((def.forTexture("common/clip").surface & 0x80u) != 0);  // nodraw
    CHECK(def.forTexture("liquid/water1").contents == (32u | 0x20000000u));
    std::vector<std::string> warn;
    const BspContentRules cfg = BspContentRules::fromJson(json::parse(R"({
        "contentFlags": {"glass": 4096},
        "textures": [{"match": "dev/glass*", "contents": ["glass"], "surface": ["nomarks"]}, {"match": "common/clip", "contents": ["solid"]}],
        "classes": [{"match": "func_illusionary", "contents": []}, {"match": "x", "contents": ["nope"]}]})"), &warn);
    CHECK(cfg.forTexture("dev/glass").contents == 4096u);
    CHECK(cfg.forTexture("dev/glass").surface == 32u);
    CHECK(cfg.forTexture("common/clip").contents == 1u);   // the config's rule wins over the built-in one
    uint32_t c = 99;
    CHECK(cfg.forClass("func_illusionary", c));
    CHECK(c == 0u);
    CHECK(cfg.forClass("trigger_hurt", c));
    CHECK(c == 0x40000000u);
    CHECK_FALSE(cfg.forClass("func_door", c));
    REQUIRE(warn.size() == 1);  // the unknown flag name
    CHECK(warn[0].find("nope") != std::string::npos);

    // a trigger brush gets a shader with trigger contents, the world's brushes stay solid
    Document d = twoRooms();
    d.entities.push_back(ent("t", "trigger_multiple", {Brush::makeBox("tb", {2, 0, 2}, {3, 2, 3}, "common/trigger")}));
    std::shared_ptr<SolidSet> solids;
    BspInput in = inputFor(d, solids);
    in.settings.visibility = false;
    const BspOutput out = compileBsp(in);
    BspView v;
    REQUIRE(v.open(out.bsp));
    const auto models = v.lump<rbsp::Model>(rbsp::kModels);
    const auto brushes = v.lump<rbsp::Brush>(rbsp::kBrushes);
    const auto shaders = v.lump<rbsp::Shader>(rbsp::kShaders);
    REQUIRE(models.size() == 2);
    CHECK(shaders[static_cast<size_t>(brushes[static_cast<size_t>(models[1].firstBrush)].shader)].contentFlags == 0x40000000);
    CHECK(shaders[static_cast<size_t>(brushes[static_cast<size_t>(models[0].firstBrush)].shader)].contentFlags == 1);
}

TEST_CASE("bsp decals: triangle surfaces flagged `decal`, first in their model, lifted off the wall, lit by the face under them") {
    Document d = twoRooms();
    Entity decal = ent("dc", "decal", {}, {8, 0.1, 8});  // zero angles: projects straight down onto the floor
    decal.keyvalues = {{"texture", "decals/blood"}, {"width", "2"}, {"height", "2"}, {"depth", "1"}};
    d.entities.push_back(decal);
    std::shared_ptr<SolidSet> solids;
    BspInput in = inputFor(d, solids);
    in.decals = collectDecals(d, {});
    REQUIRE(!in.decals.empty());
    in.settings.visibility = false;
    LightmapSettings ls;
    ls.texelsPerMeter = 2;
    ls.atlasSize = 256;
    const LightmapLayout layout = buildLightmapLayout(in.surfaces, in.classes, ls);
    in.layout = &layout;
    LightmapPages p;
    p.atlas = 256;
    p.pageCount = layout.pageCount;
    for (int k = 0; k < p.pageCount; ++k) p.rgb.push_back(std::vector<float>(256 * 256 * 3, 1.0f));
    in.pages[0] = p;
    const BspOutput out = compileBsp(in);
    CHECK(out.stats.decals == in.decals.size());
    BspView v;
    REQUIRE(v.open(out.bsp));
    const auto surfaces = v.lump<rbsp::Surface>(rbsp::kSurfaces);
    const auto shaders = v.lump<rbsp::Shader>(rbsp::kShaders);
    const auto verts = v.lump<rbsp::DrawVert>(rbsp::kDrawVerts);
    const auto models = v.lump<rbsp::Model>(rbsp::kModels);
    const uint32_t decalFlag = BspContentRules::defaults().decalFlag();
    // the decal surfaces are the first ones of model 0
    const size_t n = in.decals.size();
    for (size_t i = 0; i < surfaces.size(); ++i) {
        const auto& s = surfaces[i];
        const bool isDecal = (static_cast<uint32_t>(shaders[static_cast<size_t>(s.shader)].surfaceFlags) & decalFlag) != 0;
        CHECK(isDecal == (static_cast<int>(i) >= models[0].firstSurface && i < static_cast<size_t>(models[0].firstSurface) + n));
        if (!isDecal) continue;
        CHECK(std::string(shaders[static_cast<size_t>(s.shader)].name) == "textures/decals/blood");
        CHECK(shaders[static_cast<size_t>(s.shader)].contentFlags == 0);  // not solid
        CHECK(s.type == rbsp::kSurfaceTriangles);
        CHECK(s.lightmapStyles[0] == 0);  // the floor's chart
        CHECK(s.lightmapNum[0] >= 0);
        for (int k = 0; k < s.numVerts; ++k) CHECK(verts[static_cast<size_t>(s.firstVert + k)].xyz[2] == doctest::Approx(0.25));  // floor top z = 0, lifted
    }
}

namespace {
// a w x h TGA (32 bit, top-left origin): one colour (seamless), or a vertical gradient (its top and bottom rows do not continue each other)
void writeTga(const std::filesystem::path& p, int w, int h, bool gradient = false) {
    std::filesystem::create_directories(p.parent_path());
    std::vector<unsigned char> b(18, 0);
    b[2] = 2; b[12] = static_cast<unsigned char>(w); b[13] = static_cast<unsigned char>(w >> 8); b[14] = static_cast<unsigned char>(h); b[15] = static_cast<unsigned char>(h >> 8);
    b[16] = 32; b[17] = 0x28;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) { b.push_back(static_cast<unsigned char>(gradient ? y * 4 : 90)); b.push_back(128); b.push_back(60); b.push_back(255); }
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}
}  // namespace

TEST_CASE("bsp export: repeat modes, texture group layouts and reliefs are exported as the editor lays them (texplan), also from the command line") {
    TempDir dir;
    writeTga(dir.path / "textures/t/plain.tga", 32, 32);
    writeTga(dir.path / "textures/t/mirror.tga", 32, 32, true);  // not seamless: also edge clamped (cut at every repeat anyway)
    writeTga(dir.path / "textures/t/bump.tga", 32, 32);
    std::ofstream(dir.path / "textures/t/mirror.tga.json") << R"({"pixelsPerMeter": 32, "repeat": {"u": {"mode": "mirror"}, "v": {"mode": "mirror"}}})";
    std::ofstream(dir.path / "textures/t/bump.tga.json") << R"({"pixelsPerMeter": 32, "relief": {"vertices": [[0,0,0],[1,0,0],[1,1,0],[0,1,0],[0.25,0.25,0.1],[0.75,0.25,0.1],[0.75,0.75,0.1],[0.25,0.75,0.1]],
        "faces": [[4,5,6,7],[0,1,5,4],[1,2,6,5],[2,3,7,6],[3,0,4,7]]}})";
    std::filesystem::create_directories(dir.path / "texgroups");
    std::ofstream(dir.path / "texgroups/stripes.texgroup.json") << R"({"format": 1, "slots": {"a": "t/plain", "b": "t/mirror"}, "root": {"split": "v", "parts": [{"size": 1, "node": "a"}, {"size": "*", "node": "b"}]}})";
    Brush box = Brush::makeBox("box", {0, 0, 0}, {5, 3, 4}, "t/plain");
    box.faces[0].texture = "t/mirror";  // -Z: mirrored both ways (cut at every repeat: 1 m)
    box.faces[3].texture = "t/bump";    // +X: relief
    json bj = brushJson(box);
    bj["faces"][5]["layout"] = {{"group", "stripes"}};  // +Y (top): laid out by the group
    json w = entityJson("w", "worldspawn", {});
    w["brushes"] = json::array({bj});
    dir.write("maps/t.map.json", docJson({w}));

    BspExportRequest req;
    req.assetsRoot = dir.path;
    req.mapPath = "maps/t.map.json";
    req.outPath = "maps/t.bsp";
    req.bake = false;
    {
        Scene scene(dir.path);
        scene.loadGameProfile();
        scene.openDocument(req.mapPath);
        req.document = scene.documentSyncJson()["doc"];
    }
    const BspExportResult r = exportBsp(req);
    INFO(r.toJson().dump());
    REQUIRE(r.ok);
    CHECK(r.stats.laidOutSurfaces == 3);
    std::ifstream f(dir.path / "maps/t.bsp", std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    BspView v;
    REQUIRE(v.open(bytes));
    const auto surfaces = v.lump<rbsp::Surface>(rbsp::kSurfaces);
    const auto verts = v.lump<rbsp::DrawVert>(rbsp::kDrawVerts);
    const auto shaders = v.lump<rbsp::Shader>(rbsp::kShaders);
    std::map<std::string, std::vector<const rbsp::Surface*>> byShader;
    for (const auto& s : surfaces) byShader[shaders[static_cast<size_t>(s.shader)].name].push_back(&s);
    // the mirrored face: triangles whose texture coordinates stay inside one repeat (0..1), the image flipped every metre
    bool mirrored = false;
    for (const auto* s : byShader["textures/t/mirror"]) {
        if (s->type != rbsp::kSurfaceTriangles) continue;
        if (std::fabs(s->normal[1] - 1) > 1e-3) continue;  // the -Z face: Quake +Y
        mirrored = true;
        for (int k = 0; k < s->numVerts; ++k) {
            const auto& dv = verts[static_cast<size_t>(s->firstVert + k)];
            CHECK(dv.st[0] >= -1e-4f); CHECK(dv.st[0] <= 1 + 1e-4f);
            CHECK(dv.st[1] >= -1e-4f); CHECK(dv.st[1] <= 1 + 1e-4f);
        }
    }
    CHECK(mirrored);
    // the relief face: some vertices stand 0.1 m (3.2 Quake units) off the +X plane (x = 5 m = 160 units), with tilted normals on the walls
    bool raised = false, tilted = false;
    for (const auto* s : byShader["textures/t/bump"])
        for (int k = 0; k < s->numVerts; ++k) {
            const auto& dv = verts[static_cast<size_t>(s->firstVert + k)];
            raised |= std::fabs(dv.xyz[0] - (160 + 3.2f)) < 1e-2f;
            tilted |= dv.normal[0] < 0.99f;  // the sloped sides of the bump (about 22 degrees)
        }
    CHECK(raised);
    CHECK(tilted);
    // the laid out top face: both slot textures appear on it
    bool topPlain = false, topMirror = false;
    for (const auto& s : surfaces) {
        if (s.normal[2] < 0.99f) continue;  // the top: Quake +Z
        const std::string name = shaders[static_cast<size_t>(s.shader)].name;
        topPlain |= name == "textures/t/plain" && s.type == rbsp::kSurfaceTriangles;
        topMirror |= name == "textures/t/mirror";
    }
    CHECK(topPlain);
    CHECK(topMirror);
}

// ---- sky (REQUIREMENTS §28, DESIGN §12 "Sky") ------------------------------------------------------------------------------------------------
namespace {
// the engine's own code, transcribed (SdlWasmEngine TextureCube.hpp bgfxFaceDir): the direction a pixel of a generated face slot shows
Vec3 bgfxFaceDir(int slot, double u, double v) {
    switch (slot) {
        case 0: return {-1, -v, u};
        case 1: return {1, -v, -u};
        case 2: return {u, 1, v};
        case 3: return {u, -1, -v};
        case 4: return {u, -v, 1};
        default: return {-u, -v, -1};
    }
}
void writeBytes(const TempDir& dir, const std::string& rel, const std::string& bytes) {
    const auto full = dir.path / rel;
    std::filesystem::create_directories(full.parent_path());
    std::ofstream(full, std::ios::binary) << bytes;
}
// a solid-colour face as a PNG (writePagePng: linear 1 = white)
void writeFace(const TempDir& dir, const std::string& rel, float value) {
    LightmapPages p;
    p.atlas = 4;
    p.pageCount = 1;
    p.rgb = {std::vector<float>(48, value)};
    p.valid = {std::vector<uint8_t>(16, 1)};
    std::filesystem::create_directories((dir.path / rel).parent_path());
    REQUIRE(writePagePng(p, 0, (dir.path / rel).string()));
}
std::vector<int> surfacesWithShader(const BspView& v, const std::string& shader) {
    const auto shaders = v.lump<rbsp::Shader>(rbsp::kShaders);
    const auto surfaces = v.lump<rbsp::Surface>(rbsp::kSurfaces);
    std::vector<int> out;
    for (size_t i = 0; i < surfaces.size(); ++i) if (shaders[static_cast<size_t>(surfaces[i].shader)].name == shader) out.push_back(static_cast<int>(i));
    return out;
}
}  // namespace

TEST_CASE("sky: the engine's skybox mapping for known directions (up, north, east, ...), and its panorama conversion agrees") {
    // which face file: north (-Z, Quake +Y) = _bk, east (+X) = _rt, south = _ft, west = _lf, up = _up, down = _dn; the centre of each
    const std::pair<Vec3, int> faces[] = {{{0, 0, -1}, 5}, {{1, 0, 0}, 1}, {{0, 0, 1}, 4}, {{-1, 0, 0}, 0}, {{0, 1, 0}, 2}, {{0, -1, 0}, 3}};
    for (const auto& [d, f] : faces) {
        CAPTURE(d.x); CAPTURE(d.y); CAPTURE(d.z);
        const SkyTexel t = cubemapLookup(d);
        CHECK(t.face == f);
        CHECK(t.u == doctest::Approx(0.5));
        CHECK(t.v == doctest::Approx(0.5));
    }
    // upright and not mirrored, as seen from inside: looking north, east is to the right and up is above
    CHECK(cubemapLookup({0.2, 0, -1}).u > 0.5);
    CHECK(cubemapLookup({0, 0.2, -1}).v < 0.5);
    CHECK(cubemapLookup({1, 0, 0.2}).u > 0.5);    // looking east, south is to the right
    CHECK(cubemapLookup({-1, 0, -0.2}).u > 0.5);  // looking west, north is to the right
    CHECK(cubemapLookup({1, 0.2, 0}).v < 0.5);
    // _up (turned on load): west at its top, south at its right
    SkyTexel up = cubemapLookup({-0.2, 1, 0});
    CHECK(up.face == 2); CHECK(up.v < 0.5); CHECK(up.u == doctest::Approx(0.5));
    up = cubemapLookup({0, 1, 0.2});
    CHECK(up.u > 0.5); CHECK(up.v == doctest::Approx(0.5));
    // panorama: north in the middle, east at three quarters, up at the top
    CHECK(panoramaLookup({0, 0, -1}).u == doctest::Approx(0.5));
    CHECK(panoramaLookup({0, 0, -1}).v == doctest::Approx(0.5));
    CHECK(panoramaLookup({1, 0, 0}).u == doctest::Approx(0.75));
    CHECK(panoramaLookup({0, 1, 0}).v == doctest::Approx(0));
    // The engine turns a panorama into face files (DecodeFromPanorama: slot k of bgfxFaceDir written as _lf _rt _up (turned cw) _dn (turned ccw) _bk _ft)
    // and loads them like any faces: for every direction, the faces' lookup lands on the panorama point panoramaLookup gives.
    const int slotOfFile[6] = {0, 1, 2, 3, 5, 4};  // file lf rt up dn ft bk -> the slot it was written from
    for (int i = 0; i < 400; ++i) {
        const double z = 1 - 2 * (i + 0.5) / 400, r = std::sqrt(1 - z * z), phi = i * 2.399963;
        const Vec3 d{r * std::cos(phi), z, r * std::sin(phi)};
        const SkyTexel t = cubemapLookup(d);
        double ru = t.u, rv = t.v;  // the file's pixel -> the raw slot pixel it was written from
        if (t.face == 2) { ru = t.v; rv = 1 - t.u; }       // written turned clockwise
        else if (t.face == 3) { ru = 1 - t.v; rv = t.u; }  // written turned counter-clockwise
        const Vec3 p = bgfxFaceDir(slotOfFile[t.face], ru * 2 - 1, rv * 2 - 1).normalized();
        const double pu = std::atan2(p.x, p.z) / (2 * 3.14159265358979323846) + 0.5;  // sampleEquirect: lon = atan2(x, z), lat = asin(y)
        const double pv = 0.5 - std::asin(p.y) / 3.14159265358979323846;
        const Vec2 mine = panoramaLookup(d);
        double du = std::fabs(pu - mine.u);
        du = std::min(du, 1 - du);
        CAPTURE(i);
        CHECK(du < 1e-9);
        CHECK(pv == doctest::Approx(mine.v));
    }
}

TEST_CASE("sky: skyboxes in env/ are listed and read the engine's way (per extension: faces first, then the panorama)") {
    TempDir dir;
    for (const char* f : {"_lf", "_rt", "_up", "_dn", "_ft", "_bk"}) {
        writeBytes(dir, std::string("env/day_cube") + f + ".jpg", f);
        writeBytes(dir, std::string("env/dusk_cube") + f + ".jpg", "jpg face");
    }
    writeBytes(dir, "env/dusk_cube.png", "png panorama");  // .png is tried first: its panorama wins over the .jpg faces
    writeBytes(dir, "env/night_cube.jpg", "panorama");
    writeBytes(dir, "env/notes.png", "not a skybox");
    writeBytes(dir, "env/half_cube_lf.png", "one face only");
    writeBytes(dir, "textures/dev/wall.png", "wall");
    CHECK(listSkyboxes(dir.path) == std::vector<std::string>{"env/day_cube", "env/dusk_cube", "env/half_cube", "env/night_cube"});
    const auto all = listTextures(dir.path, GameProfile{});
    CHECK(std::find(all.begin(), all.end(), "env/day_cube") != all.end());
    CHECK(std::find(all.begin(), all.end(), "dev/wall") != all.end());
    CHECK(isCubemapTexture("env/day_cube"));
    CHECK(isCubemapTexture("sky/whatever/day_cube"));
    CHECK_FALSE(isCubemapTexture("env/cube_day"));
    const auto day = readSkybox(dir.path, "env/day_cube");
    REQUIRE(day);
    CHECK_FALSE(day->panorama);
    REQUIRE(day->images.size() == 6);
    CHECK(std::string(day->images[2].bytes.begin(), day->images[2].bytes.end()) == "_up");  // the engine's order lf rt up dn ft bk
    CHECK(day->images[5].ext == ".jpg");
    const auto dusk = readSkybox(dir.path, "anything/dusk_cube");  // only the last part of the name counts, as in the engine
    REQUIRE(dusk);
    CHECK(dusk->panorama);
    CHECK(std::string(dusk->images[0].bytes.begin(), dusk->images[0].bytes.end()) == "png panorama");
    CHECK(readSkybox(dir.path, "env/night_cube")->panorama);
    CHECK_FALSE(readSkybox(dir.path, "env/half_cube"));
    CHECK_FALSE(readSkybox(dir.path, "env/../x_cube"));
}

TEST_CASE("bsp sky: auto sky box of 6 inward quads, no lightmap, non-solid, in every cluster; per area; far plane warning") {
    // an open level: a floor slab with a wall on it
    Document d;
    d.entities.push_back(ent("w", "worldspawn", {Brush::makeBox("floor", {0, -0.5, 0}, {20, 0, 20}, "dev/floor"), Brush::makeBox("wall", {5, 0, 5}, {6, 3, 15}, "dev/wall")}));
    d.entities.push_back(ent("p", "info_player_start", {}, {2, 1, 2}));
    std::shared_ptr<SolidSet> solids;
    BspInput in = inputFor(d, solids);
    in.settings.visCell = 4;
    const BspOutput plain = compileBsp(in);
    in.settings.sky = {AutoSky::Box, 8, "env/test_cube"};
    const BspOutput out = compileBsp(in);
    CHECK(out.stats.skySurfaces == 6);
    CHECK(out.stats.clusters == plain.stats.clusters);  // the sky is not cut into cells
    for (const auto& w : out.warnings) CHECK(w.find("far plane") == std::string::npos);
    BspView v;
    REQUIRE(v.open(out.bsp));
    const auto sky = surfacesWithShader(v, "textures/env/test_cube");
    REQUIRE(sky.size() == 6);
    const auto shaders = v.lump<rbsp::Shader>(rbsp::kShaders);
    const auto surfaces = v.lump<rbsp::Surface>(rbsp::kSurfaces);
    const auto verts = v.lump<rbsp::DrawVert>(rbsp::kDrawVerts);
    const auto idx = v.lump<int32_t>(rbsp::kDrawIndexes);
    const Vec3 centre = toQuake({10, 1, 10});
    for (int si : sky) {
        const auto& s = surfaces[static_cast<size_t>(si)];
        const auto& sh = shaders[static_cast<size_t>(s.shader)];
        CHECK(sh.contentFlags == 0);
        CHECK((sh.surfaceFlags & 4) != 0);     // sky
        CHECK((sh.surfaceFlags & 1024) != 0);  // nolightmap
        CHECK(s.lightmapStyles[0] == rbsp::kNoStyle);
        CHECK(s.lightmapNum[0] == -1);
        CHECK(s.numVerts == 4);
        // faces inwards (towards the level), wound clockwise seen from inside like every other face
        const Vec3 n{s.normal[0], s.normal[1], s.normal[2]};
        const float* p0 = verts[static_cast<size_t>(s.firstVert)].xyz;
        CHECK(n.dot(centre - Vec3{p0[0], p0[1], p0[2]}) > 0);
        const auto& a = verts[static_cast<size_t>(s.firstVert + idx[static_cast<size_t>(s.firstIndex)])].xyz;
        const auto& b = verts[static_cast<size_t>(s.firstVert + idx[static_cast<size_t>(s.firstIndex) + 1])].xyz;
        const auto& c = verts[static_cast<size_t>(s.firstVert + idx[static_cast<size_t>(s.firstIndex) + 2])].xyz;
        CHECK(Vec3{b[0] - a[0], b[1] - a[1], b[2] - a[2]}.cross(Vec3{c[0] - a[0], c[1] - a[1], c[2] - a[2]}).dot(n) < 0);
    }
    // the box: the geometry's bounds (x 0..20, y -0.5..3, z 0..20) plus 8 m; the floor is sky too
    const auto models = v.lump<rbsp::Model>(rbsp::kModels);
    CHECK(models[0].mins[2] == doctest::Approx(-8.5 * 32));
    CHECK(models[0].maxs[2] == doctest::Approx(11 * 32));
    CHECK(models[0].mins[0] == doctest::Approx(-8 * 32));
    // every leaf with a cluster lists all six sky faces: seen from everywhere
    const auto leafs = v.lump<rbsp::Leaf>(rbsp::kLeafs);
    const auto leafSurfaces = v.lump<int32_t>(rbsp::kLeafSurfaces);
    int withCluster = 0;
    for (const auto& l : leafs) {
        if (l.cluster < 0) continue;
        ++withCluster;
        const std::set<int> has(leafSurfaces.begin() + l.firstLeafSurface, leafSurfaces.begin() + l.firstLeafSurface + l.numLeafSurfaces);
        for (int si : sky) CHECK(has.count(si) == 1);
    }
    CHECK(withCluster > 1);
    CHECK(out.stats.brushes == plain.stats.brushes);  // no collision brush for the auto sky

    // per area: two islands 100 m apart get a box each; 10 m apart their boxes (with the 8 m margin) overlap and merge
    for (const double gap : {100.0, 10.0}) {
        CAPTURE(gap);
        Document two;
        two.entities.push_back(ent("w", "worldspawn", {Brush::makeBox("a", {0, -0.5, 0}, {10, 0, 10}), Brush::makeBox("a2", {2, 0, 2}, {3, 2, 3}),
                                                       Brush::makeBox("b", {10 + gap, -0.5, 0}, {20 + gap, 0, 10})}));
        std::shared_ptr<SolidSet> s2;
        BspInput ti = inputFor(two, s2);
        ti.settings.visibility = false;
        ti.settings.sky = {AutoSky::Areas, 8, "env/test_cube"};
        CHECK(compileBsp(ti).stats.skySurfaces == (gap > 16 ? 12u : 6u));
        ti.settings.sky.mode = AutoSky::Box;
        CHECK(compileBsp(ti).stats.skySurfaces == 6u);
    }
    // a box larger than the engine's far plane (3000 m) is reported
    in.settings.sky.margin = 1000;
    in.settings.visibility = false;
    const BspOutput huge = compileBsp(in);
    bool warned = false;
    for (const auto& w : huge.warnings) warned |= w.find("far plane") != std::string::npos;
    CHECK(warned);
}

TEST_CASE("bsp sky: hand-placed sky faces are kept even when buried, have no lightmap and no contents; flat sky textures are reported") {
    Document d;
    // a sky slab lying on the floor (its bottom face is buried in the floor) and a flat sky texture on a second slab
    d.entities.push_back(ent("w", "worldspawn", {Brush::makeBox("floor", {0, -0.5, 0}, {20, 0, 20}, "dev/floor"), Brush::makeBox("sky", {0, 0, 0}, {20, 0.5, 20}, "env/test_cube"),
                                                 Brush::makeBox("flat", {0, 6, 0}, {4, 6.5, 4}, "sky/blue")}));
    std::shared_ptr<SolidSet> solids;
    BspInput in = inputFor(d, solids);
    in.settings.visibility = false;
    const BspOutput out = compileBsp(in);
    BspView v;
    REQUIRE(v.open(out.bsp));
    const auto sky = surfacesWithShader(v, "textures/env/test_cube");
    CHECK(sky.size() == 6);  // the face lying on the floor too
    const auto surfaces = v.lump<rbsp::Surface>(rbsp::kSurfaces);
    for (int si : sky) CHECK(surfaces[static_cast<size_t>(si)].lightmapNum[0] == -1);
    const auto brushes = v.lump<rbsp::Brush>(rbsp::kBrushes);
    const auto shaders = v.lump<rbsp::Shader>(rbsp::kShaders);
    int nonSolid = 0;
    for (const auto& b : brushes) nonSolid += shaders[static_cast<size_t>(b.shader)].contentFlags == 0;
    CHECK(nonSolid == 2);  // the two sky brushes
    bool flat = false;
    for (const auto& w : out.warnings) flat |= w.find("sky/blue") != std::string::npos;
    CHECK(flat);
    const BspContentRules def = BspContentRules::defaults();
    CHECK(def.isSky("env/test_cube"));
    CHECK(def.isSky("sky/blue"));
    CHECK_FALSE(def.isSky("dev/wall"));
    CHECK(def.forFace("env/test_cube").contents == 0);
}

TEST_CASE("bsp sky: the lighting never sees sky faces; the sky light takes the level's skybox; the export draws the auto sky") {
    TempDir dir;
    // a skybox bright in the east only (_rt), black elsewhere
    for (const char* f : {"_lf", "_rt", "_up", "_dn", "_ft", "_bk"}) writeFace(dir, std::string("env/east_cube") + f + ".png", std::string(f) == "_rt" ? 1.f : 0.f);
    json world = entityJson("w", "worldspawn", {Brush::makeBox("floor", {-4, -1, -4}, {4, 0, 4}), Brush::makeBox("lid", {-4, 3, -4}, {4, 3.2, 4}, "env/east_cube")});
    world["keyvalues"] = {{"_sky_auto", "box"}, {"_sky_margin", "4"}};
    json sun = entityJson("k", "light_sky", {}, {0, 0, 0});
    sun["keyvalues"] = {{"brightness", "2"}};
    dir.write("maps/sky.map.json", docJson({world, sun, entityJson("p", "info_player_start", {}, {1, 0.5, 1})}));
    Scene scene(dir.path);
    scene.loadGameProfile();
    scene.openDocument("maps/sky.map.json");
    size_t lidFaces = 0, all = 0;
    for (const auto& s : scene.lightSurfaces(true)) { ++all; lidFaces += s.texture == "env/east_cube"; }
    CHECK(lidFaces == 6);
    CHECK(scene.lightSurfaces().size() == all - 6);  // no lightmap for them, and they block no light
    const LightScene lights = scene.lightScene();
    REQUIRE(lights.lights.size() == 1);
    REQUIRE(lights.lights[0].sky);  // no _sky_texture: Auto sky is on, so the first skybox of env/
    CHECK(lights.lights[0].sky->texture == "env/east_cube");
    CHECK(lights.lights[0].sky->at({1, 0.3, 0}).x > 0.9);
    CHECK(lights.lights[0].sky->at({-1, 0.3, 0}).x < 0.01);
    CHECK(scene.lightScene().lights[0].sky == lights.lights[0].sky);  // cached while the files do not change

    BspExportRequest req;
    req.assetsRoot = dir.path;
    req.document = scene.documentSyncJson()["doc"];
    req.mapPath = "maps/sky.map.json";
    req.outPath = "maps/sky.bsp";
    req.bake = false;
    const BspExportResult r = exportBsp(req);
    INFO(r.toJson().dump());
    REQUIRE(r.ok);
    CHECK(r.stats.skySurfaces == 6);
    std::ifstream f(dir.path / "maps/sky.bsp", std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    BspView v;
    REQUIRE(v.open(bytes));
    CHECK(surfacesWithShader(v, "textures/env/east_cube").size() == 12);  // the lid's 6 faces and the auto box's 6
}

TEST_CASE("bsp lightmaps: the base charts are packed again into fewer pages and every vertex still reads its own chart's light") {
    Document d = twoRooms(true);
    std::shared_ptr<SolidSet> solids;
    BspInput in = inputFor(d, solids);
    in.settings.visibility = false;
    in.settings.lightmapTolerance = 0;  // this test is about the packing, not the fitting
    LightmapSettings ls;
    ls.atlasSize = 64;  // small pages: the live layout (buckets stay together) needs several, the repack fewer
    // a density at which packing all charts by size saves a page (found by trying: the layouts are deterministic)
    LightmapLayout layout;
    bool saves = false;
    for (double tpm : {2.0, 2.5, 3.0, 3.5, 4.0, 5.0, 6.0, 7.0, 8.0}) {
        ls.texelsPerMeter = tpm;
        layout = buildLightmapLayout(in.surfaces, in.classes, ls);
        std::vector<std::pair<int, int>> sizes;
        for (const auto& c : layout.charts) sizes.push_back({c.w, c.h});
        int pages = 0;
        packRects(sizes, 64, pages);
        if (pages < layout.pageCount) { saves = true; break; }
    }
    REQUIRE(saves);
    in.layout = &layout;
    LightmapPages p;
    p.atlas = 64;
    p.pageCount = layout.pageCount;
    // every chart is filled with a value of its own (its index + 1) inside its rectangle, 0 outside: after the repack a vertex must still find that value
    for (int k = 0; k < p.pageCount; ++k) {
        p.rgb.push_back(std::vector<float>(64 * 64 * 3, 0.0f));
        p.valid.push_back(std::vector<uint8_t>(64 * 64, 1));
    }
    for (size_t c = 0; c < layout.charts.size(); ++c) {
        const auto& ch = layout.charts[c];
        for (int y = 0; y < ch.h; ++y) for (int x = 0; x < ch.w; ++x) for (int q = 0; q < 3; ++q) p.rgb[static_cast<size_t>(ch.page)][(static_cast<size_t>(ch.y + y) * 64 + ch.x + x) * 3 + q] = float(c + 1);
    }
    in.pages[0] = p;
    BspInput off = in;
    off.settings.repackLightmaps = false;
    const BspOutput kept = compileBsp(off);
    const BspOutput out = compileBsp(in);
    CHECK(kept.lightmaps.size() == static_cast<size_t>(layout.pageCount));
    CHECK(out.lightmaps.size() < kept.lightmaps.size());
    BspView v;
    REQUIRE(v.open(out.bsp));
    const auto surfaces = v.lump<rbsp::Surface>(rbsp::kSurfaces);
    const auto verts = v.lump<rbsp::DrawVert>(rbsp::kDrawVerts);
    const auto idx = v.lump<int32_t>(rbsp::kDrawIndexes);
    (void)idx;
    // the same chart value must be read by the vertex in both builds
    BspView v2;
    REQUIRE(v2.open(kept.bsp));
    const auto surfaces2 = v2.lump<rbsp::Surface>(rbsp::kSurfaces);
    const auto verts2 = v2.lump<rbsp::DrawVert>(rbsp::kDrawVerts);
    REQUIRE(surfaces.size() == surfaces2.size());
    auto read = [&](const BspOutput& o, const rbsp::DrawVert& dv, int page) {
        const int x = std::clamp(static_cast<int>(dv.lightmap[0][0] * 64), 0, 63), y = std::clamp(static_cast<int>(dv.lightmap[0][1] * 64), 0, 63);
        return o.lightmaps[static_cast<size_t>(page)].rgb[(static_cast<size_t>(y) * 64 + x) * 3];
    };
    size_t checked = 0;
    for (size_t i = 0; i < surfaces.size(); ++i) {
        if (surfaces[i].lightmapNum[0] < 0) continue;
        REQUIRE(surfaces[i].numVerts == surfaces2[i].numVerts);
        for (int k = 0; k < surfaces[i].numVerts; ++k) {
            const float a = read(out, verts[static_cast<size_t>(surfaces[i].firstVert + k)], surfaces[i].lightmapNum[0]);
            const float b = read(kept, verts2[static_cast<size_t>(surfaces2[i].firstVert + k)], surfaces2[i].lightmapNum[0]);
            CHECK(a == b);
            ++checked;
        }
    }
    CHECK(checked > 0);
}

TEST_CASE("lightmap fit: flat and gradient charts need a few texels, noise needs them all, and the error is what the engine would show") {
    constexpr int A = 64;
    std::vector<float> page(A * A * 3, 0.0f);
    std::vector<uint8_t> valid(A * A, 1);
    auto fill = [&](auto f) { for (int y = 0; y < A; ++y) for (int x = 0; x < A; ++x) for (int c = 0; c < 3; ++c) page[(static_cast<size_t>(y) * A + x) * 3 + c] = f(x, y, c); };
    FitLayer l;
    l.rgb = page.data(); l.valid = valid.data(); l.atlas = A; l.x0 = 8; l.y0 = 8; l.w = 32; l.h = 24;
    FitSettings fs;
    fs.exposure = 1.8;
    fill([](int, int, int c) { return 0.04f + 0.001f * c; });
    ChartFit flat = fitChart({l}, fs);
    CHECK(flat.w == 1);
    CHECK(flat.h == 1);
    fill([](int x, int y, int) { return 0.05f + 0.002f * x + 0.001f * y; });  // a plain ramp
    ChartFit ramp = fitChart({l}, fs);
    CHECK(ramp.w <= 4);
    CHECK(ramp.h <= 4);
    uint32_t seed = 7;
    fill([&](int, int, int) { seed = seed * 1664525u + 1013904223u; return 0.05f + 0.04f * ((seed >> 8) & 255) / 255.0f; });  // noise
    ChartFit noise = fitChart({l}, fs);
    CHECK(noise.w == 32);
    CHECK(noise.h == 24);
    // identity resampling copies; a smaller one keeps the mean
    const auto same = resampleLayer(l, 32, 24);
    CHECK(same[0] == page[(8 * A + 8) * 3]);
    fill([](int x, int, int) { return x < 24 ? 0.1f : 0.3f; });
    const auto half = resampleLayer(l, 2, 1);
    CHECK(half[0] == doctest::Approx(0.1f));
    CHECK(half[3] == doctest::Approx(0.3f));
    // the same light stored twice hashes the same, other light does not
    fill([](int, int, int) { return 0.2f; });
    const ChartFit a = fitChart({l}, fs);
    l.x0 = 20;
    const ChartFit b = fitChart({l}, fs);
    CHECK(a.hash == b.hash);
    fill([](int, int, int) { return 0.21f; });
    CHECK(fitChart({l}, fs).hash != a.hash);
    // a step edge does not survive 2 x 1 texels
    l.x0 = 8;
    fill([](int x, int, int) { return x < 24 ? 0.1f : 0.5f; });
    const auto two = resampleLayer(l, 2, 1);
    CHECK(fitError(l, two, 2, 1, 1.0, 2.5).over > 0.01);
}

TEST_CASE("bsp lightmaps: smooth charts are stored with fewer texels and every vertex still reads the same light") {
    Document d = twoRooms(true);
    std::shared_ptr<SolidSet> solids;
    BspInput in = inputFor(d, solids);
    in.settings.visibility = false;
    LightmapSettings ls;
    ls.texelsPerMeter = 6;
    ls.atlasSize = 128;
    LightmapLayout layout = buildLightmapLayout(in.surfaces, in.classes, ls);
    REQUIRE(layout.charts.size() > 2);
    in.layout = &layout;
    LightmapPages p;
    p.atlas = 128;
    p.pageCount = layout.pageCount;
    for (int k = 0; k < p.pageCount; ++k) {
        p.rgb.push_back(std::vector<float>(128 * 128 * 3, 0.0f));
        p.valid.push_back(std::vector<uint8_t>(128 * 128, 1));
    }
    // a ramp across every chart (the chart's own number as the base level)
    for (size_t c = 0; c < layout.charts.size(); ++c) {
        const auto& ch = layout.charts[c];
        for (int y = 0; y < ch.h; ++y)
            for (int x = 0; x < ch.w; ++x)
                for (int q = 0; q < 3; ++q) p.rgb[static_cast<size_t>(ch.page)][(static_cast<size_t>(ch.y + y) * 128 + ch.x + x) * 3 + q] = 0.05f + 0.01f * float(c % 5) + 0.001f * x;
    }
    in.pages[0] = p;
    BspInput off = in;
    off.settings.lightmapTolerance = 0;
    const BspOutput kept = compileBsp(off);
    const BspOutput out = compileBsp(in);
    CHECK(out.stats.lightmapTexelsAfter < out.stats.lightmapTexelsBefore / 2);
    CHECK(out.lightmaps.size() <= kept.lightmaps.size());
    BspView v, v2;
    REQUIRE(v.open(out.bsp));
    REQUIRE(v2.open(kept.bsp));
    const auto s1 = v.lump<rbsp::Surface>(rbsp::kSurfaces), s2 = v2.lump<rbsp::Surface>(rbsp::kSurfaces);
    const auto d1 = v.lump<rbsp::DrawVert>(rbsp::kDrawVerts), d2 = v2.lump<rbsp::DrawVert>(rbsp::kDrawVerts);
    REQUIRE(s1.size() == s2.size());
    auto bilinear = [&](const BspOutput& o, int page, float u, float vv) {
        const float fx = u * 128 - 0.5f, fy = vv * 128 - 0.5f;
        const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
        const float tx = fx - x0, ty = fy - y0;
        auto px = [&](int x, int y) { return o.lightmaps[static_cast<size_t>(page)].rgb[(static_cast<size_t>(std::clamp(y, 0, 127)) * 128 + std::clamp(x, 0, 127)) * 3]; };
        return (px(x0, y0) * (1 - tx) + px(x0 + 1, y0) * tx) * (1 - ty) + (px(x0, y0 + 1) * (1 - tx) + px(x0 + 1, y0 + 1) * tx) * ty;
    };
    size_t checked = 0;
    for (size_t i = 0; i < s1.size(); ++i) {
        if (s1[i].lightmapNum[0] < 0) continue;
        for (int k = 0; k < s1[i].numVerts; ++k) {
            const auto& a = d1[static_cast<size_t>(s1[i].firstVert + k)];
            const auto& b = d2[static_cast<size_t>(s2[i].firstVert + k)];
            // inset a little from the chart edge (vertices lie on it): bilinear there reads the gutter
            CHECK(bilinear(out, s1[i].lightmapNum[0], a.lightmap[0][0], a.lightmap[0][1]) == doctest::Approx(bilinear(kept, s2[i].lightmapNum[0], b.lightmap[0][0], b.lightmap[0][1])).epsilon(0.1));
            ++checked;
        }
    }
    CHECK(checked > 0);
}

// Measurement on an exported level (not a regression test): MAPEDITOR_FIT_BSP=<map.bsp> (its lm_NNNN.exr pages beside it, in <map>/), MAPEDITOR_FIT_EXPOSURE
TEST_CASE("lightmap fit: measure an exported level" * doctest::skip(std::getenv("MAPEDITOR_FIT_BSP") == nullptr)) {
    namespace fs = std::filesystem;
    const fs::path bspPath = std::getenv("MAPEDITOR_FIT_BSP");
    std::ifstream f(bspPath, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    BspView v;
    REQUIRE(v.open(bytes));
    const double exposure = std::getenv("MAPEDITOR_FIT_EXPOSURE") ? std::atof(std::getenv("MAPEDITOR_FIT_EXPOSURE")) : 1.8;
    const double tolerance = std::getenv("MAPEDITOR_FIT_TOLERANCE") ? std::atof(std::getenv("MAPEDITOR_FIT_TOLERANCE")) : 2.5;
    std::map<int, ExrImage> pages;
    std::set<std::array<int, 5>> charts;  // page, x, y, w, h
    for (const auto& s : v.lump<rbsp::Surface>(rbsp::kSurfaces)) {
        if (s.lightmapNum[0] < 0 || s.lightmapStyles[0] != 0) continue;
        charts.insert({s.lightmapNum[0], s.lightmapCorner[0][0], s.lightmapCorner[0][1], s.lightmapSize[0], s.lightmapSize[1]});
    }
    for (const auto& c : charts) {
        if (pages.count(c[0])) continue;
        char name[32];
        std::snprintf(name, sizeof name, "lm_%04d.exr", c[0]);
        REQUIRE(readExr((bspPath.parent_path() / bspPath.stem() / name).string(), pages[c[0]]));
    }
    const std::vector<std::array<int, 5>> list(charts.begin(), charts.end());
    std::vector<ChartFit> fits(list.size());
    const auto t0 = std::chrono::steady_clock::now();
    parallelFor(list.size(), 8, [&](size_t b, size_t e) {
        for (size_t i = b; i < e; ++i) {
            const auto& c = list[i];
            const ExrImage& img = pages.at(c[0]);
            FitLayer l;
            l.rgb = img.rgb.data(); l.atlas = img.width; l.x0 = c[1] + 2; l.y0 = c[2] + 2; l.w = c[3] - 4; l.h = c[4] - 4;
            if (l.w < 1 || l.h < 1) { fits[i].w = std::max(1, l.w); fits[i].h = std::max(1, l.h); continue; }
            FitSettings fs;
            fs.exposure = exposure;
            fs.tolerance = tolerance;
            fits[i] = fitChart({l}, fs);
        }
    });
    {  // how much of it fits 8 bits (JPEG pages, linear bytes)
        std::vector<char> okLin(list.size(), 0);
        parallelFor(list.size(), 8, [&](size_t b, size_t e) {
            for (size_t i = b; i < e; ++i) {
                const auto& c = list[i];
                const ExrImage& img = pages.at(c[0]);
                FitLayer l;
                l.rgb = img.rgb.data(); l.atlas = img.width; l.x0 = c[1] + 2; l.y0 = c[2] + 2; l.w = c[3] - 4; l.h = c[4] - 4;
                if (l.w < 1 || l.h < 1) continue;
                FitSettings fs;
                fs.exposure = exposure;
                fs.tolerance = tolerance;
                okLin[i] = fitsEightBits({l}, fits[i].w, fits[i].h, fs);
            }
        });
        long long lin = 0, all = 0;
        for (size_t i = 0; i < list.size(); ++i) {
            const long long t = static_cast<long long>(fits[i].w + 4) * (fits[i].h + 4);
            all += t;
            if (okLin[i]) lin += t;
        }
        std::printf("8 bit pages: %.1f%% of the stored texels fit linear bytes\n", 100.0 * lin / all);
    }
    long long before = 0, after = 0, flat = 0;
    std::set<std::tuple<int, int, uint64_t>> distinct;
    for (size_t i = 0; i < list.size(); ++i) {
        before += static_cast<long long>(list[i][3]) * list[i][4];
        after += static_cast<long long>(fits[i].w + 4) * (fits[i].h + 4);
        flat += fits[i].w * fits[i].h <= 1;
        distinct.insert({fits[i].w, fits[i].h, fits[i].hash});
    }
    {
        const int edges[] = {16, 32, 64, 128, 100000};
        long long b[5] = {0}, a2[5] = {0}, n[5] = {0};
        for (size_t i = 0; i < list.size(); ++i) {
            const int m = std::max(list[i][3], list[i][4]);
            int k = 0;
            while (m > edges[k]) ++k;
            b[k] += static_cast<long long>(list[i][3]) * list[i][4];
            a2[k] += static_cast<long long>(fits[i].w + 4) * (fits[i].h + 4);
            ++n[k];
        }
        for (int k = 0; k < 5; ++k) std::printf("  charts up to %6d: %6lld, texels %.2f M -> %.2f M\n", edges[k], n[k], b[k] / 1e6, a2[k] / 1e6);
    }
    long long shared = 0;
    for (const auto& d : distinct) shared += static_cast<long long>(std::get<0>(d) + 4) * (std::get<1>(d) + 4);
    std::printf("charts %zu | texels %.2f M -> %.2f M (%.1f%%), with identical charts shared %.2f M (%.1f%%) | 1x1 charts %lld | tolerance %.1f levels, exposure %.2f | %.1f s\n", list.size(),
                before / 1e6, after / 1e6, 100.0 * after / before, shared / 1e6, 100.0 * shared / before, flat, tolerance, exposure,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
}

TEST_CASE("jpeg: the encoder's baseline files are read back by the baseline decoder, smooth data within a few levels") {
    constexpr int W = 40, H = 24;  // not multiples of 8: the edge blocks are padded
    std::vector<uint8_t> rgb(W * H * 3);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { rgb[(y * W + x) * 3] = uint8_t(20 + x * 4); rgb[(y * W + x) * 3 + 1] = uint8_t(200 - y * 5); rgb[(y * W + x) * 3 + 2] = 90; }
    const std::vector<uint8_t> file = encodeJpeg(W, H, rgb.data(), 95);
    REQUIRE(file.size() > 100);
    CHECK(file[0] == 0xFF);
    CHECK(file[1] == 0xD8);
    const auto img = decodeJpeg(file);
    REQUIRE(img.has_value());
    CHECK(img->width == W);
    CHECK(img->height == H);
    int worst = 0;
    for (int i = 0; i < W * H; ++i) for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(int(img->rgba[i * 4 + c]) - int(rgb[i * 3 + c])));
    CHECK(worst <= 4);
}

namespace fs = std::filesystem;

TEST_CASE("bsp lightmaps: with lightmapJpeg the charts that fit 8 bits go to .jpg pages, the others (above 1) stay .exr, and every vertex reads the same light") {
    Document d = twoRooms(true);
    std::shared_ptr<SolidSet> solids;
    BspInput in = inputFor(d, solids);
    in.settings.visibility = false;
    LightmapSettings ls;
    ls.texelsPerMeter = 6;
    ls.atlasSize = 128;
    LightmapLayout layout = buildLightmapLayout(in.surfaces, in.classes, ls);
    REQUIRE(layout.charts.size() > 3);
    in.layout = &layout;
    LightmapPages p;
    p.atlas = 128;
    p.pageCount = layout.pageCount;
    for (int k = 0; k < p.pageCount; ++k) {
        p.rgb.push_back(std::vector<float>(128 * 128 * 3, 0.0f));
        p.valid.push_back(std::vector<uint8_t>(128 * 128, 1));
    }
    // even charts: a smooth 0.4..0.7 ramp; odd charts: bright (3.0), above what a JPEG holds
    for (size_t c = 0; c < layout.charts.size(); ++c) {
        const auto& ch = layout.charts[c];
        for (int y = 0; y < ch.h; ++y)
            for (int x = 0; x < ch.w; ++x)
                for (int q = 0; q < 3; ++q) p.rgb[static_cast<size_t>(ch.page)][(static_cast<size_t>(ch.y + y) * 128 + ch.x + x) * 3 + q] = c % 2 ? 3.0f : 0.4f + 0.3f * float(x) / float(std::max(1, ch.w));
    }
    in.pages[0] = p;
    in.settings.lightmapJpeg = true;
    in.settings.lightmapExposure = 1.0;
    const BspOutput out = compileBsp(in);
    MESSAGE("jpeg pages ", out.stats.lightmapJpegPages, " rejected ", out.stats.lightmapJpegRejected, " pages ", out.lightmaps.size());
    CHECK(out.stats.lightmapJpegPages >= 1);
    size_t jpgPages = 0, exrPages = 0;
    for (const auto& pg : out.lightmaps) {
        if (pg.ldr) {
            ++jpgPages;
            CHECK(!pg.jpg.empty());
            for (float v : pg.rgb) CHECK(v <= 1.0f);
        } else {
            ++exrPages;
            CHECK(pg.jpg.empty());
        }
    }
    CHECK(jpgPages >= 1);
    CHECK(exrPages >= 1);
    BspView v;
    REQUIRE(v.open(out.bsp));
    const auto surfaces = v.lump<rbsp::Surface>(rbsp::kSurfaces);
    const auto verts = v.lump<rbsp::DrawVert>(rbsp::kDrawVerts);
    size_t bright = 0, smooth = 0;
    for (const auto& s : surfaces) {
        if (s.lightmapNum[0] < 0) continue;
        const BspLightmapPage& pg = out.lightmaps[static_cast<size_t>(s.lightmapNum[0])];
        const auto& dv = verts[static_cast<size_t>(s.firstVert)];
        const int x = std::clamp(static_cast<int>(dv.lightmap[0][0] * 128), 0, 127), y = std::clamp(static_cast<int>(dv.lightmap[0][1] * 128), 0, 127);
        const float value = pg.rgb[(static_cast<size_t>(y) * 128 + x) * 3];
        if (value > 1.5f) { CHECK(!pg.ldr); ++bright; }
        else { CHECK(pg.ldr); CHECK(value >= 0.3f); CHECK(value <= 0.8f); ++smooth; }
    }
    CHECK(bright > 0);
    CHECK(smooth > 0);
    // the files: .jpg for 8 bit pages, .exr for the others; an older export's leftovers are gone
    TempDir dir;
    const fs::path lm = dir.path / "map";
    fs::create_directories(lm);
    std::ofstream(lm / "lm_0007.exr") << "old";
    std::ofstream(lm / "lm_0000.jpg") << "old";
    std::ofstream(lm / "keep.txt") << "mine";
    writeLightmapPages(lm, out.lightmaps);
    for (size_t i = 0; i < out.lightmaps.size(); ++i) {
        char name[32];
        std::snprintf(name, sizeof name, out.lightmaps[i].ldr ? "lm_%04d.jpg" : "lm_%04d.exr", static_cast<int>(i));
        CHECK(fs::exists(lm / name));
        std::snprintf(name, sizeof name, out.lightmaps[i].ldr ? "lm_%04d.exr" : "lm_%04d.jpg", static_cast<int>(i));
        CHECK(!fs::exists(lm / name));
    }
    CHECK(!fs::exists(lm / "lm_0007.exr"));
    CHECK(fs::exists(lm / "keep.txt"));
}

TEST_CASE("bsp compile: triangles without area are not written (the engine's physics mesh refuses them)") {
    Document d = twoRooms();
    std::shared_ptr<SolidSet> solids;
    BspInput in = inputFor(d, solids);
    in.settings.visibility = false;
    // a square with an extra corner in the middle of one side, listed so that the fan from the first stored vertex starts with three collinear corners
    LightSurface extra = in.surfaces.front();
    extra.verts = {{3, 5, 3}, {2, 5, 3}, {2, 5, 2}, {2.5, 5, 2}, {3, 5, 2}};
    extra.normal = {0, -1, 0};
    in.surfaces.push_back(extra);
    const BspOutput out = compileBsp(in);
    CHECK(out.stats.degenerateTriangles >= 1);
    BspView v;
    REQUIRE(v.open(out.bsp));
    const auto surfaces = v.lump<rbsp::Surface>(rbsp::kSurfaces);
    const auto verts = v.lump<rbsp::DrawVert>(rbsp::kDrawVerts);
    const auto idx = v.lump<int32_t>(rbsp::kDrawIndexes);
    size_t tris = 0;
    for (const auto& s : surfaces)
        for (int t = 0; t + 2 < s.numIndexes; t += 3) {
            auto p = [&](int k) { const auto& dv = verts[static_cast<size_t>(s.firstVert + idx[static_cast<size_t>(s.firstIndex + t + k)])]; return Vec3{dv.xyz[0], dv.xyz[1], dv.xyz[2]}; };
            CHECK((p(1) - p(0)).cross(p(2) - p(0)).length() > 1e-3);
            ++tris;
        }
    CHECK(tris > 0);
}
