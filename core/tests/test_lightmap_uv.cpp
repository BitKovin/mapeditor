#include "core/derived.h"
#include "core/lighting_classes.h"
#include "core/lightmap_uv.h"
#include "core/scene.h"
#include "test_util.h"
#include <doctest.h>
#include <cmath>

using namespace core;
using namespace testutil;

namespace {

Entity ent(const std::string& id, const std::string& cls, std::vector<Brush> brushes) {
    Entity e;
    e.id = id;
    e.classname = cls;
    e.brushes = std::move(brushes);
    return e;
}
Brush sub(const std::string& id, Vec3 lo, Vec3 hi) {
    Brush b = Brush::makeBox(id, lo, hi);
    b.op = BrushOp::Subtract;
    return b;
}
std::vector<LightSurface> surfacesOf(std::vector<Entity> entities) {
    Document doc;
    doc.entities = std::move(entities);
    return collectSurfaces(doc, {});
}
LightmapLayout layoutOf(const std::vector<LightSurface>& s, LightmapSettings cfg = {}) {
    return buildLightmapLayout(s, LightingClasses::defaults(), cfg);
}

// ---- independent checks (no code shared with the generator) -------------------------------------------------------------
using Poly2 = std::vector<std::array<double, 2>>;
double area2(const Poly2& p) {
    double a = 0;
    for (size_t i = 0; i < p.size(); ++i) { const auto &x = p[i], &y = p[(i + 1) % p.size()]; a += x[0] * y[1] - y[0] * x[1]; }
    return 0.5 * a;
}
// Sutherland-Hodgman: intersection area of two convex polygons
double overlapArea(Poly2 subject, Poly2 clip) {
    if (area2(subject) < 0) std::reverse(subject.begin(), subject.end());
    if (area2(clip) < 0) std::reverse(clip.begin(), clip.end());
    for (size_t i = 0; i < clip.size() && !subject.empty(); ++i) {
        const auto a = clip[i], b = clip[(i + 1) % clip.size()];
        auto side = [&](const std::array<double, 2>& p) { return (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0]); };
        Poly2 next;
        for (size_t j = 0; j < subject.size(); ++j) {
            const auto p = subject[j], q = subject[(j + 1) % subject.size()];
            const double sp = side(p), sq = side(q);
            if (sp >= 0) next.push_back(p);
            if ((sp >= 0) != (sq >= 0)) {
                const double t = sp / (sp - sq);
                next.push_back({p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t});
            }
        }
        subject = std::move(next);
    }
    return std::fabs(area2(subject));
}

struct PiecePoly {
    int face;
    int chart;
    Poly2 poly;
};
std::vector<PiecePoly> pieces(const LightmapLayout& L, const std::vector<LightSurface>& s) {
    std::vector<PiecePoly> out;
    for (const auto& surf : s) {
        const LightmapFace* f = L.findForFragment(surf.instance, surf.entityId, surf.brushInstance, surf.brushId, surf.faceId, surf.verts);
        if (!f) continue;
        PiecePoly p{static_cast<int>(f - L.faces.data()), f->chart, {}};
        for (const auto& v : surf.verts) { const Vec2 t = f->texel(v); p.poly.push_back({t.u, t.v}); }
        out.push_back(std::move(p));
    }
    return out;
}

