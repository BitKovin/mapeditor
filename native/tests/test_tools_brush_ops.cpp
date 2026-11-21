// T11.2a: the whole-brush operations (clip, hollow, hull / merge, snap, union volume), face drag and extend, 2D resize, select inside / touching and the clipper point helpers
// (ports of brushOps.test.ts, faceDrag.test.ts, resize2d.test.ts, selectVolume.test.ts, clipperPoints.test.ts).
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <set>

#include "render/camera.h"
#include "tools/brush_ops.h"
#include "tools/face_drag.h"
#include "tools/mesh/ops.h"
#include "tools/resize2d.h"

using namespace native;
using core::Brush;
using core::BrushFace;
using core::Vec3;
using tools::Plane;

namespace {

Brush box() { return Brush::makeBox("b", {0, 0, 0}, {4, 4, 4}); }
Brush mkbox(const char* id, Vec3 a, Vec3 b) { return Brush::makeBox(id, a, b); }
const double kS = 0.7071067811865476;

Brush wedgeOf(double size) {
    const tools::ClipResult r = tools::clipBrush(Brush::makeBox("b", {0, 0, 0}, {size, size, size}), Plane{{kS, kS, 0}, kS * size});
    REQUIRE(r.kind == tools::ClipKind::Clipped);
    return r.brush;
}

double maxX(const Brush& b) {
    double m = -1e300;
    for (const Vec3& v : b.vertices) m = std::max(m, v.x);
    return m;
}

Brush faceMapped(Brush b, size_t face, int a0, int a1) {   // uvs = two world coordinates
    auto c = [](const Vec3& v, int a) { return a == 0 ? v.x : a == 1 ? v.y : v.z; };
    b.faces[face].uvs.clear();
    for (uint32_t vi : b.faces[face].loop) b.faces[face].uvs.push_back({c(b.vertices[vi], a0), c(b.vertices[vi], a1)});
    return b;
}

} // namespace

TEST_CASE("clipBrush: an axis cut halves a box and the cap gets a new face id") {
    const tools::ClipResult r = tools::clipBrush(box(), Plane{{1, 0, 0}, 1});
    REQUIRE(r.kind == tools::ClipKind::Clipped);
    CHECK(r.brush.signedVolume() == doctest::Approx(16));
    CHECK(tools::mesh::isProperSolid(r.brush));
    CHECK(r.brush.faces.size() == 6);
    std::set<uint32_t> ids;
    for (const BrushFace& f : r.brush.faces) ids.insert(f.id);
    CHECK(ids.size() == 6);
    CHECK(maxX(r.brush) == doctest::Approx(1));
}

TEST_CASE("clipBrush: a diagonal cut makes a wedge; a plane that misses leaves the brush, one that takes everything leaves nothing") {
    const tools::ClipResult w = tools::clipBrush(box(), Plane{{kS, kS, 0}, kS * 4});
    REQUIRE(w.has());
    CHECK(w.brush.signedVolume() == doctest::Approx(32));
    CHECK(tools::mesh::isProperSolid(w.brush));
    CHECK(tools::clipBrush(box(), Plane{{1, 0, 0}, 9}).kind == tools::ClipKind::Unchanged);
    CHECK(tools::clipBrush(box(), Plane{{1, 0, 0}, -1}).kind == tools::ClipKind::Empty);
}

TEST_CASE("clipBrush: uvs stay continuous across the cut") {
    Brush b = faceMapped(box(), 5, 0, 2);   // +Y: u = x, v = z
    const tools::ClipResult r = tools::clipBrush(b, Plane{{1, 0, 0}, 1.5});
    REQUIRE(r.has());
    bool seen = false;
    for (const BrushFace& f : r.brush.faces) {
        if (f.uvs.empty()) continue;
        seen = true;
        for (size_t k = 0; k < f.loop.size(); ++k) {
            const Vec3& p = r.brush.vertices[f.loop[k]];
            CHECK(f.uvs[k].u == doctest::Approx(p.x));
            CHECK(f.uvs[k].v == doctest::Approx(p.z));
        }
    }
    CHECK(seen);
}

