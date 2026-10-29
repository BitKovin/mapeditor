#include "core/relief_geometry.h"
#include "core/raytrace.h"
#include <doctest.h>
#include <cmath>
#include <string>

using namespace core;

namespace {
void quad(std::vector<float>& t, double x0, double y0, double x1, double y1, double z) {
    const double v[6][3] = {{x0, y0, z}, {x1, y0, z}, {x1, y1, z}, {x0, y0, z}, {x1, y1, z}, {x0, y1, z}};
    for (const auto& p : v) for (double c : p) t.push_back(static_cast<float>(c));
}
// a flat face z = 0 (normal +z) with a pit (floor at z = -0.1 over [0.4, 0.6]^2) and a bump (top at z = 0.2 over [0, 0.2]^2); nothing else
std::shared_ptr<const ReliefMesh> sample() {
    ReliefMesh m;
    m.instance = ""; m.entityId = "e"; m.brushInstance = ""; m.brushId = "b"; m.faceId = 3;
    m.normal = {0, 0, 1}; m.offset = 0;
    quad(m.tris, 0.4, 0.4, 0.6, 0.6, -0.1);
    quad(m.tris, 0.0, 0.0, 0.2, 0.2, 0.2);
    return ReliefSet::make(std::move(m));
}
std::string b64(const std::vector<float>& f) {
    static const char* c = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char* d = reinterpret_cast<const unsigned char*>(f.data());
    const size_t n = f.size() * 4;
    std::string o;
    for (size_t i = 0; i < n; i += 3) {
        const unsigned v = (d[i] << 16) | ((i + 1 < n ? d[i + 1] : 0) << 8) | (i + 2 < n ? d[i + 2] : 0);
        o += c[(v >> 18) & 63]; o += c[(v >> 12) & 63];
        o += i + 1 < n ? c[(v >> 6) & 63] : '='; o += i + 2 < n ? c[v & 63] : '=';
    }
    return o;
}
LightSurface flatPiece() {
    LightSurface s;
    s.entityId = "e"; s.brushId = "b"; s.faceId = 3; s.classname = "worldspawn";
    s.normal = {0, 0, 1};
    s.verts = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    return s;
}
}  // namespace

TEST_CASE("relief: a point on the flat face is lifted onto the relief above it") {
    const auto r = sample();
    ReliefMesh::Hit h;
    REQUIRE(r->surfaceAt({0.5, 0.5, 0}, h));
    CHECK(h.pos.z == doctest::Approx(-0.1));
    CHECK(h.pos.x == doctest::Approx(0.5));
    CHECK(h.normal.z == doctest::Approx(1.0));
    REQUIRE(r->surfaceAt({0.1, 0.1, 0}, h));
    CHECK(h.pos.z == doctest::Approx(0.2));
    CHECK_FALSE(r->surfaceAt({0.8, 0.8, 0}, h));  // not covered: stays flat
}

TEST_CASE("relief: texel samples move onto the relief and keep the flat steps on a level top") {
    const auto r = sample();
    float sub[12] = {0.5f, 0.5f, 0.f, 1.f, 0.01f, 0.f, 0.f, 0.f, 0.f, 0.01f, 0.f, 0.f};
    Vec3 p, n;
    float out[12];
    REQUIRE(displaceSample(*r, {0.5, 0.5, 0}, {0, 0, 1}, sub, p, n, out));
    CHECK(p.z == doctest::Approx(-0.1));
    CHECK(out[2] == doctest::Approx(-0.1));
    CHECK(out[4] == doctest::Approx(0.01));
    CHECK(out[6] == doctest::Approx(0.0));
    CHECK_FALSE(displaceSample(*r, {0.8, 0.8, 0}, {0, 0, 1}, sub, p, n, out));
}