// The invariants every layout must satisfy, whatever the input.
void checkInvariants(const LightmapLayout& L, const std::vector<LightSurface>& s) {
    const int pad = L.settings.padding, atlas = L.settings.atlasSize;
    // charts: inside their page, disjoint on a page
    for (size_t i = 0; i < L.charts.size(); ++i) {
        const auto& a = L.charts[i];
        REQUIRE(a.page >= 0);
        REQUIRE(a.page < L.pageCount);
        CHECK(a.x >= 0);
        CHECK(a.y >= 0);
        CHECK(a.x + a.w <= atlas);
        CHECK(a.y + a.h <= atlas);
        for (size_t j = i + 1; j < L.charts.size(); ++j) {
            const auto& b = L.charts[j];
            if (a.page != b.page) continue;
            const bool apart = a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y;
            CHECK_MESSAGE(apart, "charts " << i << " and " << j << " overlap");
        }
    }
    const auto ps = pieces(L, s);
    for (const auto& p : ps) {
        const auto& c = L.charts[static_cast<size_t>(p.chart)];
        for (const auto& t : p.poly) {  // every vertex inside the chart, padding excluded
            CHECK(t[0] >= c.x + pad - 1e-6);
            CHECK(t[1] >= c.y + pad - 1e-6);
            CHECK(t[0] <= c.x + c.w - pad + 1e-6);
            CHECK(t[1] <= c.y + c.h - pad + 1e-6);
        }
    }
    // no two pieces of a chart overlap (this is what unfolding must guarantee)
    for (size_t i = 0; i < ps.size(); ++i)
        for (size_t j = i + 1; j < ps.size(); ++j)
            if (ps[i].chart == ps[j].chart) CHECK_MESSAGE(overlapArea(ps[i].poly, ps[j].poly) < 1e-6, "pieces " << i << " and " << j << " overlap in chart " << ps[i].chart);
    // isometry: texel distances = world distances * density
    for (const auto& surf : s) {
        const LightmapFace* f = L.findForFragment(surf.instance, surf.entityId, surf.brushInstance, surf.brushId, surf.faceId, surf.verts);
        if (!f) continue;
        for (size_t i = 0; i < surf.verts.size(); ++i) {
            const Vec3 a = surf.verts[i], b = surf.verts[(i + 1) % surf.verts.size()];
            const Vec2 ta = f->texel(a), tb = f->texel(b);
            CHECK(std::hypot(tb.u - ta.u, tb.v - ta.v) == doctest::Approx((b - a).length() * f->texelsPerMeter).epsilon(1e-9));
        }
    }
}
}  // namespace

TEST_CASE("lightmap uv: a floor gets a world-aligned lattice at the requested density") {
    const auto s = surfacesOf({ent("w", "worldspawn", {Brush::makeBox("floor", {0, -0.2, 0}, {4, 0, 3})})});
    LightmapSettings cfg;
    cfg.texelsPerMeter = 16;
    const auto L = layoutOf(s, cfg);
    checkInvariants(L, s);
    REQUIRE(L.faces.size() == 6);
    const LightmapFace* top = L.find("", "w", "", "floor", 5, FaceSide::Front);  // +Y face
    REQUIRE(top);
    CHECK(top->normal.y == doctest::Approx(1));
    CHECK(top->texelsPerMeter == doctest::Approx(16));
    // 1/16 m along world X = exactly one texel along s
    const Vec2 a = top->texel({1, 0, 1}), b = top->texel({1 + 1.0 / 16, 0, 1});
    CHECK(std::hypot(b.u - a.u, b.v - a.v) == doctest::Approx(1));
    // uv is the texel over the atlas size
    const Vec2 uv = L.uv(*top, {1, 0, 1});
    CHECK(uv.u * L.settings.atlasSize == doctest::Approx(a.u));
}

TEST_CASE("lightmap uv: resizing a face keeps its texels where they were") {
    LightmapSettings cfg;
    cfg.mergeAngleDeg = 0;
    const auto s1 = surfacesOf({ent("w", "worldspawn", {Brush::makeBox("floor", {0.3, -0.2, 0.7}, {4.1, 0, 3.3})})});
    const auto s2 = surfacesOf({ent("w", "worldspawn", {Brush::makeBox("floor", {-2.9, -0.2, -1.37}, {7.77, 0, 5.5})})});
    const auto L1 = layoutOf(s1, cfg), L2 = layoutOf(s2, cfg);
    const auto* a = L1.find("", "w", "", "floor", 5, FaceSide::Front);
    const auto* b = L2.find("", "w", "", "floor", 5, FaceSide::Front);
    REQUIRE((a && b));
    for (Vec3 p : {Vec3{1, 0, 1}, Vec3{2.25, 0, 2.5}, Vec3{3.9, 0, 3.1}}) {
        const Vec2 ta = a->texel(p), tb = b->texel(p);
        // same lattice: the fractional parts of the texel coordinates agree
        CHECK(std::remainder(ta.u - tb.u, 1.0) == doctest::Approx(0).epsilon(1e-9).scale(1));
        CHECK(std::remainder(ta.v - tb.v, 1.0) == doctest::Approx(0).epsilon(1e-9).scale(1));
    }
}

