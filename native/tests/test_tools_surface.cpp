// T11.3a: the Surface Inspector's model (port of shell/tests/surface.test.ts, plus the face write-back the Qt side needs). Pure, no window, no GPU.
#include <doctest.h>

#include <algorithm>
#include <cmath>

#include "tools/texturing/surface.h"

using namespace native::tools::tex;
namespace tx = core::texplan;

namespace {

InfoFn loaded() {
    static const tx::TexInfo info = [] {
        tx::TexInfo i;
        i.path = "t";
        i.width = 256;
        i.height = 128;
        i.ppm = 64;
        i.props = tx::defaultProps();
        return i;
    }();
    return [](const std::string&) { return &info; };
}
const InfoFn kNone{};

struct Wall {
    core::Brush b = core::Brush::makeBox("b", {0, 0, 0}, {4, 3, 0.2}, "t");
    const core::BrushFace* f = nullptr;
    Wall() {
        for (const core::BrushFace& x : b.faces)
            if (core::texplan::faceNormalOf(b, x)[2] > 0.9) f = &x;
        REQUIRE(f);
    }
};

Projection P(V2 scale = {4, 2}, V2 shift = {0, 0}, double rotate = 0) {
    Projection p;
    p.scale = scale;
    p.shift = shift;
    p.rotate = rotate;
    return p;
}
double lo(const std::vector<V2>& uv, int k) {
    double m = 1e300;
    for (const V2& q : uv) m = std::min(m, q[static_cast<size_t>(k)]);
    return m;
}
double hi(const std::vector<V2>& uv, int k) {
    double m = -1e300;
    for (const V2& q : uv) m = std::max(m, q[static_cast<size_t>(k)]);
    return m;
}
double spread(const std::vector<V2>& uv, int k) { return hi(uv, k) - lo(uv, k); }

} // namespace

TEST_CASE("surface basis: texels and natural size from the texture, 64 px / 1 m while it is not loaded") {
    Wall w;
    CHECK(surfaceBasis(*w.f, kNone).texels == V2{64, 64});
    CHECK(surfaceBasis(*w.f, kNone).natural == V2{1, 1});
    CHECK(surfaceBasis(*w.f, loaded()).texels == V2{256, 128});
    CHECK(surfaceBasis(*w.f, loaded()).natural == V2{4, 2});
}

TEST_CASE("shift is shown in texels and set in texels") {
    const Projection p = withShiftTexels(P(), 0, 64, {256, 128});
    CHECK(p.shift[0] == doctest::Approx(0.25));
    CHECK(shiftTexels(p, {256, 128}) == V2{64, 0});
}

TEST_CASE("stretch is a multiplier of the natural size; zero is refused; mirror negates") {
    CHECK(stretchOf(P({8, 1}), {4, 2}) == V2{2, 0.5});
    CHECK(withStretch(P(), 0, 2, {4, 2}).scale[0] == 8);
    CHECK(withStretch(P(), 0, 0, {4, 2}).scale == P().scale);
    CHECK(mirrorAxis(P(), 1).scale == V2{4, -2});
    CHECK(mirrorAxis(mirrorAxis(P(), 1), 1).scale == P().scale);
}

TEST_CASE("Fit / Width / Height: the texture covers the face the given number of times, edge to edge") {
    Wall w;
    const auto uv = projectFace(w.b, *w.f, fitBoth(w.b, *w.f, P(), {1, 1}));
    CHECK(spread(uv, 0) == doctest::Approx(1).epsilon(1e-6));
    CHECK(lo(uv, 0) == doctest::Approx(0).epsilon(1e-6));
    CHECK(spread(uv, 1) == doctest::Approx(1).epsilon(1e-6));
    CHECK(lo(uv, 1) == doctest::Approx(0).epsilon(1e-6));
    const auto wide = projectFace(w.b, *w.f, fitAxis(w.b, *w.f, P(), 0, 2));
    CHECK(spread(wide, 0) == doctest::Approx(2).epsilon(1e-6));
    CHECK(fitAxis(w.b, *w.f, P(), 0).scale[1] == 2);   // the other axis is untouched
}

TEST_CASE("Fit keeps a mirrored axis mirrored and still covers the face") {
    Wall w;
    const auto uv = projectFace(w.b, *w.f, fitAxis(w.b, *w.f, P({-4, 2}), 0, 1));
    CHECK(lo(uv, 0) == doctest::Approx(0).epsilon(1e-6));
    CHECK(hi(uv, 0) == doctest::Approx(1).epsilon(1e-6));
}