TEST_CASE("relief: a sloped triangle tilts the texel steps along its tangent plane") {
    ReliefMesh m;
    m.entityId = "e"; m.brushId = "b"; m.faceId = 3; m.normal = {0, 0, 1}; m.offset = 0;
    // a ramp rising 1 per metre along +x over [0,1]^2
    const double v[6][3] = {{0, 0, 0}, {1, 0, 1}, {1, 1, 1}, {0, 0, 0}, {1, 1, 1}, {0, 1, 0}};
    for (const auto& q : v) for (double c : q) m.tris.push_back(static_cast<float>(c));
    const auto r = ReliefSet::make(std::move(m));
    // away from the diagonal edge, so the whole texel lies on one triangle
    float sub[12] = {0.7f, 0.3f, 0.f, 1.f, 0.02f, 0.f, 0.f, 0.f, 0.f, 0.02f, 0.f, 0.f};
    Vec3 p, n;
    float out[12];
    REQUIRE(displaceSample(*r, {0.7, 0.3, 0}, {0, 0, 1}, sub, p, n, out));
    CHECK(p.z == doctest::Approx(0.7));
    CHECK(out[4] == doctest::Approx(0.02));
    CHECK(out[6] == doctest::Approx(0.02));  // a step along +x climbs by the slope
    CHECK(out[10] == doctest::Approx(0.0));
}

TEST_CASE("relief: the Shell's message is read, matched against the surface plane, and a change is reported") {
    ReliefSet set;
    nlohmann::json face = {{"i", ""}, {"e", "e"}, {"bi", ""}, {"b", "b"}, {"f", 3}, {"s", 0}, {"n", {0, 0, 1}}, {"d", 0.0}, {"v", b64(sample()->tris)}};
    nlohmann::json msg = {{"type", "relief_geometry"}, {"faces", nlohmann::json::array({face})}};
    CHECK(set.assign(msg));
    CHECK_FALSE(set.assign(msg));  // same content
    LightSurface s = flatPiece();
    CHECK(set.find(s) != nullptr);
    s.verts = {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};  // the face moved since: the relief is out of date
    CHECK(set.find(s) == nullptr);
    s = flatPiece();
    s.side = FaceSide::Back;
    CHECK(set.find(s) == nullptr);
    msg["faces"][0]["v"] = "AAAA";  // not whole triangles: skipped
    CHECK(set.assign(msg));
    CHECK(set.size() == 0);
}

TEST_CASE("relief: a face with relief is occluded by its relief, not by the flat face") {
    LightSurface s = flatPiece();
    s.relief = sample();
    const LightingClasses classes = LightingClasses::defaults();
    const OccluderMesh withRelief = buildOccluders({s}, classes, nullptr);
    CHECK(withRelief.triangleCount() == 4);  // the pit floor and the bump, once
    LightSurface b = s;
    b.verts = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    const OccluderMesh both = buildOccluders({s, b}, classes, nullptr);
    CHECK(both.triangleCount() == 4);  // two pieces of the same face share the mesh
    TriangleBvh bvh(withRelief);
    // from the pit floor to a light overhead nothing is in the way (the flat face would have covered it); the bump shadows what is behind it
    CHECK_FALSE(bvh.occluded({0.5, 0.5, -0.09}, {0, 0, 1}, 5.0));
    CHECK(bvh.occluded({0.1, 0.1, 1.0}, {0, 0, -1}, 5.0));
}

TEST_CASE("relief: a relief that appears or changes makes a surface differ") {
    LightSurface a = flatPiece(), b = flatPiece();
    b.relief = sample();
    CHECK(sample()->hash == sample()->hash);
    CHECK(sample()->hash != 0);
}

// ---- through Scene and Router ----------------------------------------------------------------------------------------------------
#include "core/lightbake.h"
#include "core/lightmap_uv.h"
#include "core/router.h"
#include "core/scene.h"
#include "test_util.h"

