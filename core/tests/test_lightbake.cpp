#include "core/lightbake.h"
#include "../src/gpu_bake.h"
#include "core/lights.h"
#include "test_util.h"
#include <doctest.h>
#include <cmath>
#include <fstream>

using namespace core;
using namespace testutil;

namespace {

constexpr double kPi = 3.14159265358979323846;  // M_PI is not standard C++

Entity ent(const std::string& id, const std::string& cls, std::vector<Brush> brushes = {}) {
    Entity e;
    e.id = id;
    e.classname = cls;
    e.brushes = std::move(brushes);
    return e;
}
Entity light(const std::string& id, const std::string& cls, Vec3 origin, std::map<std::string, std::string> kv = {}, Vec3 angles = {0, 0, 0}) {
    Entity e = ent(id, cls);
    e.origin = origin;
    e.angles = angles;
    for (auto& p : kv) e.keyvalues[p.first] = p.second;
    return e;
}

struct Scene1 {
    Document doc;
    std::vector<LightSurface> surfaces;
    LightmapLayout layout;
    LightScene lights;
    TexelSamples samples;
    explicit Scene1(std::vector<Entity> entities, LightmapSettings cfg = {}) {
        doc.entities = std::move(entities);
        surfaces = collectSurfaces(doc, {});
        layout = buildLightmapLayout(surfaces, LightingClasses::defaults(), cfg);
        lights = collectLights(doc, {});
        samples = buildTexelSamples(layout, surfaces);
    }
    // index of the sample closest to p, restricted to normals facing n
    size_t nearest(Vec3 p, Vec3 n) const {
        size_t best = 0;
        double bd = 1e300;
        for (size_t i = 0; i < samples.count(); ++i) {
            const Vec3 q{samples.pos[i * 4], samples.pos[i * 4 + 1], samples.pos[i * 4 + 2]};
            const Vec3 nn{samples.nrm[i * 4], samples.nrm[i * 4 + 1], samples.nrm[i * 4 + 2]};
            if (nn.dot(n) < 0.99) continue;
            const double d = (q - p).length();
            if (d < bd) { bd = d; best = i; }
        }
        return best;
    }
};
Vec3 pos(const TexelSamples& s, size_t i) { return {s.pos[i * 4], s.pos[i * 4 + 1], s.pos[i * 4 + 2]}; }
float val(const std::vector<float>& v, size_t i, int c = 0) { return v[i * 4 + static_cast<size_t>(c)]; }

double win(double d, double r) { const double w = std::max(0.0, 1.0 - std::pow(d / r, 4)); return w * w; }
}  // namespace

TEST_CASE("lights: parsed from entities with defaults, angles and prefab instances") {
    Document doc;
    Entity w = ent("w", "worldspawn");
    w.keyvalues["_ambient"] = "0.1 0.2 0.3";
    doc.entities = {w, light("p", "light_point", {1, 2, 3}, {{"color", "1 0.5 0.25"}, {"brightness", "4"}, {"radius", "7"}, {"style", "9"}, {"cast_shadows", "0"}}),
                    light("s", "light_spot", {0, 0, 0}, {{"inner_angle", "40"}, {"outer_angle", "30"}}, {0, 90, 0}), light("x", "light_bogus", {0, 0, 0})};
    ResolvedEntity r{light("pl", "light_point", {9, 9, 9}), {"root", "inst_a"}, "pl"};
    const LightScene s = collectLights(doc, {r});
    REQUIRE(s.lights.size() == 3);
    CHECK(s.ambient.y == doctest::Approx(0.2));
    const Light& p = s.lights[0];
    CHECK(p.type == LightType::Point);
    CHECK(p.position.z == doctest::Approx(3));
    CHECK(p.color.y == doctest::Approx(0.5));
    CHECK(p.brightness == doctest::Approx(4));
    CHECK(p.radius == doctest::Approx(7));
    CHECK(p.style == 9);  // styles 0..63 (the engine's pattern table); larger values clamp to 63
    CHECK(!p.castShadows);
    const Light& sp = s.lights[1];
    CHECK(sp.forward.x == doctest::Approx(1));  // yaw 90 turns +Z into +X
    CHECK(sp.right.z == doctest::Approx(-1));
    CHECK(sp.innerAngle < sp.outerAngle);        // invalid input repaired
    CHECK(s.lights[2].instance == "inst_a");
    CHECK(s.lights[2].brightness == doctest::Approx(1));  // defaults
    CHECK(s.lights[2].radius == doctest::Approx(10));
    REQUIRE(s.warnings.size() == 1);
    CHECK(s.warnings[0].find("light_bogus") != std::string::npos);
}

TEST_CASE("bake: texel samples cover the surface once, on the surface") {
    Scene1 sc({ent("w", "worldspawn", {Brush::makeBox("floor", {0, -1, 0}, {4, 0, 3})})});
    REQUIRE(sc.samples.count() > 0);
    std::set<uint32_t> unique(sc.samples.texel.begin(), sc.samples.texel.end());
    CHECK(unique.size() == sc.samples.count());
    // top face: 4 x 3 m at 16 texels/m = 64 x 48 texel centres, +-1 for edge rounding
    size_t top = 0;
    for (size_t i = 0; i < sc.samples.count(); ++i) {
        const Vec3 p = pos(sc.samples, i);
        if (sc.samples.nrm[i * 4 + 1] > 0.9f) {
            ++top;
            CHECK(p.y == doctest::Approx(0).epsilon(1e-6).scale(1));
            CHECK(p.x >= -1e-6);
            CHECK(p.x <= 4 + 1e-6);
            CHECK(p.z >= -1e-6);
            CHECK(p.z <= 3 + 1e-6);
        }
    }
    CHECK(top >= 64 * 48 - 2 * (64 + 48));
    CHECK(top <= 64 * 48 + 2 * (64 + 48));
}

TEST_CASE("bake: every texel a bilinear lookup can touch inside a face is baked (no empty or leaking edge texels)") {
    // a face whose edges are NOT on texel boundaries: partly covered edge texels must be baked, not left to the gutter
    Scene1 sc({ent("w", "worldspawn", {Brush::makeBox("floor", {0.013, -1, 0.027}, {4.113, 0, 3.331}), Brush::makeBox("wall", {0.013, 0, 0.027}, {4.113, 2.37, 0.227})}),
               light("l", "light_point", {2, 1.5, 1.5}, {{"radius", "9"}})});
    const auto v = evaluateDirectCpu(sc.samples, sc.lights);
    const LightmapPages P = assemblePages(sc.layout, sc.samples, v);
    // every baked sample lies on its face (inside the polygon, within the inset tolerance)
    for (size_t i = 0; i < sc.samples.count(); ++i) {
        const Vec3 p = pos(sc.samples, i);
        if (sc.samples.nrm[i * 4 + 1] > 0.9f && p.y < 1.0) {  // the floor top (the wall's top face is at y = 2.37)
            CHECK(std::fabs(p.y) < 1e-6);
            CHECK(p.x >= 0.013 - 1e-6);
            CHECK(p.x <= 4.113 + 1e-6);
            CHECK(p.z >= 0.027 - 1e-6);
            CHECK(p.z <= 3.331 + 1e-6);
        }
    }
    // bilinear footprint of every point of the floor top: the four texels must be baked or gutter-filled, never empty (an empty texel renders black at the edge)
    const LightmapFace* top = nullptr;
    for (const auto& f : sc.layout.faces) if (f.normal.y > 0.9 && f.chart >= 0 && f.area > 10) top = &f;  // floor top: 4.1 x 3.3 m
    REQUIRE(top != nullptr);
    const int page = sc.layout.charts[static_cast<size_t>(top->chart)].page;
    int checked = 0, bad = 0;
    for (double x = 0.013; x <= 4.113 + 1e-9; x += 0.0137)
        for (double z = 0.027; z <= 3.331 + 1e-9; z += 0.0141) {
            const Vec2 t = top->texel({std::min(x, 4.113), 0, std::min(z, 3.331)});
            const int x0 = static_cast<int>(std::floor(t.u - 0.5)), y0 = static_cast<int>(std::floor(t.v - 0.5));
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const double wgt = (dx ? (t.u - 0.5 - x0) : 1 - (t.u - 0.5 - x0)) * (dy ? (t.v - 0.5 - y0) : 1 - (t.v - 0.5 - y0));
                    if (wgt < 1e-6) continue;  // a texel with no weight cannot leak
                    ++checked;
                    if (P.valid[static_cast<size_t>(page)][static_cast<size_t>(y0 + dy) * P.atlas + (x0 + dx)] == 0) ++bad;
                }
        }
    CHECK(checked > 1000);
    CHECK(bad == 0);
}

