// Smooth edges (REQUIREMENTS §33 H6): corner normals, interpolation across a face, the light baker's sample normals, one lightmap chart.
#include "core/bsp_compile.h"
#include "core/derived.h"
#include "core/document.h"
#include "core/lightbake.h"
#include "core/lightmap_uv.h"
#include "core/smoothing.h"
#include "doctest.h"
#include <cmath>
#include <nlohmann/json.hpp>
#include <set>

using namespace core;

namespace {

constexpr double kPi = 3.14159265358979323846;

// an octagonal prism standing on y = 0, 2 m high; `smooth` marks its 8 vertical edges
Brush octagon(bool smooth) {
    Brush b;
    b.id = "oct";
    for (int k = 0; k < 2; ++k)
        for (int i = 0; i < 8; ++i) {
            const double a = 2 * kPi * i / 8;
            b.vertices.push_back({std::cos(a), k * 2.0, std::sin(a)});
        }
    uint32_t id = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        BrushFace f;
        f.id = id++;
        f.loop = {i, 8 + i, 8 + (i + 1) % 8, (i + 1) % 8};
        b.faces.push_back(f);
    }
    BrushFace bottom, top;
    bottom.id = id++;
    top.id = id++;
    for (uint32_t i = 0; i < 8; ++i) { bottom.loop.push_back(i); top.loop.push_back(15 - i); }
    b.faces.push_back(bottom);
    b.faces.push_back(top);
    if (b.signedVolume() < 0)
        for (auto& f : b.faces) std::reverse(f.loop.begin(), f.loop.end());
    if (smooth)
        for (uint32_t i = 0; i < 8; ++i) b.smoothEdges.push_back({i, (i + 1) % 8});  // side i meets side i + 1 at a vertical edge
    return b;
}

Document docWith(const Brush& b) {
    Document doc;
    Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    w.brushes = {b};
    doc.entities = {w};
    return doc;
}

}  // namespace

TEST_CASE("smoothing: corner normals average the faces joined by smooth edges, the caps stay flat") {
    const Brush b = octagon(true);
    REQUIRE(!b.checkConvexSolid());
    const auto s = brushSmoothing(b);
    REQUIRE(s);
    CHECK(s->faces.size() == 8);  // the sides; the caps meet the sides at unmarked edges
    for (const auto& [id, sh] : s->faces) {
        REQUIRE(sh.corners.size() == 4);
        for (size_t c = 0; c < 4; ++c) {
            const Vec3 radial = Vec3{sh.corners[c].x, 0, sh.corners[c].z}.normalized();
            CHECK(sh.normals[c].dot(radial) > 0.9999);  // a vertex of the octagon: the normal points away from the axis
        }
        // the middle of the face keeps (by symmetry) its own normal
        Vec3 mid;
        for (const Vec3& p : sh.corners) mid = mid + p * 0.25;
        const Vec3 fn = b.faceNormal(*std::find_if(b.faces.begin(), b.faces.end(), [&](const BrushFace& f) { return f.id == id; }));
        CHECK(shadingNormalAt(sh, mid, fn).dot(fn) > 0.99999);
        // on an edge: linear between its corners
        const Vec3 e = (sh.corners[0] + sh.corners[1]) * 0.5;
        const Vec3 want = (sh.normals[0] + sh.normals[1]).normalized();
        CHECK(shadingNormalAt(sh, e, fn).dot(want) > 0.99999);
    }
    CHECK(s->onSmoothEdge(b.vertices[0], b.vertices[8]));
    CHECK(s->onSmoothEdge(b.vertices[0] + Vec3{0, 0.5, 0}, b.vertices[0] + Vec3{0, 1.5, 0}));
    CHECK_FALSE(s->onSmoothEdge(b.vertices[0], b.vertices[1]));
    CHECK_FALSE(brushSmoothing(octagon(false)));
}

TEST_CASE("smoothing: the document's `smooth` key (face id pairs), malformed entries skipped") {
    const nlohmann::json j = {{"id", "b"}, {"vertices", {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}}},
                              {"faces", nlohmann::json::array({{{"id", 0}, {"loop", {0, 2, 1}}}, {{"id", 1}, {"loop", {0, 1, 3}}}, {{"id", 2}, {"loop", {0, 3, 2}}}, {{"id", 3}, {"loop", {1, 2, 3}}}})},
                              {"smooth", nlohmann::json::array({{0, 1}, "x", {2}, 7, {-1, 2}, {1, 3}})}};
    const Brush b = brushFromJson(j);
    REQUIRE(b.smoothEdges.size() == 2);
    CHECK(b.smoothEdges[0] == std::pair<uint32_t, uint32_t>{0, 1});
    CHECK(b.smoothEdges[1] == std::pair<uint32_t, uint32_t>{1, 3});
}