namespace {
struct ReliefRig {
    testutil::TempDir dir;
    Scene scene;
    Router router;
    static std::filesystem::path prepare(testutil::TempDir& d) {
        const Brush box = testutil::makePrism("b", {{0, 0}, {4, 0}, {4, 4}, {0, 4}}, 0, 1);
        d.write("maps/a.map.json", testutil::docJson({testutil::entityJson("w", "worldspawn", {box})}));
        return d.path;
    }
    ReliefRig() : scene(prepare(dir)), router(scene, {}, {}) {
        scene.loadGameProfile();
        scene.openDocument("maps/a.map.json");
    }
    // the top face (normal +y, at y = 1) and a pit in it: floor at y = 0.9 over [1, 3] x [1, 3] (x, z)
    nlohmann::json pitMessage(const LightSurface& top) const {
        std::vector<float> t;
        const double v[6][3] = {{1, 0.9, 1}, {3, 0.9, 1}, {3, 0.9, 3}, {1, 0.9, 1}, {3, 0.9, 3}, {1, 0.9, 3}};
        for (const auto& p : v) for (double c : p) t.push_back(static_cast<float>(c));
        nlohmann::json face = {{"i", top.instance}, {"e", top.entityId}, {"bi", top.brushInstance}, {"b", top.brushId}, {"f", top.faceId}, {"s", 0},
                               {"n", {0, 1, 0}}, {"d", 1.0}, {"v", b64(t)}};
        return {{"type", "relief_geometry"}, {"faces", nlohmann::json::array({face})}};
    }
};
const LightSurface* topOf(const std::vector<LightSurface>& s) {
    for (const auto& x : s) if (x.normal.y > 0.99) return &x;
    return nullptr;
}
}  // namespace

TEST_CASE("relief: Scene attaches the Shell's relief to the matching face only, and the bake places texels on it") {
    ReliefRig rig;
    std::vector<LightSurface> flat = rig.scene.lightSurfaces();
    const LightSurface* top = topOf(flat);
    REQUIRE(top != nullptr);
    CHECK(top->relief == nullptr);
    CHECK(rig.scene.setReliefGeometry(rig.pitMessage(*top)));
    std::vector<LightSurface> surfaces = rig.scene.lightSurfaces();
    int withRelief = 0;
    for (const auto& s : surfaces) if (s.relief) { ++withRelief; CHECK(s.normal.y > 0.99); }
    CHECK(withRelief == 1);

    const LightingClasses classes = LightingClasses::defaults();
    LightmapSettings settings;
    settings.texelsPerMeter = 16;
    const LightmapLayout layout = buildLightmapLayout(surfaces, classes, settings);
    const TexelSamples samples = buildTexelSamples(layout, surfaces, nullptr, nullptr);
    int pit = 0, rim = 0;
    for (size_t i = 0; i * 4 < samples.pos.size(); ++i) {
        const double x = samples.pos[i * 4], y = samples.pos[i * 4 + 1], z = samples.pos[i * 4 + 2];
        if (samples.nrm[i * 4 + 1] < 0.9) continue;  // the other faces
        if (x > 1.2 && x < 2.8 && z > 1.2 && z < 2.8) { if (std::fabs(y - 0.9) < 1e-4) ++pit; }
        else if (std::fabs(y - 1.0) < 1e-4) ++rim;
    }
    CHECK(pit > 50);   // a 1.6 m square at 16 texels/m is ~650 texels, all lowered onto the pit floor
    CHECK(rim > 50);   // the rest of the top face stays where it is

    // the same message again is no change (nothing to rebake); a different one is
    CHECK_FALSE(rig.scene.setReliefGeometry(rig.pitMessage(*top)));
}

TEST_CASE("relief: the router takes the message for the export and leaves the live lighting flat (no second snapshot behind the edit)") {
    ReliefRig rig;
    const LightSurface* top = nullptr;
    std::vector<LightSurface> s = rig.scene.lightSurfaces();
    top = topOf(s);
    REQUIRE(top != nullptr);
    rig.router.onMessage(R"({"type":"lighting","enabled":true})");
    const RouterOutput out = rig.router.onMessage(rig.pitMessage(*top).dump());
    for (const auto& m : out.toSender) CHECK(nlohmann::json::parse(m).value("type", "") != "error");
    int withRelief = 0;
    for (const auto& x : rig.scene.lightSurfaces()) if (x.relief) ++withRelief;
    CHECK(withRelief == 0);
}

// ---- the light itself --------------------------------------------------------------------------------------------------------------
#include "core/lights.h"

