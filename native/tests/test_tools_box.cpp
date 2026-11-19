// Converted from shell/tests/tools.test.ts (box tool parts) and batch33.test.ts (shapes H4 / H5, smooth edges H6).
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

#include "tools/box_tool.h"
#include "tools/shapes.h"

using namespace native;
using tools::DrawPlane;
using tools::Rect;
using tools::Vec3;

namespace {

bool near(const Vec3& a, const Vec3& b, double eps = 1e-6) { return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps; }

struct Bounds {
    core::Vec3 lo, hi;
    double size(int axis) const { return axis == 0 ? hi.x - lo.x : (axis == 1 ? hi.y - lo.y : hi.z - lo.z); }
};
Bounds boundsOf(const core::Brush& b) {
    Bounds r;
    REQUIRE(b.bounds(r.lo, r.hi));
    return r;
}

} // namespace

TEST_CASE("box tool planes: world-axis basis on axis-aligned faces, orthonormal on tilted ones") {
    const DrawPlane p = tools::planeThrough({1, 2, 3}, {0, 1, 0});
    CHECK(near(p.u, {0, 0, 1}));
    CHECK(near(p.v, {1, 0, 0}));
    CHECK(near(p.normal, {0, 1, 0}));
    const DrawPlane t = tools::planeThrough({0, 0, 0}, {1, 1, 0});
    CHECK(render::dot(t.u, t.v) == doctest::Approx(0).epsilon(1e-9));
    CHECK(render::dot(t.u, t.normal) == doctest::Approx(0).epsilon(1e-9));
    CHECK(render::dot(t.v, t.normal) == doctest::Approx(0).epsilon(1e-9));
    CHECK(render::length(t.u) == doctest::Approx(1));
}

TEST_CASE("box tool: with no surface hit the ground plane, else a horizontal plane at a set distance") {
    const tools::DrawPlane down = tools::fallbackPlane({{0, 5, 0}, {0, -std::sqrt(0.5), std::sqrt(0.5)}});
    CHECK(down.origin.y == doctest::Approx(0));
    CHECK(down.origin.z == doctest::Approx(5));
    const tools::DrawPlane up = tools::fallbackPlane({{0, 5, 0}, {0, 0, 1}}, 10);
    CHECK(near(up.origin, {0, 5, 10}));
    CHECK(near(up.normal, {0, 1, 0}));
    const tools::DrawPlane far = tools::fallbackPlane({{0, 1, 0}, {0, -0.001, 1}}, 10);   // would hit the ground ~1000 m away
    CHECK(far.origin.z == doctest::Approx(10).epsilon(0.05));
}

TEST_CASE("box tool: rayPlane intersects in front of the ray only") {
    const DrawPlane p = tools::planeThrough({0, 0, 0}, {0, 1, 0});
    const auto hit = tools::rayPlane({{1, 4, 1}, {0, -1, 0}}, p);
    REQUIRE(hit);
    CHECK(near(*hit, {1, 0, 1}));
    CHECK_FALSE(tools::rayPlane({{1, 4, 1}, {0, 1, 0}}, p));
}

TEST_CASE("box tool: snapping touches the in-plane components only, anchored to the world grid") {
    const DrawPlane p = tools::planeThrough({0, 0, 0}, {0, 1, 0});
    CHECK(near(tools::snapToPlane({1.26, 3.3, -0.74}, p, 0.5), {1.5, 3.3, -0.5}));   // y (the plane's own axis) untouched
    CHECK(near(tools::snapToPlane({1.26, 3.3, -0.74}, p, 0), {1.26, 3.3, -0.74}));
    // the plane origin being the click point changes nothing
    const Vec3 hit{1.26, 3.3, -0.74};
    const DrawPlane q = tools::planeThrough(hit, {0, 1, 0});
    const Vec3 s = tools::snapToPlane(hit, q, 0.5);
    CHECK(s.x == doctest::Approx(1.5));
    CHECK(s.z == doctest::Approx(-0.5));
    CHECK(s.y == doctest::Approx(3.3));
    // a slanted surface: the start point lands on a grid line of the surface and stays on the plane
    const Vec3 n{0.6, 0.8, 0}, start{1.1, 2.3, 0.7};
    const DrawPlane sl = tools::planeThrough(start, n);
    const Vec3 t = tools::snapToPlane(start, sl, 0.5);
    const double a = render::dot(t, sl.u), b = render::dot(t, sl.v);
    CHECK(a / 0.5 == doctest::Approx(std::round(a / 0.5)).epsilon(1e-9));
    CHECK(b / 0.5 == doctest::Approx(std::round(b / 0.5)).epsilon(1e-9));
    CHECK(std::fabs(render::dot(t - start, n)) < 1e-9);
}