TEST_CASE("Match Grid: whole grid cells per repeat, at least one, sign kept") {
    CHECK(matchGrid(P({3.9, 0.3}), 0.5).scale == V2{4, 0.5});
    CHECK(matchGrid(P({-1.2, 2}), 1).scale == V2{-1, 2});
    CHECK(matchGrid(P(), 0).scale == P().scale);
}

TEST_CASE("Project Axial clears rotation and shift, keeps the stretch") {
    const Projection a = projectAxial(P({4, 2}, {0.3, 0.2}, 30));
    CHECK(a.scale == V2{4, 2});
    CHECK(a.shift == V2{0, 0});
    CHECK(a.rotate == 0);
    CHECK(a.kind == tx::ProjKind::Axial);
}

TEST_CASE("Justify: each edge of the texture touches the same edge of the face; stretch kept; mirrored and rotated faces too") {
    Wall w;
    // 4 x 3 m face, 4 x 2 m repeats: u spans 1, v spans 1.5
    CHECK(lo(projectFace(w.b, *w.f, justify(w.b, *w.f, P({4, 2}, {0.3, 0.3}), JustifyEdge::Left)), 0) == doctest::Approx(0).epsilon(1e-9));
    CHECK(hi(projectFace(w.b, *w.f, justify(w.b, *w.f, P(), JustifyEdge::Right)), 0) == doctest::Approx(1).epsilon(1e-9));
    CHECK(lo(projectFace(w.b, *w.f, justify(w.b, *w.f, P(), JustifyEdge::Top)), 1) == doctest::Approx(0).epsilon(1e-9));
    CHECK(hi(projectFace(w.b, *w.f, justify(w.b, *w.f, P(), JustifyEdge::Bottom)), 1) == doctest::Approx(1).epsilon(1e-9));
    const Projection c = justify(w.b, *w.f, P(), JustifyEdge::Center);
    const auto cu = projectFace(w.b, *w.f, c);
    CHECK((lo(cu, 0) + hi(cu, 0)) / 2 == doctest::Approx(0.5).epsilon(1e-9));
    CHECK((lo(cu, 1) + hi(cu, 1)) / 2 == doctest::Approx(0.5).epsilon(1e-9));
    CHECK(c.scale == V2{4, 2});
    CHECK(lo(projectFace(w.b, *w.f, justify(w.b, *w.f, P({-4, 2}), JustifyEdge::Left)), 0) == doctest::Approx(0).epsilon(1e-9));
    CHECK(lo(projectFace(w.b, *w.f, justify(w.b, *w.f, P({4, 2}, {0, 0}, 30), JustifyEdge::Top)), 1) == doctest::Approx(0).epsilon(1e-9));
}

TEST_CASE("bottom-left origin: Reset puts the texture bottom-left on the face bottom-left, a fractional count leaves the partial repeat at the top") {
    Wall w;
    const auto uv = projectFace(w.b, *w.f, resetSurface(w.b, *w.f, kNone));
    CHECK(lo(uv, 0) == doctest::Approx(0).epsilon(1e-9));
    CHECK(hi(uv, 1) == doctest::Approx(1).epsilon(1e-9));
    const auto v = projectFace(w.b, *w.f, fitAxis(w.b, *w.f, P(), 1, 2.5));   // 4 x 3 m
    CHECK(hi(v, 1) == doctest::Approx(1).epsilon(1e-9));
    CHECK(lo(v, 1) == doctest::Approx(1 - 2.5).epsilon(1e-9));
}

TEST_CASE("baked bottom-left default: a known texture gets its projection, an unknown one stays lazy, an explicit projection is kept") {
    Wall w;
    const core::BrushFace same = withBottomLeftDefault(w.b, *w.f, kNone);
    CHECK_FALSE(hasProjection(same));
    CHECK(same.uvs.empty());
    const core::BrushFace g = withBottomLeftDefault(w.b, *w.f, loaded());
    REQUIRE(hasProjection(g));
    CHECK(projectionOfFace(g, loaded()).scale == V2{4, 2});
    REQUIRE(g.uvs.size() == g.loop.size());
    double umin = 1e300, vmax = -1e300;
    for (const core::Vec2& q : g.uvs) {
        umin = std::min(umin, q.u);
        vmax = std::max(vmax, q.v);
    }
    CHECK(umin == doctest::Approx(0).epsilon(1e-9));
    CHECK(vmax == doctest::Approx(1).epsilon(1e-9));
    CHECK(g.lookHash != 0);
    const core::BrushFace own = withProjection(w.b, *w.f, P({1, 1}, {0.3, 0.3}));
    const core::BrushFace kept = withBottomLeftDefault(w.b, own, loaded());
    CHECK(kept.uvs.size() == own.uvs.size());
    CHECK(projectionOfFace(kept, loaded()).scale == V2{1, 1});
    const core::Brush aligned = alignBrush(w.b, loaded());
    for (const core::BrushFace& f : aligned.faces) CHECK(hasProjection(f));
}