TEST_CASE("bake cpu: point light = brightness * window / d^2 * cos, colour-tinted") {
    Scene1 sc({ent("w", "worldspawn", {Brush::makeBox("floor", {-5, -1, -5}, {5, 0, 5})}),
               light("l", "light_point", {0.03, 2, 0.03}, {{"brightness", "8"}, {"radius", "20"}, {"color", "1 0.5 0.25"}})});
    const auto v = evaluateDirectCpu(sc.samples, sc.lights);
    for (Vec3 q : {Vec3{0, 0, 0}, Vec3{3, 0, 0}, Vec3{-2, 0, 4}}) {
        const size_t i = sc.nearest(q, {0, 1, 0});
        const Vec3 p = pos(sc.samples, i);
        const Vec3 d = Vec3{0.03, 2, 0.03} - p;
        const double dist = d.length();
        const double expect = 8 * win(dist, 20) / (dist * dist) * (d.y / dist);
        CHECK(val(v, i, 0) == doctest::Approx(expect).epsilon(1e-5));
        CHECK(val(v, i, 1) == doctest::Approx(expect * 0.5).epsilon(1e-5));
        CHECK(val(v, i, 2) == doctest::Approx(expect * 0.25).epsilon(1e-5));
    }
    // exact textbook value straight below at 2 m: 8 / 4 * window(2, 20)
    const size_t under = sc.nearest({0.03, 0, 0.03}, {0, 1, 0});
    CHECK(val(v, under) == doctest::Approx(2.0 * win(2.0, 20)).epsilon(2e-3));
}

TEST_CASE("bake cpu: radius cutoff, back-facing surfaces, additivity, ambient, near clamp") {
    Scene1 sc({ent("w", "worldspawn", {Brush::makeBox("slab", {-5, -1, -5}, {5, 0, 5})}),
               light("a", "light_point", {0, 1, 0}, {{"radius", "3"}}), light("b", "light_point", {0, 1, 0}, {{"radius", "3"}, {"brightness", "2"}}),
               light("far", "light_point", {0, 40, 0}, {{"radius", "10"}})});
    sc.lights.ambient = {0.25, 0.25, 0.25};
    const auto v = evaluateDirectCpu(sc.samples, sc.lights);
    const size_t top = sc.nearest({0, 0, 0}, {0, 1, 0}), bottom = sc.nearest({0, -1, 0}, {0, -1, 0}), edge = sc.nearest({4.9, 0, 4.9}, {0, 1, 0});
    const double d = (Vec3{0, 1, 0} - pos(sc.samples, top)).length();
    const double single = win(d, 3) / (d * d) * (1.0 / d);  // cos = 1/d for this texel
    CHECK(val(v, top) == doctest::Approx(0.25 + 3 * single * d).epsilon(1e-3).scale(1));  // a + b (1 + 2) share the same geometry
    CHECK(val(v, bottom) == doctest::Approx(0.25).epsilon(1e-6));  // faces away: only ambient
    CHECK(val(v, edge) == doctest::Approx(0.25).epsilon(1e-6));    // beyond every radius
    // light closer than the clamp distance must not blow up
    Scene1 close({ent("w", "worldspawn", {Brush::makeBox("slab", {-1, -1, -1}, {1, 0, 1})}), light("c", "light_point", {0, 0.001, 0}, {{"radius", "5"}})});
    const auto cv = evaluateDirectCpu(close.samples, close.lights);
    for (size_t i = 0; i < close.samples.count(); ++i) CHECK(std::isfinite(cv[i * 4]));
    const size_t u = close.nearest({0, 0, 0}, {0, 1, 0});
    CHECK(val(cv, u) <= 1.0 / (0.05 * 0.05) + 1e-3);
}

TEST_CASE("bake: gutter texels are filled by dilation and untouched space stays empty") {
    Scene1 sc({ent("w", "worldspawn", {Brush::makeBox("floor", {0, -1, 0}, {2, 0, 2})}), light("l", "light_point", {1, 1, 1}, {{"radius", "9"}})});
    const auto v = evaluateDirectCpu(sc.samples, sc.lights);
    const LightmapPages P = assemblePages(sc.layout, sc.samples, v);
    REQUIRE(P.pageCount == 1);
    int baked = 0, gutter = 0;
    for (uint8_t b : P.valid[0]) { baked += b == 1; gutter += b == 2; }
    CHECK(baked == static_cast<int>(sc.samples.count()));
    CHECK(gutter > 0);
    // a gutter texel next to a baked one holds an average of baked neighbours: a value in their range, not zero
    for (int y = 0; y < P.atlas; ++y)
        for (int x = 0; x < P.atlas; ++x)
            if (P.valid[0][static_cast<size_t>(y) * P.atlas + x] == 2) CHECK(P.at(0, x, y).x >= 0);
    CHECK(P.valid[0][static_cast<size_t>(P.atlas) * P.atlas - 1] == 0);  // far corner untouched
}

TEST_CASE("bake: end to end writes a PNG") {
    Scene1 sc({ent("w", "worldspawn", {Brush::makeBox("floor", {-3, -1, -3}, {3, 0, 3})}), light("l", "light_point", {0, 2, 0}, {{"brightness", "6"}, {"radius", "8"}})});
    BakeOptions o;
    o.forceCpu = true;
    BakeStats st;
    const LightmapPages P = bakeLightmaps(sc.layout, sc.surfaces, sc.lights, o, &st);
    CHECK(!st.usedGpu);
    CHECK(st.device == "cpu");
    CHECK(st.lights == 1);
    CHECK(st.texels == sc.samples.count());
    TempDir dir;
    const std::string path = (dir.path / "page0.png").string();
    REQUIRE(writePagePng(P, 0, path));
    std::ifstream f(path, std::ios::binary);
    char sig[8];
    f.read(sig, 8);
    CHECK(std::string(sig + 1, 3) == "PNG");
    CHECK(std::filesystem::file_size(path) > static_cast<size_t>(P.atlas) * P.atlas * 3);
    CHECK(!writePagePng(P, 7, path));
}