TEST_CASE("smoothing: texel samples of a smoothed cylinder turn across each face, its sides share one lightmap chart") {
    for (const bool smooth : {false, true}) {
        CAPTURE(smooth);
        const Document doc = docWith(octagon(smooth));
        const auto surfaces = collectSurfaces(doc, {});
        size_t shaded = 0;
        for (const auto& s : surfaces) if (s.shading) ++shaded;
        CHECK(shaded == (smooth ? 8u : 0u));
        LightmapSettings cfg;
        cfg.texelsPerMeter = 8;
        const LightmapLayout layout = buildLightmapLayout(surfaces, LightingClasses::defaults(), cfg);
        std::set<int> sideCharts;
        for (const auto& f : layout.faces) if (f.faceId < 8) sideCharts.insert(f.chart);
        CHECK(sideCharts.size() == (smooth ? 1u : 8u));  // 45 degrees between sides: only a smooth edge joins them
        const TexelSamples t = buildTexelSamples(layout, surfaces, nullptr);
        // the most different normals among the side samples (y ~ 0): faceted = 8 directions only, smooth = many in between
        std::set<int> directions;
        for (size_t i = 0; i < t.count(); ++i) {
            const double nx = t.nrm[i * 4], ny = t.nrm[i * 4 + 1], nz = t.nrm[i * 4 + 2];
            if (std::fabs(ny) > 0.5) continue;
            directions.insert(static_cast<int>(std::lround(std::atan2(nz, nx) * 180 / kPi)));
        }
        if (smooth) CHECK(directions.size() > 30);
        else CHECK(directions.size() == 8);
    }
}

TEST_CASE("smoothing: the BSP export writes the smooth vertex normals (radial at every corner of a smoothed cylinder)") {
    for (const bool smooth : {false, true}) {
        CAPTURE(smooth);
        Document doc = docWith(octagon(smooth));
        Entity p;
        p.id = "p";
        p.classname = "info_player_start";
        p.origin = {3, 1, 0};
        doc.entities.push_back(p);
        BspInput in;
        const DocumentLoader loader = [](const std::string& f) -> const Document& { throw std::runtime_error("no prefab " + f); };
        in.entities = flattenForCompile(doc, loader).entities;
        in.surfaces = collectSurfaces(doc, {});
        in.classes = LightingClasses::defaults();
        const BspOutput out = compileBsp(in);
        BspView v;
        std::string err;
        REQUIRE(v.open(out.bsp, &err));
        size_t sideVerts = 0, radial = 0;
        for (const auto& dv : v.lump<rbsp::DrawVert>(rbsp::kDrawVerts)) {
            const Vec3 n{dv.normal[0], dv.normal[2], -dv.normal[1]}, pos{dv.xyz[0] / 32, dv.xyz[2] / 32, -dv.xyz[1] / 32};  // back to editor space
            if (std::fabs(n.y) > 0.5) continue;  // the caps
            ++sideVerts;
            if (n.dot(Vec3{pos.x, 0, pos.z}.normalized()) > 0.999) ++radial;
        }
        REQUIRE(sideVerts >= 32);
        CHECK(radial == (smooth ? sideVerts : 0u));
    }
}