TEST_CASE("box tool drawing: a free rectangle then a height makes a valid convex box of that size") {
    const DrawPlane plane = tools::planeThrough({0, 0, 0}, {0, 1, 0});
    const Rect rect = tools::rectFromDrag(plane, {0, 0, 0}, {2, 0, 3}, false);
    const auto b = tools::buildBrush("b", plane, rect, 1.5);
    REQUIRE(b);
    CHECK_FALSE(b->checkConvexSolid());
    const Bounds bb = boundsOf(*b);
    std::vector<double> sizes = {bb.size(0), bb.size(1), bb.size(2)};
    std::sort(sizes.begin(), sizes.end());
    CHECK(sizes[0] == doctest::Approx(1.5));
    CHECK(sizes[1] == doctest::Approx(2));
    CHECK(sizes[2] == doctest::Approx(3));
    CHECK(b->signedVolume() == doctest::Approx(9));
}

TEST_CASE("box tool drawing: Shift makes a square keeping the drag direction, then a cube; height snaps without Shift") {
    const DrawPlane plane = tools::planeThrough({0, 0, 0}, {0, 1, 0});
    const Rect sq = tools::rectFromDrag(plane, {0, 0, 0}, {-1, 0, 4}, true);
    CHECK(std::fabs(sq.du) == doctest::Approx(4));
    CHECK(std::fabs(sq.dv) == doctest::Approx(4));
    CHECK((sq.du < 0 ? -1 : 1) * (sq.dv < 0 ? -1 : 1) == -1);   // still drawn toward (-x, +z)
    const Rect r = tools::rectFromDrag(plane, {0, 0, 0}, {2, 0, 1}, true);
    CHECK(tools::heightFor(0.3, r, true, 0.5) == 2);
    CHECK(tools::heightFor(-7, r, true, 0.5) == -2);
    const auto cube = tools::buildBrush("c", plane, r, tools::heightFor(1, r, true, 0.5));
    REQUIRE(cube);
    const Bounds bb = boundsOf(*cube);
    for (int i = 0; i < 3; ++i) CHECK(bb.size(i) == doctest::Approx(2));
    const Rect free = tools::rectFromDrag(plane, {0, 0, 0}, {1, 0, 1}, false);
    CHECK(tools::heightFor(1.37, free, false, 0.5) == 1.5);
    CHECK(tools::heightFor(1.37, free, false, 0) == 1.37);
}

TEST_CASE("box tool drawing: extruding downward or drawing backwards still gives an outward-wound solid") {
    const DrawPlane plane = tools::planeThrough({0, 0, 0}, {0, 1, 0});
    const double cases[3][3] = {{2, 2, -1}, {-2, 2, 1}, {-2, -3, -1}};
    for (const auto& c : cases) {
        const Rect rect = tools::rectFromDrag(plane, {0, 0, 0}, {c[0], 0, c[1]}, false);
        const auto b = tools::buildBrush("b", plane, rect, c[2]);
        REQUIRE(b);
        CHECK_FALSE(b->checkConvexSolid());
    }
}

TEST_CASE("box tool drawing: on a wall the box grows along the wall normal") {
    const DrawPlane wall = tools::planeThrough({0, 1, 5}, {0, 0, -1});
    const auto b = tools::buildBrush("w", wall, tools::rectFromDrag(wall, {0, 1, 5}, {1, 2, 5}, false), 0.5);
    REQUIRE(b);
    CHECK_FALSE(b->checkConvexSolid());
    const Bounds bb = boundsOf(*b);
    CHECK(bb.hi.z == doctest::Approx(5));
    CHECK(bb.lo.z == doctest::Approx(4.5));
}

TEST_CASE("box tool drawing: degenerate sizes produce no brush; the preview shows the flat rectangle first") {
    const DrawPlane plane = tools::planeThrough({0, 0, 0}, {0, 1, 0});
    CHECK_FALSE(tools::buildBrush("z", plane, {{0, 0, 0}, 0, 2}, 1));
    CHECK_FALSE(tools::buildBrush("z", plane, {{0, 0, 0}, 1, 2}, 0));
    CHECK(tools::previewLines(plane, {{0, 0, 0}, 1, 2}, std::nullopt).size() == 4 * 6);
    CHECK(tools::previewLines(plane, {{0, 0, 0}, 1, 2}, 1.0).size() == 12 * 6);
}

TEST_CASE("box tool: ids are fresh and carry the prefix") {
    const std::string a = tools::newId("b"), b = tools::newId("b");
    CHECK(a.rfind("b_", 0) == 0);
    CHECK(a.size() == 10);
    CHECK(a != b);
}

namespace {
const DrawPlane kGround = tools::planeThrough({0, 0, 0}, {0, 1, 0});
const Rect kRect{{0, 0, 0}, 4, 2};
} // namespace