TEST_CASE("lightmap uv: unfolding keeps connected edges connected") {
    const auto s = surfacesOf({ent("w", "worldspawn", {Brush::makeBox("box", {0, 0, 0}, {4, 3, 2})})});
    {
        LightmapSettings cfg;
        cfg.mergeAngleDeg = 0;  // nothing merges: six separate charts
        const auto L = layoutOf(s, cfg);
        CHECK(L.charts.size() == 6);
        checkInvariants(L, s);
    }
    LightmapSettings cfg;
    cfg.mergeAngleDeg = 90;
    const auto L = layoutOf(s, cfg);
    checkInvariants(L, s);
    CHECK(L.charts.size() == 1);  // the six faces form one net
    // faces that share a world edge and map to the same texels on both sides: the net has 5 such edges
    int joined = 0;
    const Brush box = Brush::makeBox("box", {0, 0, 0}, {4, 3, 2});
    for (size_t i = 0; i < L.faces.size(); ++i)
        for (size_t j = i + 1; j < L.faces.size(); ++j) {
            std::vector<Vec3> common;
            const auto& fa = box.faces[L.faces[i].faceId];
            const auto& fb = box.faces[L.faces[j].faceId];
            for (uint32_t va : fa.loop) for (uint32_t vb : fb.loop) if (va == vb) common.push_back(box.vertices[va]);
            if (common.size() != 2) continue;
            bool same = true;
            for (const auto& p : common) {
                const Vec2 ta = L.faces[i].texel(p), tb = L.faces[j].texel(p);
                if (std::hypot(ta.u - tb.u, ta.v - tb.v) > 1e-6) same = false;
            }
            if (same) ++joined;
        }
    CHECK(joined >= 5);
}

TEST_CASE("lightmap uv: coplanar neighbours merge at angle 0, but never across entities") {
    LightmapSettings cfg;
    cfg.mergeAngleDeg = 0;
    const auto touching = surfacesOf({ent("w", "worldspawn", {Brush::makeBox("a", {0, -1, 0}, {2, 0, 2}), Brush::makeBox("b", {2, -1, 0}, {5, 0, 2})})});
    const auto L = layoutOf(touching, cfg);
    checkInvariants(L, touching);
    const auto *ta = L.find("", "w", "", "a", 5, FaceSide::Front), *tb = L.find("", "w", "", "b", 5, FaceSide::Front);
    REQUIRE((ta && tb));
    CHECK(ta->chart == tb->chart);
    CHECK(ta->texel({2, 0, 1}).u == doctest::Approx(tb->texel({2, 0, 1}).u));

    const auto split = surfacesOf({ent("w", "worldspawn", {Brush::makeBox("a", {0, -1, 0}, {2, 0, 2})}),
                                   ent("d", "func_detail", {Brush::makeBox("b", {2, -1, 0}, {5, 0, 2})})});
    const auto L2 = layoutOf(split, cfg);
    checkInvariants(L2, split);
    CHECK(L2.find("", "w", "", "a", 5, FaceSide::Front)->chart != L2.find("", "d", "", "b", 5, FaceSide::Front)->chart);
}

TEST_CASE("lightmap uv: faces that would overlap when unfolded are split into separate charts") {
    // Six triangles around an apex whose angles sum far past 360 degrees: unfolding them all must overlap.
    std::vector<LightSurface> s;
    const double h = 1.5;
    auto rim = [&](int k) { return Vec3{std::cos(k * 3.14159265358979 / 3), (k % 2 ? -h : h), std::sin(k * 3.14159265358979 / 3)}; };
    for (int k = 0; k < 6; ++k) {
        LightSurface t;
        t.entityId = "w";
        t.classname = "worldspawn";
        t.brushId = "saddle";
        t.faceId = static_cast<uint32_t>(k);
        t.verts = {Vec3{0, 0, 0}, rim(k), rim(k + 1)};
        t.normal = (t.verts[1] - t.verts[0]).cross(t.verts[2] - t.verts[0]).normalized();
        s.push_back(t);
    }
    LightmapSettings cfg;
    cfg.mergeAngleDeg = 180;
    const auto L = layoutOf(s, cfg);
    checkInvariants(L, s);
    CHECK(L.charts.size() > 1);
    CHECK(L.charts.size() < 6);  // but not everything is isolated: some do unfold
}