namespace {
// a UV sphere of radius 1 standing on y = 0 (segments around, rings pole to pole), every edge smooth when asked: the Shell's sphere shape
Brush uvSphere(int segments, int rings, bool smooth) {
    Brush b;
    b.id = "sph";
    b.vertices.push_back({0, 0, 0});
    for (int k = 1; k < rings; ++k)
        for (int j = 0; j < segments; ++j) {
            const double t = kPi * k / rings, a = 2 * kPi * j / segments;
            b.vertices.push_back({std::sin(t) * std::cos(a), 1 - std::cos(t), std::sin(t) * std::sin(a)});
        }
    const uint32_t top = static_cast<uint32_t>(b.vertices.size());
    b.vertices.push_back({0, 2, 0});
    auto ring = [&](int k, int j) { return static_cast<uint32_t>(1 + (k - 1) * segments + ((j % segments) + segments) % segments); };
    uint32_t id = 0;
    auto face = [&](std::vector<uint32_t> loop) { BrushFace f; f.id = id++; f.loop = std::move(loop); b.faces.push_back(f); };
    for (int j = 0; j < segments; ++j) face({0, ring(1, j + 1), ring(1, j)});
    for (int k = 1; k + 1 < rings; ++k)
        for (int j = 0; j < segments; ++j) face({ring(k, j), ring(k, j + 1), ring(k + 1, j + 1), ring(k + 1, j)});
    for (int j = 0; j < segments; ++j) face({top, ring(rings - 1, j), ring(rings - 1, j + 1)});
    if (b.signedVolume() < 0)
        for (auto& f : b.faces) std::reverse(f.loop.begin(), f.loop.end());
    if (smooth)
        for (uint32_t x = 0; x < id; ++x)
            for (uint32_t y = x + 1; y < id; ++y) b.smoothEdges.push_back({x, y});  // pairs that do not meet are ignored
    return b;
}
}  // namespace

TEST_CASE("smoothing: a smoothed sphere unfolds into few large charts (best-first growth), every face in one, none overlapping") {
    const Document flat = docWith(uvSphere(12, 6, false)), round = docWith(uvSphere(12, 6, true));
    LightmapSettings cfg;
    cfg.texelsPerMeter = 16;
    auto charts = [&](const Document& d) {
        const LightmapLayout l = buildLightmapLayout(collectSurfaces(d, {}), LightingClasses::defaults(), cfg);
        std::set<int> c;
        for (const auto& f : l.faces) { CHECK(f.chart >= 0); c.insert(f.chart); }
        return std::make_pair(c.size(), l.faces.size());
    };
    const auto [flatCharts, flatFaces] = charts(flat);
    const auto [roundCharts, roundFaces] = charts(round);
    MESSAGE("sphere 12 x 6: ", flatCharts, " charts faceted, ", roundCharts, " smooth (", roundFaces, " faces)");
    CHECK(flatFaces == 72);
    CHECK(flatCharts > 50);        // 30 degrees between neighbours: (nearly) every face alone
    CHECK(roundCharts <= 8);       // smooth: a handful of caps / strips
}

TEST_CASE("bsp export: brush entities carry origin (their pivot) and angles; every keyvalue goes out") {
    Document doc = docWith(octagon(false));
    Entity door;
    door.id = "door";
    door.classname = "func_door";
    door.brushes = {Brush::makeBox("d", {3, 0, 0}, {3.2, 2, 1}, "dev/door")};
    door.origin = {3.1, 0, 0};
    door.angles = {0, 90, 0};
    door.keyvalues = {{"time", "2"}, {"targetname", "d1"}, {"origin", "9 9 9"}};
    doc.entities.push_back(door);
    BspInput in;
    const DocumentLoader loader = [](const std::string& f) -> const Document& { throw std::runtime_error("no prefab " + f); };
    in.entities = flattenForCompile(doc, loader).entities;
    in.surfaces = collectSurfaces(doc, {});
    in.classes = LightingClasses::defaults();
    const BspOutput out = compileBsp(in);
    BspView v;
    std::string err;
    REQUIRE(v.open(out.bsp, &err));
    const auto raw = v.lump<char>(rbsp::kEntities);
    const std::string ents(raw.begin(), raw.end());
    const size_t at = ents.find("\"classname\" \"func_door\"");
    REQUIRE(at != std::string::npos);
    const std::string block = ents.substr(at, ents.find('}', at) - at);
    CHECK(block.find("\"origin\" \"3.1 0 0\"") != std::string::npos);  // the field, not the stray keyvalue
    CHECK(block.find("9 9 9") == std::string::npos);
    CHECK(block.find("\"angles\" \"0 90 0\"") != std::string::npos);
    CHECK(block.find("\"model\" \"*1\"") != std::string::npos);
    CHECK(block.find("\"time\" \"2\"") != std::string::npos);
    CHECK(block.find("\"targetname\" \"d1\"") != std::string::npos);
    // worldspawn gets no origin of its own
    const std::string world = ents.substr(0, ents.find('}'));
    CHECK(world.find("\"origin\"") == std::string::npos);
}