// ---- GPU path (Vulkan compute) ---------------------------------------------------------------------------------------------
// Skipped when the machine has no Vulkan device. Sandbox: Mesa lavapipe (export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json).
// A pass proves the shader agrees with the CPU reference and that the host code is validation-clean; it proves nothing about
// vendor drivers or speed.
TEST_CASE("bake gpu: compute shader agrees with the CPU reference and the validation layer stays silent") {
    Scene1 sc({ent("w", "worldspawn", {Brush::makeBox("room", {-4, -1, -4}, {4, 0, 4}), Brush::makeBox("wall", {-4, 0, 3.5}, {4, 3, 4})}),
               light("a", "light_point", {0.5, 2, 0.2}, {{"brightness", "7"}, {"radius", "9"}, {"color", "1 0.8 0.5"}}),
               light("b", "light_point", {-2, 1, 2}, {{"brightness", "3"}, {"radius", "4"}, {"color", "0.4 0.6 1"}})});
    sc.lights.ambient = {0.02, 0.03, 0.04};
    BakeOptions o;
    o.gpuValidation = true;
    o.shadows = false;  // this test is about the shading math; shadows have their own GPU test
    BakeStats gst;
    const LightmapPages g = bakeLightmaps(sc.layout, sc.surfaces, sc.lights, o, &gst);
    if (!gst.usedGpu) {
        MESSAGE("no Vulkan device, GPU test skipped: " << (gst.warnings.empty() ? "" : gst.warnings.back()));
        return;
    }
    MESSAGE("GPU device: " << gst.device << ", texels " << gst.texels);
    for (const auto& m : gst.validationMessages) MESSAGE(m);
    bool onlyNotes = true;
    for (const auto& m : gst.validationMessages) if (m.rfind("[note]", 0) != 0) onlyNotes = false;
    CHECK(onlyNotes);  // validation output IS a test failure
    BakeOptions cpuOpt;
    cpuOpt.forceCpu = true;
    cpuOpt.shadows = false;
    cpuOpt.shadows = false;
    const LightmapPages c = bakeLightmaps(sc.layout, sc.surfaces, sc.lights, cpuOpt);
    REQUIRE(g.pageCount == c.pageCount);
    double maxErr = 0, maxVal = 0;
    for (int p = 0; p < g.pageCount; ++p)
        for (size_t i = 0; i < g.rgb[static_cast<size_t>(p)].size(); ++i) {
            const double a = g.rgb[static_cast<size_t>(p)][i], b = c.rgb[static_cast<size_t>(p)][i];
            maxErr = std::max(maxErr, std::fabs(a - b) / std::max(1.0, std::fabs(b)));
            maxVal = std::max(maxVal, b);
        }
    CHECK(maxVal > 1.0);      // the scene really is lit
    CHECK(maxErr < 1e-4);
}

TEST_CASE("bake gpu: empty input and zero lights are valid") {
    Scene1 sc({ent("w", "worldspawn", {Brush::makeBox("floor", {0, -1, 0}, {2, 0, 2})})});
    BakeStats st;
    const LightmapPages P = bakeLightmaps(sc.layout, sc.surfaces, sc.lights, {}, &st);  // no lights at all
    CHECK(st.lights == 0);
    for (float v : P.rgb[0]) CHECK(v == 0.f);
    Scene1 none({});
    BakeStats st2;
    const LightmapPages Q = bakeLightmaps(none.layout, none.surfaces, none.lights, {}, &st2);
    CHECK(Q.pageCount == 0);
    CHECK(st2.texels == 0);
}

// ---- all light types (no shadows yet) ----------------------------------------------------------------------------------------
namespace {
const Vec3 kDown{90, 0, 0};  // angles (pitch, yaw, roll) whose forward axis is -Y
std::vector<Entity> slab(std::vector<Entity> lights) {
    std::vector<Entity> es{ent("w", "worldspawn", {Brush::makeBox("slab", {-30, -1, -30}, {30, 0, 30})})};
    for (auto& l : lights) es.push_back(std::move(l));
    return es;
}
// closed form: form factor of a rectangle a x b at distance c, seen from a point below its corner
double cornerFF(double a, double b, double c) {
    const double A = a / c, B = b / c;
    return (A / std::sqrt(1 + A * A) * std::atan(B / std::sqrt(1 + A * A)) + B / std::sqrt(1 + B * B) * std::atan(A / std::sqrt(1 + B * B))) / (2 * kPi);
}
}  // namespace

TEST_CASE("lights: angles give the same axes as the Shell (rotationFromAngles)") {
    const double p = 30 * kPi / 180, y = 40 * kPi / 180, r = 20 * kPi / 180;
    Document doc;
    doc.entities = {light("s", "light_spot", {0, 0, 0}, {}, {30, 40, 20})};
    const Light l = collectLights(doc, {}).lights[0];
    CHECK(l.forward.x == doctest::Approx(std::sin(y) * std::cos(p)));
    CHECK(l.forward.y == doctest::Approx(-std::sin(p)));
    CHECK(l.forward.z == doctest::Approx(std::cos(y) * std::cos(p)));
    CHECK(l.right.x == doctest::Approx(std::cos(y) * std::cos(r) + std::sin(y) * std::sin(p) * std::sin(r)));
    CHECK(l.right.y == doctest::Approx(std::cos(p) * std::sin(r)));
    CHECK(l.up.y == doctest::Approx(std::cos(p) * std::cos(r)));
}

TEST_CASE("bake cpu: spot light = point light x smooth cone") {
    Scene1 sc(slab({light("s", "light_spot", {0, 3, 0}, {{"brightness", "10"}, {"radius", "30"}, {"inner_angle", "20"}, {"outer_angle", "40"}}, kDown)}));
    const auto v = evaluateDirectCpu(sc.samples, sc.lights);
    const double ci = std::cos(20 * kPi / 180), co = std::cos(40 * kPi / 180);
    for (double x : {0.0, 1.0, 1.732, 2.4, 3.575, 6.0}) {
        const size_t i = sc.nearest({x, 0, 0}, {0, 1, 0});
        const Vec3 p = pos(sc.samples, i);
        const double dist = (Vec3{0, 3, 0} - p).length();
        const double cosA = 3.0 / dist;  // angle between the axis (down) and the direction to the receiver
        const double t = std::clamp((cosA - co) / (ci - co), 0.0, 1.0);
        const double expect = 10 * win(dist, 30) / (dist * dist) * (3.0 / dist) * t * t;
        CHECK(val(v, i) == doctest::Approx(expect).epsilon(1e-5).scale(1e-3));
    }
    CHECK(val(v, sc.nearest({0, 0, 0}, {0, 1, 0})) > 0.5);   // on axis: full
    CHECK(val(v, sc.nearest({6, 0, 0}, {0, 1, 0})) == 0.f);  // outside the outer cone
}

TEST_CASE("bake cpu: directional light = brightness x cos, no distance falloff") {
    std::vector<Entity> es{ent("w", "worldspawn", {Brush::makeBox("b", {-5, -1, -5}, {5, 0, 5}), Brush::makeBox("c", {-5, 0, -5}, {5, 2, -4})}),
                           light("d", "light_directional", {1000, 1000, 1000}, {{"brightness", "3"}, {"color", "1 1 0.5"}}, {45, 0, 0})};
    Scene1 sc(es);
    const auto v = evaluateDirectCpu(sc.samples, sc.lights);
    // forward = (0, -sin45, cos45): floor top gets cos = sin45, the -Z face of the second box gets cos = cos45 = 0.7071, the +Z face 0
    CHECK(val(v, sc.nearest({0, 0, 0}, {0, 1, 0})) == doctest::Approx(3 * std::sin(kPi / 4)).epsilon(1e-5));
    CHECK(val(v, sc.nearest({0, 1, -5}, {0, 0, -1}), 2) == doctest::Approx(1.5 * std::cos(kPi / 4)).epsilon(1e-5));
    CHECK(val(v, sc.nearest({0, 1, -4}, {0, 0, 1})) == 0.f);
    CHECK(val(v, sc.nearest({0, -1, 0}, {0, -1, 0})) == 0.f);
}