TEST_CASE("lightmap uv: carving produces back-side cut walls with their own charts") {
    const auto s = surfacesOf({ent("w", "worldspawn", {Brush::makeBox("wall", {0, 0, 0}, {6, 4, 1}), sub("hole", {2, 1, -1}, {4, 3, 2})})});
    const auto L = layoutOf(s);
    checkInvariants(L, s);
    int back = 0;
    for (const auto& f : L.faces) if (f.side == FaceSide::Back) { ++back; CHECK(f.brushId == "hole"); }
    CHECK(back == 4);  // the four walls of the opening (the hole's own front/back are open)
    // the wall's -Z face has a hole: several fragments, ONE face map
    const auto* front = L.find("", "w", "", "wall", 0, FaceSide::Front);
    REQUIRE(front);
    int fragments = 0;
    for (const auto& surf : s)
        if (surf.brushId == "wall" && surf.faceId == 0) {
            ++fragments;
            CHECK(L.findForFragment("", "w", "", "wall", 0, surf.verts) == front);
        }
    CHECK(fragments > 1);
    // no fragment of the carved wall maps inside the hole's texel footprint: its area adds up to face minus hole
    double texelArea = 0;
    for (const auto& p : pieces(L, s)) if (&L.faces[static_cast<size_t>(p.face)] == front) texelArea += std::fabs(area2(p.poly));
    CHECK(texelArea == doctest::Approx((6 * 4 - 2 * 2) * front->texelsPerMeter * front->texelsPerMeter).epsilon(1e-6));
}

TEST_CASE("lightmap uv: only lightmapped classes get charts") {
    const auto s = surfacesOf({ent("w", "worldspawn", {Brush::makeBox("a", {0, 0, 0}, {1, 1, 1})}),
                               ent("door", "func_door", {Brush::makeBox("d", {3, 0, 0}, {4, 1, 1})})});
    const auto L = layoutOf(s);
    REQUIRE(!L.faces.empty());
    for (const auto& f : L.faces) CHECK(f.entityId == "w");
    LightingClasses c = LightingClasses::defaults();
    c.lightmapped.insert("func_door");
    CHECK(buildLightmapLayout(s, c).faces.size() == 12);
}

TEST_CASE("lightmap uv: decal fragments resolve to their parent face") {
    const auto s = surfacesOf({ent("w", "worldspawn", {Brush::makeBox("floor", {0, -1, 0}, {10, 0, 10})})});
    const auto L = layoutOf(s);
    // a decal fragment lying on the floor (+Y face, id 5), CCW seen from +Y
    const std::vector<Vec3> ccw = {{1, 0, 1}, {2, 0, 1}, {2, 0, 2}, {1, 0, 2}};
    const auto* f = L.findForFragment("", "w", "", "floor", 5, ccw);
    REQUIRE(f);
    CHECK(f->side == FaceSide::Front);
    CHECK(f->faceId == 5);
    // the decal samples the parent's chart: its corners land inside that chart's rectangle
    const auto& chart = L.charts[static_cast<size_t>(f->chart)];
    for (const auto& p : ccw) {
        const Vec2 t = f->texel(p);
        CHECK(t.u >= chart.x);
        CHECK(t.u <= chart.x + chart.w);
        CHECK(t.v >= chart.y);
        CHECK(t.v <= chart.y + chart.h);
    }
    // a fragment facing the other way resolves to the back side when that exists, else to the only side
    CHECK(L.findForFragment("", "w", "", "floor", 5, std::vector<Vec3>(ccw.rbegin(), ccw.rend())) == f);
    CHECK(L.findForFragment("", "w", "", "floor", 99, ccw) == nullptr);
}