TEST_CASE("clipBy: both halves add up; one side keeps one brush; planeFromPoints orients by the winding") {
    const tools::ClipPair both = tools::clipBy(box(), Plane{{0, 0, 1}, 1}, tools::Keep::Both);
    REQUIRE(both.back);
    REQUIRE(both.front);
    CHECK(both.back->signedVolume() + both.front->signedVolume() == doctest::Approx(64));
    CHECK(both.back->signedVolume() == doctest::Approx(16));
    const tools::ClipPair front = tools::clipBy(box(), Plane{{0, 0, 1}, 1}, tools::Keep::Front);
    CHECK_FALSE(front.back);
    CHECK(front.front);
    const auto pl = tools::planeFromPoints({0, 0, 0}, {1, 0, 0}, {0, 1, 0});
    REQUIRE(pl);
    CHECK(pl->n.z == doctest::Approx(1));
    CHECK_FALSE(tools::planeFromPoints({0, 0, 0}, {1, 0, 0}, {2, 0, 0}));
}

TEST_CASE("hollowBrush: a box becomes six walls whose volume is the shell; a wall as thick as the brush is refused; works on a wedge") {
    const auto walls = tools::hollowBrush(box(), 1);
    REQUIRE(walls);
    CHECK(walls->size() == 6);
    double vol = 0;
    for (const Brush& w : *walls) {
        vol += w.signedVolume();
        CHECK(tools::mesh::isProperSolid(w));
    }
    CHECK(vol == doctest::Approx(64 - 8));
    CHECK_FALSE(tools::hollowBrush(box(), 2));
    CHECK_FALSE(tools::hollowBrush(box(), 0));
    const Brush wedge = wedgeOf(4);
    const auto ww = tools::hollowBrush(wedge, 0.5);
    REQUIRE(ww);
    CHECK(ww->size() >= 4);
    double v2 = 0;
    for (const Brush& w : *ww) v2 += w.signedVolume();
    CHECK(v2 < wedge.signedVolume());
}

TEST_CASE("hullBrush: two touching boxes merge into one box; fewer than four points or a flat set are refused") {
    const Brush a = mkbox("a", {0, 0, 0}, {2, 2, 2}), c = mkbox("c", {2, 0, 0}, {4, 2, 2});
    std::vector<Vec3> pts = a.vertices;
    pts.insert(pts.end(), c.vertices.begin(), c.vertices.end());
    const auto h = tools::hullBrush(pts, "dev/grey");
    REQUIRE(h);
    CHECK(h->signedVolume() == doctest::Approx(16));
    CHECK(h->faces.size() == 6);
    CHECK(tools::mesh::isProperSolid(*h));
    CHECK_FALSE(tools::hullBrush({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}));
    CHECK_FALSE(tools::hullBrush({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}}));
}

TEST_CASE("snapBrush: snaps a box to the grid; a snap that would collapse the brush is refused") {
    const Brush b = mkbox("b", {0.13, 0.02, -0.1}, {3.9, 4.05, 4.2});
    const auto r = tools::snapBrush(b, 0.5);
    REQUIRE(r);
    for (const Vec3& v : r->vertices) {
        for (double x : {v.x, v.y, v.z}) CHECK(std::abs(x / 0.5 - std::round(x / 0.5)) < 1e-9);
    }
    CHECK_FALSE(tools::snapBrush(mkbox("b", {0, 0, 0}, {0.1, 4, 4}), 1));
}

TEST_CASE("unionVolume counts overlap once; isConvex") {
    CHECK(tools::unionVolume({mkbox("a", {0, 0, 0}, {2, 2, 2}), mkbox("b", {1, 0, 0}, {3, 2, 2})}) == doctest::Approx(12));
    CHECK(tools::unionVolume({mkbox("a", {0, 0, 0}, {2, 2, 2}), mkbox("b", {1, 0, 0}, {3, 2, 2}), mkbox("c", {1, 1, 0}, {2, 3, 2})}) == doctest::Approx(14));
    CHECK(tools::isConvex(box()));
    CHECK(tools::isConvex(wedgeOf(4)));
}

// ---- face drag ----