TEST_CASE("withProjection writes look, the typed projection and the uvs, and the hash follows") {
    Wall w;
    const Projection p = P({3, 1.5}, {0.25, 0.5}, 15);
    const core::BrushFace nf = withProjection(w.b, *w.f, p);
    const Projection back = projectionOfFace(nf, kNone);
    CHECK(back.scale == p.scale);
    CHECK(back.shift == p.shift);
    CHECK(back.rotate == doctest::Approx(15));
    CHECK(nf.projection.scaleU == 3);
    CHECK(nf.projection.shiftV == 0.5);
    REQUIRE(nf.uvs.size() == nf.loop.size());
    const auto expect = projectFace(w.b, *w.f, p);
    for (size_t i = 0; i < expect.size(); ++i) {
        CHECK(nf.uvs[i].u == doctest::Approx(expect[i][0]));
        CHECK(nf.uvs[i].v == doctest::Approx(expect[i][1]));
    }
    CHECK(nf.lookHash != 0);
    const core::BrushFace other = withProjection(w.b, *w.f, P({3, 1.5}, {0.25, 0.75}, 15));
    CHECK(other.lookHash != nf.lookHash);
    CHECK(withProjection(w.b, *w.f, p).lookHash == nf.lookHash);   // deterministic
    // a repeat override already on the face survives
    core::BrushFace rep = *w.f;
    rep.look = std::make_shared<const nlohmann::json>(nlohmann::json{{"repeat", {{"u", {{"mode", "mirror"}}}}}});
    const core::BrushFace both = withProjection(w.b, rep, p);
    REQUIRE(both.look);
    CHECK(both.look->contains("repeat"));
    CHECK(both.look->contains("projection"));
}

TEST_CASE("projection kinds: Ortho equals Axial (up to the shift) on axis-aligned faces, and keeps stretch") {
    const core::Brush b = core::Brush::makeBox("b", {0, 0, 0}, {4, 3, 2}, "t");
    for (const core::BrushFace& f : b.faces) {
        const auto axial = projectFace(b, f, P({2, 2}));
        const Projection o = projectOrtho(b, f, P({2, 2}));
        const auto ortho = projectFace(b, f, o);
        for (size_t i = 1; i < axial.size(); ++i) {
            CHECK(ortho[i][0] - ortho[0][0] == doctest::Approx(axial[i][0] - axial[0][0]).epsilon(1e-9));
            CHECK(ortho[i][1] - ortho[0][1] == doctest::Approx(axial[i][1] - axial[0][1]).epsilon(1e-9));
        }
        CHECK(o.kind == tx::ProjKind::Ortho);
    }
}

TEST_CASE("on a sloped face Ortho does not stretch: v measures the length along the slope") {
    core::Brush b = core::Brush::makeBox("b", {0, 0, 0}, {4, 2, 1}, "t");
    for (core::Vec3& v : b.vertices)
        if (v.y > 1 && v.z > 0.5) v.z += 2;
    const core::BrushFace* f = nullptr;
    for (const core::BrushFace& x : b.faces) {
        const V3 n = tx::faceNormalOf(b, x);
        if (n[2] > 0.5 && std::fabs(n[1]) > 0.2) f = &x;
    }
    REQUIRE(f);
    const auto ortho = projectFace(b, *f, projectOrtho(b, *f, P({1, 1})));
    const auto axial = projectFace(b, *f, P({1, 1}));
    const double slope = std::hypot(2, 2);
    CHECK(spread(ortho, 1) == doctest::Approx(slope).epsilon(1e-6));
    CHECK(spread(axial, 1) < slope - 0.1);   // axial compresses it
    CHECK(spread(ortho, 0) == doctest::Approx(4).epsilon(1e-6));
}

