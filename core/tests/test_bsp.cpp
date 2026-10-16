// rBSP export (REQUIREMENTS В§25, DESIGN В§12): EXR lightmaps, convex collision pieces, the compiler.
#include "core/bsp_compile.h"
#include "core/exr.h"
#include "core/lighting_classes.h"
#include "doctest.h"
#include "test_util.h"
#include <cmath>
#include <fstream>
#include <set>
#include <thread>
#include "core/router.h"
#include "core/bsp_export.h"
#include "core/scene.h"

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