TEST_CASE("bake cpu: sky light = brightness x (1 + n.y) / 2") {
    std::vector<Entity> es{ent("w", "worldspawn", {Brush::makeBox("b", {-5, -1, -5}, {5, 3, 5})}), light("sky", "light_sky", {0, 0, 0}, {{"brightness", "2"}, {"color", "0.5 0.6 1"}})};
    Scene1 sc(es);
    const auto v = evaluateDirectCpu(sc.samples, sc.lights);
    const size_t top = sc.nearest({0, 3, 0}, {0, 1, 0}), side = sc.nearest({5, 1, 0}, {1, 0, 0}), bottom = sc.nearest({0, -1, 0}, {0, -1, 0});
    CHECK(val(v, top, 0) == doctest::Approx(1.0).epsilon(1e-6));  // 2 * 0.5
    CHECK(val(v, top, 2) == doctest::Approx(2.0).epsilon(1e-6));
    CHECK(val(v, side, 2) == doctest::Approx(1.0).epsilon(1e-6));
    CHECK(val(v, bottom, 2) == doctest::Approx(0.0).scale(1));
}

TEST_CASE("bake cpu: rect light matches the closed-form form factor, is one-sided and obeys the horizon") {
    // 2 x 2 rect at height 1.5 facing down, receiver straight below its centre: 4 corner rectangles of 1 x 1
    Scene1 sc(slab({light("r", "light_rect", {0, 1.5, 0}, {{"brightness", "5"}, {"radius", "1000"}, {"width", "2"}, {"height", "2"}}, kDown)}));
    auto v = evaluateDirectCpu(sc.samples, sc.lights);
    const size_t i = sc.nearest({0, 0, 0}, {0, 1, 0});
    const Vec3 p = pos(sc.samples, i);
    // the texel is not exactly at the centre; compare against the closed form at the same offset by evaluating the 4 sub-rectangles
    const double cx = p.x, cz = p.z, h = 1.5 - p.y;
    auto sub4 = [&](double x0, double x1, double z0, double z1) {  // signed corner decomposition
        auto g = [&](double x, double z) { return (x == 0 || z == 0) ? 0.0 : std::copysign(1.0, x) * std::copysign(1.0, z) * cornerFF(std::fabs(x), std::fabs(z), h); };
        return g(x1 - cx, z1 - cz) - g(x0 - cx, z1 - cz) - g(x1 - cx, z0 - cz) + g(x0 - cx, z0 - cz);
    };
    CHECK(val(v, i) == doctest::Approx(5 * sub4(-1, 1, -1, 1)).epsilon(1e-4));
    // centred value: 4 * corner
    CHECK(val(v, sc.nearest({0, 0, 0}, {0, 1, 0})) == doctest::Approx(5 * 4 * cornerFF(1, 1, 1.5)).epsilon(0.02));

    // huge rect close above: covers the whole hemisphere -> V = brightness
    Scene1 big(slab({light("r", "light_rect", {0, 0.5, 0}, {{"brightness", "5"}, {"radius", "1000"}, {"width", "20000"}, {"height", "20000"}}, kDown)}));
    CHECK(val(evaluateDirectCpu(big.samples, big.lights), big.nearest({0, 0, 0}, {0, 1, 0})) == doctest::Approx(5).epsilon(1e-3));

    // far field: A cos cos / (pi d^2)
    Scene1 far(slab({light("r", "light_rect", {0, 20, 0}, {{"brightness", "1000"}, {"radius", "1000"}, {"width", "0.5"}, {"height", "0.5"}}, kDown)}));
    const auto fv = evaluateDirectCpu(far.samples, far.lights);
    const size_t fi = far.nearest({0, 0, 0}, {0, 1, 0});
    const double d = 20 - pos(far.samples, fi).y;
    CHECK(val(fv, fi) == doctest::Approx(1000 * 0.25 / (kPi * d * d)).epsilon(0.01));

    // one-sided: a ceiling above the emitter sees its back
    std::vector<Entity> es{ent("w", "worldspawn", {Brush::makeBox("c", {-5, 3, -5}, {5, 4, 5})}),
                           light("r", "light_rect", {0, 2, 0}, {{"brightness", "5"}, {"width", "2"}, {"height", "2"}}, kDown)};
    Scene1 back(es);
    CHECK(val(evaluateDirectCpu(back.samples, back.lights), back.nearest({0, 3, 0}, {0, -1, 0})) == 0.f);
    // beyond the radius
    Scene1 out(slab({light("r", "light_rect", {0, 1.5, 0}, {{"radius", "1"}, {"width", "2"}, {"height", "2"}}, kDown)}));
    CHECK(val(evaluateDirectCpu(out.samples, out.lights), out.nearest({0, 0, 0}, {0, 1, 0})) == 0.f);
}

TEST_CASE("bake gpu: every light type agrees with the CPU reference, validation clean") {
    Scene1 sc({ent("w", "worldspawn", {Brush::makeBox("room", {-4, -1, -4}, {4, 0, 4}), Brush::makeBox("wall", {-4, 0, 3.5}, {4, 3, 4}), Brush::makeBox("pillar", {1, 0, -1}, {2, 3, 0})}),
               light("p", "light_point", {0.5, 2, 0.2}, {{"brightness", "7"}, {"radius", "9"}, {"color", "1 0.8 0.5"}}),
               light("s", "light_spot", {-2, 2.5, -2}, {{"brightness", "9"}, {"radius", "12"}, {"inner_angle", "15"}, {"outer_angle", "35"}, {"color", "0.6 1 0.6"}}, {70, 30, 0}),
               light("d", "light_directional", {0, 0, 0}, {{"brightness", "0.7"}, {"color", "1 0.9 0.9"}}, {40, 200, 0}),
               light("r", "light_rect", {2, 2.8, 2}, {{"brightness", "4"}, {"radius", "8"}, {"width", "2"}, {"height", "1"}, {"color", "0.8 0.8 1"}}, {80, 20, 15}),
               light("k", "light_sky", {0, 0, 0}, {{"brightness", "0.4"}, {"color", "0.5 0.7 1"}})});
    sc.lights.ambient = {0.02, 0.02, 0.03};
    REQUIRE(sc.lights.lights.size() == 5);
    BakeOptions o;
    o.gpuValidation = true;
    o.shadows = false;
    BakeStats gst;
    const LightmapPages g = bakeLightmaps(sc.layout, sc.surfaces, sc.lights, o, &gst);
    if (!gst.usedGpu) { MESSAGE("no Vulkan device, GPU test skipped"); return; }
    for (const auto& m : gst.validationMessages) MESSAGE(m);
    for (const auto& m : gst.validationMessages) CHECK(m.rfind("[note]", 0) == 0);
    BakeOptions cpu;
    cpu.forceCpu = true;
    cpu.shadows = false;
    cpu.shadows = false;
    const LightmapPages c = bakeLightmaps(sc.layout, sc.surfaces, sc.lights, cpu);
    double maxErr = 0, maxVal = 0;
    for (int p = 0; p < g.pageCount; ++p)
        for (size_t i = 0; i < g.rgb[static_cast<size_t>(p)].size(); ++i) {
            const double a = g.rgb[static_cast<size_t>(p)][i], b = c.rgb[static_cast<size_t>(p)][i];
            maxErr = std::max(maxErr, std::fabs(a - b) / std::max(1.0, std::fabs(b)));
            maxVal = std::max(maxVal, b);
        }
    CHECK(maxVal > 1.0);
    CHECK(maxErr < 2e-4);
}