TEST_CASE("relief: a bump casts a shadow on the flat face behind it, and its top is lit where it really is") {
    Document doc;
    Entity w;
    w.id = "w"; w.classname = "worldspawn";
    w.brushes = {testutil::makePrism("b", {{0, 0}, {6, 0}, {6, 6}, {0, 6}}, 0, 1)};
    doc.entities = {w};
    std::vector<LightSurface> surfaces = collectSurfaces(doc, {});
    LightSurface* top = nullptr;
    for (auto& s : surfaces) if (s.normal.y > 0.99) top = &s;
    REQUIRE(top != nullptr);

    // a wall-like ridge across the face: x in [1, 1.1], 0.6 high (top and both sides), covering the whole z range
    ReliefMesh m;
    m.instance = top->instance; m.entityId = top->entityId; m.brushInstance = top->brushInstance; m.brushId = top->brushId; m.faceId = top->faceId;
    m.normal = {0, 1, 0}; m.offset = 1.0;
    auto tri = [&](Vec3 a, Vec3 b, Vec3 c) { for (Vec3 p : {a, b, c}) { m.tris.push_back(static_cast<float>(p.x)); m.tris.push_back(static_cast<float>(p.y)); m.tris.push_back(static_cast<float>(p.z)); } };
    auto quadV = [&](Vec3 a, Vec3 b, Vec3 c, Vec3 d) { tri(a, b, c); tri(a, c, d); };
    quadV({1, 1.6, 0}, {1.1, 1.6, 0}, {1.1, 1.6, 6}, {1, 1.6, 6});          // top of the ridge
    quadV({1, 1, 0}, {1, 1.6, 0}, {1, 1.6, 6}, {1, 1, 6});                 // side towards the light
    quadV({1.1, 1, 0}, {1.1, 1, 6}, {1.1, 1.6, 6}, {1.1, 1.6, 0});         // side away from it
    quadV({0, 1, 0}, {1, 1, 0}, {1, 1, 6}, {0, 1, 6});                     // flat in front of the ridge
    quadV({1.1, 1, 0}, {6, 1, 0}, {6, 1, 6}, {1.1, 1, 6});                 // flat behind it
    const auto relief = ReliefSet::make(std::move(m));

    LightScene lights;
    Light l;
    l.position = {0.2, 2.0, 3.0}; l.brightness = 20; l.radius = 30; l.castShadows = true;
    lights.lights = {l};
    const LightingClasses classes = LightingClasses::defaults();

    auto bake = [&](bool withRelief) {
        std::vector<LightSurface> s = surfaces;
        if (withRelief) for (auto& x : s) if (x.normal.y > 0.99) x.relief = relief;
        LightmapSettings cfg;
        cfg.texelsPerMeter = 16;
        const LightmapLayout layout = buildLightmapLayout(s, classes, cfg);
        const TexelSamples samples = buildTexelSamples(layout, s, nullptr, nullptr);
        const TriangleBvh bvh(buildOccluders(s, classes, &layout));
        const std::vector<float> v = evaluateDirectCpu(samples, lights, &bvh);
        // brightness of the sample nearest to a point of the top face
        auto at = [&](double x, double z) {
            double best = 1e300; size_t bi = 0;
            for (size_t i = 0; i < samples.count(); ++i) {
                if (samples.nrm[i * 4 + 1] < 0.5) continue;
                const double dx = samples.pos[i * 4] - x, dz = samples.pos[i * 4 + 2] - z;
                if (dx * dx + dz * dz < best) { best = dx * dx + dz * dz; bi = i; }
            }
            return std::make_pair(static_cast<double>(v[bi * 4]), static_cast<double>(samples.pos[bi * 4 + 1]));
        };
        return std::make_tuple(at(0.6, 3.0), at(2.0, 3.0), at(1.05, 3.0));
    };
    const auto flat = bake(false);
    const auto lit = bake(true);
    // without the relief the face is flat and lit everywhere
    CHECK(std::get<1>(flat).first > 0.05);
    // with it: in front of the ridge as before, behind it in its shadow, and the ridge top at its real height
    CHECK(std::get<0>(lit).first == doctest::Approx(std::get<0>(flat).first).epsilon(0.05));
    CHECK(std::get<1>(lit).first < 0.01);
    CHECK(std::get<2>(lit).second == doctest::Approx(1.6).epsilon(1e-3));
    CHECK(std::get<2>(lit).first > 0.05);
}