TEST_CASE("dragFace: moves one face of a box; the other planes keep their normals; shrinking and collapse") {
    const Brush b = mkbox("b", {0, 0, 0}, {2, 2, 2});   // faces: 0 -Z, 1 +Z, 2 -X, 3 +X, 4 -Y, 5 +Y
    const tools::DragResult r = tools::dragFace(b, 3, 3);
    REQUIRE(r.ok());
    CHECK(r.brush.signedVolume() == doctest::Approx(5 * 2 * 2));
    for (size_t i = 0; i < b.faces.size(); ++i) CHECK(b.faceNormal(b.faces[i]).dot(r.brush.faceNormal(r.brush.faces[i])) == doctest::Approx(1));
    int atFive = 0;
    for (const Vec3& v : r.brush.vertices)
        if (std::abs(v.x - 5) < 1e-9) ++atFive;
    CHECK(atFive == 4);
    const tools::DragResult shrunk = tools::dragFace(b, 1, -1);
    REQUIRE(shrunk.ok());
    CHECK(shrunk.brush.signedVolume() == doctest::Approx(2 * 2 * 1));
    CHECK_FALSE(tools::dragFace(b, 1, -2.5).ok());
    CHECK_FALSE(tools::dragFace(b, 1, -2).ok());
    CHECK_FALSE(tools::dragFace(b, 99, 1).ok());
}

TEST_CASE("dragFace: on a wedge the slanted face moves along its own normal") {
    Brush wedge;
    wedge.id = "w";
    wedge.vertices = {{0, 0, 0}, {2, 0, 0}, {2, 0, 2}, {0, 2, 0}, {2, 2, 0}, {2, 2, 2}};
    const std::vector<std::vector<uint32_t>> loops = {{0, 3, 4, 1}, {1, 4, 5, 2}, {0, 1, 2}, {3, 5, 4}, {0, 2, 5, 3}};
    for (uint32_t i = 0; i < loops.size(); ++i) {
        BrushFace f;
        f.id = i;
        f.loop = loops[i];
        wedge.faces.push_back(f);
    }
    REQUIRE(wedge.signedVolume() > 0);
    const Vec3 n0 = wedge.faceNormal(wedge.faces[4]);
    const tools::DragResult r = tools::dragFace(wedge, 4, 0.5);
    REQUIRE(r.ok());
    CHECK(n0.dot(r.brush.faceNormal(r.brush.faces[4])) == doctest::Approx(1));
    CHECK(n0.dot(r.brush.vertices[0]) - n0.dot(wedge.vertices[0]) == doctest::Approx(0.5));
    for (size_t i = 0; i < 4; ++i) CHECK(wedge.faceNormal(wedge.faces[i]).dot(r.brush.faceNormal(r.brush.faces[i])) == doctest::Approx(1));
}

TEST_CASE("dragFace: a face with a 3+ face corner (pyramid side) is dragged by re-intersecting the planes") {
    Brush pyr;
    pyr.id = "p";
    pyr.vertices = {{0, 0, 0}, {2, 0, 0}, {2, 2, 0}, {0, 2, 0}, {1, 1, 2}};
    const std::vector<std::vector<uint32_t>> loops = {{0, 3, 2, 1}, {0, 1, 4}, {1, 2, 4}, {2, 3, 4}, {3, 0, 4}};
    for (uint32_t i = 0; i < loops.size(); ++i) {
        BrushFace f;
        f.id = i;
        f.loop = loops[i];
        pyr.faces.push_back(f);
    }
    const Vec3 n0 = pyr.faceNormal(pyr.faces[1]);
    for (double dist : {0.2, -0.2}) {
        const tools::DragResult r = tools::dragFace(pyr, 1, dist);
        REQUIRE(r.ok());
        const BrushFace* f = nullptr;
        for (const BrushFace& x : r.brush.faces)
            if (x.id == 1) f = &x;
        REQUIRE(f);
        CHECK(r.brush.faceNormal(*f).dot(n0) == doctest::Approx(1));
        CHECK(n0.dot(r.brush.vertices[f->loop[0]]) - n0.dot(pyr.vertices[0]) == doctest::Approx(dist));
        CHECK(r.brush.signedVolume() > 0);
        for (uint32_t id : {0u, 2u, 3u, 4u}) {
            bool found = false;
            for (const BrushFace& x : r.brush.faces)
                if (x.id == id) found = true;
            CHECK(found);
        }
    }
    CHECK_FALSE(tools::dragFace(pyr, 1, -5).ok());
}