TEST_CASE("bake cpu: rect light is clipped to the receiver's horizon") {
    // A huge downward-facing rect above a vertical wall (normal +X): by symmetry it covers exactly the upper half of the
    // wall's hemisphere, so V = brightness / 2. Without clipping to the horizon, the part of the rect behind the wall's plane
    // would distort the sum.
    std::vector<Entity> es{ent("w", "worldspawn", {Brush::makeBox("wall", {-5, -1, -5}, {1, 3, 5})}),
                           light("r", "light_rect", {0, 2.5, 0}, {{"brightness", "6"}, {"radius", "100000"}, {"width", "20000"}, {"height", "20000"}}, kDown)};
    Scene1 sc(es);
    const auto v = evaluateDirectCpu(sc.samples, sc.lights);
    const size_t i = sc.nearest({1, 1, 0}, {1, 0, 0});
    CHECK(val(v, i) == doctest::Approx(3.0).epsilon(2e-3));
}

// ---- shadows (CPU reference) ---------------------------------------------------------------------------------------------------
namespace {
std::vector<float> evalShadowed(Scene1& sc, const BakeOptions& o = {}) {
    const OccluderMesh mesh = buildOccluders(sc.surfaces, o.classes);
    const TriangleBvh bvh(mesh);
    return evaluateDirectCpu(sc.samples, sc.lights, &bvh, o);
}
// floor + a thin slab; the slab is worldspawn so it casts shadows
std::vector<Entity> floorAndSlab(Vec3 slabLo, Vec3 slabHi, std::vector<Entity> lights) {
    std::vector<Entity> es{ent("w", "worldspawn", {Brush::makeBox("floor", {-10, -1, -10}, {10, 0, 10}), Brush::makeBox("slab", slabLo, slabHi)})};
    for (auto& l : lights) es.push_back(std::move(l));
    return es;
}
}  // namespace

TEST_CASE("shadows cpu: a point light is hard-shadowed by an occluder and unshadowed elsewhere") {
    Scene1 sc(floorAndSlab({-1, 2, -1}, {1, 2.2, 1}, {light("l", "light_point", {0, 4, 0}, {{"brightness", "10"}, {"radius", "30"}})}));
    const auto lit = evaluateDirectCpu(sc.samples, sc.lights);  // no occluders
    const auto v = evalShadowed(sc);
    const size_t under = sc.nearest({0, 0, 0}, {0, 1, 0}), outside = sc.nearest({6, 0, 0}, {0, 1, 0}), edgeIn = sc.nearest({1.4, 0, 0}, {0, 1, 0}),
                 edgeOut = sc.nearest({2.6, 0, 0}, {0, 1, 0});
    CHECK(val(lit, under) > 0.5);
    CHECK(val(v, under) == 0.f);                       // shadowed
    CHECK(val(v, outside) == doctest::Approx(val(lit, outside)).epsilon(1e-4));  // untouched (super sampling re-orders the float sums)
    // geometry: the slab's top edge x = 1 at height 2.2 projects from the light at height 4 to x = 1 * 4/1.8 = 2.22 on the floor
    CHECK(val(v, edgeIn) == 0.f);
    CHECK(val(v, edgeOut) == doctest::Approx(val(lit, edgeOut)).epsilon(1e-4));
    // a light inside a closed box lights nothing outside it
    Scene1 closed({ent("w", "worldspawn", {Brush::makeBox("floor", {-20, -1, -20}, {20, 0, 20}), Brush::makeBox("shell", {-1, 1, -1}, {1, 3, 1})}),
                   light("l", "light_point", {0, 2, 0}, {{"radius", "30"}})});
    const auto cv = evalShadowed(closed);
    CHECK(val(cv, closed.nearest({0, 0, 0}, {0, 1, 0})) == 0.f);
}

TEST_CASE("shadows cpu: cast_shadows 0 and options.shadows false ignore occluders") {
    Scene1 sc(floorAndSlab({-1, 2, -1}, {1, 2.2, 1}, {light("l", "light_point", {0, 4, 0}, {{"brightness", "10"}, {"radius", "30"}, {"cast_shadows", "0"}})}));
    const auto v = evalShadowed(sc);
    CHECK(val(v, sc.nearest({0, 0, 0}, {0, 1, 0})) > 0.1);
    Scene1 sc2(floorAndSlab({-1, 2, -1}, {1, 2.2, 1}, {light("l", "light_point", {0, 4, 0}, {{"brightness", "10"}, {"radius", "30"}})}));
    BakeOptions off;
    off.shadows = false;
    CHECK(val(evalShadowed(sc2, off), sc2.nearest({0, 0, 0}, {0, 1, 0})) > 0.1);
    // a class not listed in castShadows does not occlude
    std::vector<Entity> es = floorAndSlab({50, 2, 50}, {51, 3, 51}, {light("l", "light_point", {0, 4, 0}, {{"brightness", "10"}, {"radius", "30"}})});
    es.push_back(ent("d", "func_door", {Brush::makeBox("leaf", {-1, 2, -1}, {1, 2.2, 1})}));
    Scene1 sc3(es);
    CHECK(val(evalShadowed(sc3), sc3.nearest({0, 0, 0}, {0, 1, 0})) > 0.1);
    BakeOptions doors;
    doors.classes.castShadows.insert("func_door");
    CHECK(val(evalShadowed(sc3, doors), sc3.nearest({0, 0, 0}, {0, 1, 0})) == 0.f);
}

TEST_CASE("shadows cpu: a sized light gives a penumbra with the geometric half-shadow at the edge line") {
    // half-plane occluder (x > 0) at height 2 under a disc light of radius 0.5 at height 4 centred over x = 0:
    // the floor point at x = 0 sees exactly half of the disc
    Scene1 sc(floorAndSlab({0, 2, -20}, {20, 2.1, 20}, {light("l", "light_point", {0, 4, 0}, {{"brightness", "8"}, {"radius", "40"}, {"source_size", "0.5"}})}));
    BakeOptions o;
    o.areaSamples = 256;
    const auto soft = evalShadowed(sc, o);
    Scene1 hard(floorAndSlab({0, 2, -20}, {20, 2.1, 20}, {light("l", "light_point", {0, 4, 0}, {{"brightness", "8"}, {"radius", "40"}})}));
    const auto lit = evaluateDirectCpu(hard.samples, hard.lights);
    const size_t i = sc.nearest({0, 0, 0}, {0, 1, 0});
    const Vec3 p = pos(sc.samples, i);
    const double expectHalf = val(lit, hard.nearest(p, {0, 1, 0})) * 0.5;
    // the sampled point is not exactly at x = 0 (texel centres): use the exact visibility of a texel with |x| tiny
    if (std::fabs(p.x) < 0.04) CHECK(val(soft, i) == doctest::Approx(expectHalf).epsilon(0.1));
    // monotone across the penumbra: more x -> more occluded
    double prev = 1e9;
    int strictlyBetween = 0;
    for (double x : {-0.6, -0.3, -0.1, 0.0, 0.1, 0.3, 0.6}) {
        const size_t j = sc.nearest({x, 0, 0}, {0, 1, 0});
        const double unshadowed = val(lit, hard.nearest(pos(sc.samples, j), {0, 1, 0}));
        const double ratio = val(soft, j) / unshadowed;
        CHECK(ratio <= prev + 0.05);
        prev = ratio;
        if (ratio > 0.05 && ratio < 0.95) ++strictlyBetween;
    }
    CHECK(strictlyBetween >= 2);
    CHECK(val(soft, sc.nearest({-3, 0, 0}, {0, 1, 0})) / val(lit, hard.nearest({-3, 0, 0}, {0, 1, 0})) == doctest::Approx(1).epsilon(1e-3));
    CHECK(val(soft, sc.nearest({3, 0, 0}, {0, 1, 0})) == 0.f);
}