TEST_CASE("Cam freezes the camera axes in the face plane and refuses a degenerate view") {
    Wall w;
    CHECK_FALSE(projectCam(w.b, *w.f, P({1, 1}), {0, 0, 1}, {0, 1, 0}));   // right axis along the wall normal
    const auto r = projectCam(w.b, *w.f, P({1, 1}), {1, 0, 0}, {0, 1, 0});
    REQUIRE(r);
    CHECK(r->kind == tx::ProjKind::Cam);
    const auto uv = projectFace(w.b, *w.f, *r), ax = projectFace(w.b, *w.f, P({1, 1}));
    CHECK(uv[1][0] - uv[0][0] == doctest::Approx(ax[1][0] - ax[0][0]).epsilon(1e-9));
    const auto rolled = projectCam(w.b, *w.f, P({1, 1}), {0, 1, 0}, {-1, 0, 0});   // a camera rolled 90 degrees turns the texture
    REQUIRE(rolled);
    const auto u2 = projectFace(w.b, *w.f, *rolled);
    CHECK(u2[1][0] - u2[0][0] == doctest::Approx(0).epsilon(1e-9));
}

TEST_CASE("stored kind and basis survive the face's JSON; a broken basis falls back to axial") {
    Wall w;
    Projection p = P({1, 1});
    p.kind = tx::ProjKind::Ortho;
    p.basis = std::array<V3, 2>{V3{1, 0, 0}, V3{0, -1, 0}};
    const core::BrushFace nf = withProjection(w.b, *w.f, p);
    const Projection back = projectionOfFace(nf, kNone);
    CHECK(back.kind == tx::ProjKind::Ortho);
    REQUIRE(back.basis);
    CHECK((*back.basis)[1][1] == -1);
    core::BrushFace bad = *w.f;
    bad.look = std::make_shared<const nlohmann::json>(nlohmann::json::parse(R"({"projection":{"scale":[1,1],"shift":[0,0],"rotate":0,"kind":"ortho","basis":[[1,0,0],[2,0,0]]}})"));
    CHECK(projectionOfFace(bad, kNone).kind == tx::ProjKind::Axial);
}

TEST_CASE("shear slants the texture: u += shear * v after the rotation; justify and fit still touch the edges") {
    Wall w;
    const Projection p = withShear(P({1, 1}), 0.5);
    const V3 n = tx::faceNormalOf(w.b, *w.f);
    const V3 q = tx::v3(w.b.vertices[w.f->loop[0]]);
    const V2 a0 = placedCoords(n, q, P({1, 1})), a1 = placedCoords(n, q, p);
    CHECK(a1[0] == doctest::Approx(a0[0] + 0.5 * a0[1]).epsilon(1e-9));
    CHECK(a1[1] == doctest::Approx(a0[1]).epsilon(1e-9));
    CHECK(lo(projectFace(w.b, *w.f, justify(w.b, *w.f, p, JustifyEdge::Left)), 0) == doctest::Approx(0).epsilon(1e-9));
    const auto uv = projectFace(w.b, *w.f, fitBoth(w.b, *w.f, p, {1, 1}));
    CHECK(spread(uv, 0) == doctest::Approx(1).epsilon(1e-6));
    CHECK(spread(uv, 1) == doctest::Approx(1).epsilon(1e-6));
    CHECK(withShear(p, 0).shear == 0);
    // the shear is stored and read back
    CHECK(projectionOfFace(withProjection(w.b, *w.f, p), kNone).shear == doctest::Approx(0.5));
    CHECK_FALSE(projectionJson(withShear(p, 0)).contains("shear"));
}

TEST_CASE("projectionFromAffine reproduces a map exactly (the free kind): the inspector can edit a face whose uvs came from an operation") {
    core::Brush b = core::Brush::makeBox("b", {0, 0, 0}, {4, 3, 2}, "t");
    for (const core::BrushFace& f : b.faces) {
        const V3 n = tx::faceNormalOf(b, f);
        Projection src = P({2.5, 1.5}, {0.1, 0.2}, 20);
        src = withShear(src, 0.3);
        const tx::UvAffine m = tx::projectionAffine(n, src);
        const auto fromAff = projectionFromAffine(n, {m.u[0], m.u[1], m.u[2]}, {m.v[0], m.v[1], m.v[2]}, m.u[3], m.v[3]);
        REQUIRE(fromAff);
        CHECK(fromAff->kind == tx::ProjKind::Free);
        const auto a = projectFace(b, f, src), c = projectFace(b, f, *fromAff);
        for (size_t i = 0; i < a.size(); ++i) {
            CHECK(c[i][0] == doctest::Approx(a[i][0]).epsilon(1e-6));
            CHECK(c[i][1] == doctest::Approx(a[i][1]).epsilon(1e-6));
        }
    }
    CHECK_FALSE(projectionFromAffine({0, 0, 0}, {1, 0, 0}, {0, 1, 0}, 0, 0));
    CHECK_FALSE(projectionFromAffine({0, 0, 1}, {1, 0, 0}, {0, 0, 0}, 0, 0));
}