TEST_CASE("lightmap uv: packing is deterministic, coherent per entity and texture, and respects the atlas") {
    std::vector<Entity> es;
    for (int e = 0; e < 3; ++e) {
        std::vector<Brush> bs;
        for (int i = 0; i < 6; ++i) {
            const double x = i * 3.0 + e * 40.0;
            bs.push_back(Brush::makeBox("b" + std::to_string(i), {x, 0, 0}, {x + 2, 2, 2}, i % 2 ? "dev/wall" : "dev/floor"));
        }
        es.push_back(ent("e" + std::to_string(e), e == 0 ? "worldspawn" : "func_detail", bs));
    }
    const auto s = surfacesOf(es);
    LightmapSettings cfg;
    cfg.atlasSize = 128;
    cfg.mergeAngleDeg = 0;
    const auto L = layoutOf(s, cfg);
    checkInvariants(L, s);
    CHECK(L.pageCount > 1);
    REQUIRE(L.pageFill.size() == static_cast<size_t>(L.pageCount));
    for (double f : L.pageFill) { CHECK(f > 0); CHECK(f <= 1.0); }
    CHECK(layoutOf(s, cfg).toJson() == L.toJson());  // deterministic

    // with a roomy atlas every (entity, texture) bucket sits on exactly one page
    LightmapSettings roomy;
    roomy.atlasSize = 1024;
    roomy.mergeAngleDeg = 0;
    const auto R = layoutOf(s, roomy);
    checkInvariants(R, s);
    std::map<std::string, std::set<int>> pagesOf;
    for (const auto& c : R.charts) pagesOf[c.entityId + "|" + c.texture].insert(c.page);
    for (const auto& kv : pagesOf) CHECK_MESSAGE(kv.second.size() == 1, kv.first);
}

TEST_CASE("lightmap uv: a face bigger than the chart limit is scaled down with a warning") {
    const auto s = surfacesOf({ent("w", "worldspawn", {Brush::makeBox("huge", {0, 0, 0}, {100, 0.1, 100})})});
    LightmapSettings cfg;
    cfg.texelsPerMeter = 16;       // 1600 texels wanted
    cfg.maxChartTexels = 256;
    cfg.atlasSize = 512;
    cfg.mergeAngleDeg = 0;
    const auto L = layoutOf(s, cfg);
    checkInvariants(L, s);
    const auto* top = L.find("", "w", "", "huge", 5, FaceSide::Front);
    REQUIRE(top);
    CHECK(top->texelsPerMeter < 16);
    CHECK(top->texelsPerMeter * 100 <= 256 + 1);
    CHECK(!L.warnings.empty());
}

TEST_CASE("lightmap uv: per-face density scale changes density and prevents merging") {
    const auto s = surfacesOf({ent("w", "worldspawn", {Brush::makeBox("a", {0, -1, 0}, {2, 0, 2}), Brush::makeBox("b", {2, -1, 0}, {4, 0, 2})})});
    LightmapSettings cfg;
    cfg.mergeAngleDeg = 0;
    const auto base = layoutOf(s, cfg);
    CHECK(base.find("", "w", "", "a", 5, FaceSide::Front)->chart == base.find("", "w", "", "b", 5, FaceSide::Front)->chart);
    cfg.densityScale[base.find("", "w", "", "b", 5, FaceSide::Front)->key()] = 2.0;
    const auto L = layoutOf(s, cfg);
    checkInvariants(L, s);
    const auto *a = L.find("", "w", "", "a", 5, FaceSide::Front), *b = L.find("", "w", "", "b", 5, FaceSide::Front);
    CHECK(a->chart != b->chart);
    CHECK(b->texelsPerMeter == doctest::Approx(2 * a->texelsPerMeter));
}

TEST_CASE("lightmap uv: a face's own lightmapScale (from the document) sets its texel density, clamped to the allowed range") {
    Document doc;
    doc.entities = {ent("w", "worldspawn", {Brush::makeBox("a", {0, -1, 0}, {2, 0, 2}), Brush::makeBox("b", {2, -1, 0}, {4, 0, 2})})};
    for (auto& f : doc.entities[0].brushes[1].faces) f.lightmapScale = 4.0;
    for (auto& f : doc.entities[0].brushes[0].faces) f.lightmapScale = 1000;  // out of range: clamped
    LightmapSettings cfg;
    cfg.mergeAngleDeg = 0;
    const auto s = collectSurfaces(doc, {});
    const auto L = layoutOf(s, cfg);
    checkInvariants(L, s);
    const auto *a = L.find("", "w", "", "a", 5, FaceSide::Front), *b = L.find("", "w", "", "b", 5, FaceSide::Front);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->chart != b->chart);  // different densities never share a chart
    CHECK(a->texelsPerMeter == doctest::Approx(cfg.texelsPerMeter * kMaxLightmapScale));
    CHECK(b->texelsPerMeter == doctest::Approx(cfg.texelsPerMeter * 4.0));
}