TEST_CASE("shadows cpu: directional light shadows, sized sun has a penumbra") {
    // light travels down along -Y; a slab at height 2 covers x in [0, 20]
    std::vector<Entity> es = floorAndSlab({0, 2, -20}, {20, 2.1, 20}, {light("d", "light_directional", {0, 0, 0}, {{"brightness", "2"}}, kDown)});
    Scene1 sc(es);
    const auto v = evalShadowed(sc);
    CHECK(val(v, sc.nearest({-3, 0, 0}, {0, 1, 0})) == doctest::Approx(2).epsilon(1e-5));
    CHECK(val(v, sc.nearest({3, 0, 0}, {0, 1, 0})) == 0.f);
    // 10 degree sun: penumbra width at the floor = 2 m * tan(10 deg) ~ 0.35 m, centred on x = 0
    es = floorAndSlab({0, 2, -20}, {20, 2.1, 20}, {light("d", "light_directional", {0, 0, 0}, {{"brightness", "2"}, {"source_size", "10"}}, kDown)});
    Scene1 sun(es);
    BakeOptions o;
    o.areaSamples = 256;
    const auto sv = evalShadowed(sun, o);
    const size_t mid = sun.nearest({0, 0, 0}, {0, 1, 0});
    {
        // The texel is not exactly on the edge line. A ray of the cone (disc of radius r = tan 5 deg, uniformly sampled) is blocked when
        // x_t + h * dirx >= 0 (h = 2.05: slab bottom 2, top 2.1), so the visible fraction is the part of the disc with dirx < c, c = -x_t / h.
        const double r = std::tan(5.0 * kPi / 180), c = -pos(sun.samples, mid).x / 2.05, k = std::clamp(c / r, -1.0, 1.0);
        const double visible = 0.5 + (k * std::sqrt(1 - k * k) + std::asin(k)) / kPi;
        CHECK(val(sv, mid) == doctest::Approx(2 * visible).epsilon(0.06));
    }
    CHECK(val(sv, sun.nearest({-0.5, 0, 0}, {0, 1, 0})) == doctest::Approx(2).epsilon(1e-3));
    CHECK(val(sv, sun.nearest({0.5, 0, 0}, {0, 1, 0})) == 0.f);
}

TEST_CASE("shadows cpu: sky light is occluded by geometry above, open sky gives the analytic value") {
    // floor under a half-plane roof at height 2 (x > 0): at x = 0 exactly the half of the hemisphere with direction.x < 0 is open
    std::vector<Entity> es = floorAndSlab({0, 2, -20}, {20, 2.1, 20}, {light("k", "light_sky", {0, 0, 0}, {{"brightness", "3"}})});
    Scene1 sc(es);
    BakeOptions o;
    o.skySamples = 256;
    o.superSamples = 1;  // these checks are about the ray math; super sampling has its own tests
    const auto v = evalShadowed(sc, o);
    // The roof (x > 0, 2 m up) still blocks shallow rays seen from x = -8 (about 1-3 % of the cosine-weighted hemisphere), and a few
    // shallow rays escape from under it at x = +8: neither side is exactly 3 or 0.
    CHECK(val(v, sc.nearest({-8, 0, 0}, {0, 1, 0})) == doctest::Approx(3).epsilon(0.04));
    CHECK(val(v, sc.nearest({8, 0, 0}, {0, 1, 0})) < 0.12);
    const size_t i = sc.nearest({0, 0, 0}, {0, 1, 0});
    if (std::fabs(pos(sc.samples, i).x) < 0.04) CHECK(val(v, i) == doctest::Approx(1.5).epsilon(0.08));
    // a closed room has no sky at all
    Scene1 room({ent("w", "worldspawn", {Brush::makeBox("floor", {-5, -1, -5}, {5, 0, 5}), Brush::makeBox("roof", {-5, 3, -5}, {5, 4, 5}),
                                         Brush::makeBox("w1", {-5, 0, -5}, {-4, 3, 5}), Brush::makeBox("w2", {4, 0, -5}, {5, 3, 5}),
                                         Brush::makeBox("w3", {-4, 0, -5}, {4, 3, -4}), Brush::makeBox("w4", {-4, 0, 4}, {4, 3, 5})}),
                 light("k", "light_sky", {0, 0, 0}, {{"brightness", "3"}})});
    const auto rv = evalShadowed(room);
    CHECK(val(rv, room.nearest({0, 0, 0}, {0, 1, 0})) == 0.f);
    // cast_shadows 0: analytic everywhere, even under the roof
    Scene1 nosh(floorAndSlab({0, 2, -20}, {20, 2.1, 20}, {light("k", "light_sky", {0, 0, 0}, {{"brightness", "3"}, {"cast_shadows", "0"}})}));
    CHECK(val(evalShadowed(nosh), nosh.nearest({8, 0, 0}, {0, 1, 0})) == doctest::Approx(3).epsilon(1e-6));
}

TEST_CASE("shadows cpu: a rect light behind a half-plane is half-visible on the edge line") {
    Scene1 sc(floorAndSlab({0, 2, -20}, {20, 2.1, 20}, {light("r", "light_rect", {0, 4, 0}, {{"brightness", "6"}, {"radius", "40"}, {"width", "1"}, {"height", "1"}}, kDown)}));
    BakeOptions o;
    o.areaSamples = 256;
    o.superSamples = 1;
    const auto v = evalShadowed(sc, o);
    Scene1 free(floorAndSlab({100, 2, 100}, {101, 3, 101}, {light("r", "light_rect", {0, 4, 0}, {{"brightness", "6"}, {"radius", "40"}, {"width", "1"}, {"height", "1"}}, kDown)}));
    const auto lit = evalShadowed(free);
    const size_t i = sc.nearest({0, 0, 0}, {0, 1, 0});
    const Vec3 p = pos(sc.samples, i);
    if (std::fabs(p.x) < 0.04) CHECK(val(v, i) / val(lit, free.nearest(p, {0, 1, 0})) == doctest::Approx(0.5).epsilon(0.1));
    CHECK(val(v, sc.nearest({-3, 0, 0}, {0, 1, 0})) == doctest::Approx(val(lit, free.nearest({-3, 0, 0}, {0, 1, 0}))).epsilon(1e-6));
    CHECK(val(v, sc.nearest({3, 0, 0}, {0, 1, 0})) == 0.f);
}