TEST_CASE("dragFace: the texture keeps its place on the neighbours (uv fit follows the new corners)") {
    const Brush b = faceMapped(mkbox("b", {0, 0, 0}, {2, 2, 2}), 5, 0, 2);
    const tools::DragResult r = tools::dragFace(b, 3, 1);
    REQUIRE(r.ok());
    const BrushFace& f = r.brush.faces[5];
    REQUIRE(f.uvs.size() == f.loop.size());
    for (size_t k = 0; k < f.loop.size(); ++k) {
        const Vec3& p = r.brush.vertices[f.loop[k]];
        CHECK(f.uvs[k].u == doctest::Approx(p.x));
        CHECK(f.uvs[k].v == doctest::Approx(p.z));
    }
}

TEST_CASE("dragFace: Alt-drag resizes in place: outward grows, inward shrinks, too far inward is refused") {
    const Brush b = Brush::makeBox("b", {0, 0, 0}, {2, 1, 1}, "dev/wall");
    uint32_t top = 0;
    for (const BrushFace& f : b.faces) {
        bool all = true;
        for (uint32_t v : f.loop)
            if (b.vertices[v].z != 1) all = false;
        if (all) top = f.id;
    }
    const tools::DragResult grown = tools::dragFace(b, top, 0.5), shrunk = tools::dragFace(b, top, -0.25);
    REQUIRE(grown.ok());
    REQUIRE(shrunk.ok());
    CHECK(grown.brush.id == "b");
    double gz = -1e300, sz = -1e300;
    for (const Vec3& v : grown.brush.vertices) gz = std::max(gz, v.z);
    for (const Vec3& v : shrunk.brush.vertices) sz = std::max(sz, v.z);
    CHECK(gz == doctest::Approx(1.5));
    CHECK(sz == doctest::Approx(0.75));
    CHECK_FALSE(tools::dragFace(b, top, -1.5).ok());
}

TEST_CASE("extendFace: grows a new convex box out of the face and leaves the source alone") {
    Brush b = mkbox("b", {0, 0, 0}, {2, 2, 2});
    const int axes[6][2] = {{0, 1}, {0, 1}, {1, 2}, {1, 2}, {0, 2}, {0, 2}};
    for (size_t i = 0; i < 6; ++i) b = faceMapped(b, i, axes[i][0], axes[i][1]);
    const Brush before = b;
    const tools::DragResult r = tools::extendFace(b, 3, 3, "new");   // +X face
    REQUIRE(r.ok());
    CHECK(b.vertices.size() == before.vertices.size());
    CHECK(b.faces.size() == before.faces.size());
    CHECK(r.brush.id == "new");
    CHECK(r.brush.op == core::BrushOp::Add);
    CHECK(r.brush.signedVolume() == doctest::Approx(3 * 2 * 2));
    CHECK_FALSE(r.brush.checkConvexSolid());
    CHECK(tools::isConvex(r.brush));
    CHECK(tools::mesh::isProperSolid(r.brush));
    CHECK(maxX(r.brush) == doctest::Approx(5));
    CHECK(r.brush.faces.size() == 6);
    // face 0 is the end cap: it faces the same way as the dragged face
    for (const BrushFace& f : r.brush.faces)
        if (f.id == 0) CHECK(r.brush.faceNormal(f).dot({1, 0, 0}) == doctest::Approx(1));
    // the side walls continue the mapping of the faces they grow from: no seam
    for (const BrushFace& f : r.brush.faces) {
        if (f.id < 2) continue;
        REQUIRE(f.uvs.size() == f.loop.size());
        const Vec3 p0 = r.brush.vertices[f.loop[0]];
        bool flatY = true, flatZ = true;
        for (uint32_t vi : f.loop) {
            if (std::abs(r.brush.vertices[vi].y - p0.y) > 1e-9) flatY = false;
            if (std::abs(r.brush.vertices[vi].z - p0.z) > 1e-9) flatZ = false;
        }
        CHECK((flatY || flatZ));
        for (size_t k = 0; k < f.loop.size(); ++k) {
            const Vec3& p = r.brush.vertices[f.loop[k]];
            CHECK(f.uvs[k].u == doctest::Approx(p.x));
            CHECK(f.uvs[k].v == doctest::Approx(flatZ ? p.y : p.z));
        }
    }
}