TEST_CASE("lightmap uv: prefab instances get their own charts keyed by instance") {
    Document doc;
    doc.entities = {ent("w", "worldspawn", {Brush::makeBox("floor", {0, -1, 0}, {4, 0, 4})})};
    ResolvedEntity r{ent("w", "func_detail", {Brush::makeBox("floor", {10, -1, 0}, {14, 0, 4})}), {"root", "inst_a"}, "w"};
    const auto s = collectSurfaces(doc, {r});
    const auto L = layoutOf(s);
    checkInvariants(L, s);
    CHECK(L.find("", "w", "", "floor", 5, FaceSide::Front));
    CHECK(L.find("inst_a", "w", "inst_a", "floor", 5, FaceSide::Front));  // same ids, different instance
}

TEST_CASE("lighting classes: defaults, the game profile adds to them, and a broken profile is loud") {
    const auto d = LightingClasses::defaults();
    for (const char* c : {"worldspawn", "func_detail", "func_group"}) {
        CHECK(d.isLightmapped(c));
        CHECK(d.castsShadows(c));
    }
    CHECK(!d.isLightmapped("func_door"));

    GameProfile p;
    p.receiveBakedLight = {"func_wall"};
    p.blockLight = {"func_wall", "func_door"};
    const auto c = LightingClasses::fromProfile(p);
    CHECK(c.isLightmapped("func_wall"));
    CHECK(!c.isLightmapped("func_door"));      // blocks light but is lit from light volumes
    CHECK(c.castsShadows("func_door"));
    CHECK(c.isLightmapped("worldspawn"));       // the structural classes cannot be removed
    CHECK(c.castsShadows("func_group"));
    CHECK(c.toJson()["receiveBakedLight"].size() == 5);  // worldspawn, func_detail, func_group, misc_model + the profile's func_wall

    TempDir dir;
    dir.write("game_profile.json", json{{"lighting", {{"blockLight", {"func_door"}}, {"receiveBakedLight", {"func_wall"}}}}});
    const GameProfile loaded = loadProfile(dir.path);
    CHECK(loaded.blockLight == std::vector<std::string>{"func_door"});
    CHECK(loaded.toJson()["lighting"]["receiveBakedLight"][0] == "func_wall");
    for (const json& bad : {json{{"lighting", 5}}, json{{"lighting", {{"blockLight", "func_door"}}}}, json{{"lighting", {{"receiveBakedLight", {1}}}}},
                            json{{"lighting", {{"blockLight", {""}}}}}}) {
        dir.write("game_profile.json", bad);
        CHECK_THROWS(loadProfile(dir.path));
    }
}

TEST_CASE("lightmap uv: Scene builds the layout from the open document and lighting_classes.json") {
    TempDir dir;
    dir.write("maps/a.map.json", docJson({entityJson("w", "worldspawn", {Brush::makeBox("floor", {0, -1, 0}, {4, 0, 4})}),
                                          entityJson("d", "func_door", {Brush::makeBox("leaf", {5, 0, 0}, {6, 2, 1})})}));
    Scene scene{dir.path};
    scene.openDocument("maps/a.map.json");
    auto L = scene.lightmapLayout();
    CHECK(L.faces.size() == 6);
    const json j = L.toJson();
    CHECK(j["type"] == "lightmap_layout");
    CHECK(j["faces"].size() == 6);
    dir.write("game_profile.json", json{{"lighting", {{"receiveBakedLight", {"func_door"}}}}});
    scene.loadGameProfile();
    CHECK(scene.lightmapLayout().faces.size() == 12);
}