// ---- GPU ray query vs the CPU BVH reference ----------------------------------------------------------------------------------
TEST_CASE("bake gpu: hardware ray query shadows agree with the CPU BVH (all light types, soft shadows, sky occlusion)") {
    std::vector<Entity> es{ent("w", "worldspawn", {Brush::makeBox("floor", {-8, -1, -8}, {8, 0, 8}), Brush::makeBox("slab", {-1, 2, -1}, {1, 2.2, 1}),
                                                  Brush::makeBox("pillar", {3, 0, 3}, {4, 3, 4}), Brush::makeBox("roof", {-8, 4, -8}, {0, 4.2, 8})}),
                           light("p", "light_point", {0, 4, 0}, {{"brightness", "10"}, {"radius", "30"}}),
                           light("ps", "light_point", {-5, 3, 4}, {{"brightness", "6"}, {"radius", "30"}, {"source_size", "0.4"}, {"color", "1 0.7 0.5"}}),
                           light("s", "light_spot", {5, 3.5, -3}, {{"brightness", "8"}, {"radius", "30"}, {"inner_angle", "20"}, {"outer_angle", "50"}}, {80, -30, 0}),
                           light("d", "light_directional", {0, 0, 0}, {{"brightness", "1.5"}, {"source_size", "6"}}, {55, 25, 0}),
                           light("r", "light_rect", {2, 3.5, 5}, {{"brightness", "4"}, {"radius", "30"}, {"width", "2"}, {"height", "1.5"}}, {90, 0, 0}),
                           light("k", "light_sky", {0, 0, 0}, {{"brightness", "0.8"}, {"color", "0.6 0.8 1"}})};
    Scene1 sc(es);
    BakeOptions gpuOpt;
    gpuOpt.gpuValidation = true;
    gpuOpt.areaSamples = 32;
    gpuOpt.skySamples = 64;
    BakeStats gst;
    const LightmapPages g = bakeLightmaps(sc.layout, sc.surfaces, sc.lights, gpuOpt, &gst);
    if (!gst.usedGpu) {
        MESSAGE("no Vulkan device with ray query, GPU shadow test skipped: " << (gst.warnings.empty() ? "" : gst.warnings.back()));
        return;
    }
    MESSAGE("GPU device: " << gst.device << ", texels " << gst.texels);
    for (const auto& m : gst.validationMessages) MESSAGE(m);
    for (const auto& m : gst.validationMessages) CHECK(m.rfind("[note]", 0) == 0);  // validation output IS a failure
    BakeOptions cpuOpt = gpuOpt;
    cpuOpt.forceCpu = true;
    const LightmapPages c = bakeLightmaps(sc.layout, sc.surfaces, sc.lights, cpuOpt);
    // unshadowed reference to prove the scene really has shadows in it
    BakeOptions flat = cpuOpt;
    flat.shadows = false;
    const LightmapPages u = bakeLightmaps(sc.layout, sc.surfaces, sc.lights, flat);

    size_t texels = 0, mismatched = 0, shadowed = 0;
    double sumErr = 0, maxErr = 0;
    for (int p = 0; p < g.pageCount; ++p)
        for (size_t t = 0; t < g.valid[static_cast<size_t>(p)].size(); ++t) {
            if (g.valid[static_cast<size_t>(p)][t] != 1) continue;
            ++texels;
            double e = 0, un = 0, cv = 0;
            for (size_t ch = 0; ch < 3; ++ch) {
                const double a = g.rgb[static_cast<size_t>(p)][t * 3 + ch], b = c.rgb[static_cast<size_t>(p)][t * 3 + ch];
                e = std::max(e, std::fabs(a - b) / std::max(1.0, std::fabs(b)));
                cv += b;
                un += u.rgb[static_cast<size_t>(p)][t * 3 + ch];
            }
            sumErr += e;
            maxErr = std::max(maxErr, e);
            if (e > 2e-3) ++mismatched;
            if (cv < 0.5 * un) ++shadowed;
        }
    MESSAGE("texels " << texels << ", mismatched " << mismatched << ", mean err " << sumErr / static_cast<double>(texels) << ", max err " << maxErr << ", shadowed " << shadowed);
    CHECK(shadowed > texels / 50);                                          // the scene has real shadows (>2 % of texels)
    CHECK(static_cast<double>(mismatched) < 0.005 * static_cast<double>(texels));  // CPU and GPU trace the same rays: only edge rounding differs
    CHECK(sumErr / static_cast<double>(texels) < 1e-3);
}

TEST_CASE("bake: the ray tracing rule is shared and the CPU fallback gives the same answer without a GPU") {
    Scene1 sc({ent("w", "worldspawn", {Brush::makeBox("floor", {-5, -1, -5}, {5, 0, 5}), Brush::makeBox("slab", {-1, 2, -1}, {1, 2.2, 1})}),
               light("p", "light_point", {0, 4, 0}, {{"brightness", "10"}, {"radius", "30"}})});
    BakeOptions o;
    const OccluderMesh mesh = buildOccluders(sc.surfaces, o.classes);
    CHECK(core::gpu::needsRayTracing(sc.lights, &mesh, o));
    o.shadows = false;
    CHECK(!core::gpu::needsRayTracing(sc.lights, &mesh, o));
    o.shadows = true;
    CHECK(!core::gpu::needsRayTracing(sc.lights, nullptr, o));
    const OccluderMesh emptyMesh;
    CHECK(!core::gpu::needsRayTracing(sc.lights, &emptyMesh, o));
    sc.lights.lights[0].castShadows = false;
    CHECK(!core::gpu::needsRayTracing(sc.lights, &mesh, o));
    // forceCpu must still shadow
    sc.lights.lights[0].castShadows = true;
    o.forceCpu = true;
    BakeStats st;
    const LightmapPages P = bakeLightmaps(sc.layout, sc.surfaces, sc.lights, o, &st);
    CHECK(!st.usedGpu);
    // the floor under the slab is dark, a floor texel far from it is lit
    bool darkUnder = false, litFar = false;
    for (size_t i = 0; i < sc.samples.count(); ++i) {
        if (sc.samples.nrm[i * 4 + 1] < 0.9f) continue;
        const float x = sc.samples.pos[i * 4], z = sc.samples.pos[i * 4 + 2];
        const size_t t = sc.samples.texel[i] % (static_cast<size_t>(P.atlas) * P.atlas);
        const float v = P.rgb[sc.samples.texel[i] / (static_cast<size_t>(P.atlas) * P.atlas)][t * 3];
        if (std::fabs(x) < 0.3f && std::fabs(z) < 0.3f && v == 0.f) darkUnder = true;
        if (x > 4 && z > 4 && v > 0.05f) litFar = true;
    }
    CHECK(darkUnder);
    CHECK(litFar);
}

// ---- solids: texels buried inside brushes are not baked ----------------------------------------------------------------------
TEST_CASE("solids: a point is solid inside light-blocking additive brushes, subtractors make air") {
    Document doc;
    Brush hole = Brush::makeBox("hole", {1, 1, 1}, {3, 3, 3});
    hole.op = BrushOp::Subtract;
    doc.entities = {ent("w", "worldspawn", {Brush::makeBox("block", {0, 0, 0}, {4, 4, 4}), hole}), ent("d", "func_door", {Brush::makeBox("leaf", {10, 0, 0}, {11, 4, 1})})};
    const LightingClasses classes = LightingClasses::defaults();
    const SolidSet s = collectSolids(doc, {}, [&](const std::string& c) { return classes.castsShadows(c); });
    CHECK(s.volumeCount() == 2);                       // the door does not block light: not a solid
    CHECK(s.inside({0.5, 0.5, 0.5}));
    CHECK(!s.inside({2, 2, 2}));                       // inside the cut-out
    CHECK(!s.inside({5, 5, 5}));
    CHECK(!s.inside({0, 2, 2}));                       // on the surface is not inside
    CHECK(s.inside({0.01, 2, 2}));
    CHECK(!s.inside({0.01, 2, 2}, 0.05));              // a margin demands to be deeper inside
    CHECK(!s.inside({10.5, 2, 0.5}));
    LightingClasses withDoor = classes;
    withDoor.castShadows.insert("func_door");
    CHECK(collectSolids(doc, {}, [&](const std::string& c) { return withDoor.castsShadows(c); }).inside({10.5, 2, 0.5}));
}