TEST_CASE("extendFace: keeps op and carve scope; refuses zero, negative and unknown") {
    Brush b = mkbox("b", {0, 0, 0}, {2, 2, 2});
    b.op = core::BrushOp::Subtract;
    const tools::DragResult r = tools::extendFace(b, 1, 1, "new");
    REQUIRE(r.ok());
    CHECK(r.brush.op == core::BrushOp::Subtract);
    CHECK_FALSE(tools::extendFace(b, 3, 0, "n").ok());
    CHECK_FALSE(tools::extendFace(b, 3, -1, "n").ok());
    CHECK_FALSE(tools::extendFace(b, 99, 1, "n").ok());
}

// ---- 2D resize ----

namespace {

struct Axes {
    tools::ScreenAxis r, u;
};
Axes axesOf(render::OrthoAxis a) {
    render::OrthoCamera cam(a);
    const render::CameraBasis b = cam.basis();
    return {tools::screenAxis({b.right.x, b.right.y, b.right.z}), tools::screenAxis({b.up.x, b.up.y, b.up.z})};
}
void near3(const tools::V3& a, const tools::V3& b) {
    for (size_t i = 0; i < 3; ++i) CHECK(a[i] == doctest::Approx(b[i]).epsilon(1e-9));
}
const tools::V3 kMin{0, 0, 0}, kMax{4, 2, 6};

} // namespace

TEST_CASE("resize2d: screen axes of the three views") {
    const Axes top = axesOf(render::OrthoAxis::Top), front = axesOf(render::OrthoAxis::Front), side = axesOf(render::OrthoAxis::Side);
    CHECK((top.r.axis == 0 && top.r.sign == 1 && top.u.axis == 2 && top.u.sign == -1));
    CHECK((front.r.axis == 0 && front.r.sign == 1 && front.u.axis == 1 && front.u.sign == 1));
    CHECK((side.r.axis == 2 && side.r.sign == -1 && side.u.axis == 1 && side.u.sign == 1));
}

TEST_CASE("resize2d: eight handles on the rectangle; in the top view up = -Z so the top side is the min-Z side") {
    const Axes a = axesOf(render::OrthoAxis::Top);
    const auto hs = tools::handles2D(kMin, kMax, a.r, a.u);
    CHECK(hs.size() == 8);
    const auto find = [&](const char* id) { return std::find_if(hs.begin(), hs.end(), [&](const tools::Handle2D& h) { return h.id == id; })->pos; };
    near3(find("1,1"), {4, 1, 0});
    near3(find("-1,-1"), {0, 1, 6});
    near3(find("0,1"), {2, 1, 0});
}

TEST_CASE("resize2d: the right handle stretches along X from the fixed left side (snapped); a corner resizes two axes") {
    const Axes f = axesOf(render::OrthoAxis::Front);
    const tools::Affine m = tools::resizeMap(kMin, kMax, f.r, f.u, 1, 0, 1.1, 0, 0.5);
    near3(m({4, 1, 3}), {5, 1, 3});
    near3(m({2, 1, 3}), {2.5, 1, 3});
    near3(m({0, 1, 3}), {0, 1, 3});
    const Axes t = axesOf(render::OrthoAxis::Top);
    const tools::Affine c = tools::resizeMap(kMin, kMax, t.r, t.u, 1, 1, 0, 1, 1);   // top-right corner up by 1 m: the min-Z side moves to -1
    near3(c({4, 0, 0}), {4, 0, -1});
    near3(c({0, 0, 6}), {0, 0, 6});
    near3(c({2, 5, 3}), {2, 5, 2.5});
}

TEST_CASE("resize2d: the dragged side stops at minSize; the side view maps screen right to -Z; pixel deltas flip y") {
    const Axes f = axesOf(render::OrthoAxis::Front);
    const tools::Affine m = tools::resizeMap(kMin, kMax, f.r, f.u, -1, 0, 10, 0, 0, 0.25);
    near3(m({0, 0, 0}), {3.75, 0, 0});
    near3(m({4, 0, 0}), {4, 0, 0});
    const Axes s = axesOf(render::OrthoAxis::Side);
    const tools::Affine n = tools::resizeMap(kMin, kMax, s.r, s.u, 1, 0, 2, 0, 0);
    near3(n({0, 0, 0}), {0, 0, -2});
    near3(n({0, 0, 6}), {0, 0, 6});
    const auto px = tools::pixelsToMetres(10, 20, 0.1);
    CHECK(px[0] == doctest::Approx(1));
    CHECK(px[1] == doctest::Approx(-2));
}