TEST_CASE("shapes: the preview draws the shape itself, stairs have more edges than their box, every edge once") {
    tools::ShapeParams p;
    p.steps = 4;
    const auto lines = tools::shapePreviewLines(tools::ShapeId::Stairs, kGround, kRect, 3, p);
    CHECK(lines.size() / 6 > 12);
    std::set<std::string> keys;
    for (size_t i = 0; i + 5 < lines.size(); i += 6) {
        char a[80], b[80];
        std::snprintf(a, sizeof a, "%.4f,%.4f,%.4f", lines[i], lines[i + 1], lines[i + 2]);
        std::snprintf(b, sizeof b, "%.4f,%.4f,%.4f", lines[i + 3], lines[i + 4], lines[i + 5]);
        keys.insert(std::string(a) < std::string(b) ? std::string(a) + b : std::string(b) + a);
    }
    CHECK(keys.size() == lines.size() / 6);
}

TEST_CASE("shapes: the sphere is one valid convex brush that fills its box, all its edges smooth") {
    tools::ShapeParams p;
    p.segments = 8;
    p.rings = 4;
    const auto r = tools::buildShape(tools::ShapeId::Sphere, kGround, kRect, 3, "dev/grey", p, [] { return std::string("s1"); });
    REQUIRE(r.ok());
    REQUIRE(r.brushes.size() == 1);
    const core::Brush& b = r.brushes[0];
    CHECK_FALSE(b.checkConvexSolid());
    CHECK(b.faces.size() == 8 * 4);
    double lo = 1e9, hi = -1e9;
    for (const auto& v : b.vertices) {
        lo = std::min(lo, v.y);
        hi = std::max(hi, v.y);
    }
    CHECK(lo == doctest::Approx(0));
    CHECK(hi == doctest::Approx(3));
    CHECK(b.smoothEdges.size() == 56);   // every edge of the sphere (V - E + F = 2: 26 - 56 + 32)
}

TEST_CASE("shapes: a cylinder smooths its sides, not its caps; smooth off leaves it faceted") {
    tools::ShapeParams p;
    p.sides = 6;
    const auto r = tools::buildShape(tools::ShapeId::Cylinder, kGround, kRect, 3, "dev/grey", p, [] { return std::string("c"); });
    REQUIRE(r.ok());
    CHECK(r.brushes[0].smoothEdges.size() == 6);
    p.smooth = false;
    const auto flat = tools::buildShape(tools::ShapeId::Cylinder, kGround, kRect, 3, "dev/grey", p, [] { return std::string("c"); });
    REQUIRE(flat.ok());
    CHECK(flat.brushes[0].smoothEdges.empty());
}

TEST_CASE("shapes: every kind fills the drawn box and is valid, in every orientation") {
    int n = 0;
    const auto ids = [&n] { return "p" + std::to_string(n++); };
    const tools::ShapeId kinds[] = {tools::ShapeId::Box, tools::ShapeId::Cylinder, tools::ShapeId::Wedge, tools::ShapeId::Arch, tools::ShapeId::Stairs, tools::ShapeId::Sphere};
    for (const tools::ShapeId k : kinds)
        for (int turn = 0; turn < 4; ++turn) {
            tools::ShapeParams p;
            const auto r = tools::buildShape(k, kGround, {{1, 0, 1}, 4, 2}, 3, "dev/grey", p, ids, turn);
            INFO(tools::shapeName(k) << " turn " << turn << ": " << r.error);
            REQUIRE(r.ok());
            core::Vec3 lo{1e9, 1e9, 1e9}, hi{-1e9, -1e9, -1e9};
            for (const core::Brush& b : r.brushes) {
                CHECK_FALSE(b.checkConvexSolid());
                core::Vec3 a, c;
                REQUIRE(b.bounds(a, c));
                lo = {std::min(lo.x, a.x), std::min(lo.y, a.y), std::min(lo.z, a.z)};
                hi = {std::max(hi.x, c.x), std::max(hi.y, c.y), std::max(hi.z, c.z)};
            }
            // the drawn box: x 1..3 (dv = 2 along x), z 1..5 (du = 4 along z), y 0..3
            CHECK(lo.x == doctest::Approx(1).epsilon(1e-6));
            CHECK(hi.x == doctest::Approx(3).epsilon(1e-6));
            CHECK(lo.z == doctest::Approx(1).epsilon(1e-6));
            CHECK(hi.z == doctest::Approx(5).epsilon(1e-6));
            CHECK(lo.y == doctest::Approx(0).epsilon(1e-6));
            CHECK(hi.y == doctest::Approx(3).epsilon(1e-6));
        }
}

TEST_CASE("shapes: too thin or an impossible arch is refused with a reason") {
    tools::ShapeParams p;
    const auto thin = tools::buildShape(tools::ShapeId::Box, kGround, {{0, 0, 0}, 0.00001, 2}, 3, "", p, [] { return std::string("x"); });
    CHECK_FALSE(thin.ok());
    p.archThickness = 5;
    const auto arch = tools::buildShape(tools::ShapeId::Arch, kGround, kRect, 3, "", p, [] { return std::string("x"); });
    CHECK_FALSE(arch.ok());
    CHECK(arch.brushes.empty());
}