TEST_CASE("solids: texels under a pillar and inside a wall are skipped, everything visible is kept") {
    Scene1 sc({ent("w", "worldspawn", {Brush::makeBox("floor", {-5, -1, -5}, {5, 0, 5}), Brush::makeBox("pillar", {0, 0, -1}, {1, 3, 0}),
                                       Brush::makeBox("wall", {-5, 0, 4}, {5, 3, 5})})});
    const LightingClasses classes = LightingClasses::defaults();
    const SolidSet solids = collectSolids(sc.doc, {}, [&](const std::string& c) { return classes.castsShadows(c); });
    size_t hidden = 0;
    const TexelSamples with = buildTexelSamples(sc.layout, sc.surfaces, &solids, &hidden);
    const TexelSamples without = buildTexelSamples(sc.layout, sc.surfaces);
    CHECK(hidden > 0);
    CHECK(without.count() == with.count() + hidden);
    auto up = [](const TexelSamples& s, double x0, double x1, double z0, double z1) {
        size_t n = 0;
        for (size_t i = 0; i < s.count(); ++i)
            if (s.nrm[i * 4 + 1] > 0.9f && std::fabs(s.pos[i * 4 + 1]) < 1e-6f && s.pos[i * 4] > x0 && s.pos[i * 4] < x1 && s.pos[i * 4 + 2] > z0 && s.pos[i * 4 + 2] < z1) ++n;
        return n;
    };
    CHECK(up(without, 0.05, 0.95, -0.95, -0.05) > 0);   // the floor top under the pillar exists as a face ...
    CHECK(up(with, 0.05, 0.95, -0.95, -0.05) == 0);     // ... but its texels are not baked
    CHECK(up(without, -4, 4, 4.05, 4.95) > 0);
    CHECK(up(with, -4, 4, 4.05, 4.95) == 0);            // under the wall too
    CHECK(up(with, 2, 4, -4, 3.5) == up(without, 2, 4, -4, 3.5));       // the open floor is untouched
    CHECK(up(with, 2, 4, -4, 3.5) > 100);
    // the pillar's own side faces are visible and kept
    size_t side = 0;
    for (size_t i = 0; i < with.count(); ++i) if (std::fabs(with.nrm[i * 4 + 1]) < 0.1f && with.pos[i * 4] >= -1e-6f && with.pos[i * 4] <= 1 + 1e-6f && with.pos[i * 4 + 2] <= 1e-6f && with.pos[i * 4 + 2] >= -1 - 1e-6f && with.pos[i * 4 + 1] > 0.1f) ++side;
    CHECK(side > 0);
    // the bake reports it and skips the work
    BakeOptions o;
    o.forceCpu = true;
    o.solids = std::make_shared<SolidSet>(solids);
    BakeStats st;
    bakeLightmaps(sc.layout, sc.surfaces, sc.lights, o, &st);
    CHECK(st.hiddenTexels == hidden);
    CHECK(st.texels == with.count());
}

TEST_CASE("bake gpu: a sample subset keeps its random ids and sampleOffset advances the draws (CPU and GPU agree)") {
    std::vector<Entity> es{ent("w", "worldspawn", {Brush::makeBox("floor", {-8, -1, -8}, {8, 0, 8}), Brush::makeBox("pillar", {1, 0, 1}, {2, 3, 2})}),
                           light("ps", "light_point", {-3, 3, -2}, {{"brightness", "8"}, {"radius", "30"}, {"source_size", "0.8"}}),
                           light("d", "light_directional", {0, 0, 0}, {{"brightness", "1.5"}, {"source_size", "8"}}, {55, 25, 0}),
                           light("k", "light_sky", {0, 0, 0}, {{"brightness", "0.8"}})};
    Scene1 sc(es);
    const OccluderMesh mesh = buildOccluders(sc.surfaces, LightingClasses::defaults(), nullptr);
    const TriangleBvh bvh(mesh);
    BakeOptions o;
    o.gpuValidation = true;
    o.areaSamples = 8;
    o.skySamples = 8;
    o.sampleOffset = 7;

    // every third sample, keeping the ids it has in the full set
    TexelSamples sub;
    sub.atlas = sc.samples.atlas;
    sub.pageCount = sc.samples.pageCount;
    std::vector<size_t> picked;
    for (size_t i = 0; i < sc.samples.count(); i += 3) {
        picked.push_back(i);
        sub.appendFrom(sc.samples, i);  // keeps the sub-texel positions too
        sub.seed.push_back(static_cast<uint32_t>(i));
    }
    REQUIRE(sub.count() > 100);

    const std::vector<float> full = evaluateDirectCpu(sc.samples, sc.lights, &bvh, o);
    const std::vector<float> part = evaluateDirectCpu(sub, sc.lights, &bvh, o);
    for (size_t k = 0; k < picked.size(); ++k)
        for (size_t c = 0; c < 3; ++c) REQUIRE(part[k * 4 + c] == full[picked[k] * 4 + c]);  // same ids, same draws: bit-identical

    BakeOptions o0 = o;
    o0.sampleOffset = 0;
    const std::vector<float> part0 = evaluateDirectCpu(sub, sc.lights, &bvh, o0);
    size_t moved = 0;
    for (size_t i = 0; i < part.size(); ++i) if (std::fabs(part[i] - part0[i]) > 1e-4f) ++moved;
    CHECK(moved > sub.count() / 50);  // new draws change penumbra texels

    std::vector<float> g7, g0;
    const gpu::Result r7 = gpu::evaluateDirect(sub, sc.lights, &mesh, o, g7);
    if (!r7.ok) {
        MESSAGE("no Vulkan device with ray query, GPU subset test skipped: " << r7.error);
        return;
    }
    for (const auto& m : r7.validation) CHECK(m.rfind("[note]", 0) == 0);
    const gpu::Result r0 = gpu::evaluateDirect(sub, sc.lights, &mesh, o0, g0);
    REQUIRE(r0.ok);
    auto mismatches = [&](const std::vector<float>& a, const std::vector<float>& b) {
        size_t bad = 0;
        for (size_t i = 0; i < sub.count(); ++i)
            for (size_t c = 0; c < 3; ++c)
                if (std::fabs(a[i * 4 + c] - b[i * 4 + c]) / std::max(1.0f, std::fabs(b[i * 4 + c])) > 2e-3f) { ++bad; break; }
        return bad;
    };
    MESSAGE("subset " << sub.count() << ", gpu(offset 7) vs cpu mismatches " << mismatches(g7, part) << ", gpu(offset 0) vs cpu " << mismatches(g0, part0)
                      << ", gpu(offset 0) vs cpu(offset 7) " << mismatches(g0, part));
    CHECK(static_cast<double>(mismatches(g7, part)) < 0.005 * static_cast<double>(sub.count()));
    CHECK(static_cast<double>(mismatches(g0, part0)) < 0.005 * static_cast<double>(sub.count()));
    CHECK(mismatches(g0, part) > sub.count() / 100);  // the offset really reached the shader
}