// ---- select inside / touching ----

TEST_CASE("brushInside / brushesTouch") {
    const Brush big = mkbox("big", {0, 0, 0}, {10, 10, 10});
    CHECK(tools::brushInside(big, mkbox("a", {1, 1, 1}, {2, 2, 2})));
    CHECK(tools::brushInside(big, mkbox("b", {0, 0, 0}, {10, 10, 10})));
    CHECK_FALSE(tools::brushInside(big, mkbox("c", {9, 9, 9}, {11, 11, 11})));
    CHECK_FALSE(tools::brushInside(big, mkbox("d", {20, 0, 0}, {21, 1, 1})));
    CHECK(tools::brushesTouch(big, mkbox("o", {5, 5, 5}, {15, 15, 15})));
    CHECK(tools::brushesTouch(big, mkbox("f", {10, 0, 0}, {12, 3, 3})));
    CHECK(tools::brushesTouch(big, mkbox("e", {10, 10, 0}, {12, 12, 3})));
    CHECK(tools::brushesTouch(big, mkbox("k", {10, 10, 10}, {12, 12, 12})));
    CHECK_FALSE(tools::brushesTouch(big, mkbox("g", {10.01, 0, 0}, {12, 3, 3})));
    CHECK_FALSE(tools::brushesTouch(big, mkbox("n", {-3, -3, -3}, {-1, -1, -1})));
    // the real shape counts, not the bounding box: the cut-away corner of a wedge is free
    const Brush wedge = wedgeOf(10);
    CHECK_FALSE(tools::brushesTouch(wedge, mkbox("corner", {9, 9, 4}, {10, 10, 6})));
    CHECK(tools::brushesTouch(wedge, mkbox("on", {4, 4, 4}, {5, 5, 6})));
}

// ---- clipper points ----

TEST_CASE("clipper points: ray / plane, snapping, picking, clashes, depth axis") {
    const auto p = tools::rayPlaneHit({{1, 5, 2}, {0, -1, 0}}, {0, 1, 0}, {0, 1, 0});
    REQUIRE(p);
    CHECK(p->x == doctest::Approx(1));
    CHECK(p->y == doctest::Approx(1));
    CHECK(p->z == doctest::Approx(2));
    CHECK_FALSE(tools::rayPlaneHit({{0, 5, 0}, {1, 0, 0}}, {0, 1, 0}, {0, 1, 0}));
    const Vec3 s = tools::snapPoint({1.26, 3.3, 0.74}, 0.5);
    CHECK((s.x == 1.5 && s.y == 3.5 && s.z == 0.5));
    const Vec3 k = tools::snapPoint({1.26, 3.3, 0.74}, 0.5, 1);
    CHECK((k.x == 1.5 && k.y == 3.3 && k.z == 0.5));
    const Vec3 free = tools::snapPoint({1.26, 3.3, 0.74}, 0);
    CHECK((free.x == 1.26 && free.y == 3.3 && free.z == 0.74));
    const auto project = [](const Vec3& q) { return std::optional<std::pair<double, double>>{{q.x * 10, q.z * 10}}; };
    const std::vector<Vec3> pts = {{1, 0, 1}, {1.5, 0, 1}};
    CHECK(tools::hitClipPoint(pts, project, 14, 10) == std::optional<size_t>(1));
    CHECK(tools::hitClipPoint(pts, project, 12, 10) == std::optional<size_t>(0));
    CHECK_FALSE(tools::hitClipPoint(pts, project, 200, 200));
    const std::vector<Vec3> two = {{0, 0, 0}, {1, 0, 0}};
    CHECK(tools::clashes(two, 0, {1, 0, 0}));
    CHECK_FALSE(tools::clashes(two, 1, {1, 0, 0}));
    CHECK(tools::depthAxis(Vec3{0, -1, 0}) == 1);
    CHECK(tools::depthAxis(Vec3{-1, 0, 0}) == 0);
    CHECK(tools::depthAxis(std::nullopt) == -1);
}
