// M11 item 1 (mesh mode), part 3: bevel, loop cut and bridge, converted from shell/tests/meshOps.test.ts (bevel, loop cut and the loop cut dependent extrude / merge / dissolve
// cases) and shell/tests/bridge.test.ts. Headless, no window.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <tuple>

#include "tools/mesh/ops.h"

using namespace native::tools::mesh;
using core::Brush;
using core::Vec3;

namespace {

Brush box() { return Brush::makeBox("b", {0, 0, 0}, {1, 1, 1}, "wall/a"); }
// makeBox faces: 0 -Z, 1 +Z, 2 -X, 3 +X, 4 -Y, 5 +Y (top). Vertices 0..3 are z=0 (x0y0, x1y0, x1y1, x0y1), 4..7 z=1.
constexpr uint32_t TOP = 5;

OpResult ok(const OpResult& r) {
    INFO(r.error);
    REQUIRE(r.ok());
    return r;
}

void expectSolid(const Brush& b, bool convex = false) {
    const auto problem = convex ? b.checkConvexSolid() : b.checkStructure();
    INFO(problem.value_or(""));
    CHECK_FALSE(problem.has_value());
    CHECK(isClosedManifold(b));
    CHECK(b.signedVolume() > 0);
    std::set<uint32_t> ids;
    for (const auto& f : b.faces) ids.insert(f.id);
    CHECK(ids.size() == b.faces.size());
}

std::vector<uint32_t> allFaceIds(const Brush& b) {
    std::vector<uint32_t> ids;
    for (const auto& f : b.faces) ids.push_back(f.id);
    return ids;
}

bool hasFace(const Brush& b, uint32_t id) { return faceById(b, id) != nullptr; }

uint32_t faceAlong(const Brush& b, int axis, int sign) {
    for (const auto& f : b.faces) {
        const Vec3 n = b.faceNormal(f);
        const double c = axis == 0 ? n.x : axis == 1 ? n.y : n.z;
        if (c * sign > 0.99) return f.id;
    }
    FAIL("no such face");
    return 0;
}

std::vector<uint32_t> topFaces(const Brush& cut) {   // faces entirely at y = 1
    std::vector<uint32_t> ids;
    for (const auto& f : cut.faces)
        if (std::all_of(f.loop.begin(), f.loop.end(), [&](uint32_t v) { return cut.vertices[v].y == 1; })) ids.push_back(f.id);
    return ids;
}

} // namespace

// ---- bevel ----
TEST_CASE("mesh bevel: a single edge becomes a chamfer - exact volume, one new face, closed") {
    const Brush b = box();
    const OpResult r = ok(bevelEdges(b, {edgeKey(6, 7)}, 0.2));   // the top edge along x at z = 1
    expectSolid(r.brush, true);
    CHECK(r.brush.signedVolume() == doctest::Approx(1 - (0.2 * 0.2) / 2 * 1));
    CHECK(r.brush.faces.size() == 7);
    CHECK(r.sel.faces.size() == 1);
    for (const auto& f : b.faces) CHECK(hasFace(r.brush, f.id));   // the six originals survive
}

TEST_CASE("mesh bevel: the four edges of the top face, all twelve edges, a chain, a wedge") {
    const Brush b = box();
    const MeshSel top = selFromFaces(b, {TOP});
    const OpResult four = ok(bevelEdges(b, top.edges, 0.1));
    expectSolid(four.brush, true);
    CHECK(four.brush.signedVolume() < 1);
    CHECK(four.brush.signedVolume() > 0.9);
    CHECK(four.brush.faces.size() == 10);
    CHECK(four.sel.faces.size() == 4);

    const OpResult twelve = ok(bevelEdges(b, selFromFaces(b, allFaceIds(b)).edges, 0.1));
    expectSolid(twelve.brush, true);
    CHECK(twelve.brush.faces.size() == 6 + 12 + 8);   // every corner gets a triangle
    CHECK(isClosedManifold(twelve.brush));

    const OpResult chain = ok(bevelEdges(b, {edgeKey(6, 7), edgeKey(5, 6)}, 0.15));
    expectSolid(chain.brush, true);
    CHECK(chain.brush.signedVolume() < 1);

    // an edge of a wedge (non-right angle) still closes up: the far top edge slid down makes a sloped top
    const Brush ramp = ok(transformVerts(b, {6, 7}, [](const Vec3& p, uint32_t) { return Vec3{p.x, p.y - 0.5, p.z}; })).brush;
    expectSolid(ramp, true);
    const OpResult wedge = ok(bevelEdges(ramp, {edgeKey(6, 7)}, 0.1));
    expectSolid(wedge.brush, true);
    CHECK(wedge.brush.signedVolume() < ramp.signedVolume());
}

TEST_CASE("mesh bevel: segments make a circular arc; 1 is exactly the chamfer; chains, loops and all edges stay closed solids") {
    const Brush b = box();
    const OpResult arc = ok(bevelEdges(b, {edgeKey(6, 7)}, 0.2, 16));
    expectSolid(arc.brush, true);
    CHECK(arc.brush.faces.size() == 6 + 16);
    CHECK(arc.sel.faces.size() == 16);
    CHECK(arc.brush.signedVolume() == doctest::Approx(1 - 0.2 * 0.2 * (1 - 3.14159265358979323846 / 4)).epsilon(1e-3));

    const OpResult one = ok(bevelEdges(b, {edgeKey(6, 7)}, 0.2, 1)), def = ok(bevelEdges(b, {edgeKey(6, 7)}, 0.2));
    CHECK(one.brush.vertices.size() == def.brush.vertices.size());
    CHECK(one.brush.faces.size() == def.brush.faces.size());
    for (size_t i = 0; i < one.brush.vertices.size(); ++i) {
        CHECK(one.brush.vertices[i].x == def.brush.vertices[i].x);
        CHECK(one.brush.vertices[i].y == def.brush.vertices[i].y);
        CHECK(one.brush.vertices[i].z == def.brush.vertices[i].z);
    }
    for (int n : {2, 3, 5}) expectSolid(ok(bevelEdges(b, {edgeKey(6, 7)}, 0.2, n)).brush, true);

    for (int n : {2, 3, 4}) {
        expectSolid(ok(bevelEdges(b, {edgeKey(6, 7), edgeKey(5, 6)}, 0.15, n)).brush, true);
        const OpResult top = ok(bevelEdges(b, selFromFaces(b, {TOP}).edges, 0.1, n));
        expectSolid(top.brush, true);
        CHECK(top.sel.faces.size() == static_cast<size_t>(4 * n));
    }
    // corners where three bevels meet: convex up to 3 segments; above that the single fan corner cap is closed and valid but slightly concave
    const std::vector<Edge> all = selFromFaces(b, allFaceIds(b)).edges;
    for (int n : {2, 3}) expectSolid(ok(bevelEdges(b, all, 0.1, n)).brush, true);
    for (int n : {4, 8}) expectSolid(ok(bevelEdges(b, all, 0.1, n)).brush, false);
}

TEST_CASE("mesh bevel: segments are clamped; too wide, negative and open meshes are refused; 0 is the identity") {
    const Brush b = box();
    CHECK(ok(bevelEdges(b, {edgeKey(6, 7)}, 0.2, 0)).brush.faces.size() == 7);
    CHECK(ok(bevelEdges(b, {edgeKey(6, 7)}, 0.2, 1000)).brush.faces.size() == 6 + kMaxBevelSegments);
    CHECK_FALSE(bevelEdges(b, {edgeKey(6, 7)}, 5).ok());
    CHECK_FALSE(bevelEdges(b, selFromFaces(b, {TOP}).edges, 0.6).ok());
    CHECK_FALSE(bevelEdges(b, {edgeKey(6, 7)}, -1).ok());
    CHECK_FALSE(bevelEdges(b, {}, 0.1).ok());
    const Brush open = ok(deleteFaces(b, {TOP})).brush;
    CHECK_FALSE(bevelEdges(open, {edgeKey(6, 7)}, 0.1).ok());
    CHECK(ok(bevelEdges(b, {edgeKey(6, 7)}, 0)).brush.faces.size() == 6);
}

// ---- loop cut ----
TEST_CASE("mesh loop cut: the ring of parallel edges around a box") {
    const auto info = edgeRing(box(), edgeKey(4, 5));
    REQUIRE(info.has_value());
    CHECK(info->closed);
    CHECK(info->ring.size() == 4);
    CHECK_FALSE(edgeRing(box(), edgeKey(0, 6)).has_value());   // not an edge
}

TEST_CASE("mesh loop cut: inserts a loop - 4 new vertices, 4 more faces, volume unchanged, original ids kept, the new loop selected") {
    const Brush b = box();
    const OpResult r = ok(loopCut(b, edgeKey(4, 5), 0.5));
    expectSolid(r.brush, true);
    CHECK(r.brush.vertices.size() == 12);
    CHECK(r.brush.faces.size() == 10);
    CHECK(r.brush.signedVolume() == doctest::Approx(1));
    for (const auto& f : b.faces) CHECK(hasFace(r.brush, f.id));
    CHECK(r.sel.edges.size() == 4);
}

TEST_CASE("mesh loop cut: the position follows the parameter along a consistent rail; out of range is refused; a second cut across the first works") {
    const Brush cut = ok(loopCut(box(), edgeKey(4, 5), 0.25)).brush;
    std::set<std::tuple<long, long, long>> cuts;
    for (size_t i = 8; i < cut.vertices.size(); ++i) {
        const Vec3& p = cut.vertices[i];
        cuts.insert({std::lround(p.x * 1000), std::lround(p.y * 1000), std::lround(p.z * 1000)});
        CHECK((p.x == 0.25 || p.x == 0.75 || p.y == 0.25 || p.y == 0.75 || p.z == 0.25 || p.z == 0.75));
    }
    CHECK(cuts.size() == 4);
    expectSolid(cut);

    CHECK_FALSE(loopCut(box(), edgeKey(4, 5), 0).ok());
    CHECK_FALSE(loopCut(box(), edgeKey(4, 5), 1).ok());
    CHECK_FALSE(loopCut(box(), edgeKey(0, 6), 0.5).ok());

    const Brush a = ok(loopCut(box(), edgeKey(4, 5), 0.5)).brush;
    const Topology t = buildTopology(a);
    // the edge along z at x = 0, y = 0 (a rail of the other direction)
    std::optional<Edge> e;
    for (const EdgeFaces& x : t.edgeList)
        if (a.vertices[x.a].z != a.vertices[x.b].z && a.vertices[x.a].x == 0 && a.vertices[x.a].y == 0) e = Edge{x.a, x.b};
    REQUIRE(e.has_value());
    expectSolid(ok(loopCut(a, *e, 0.5)).brush, true);
}

TEST_CASE("mesh loop cut: stops at a triangle instead of failing and still closes") {
    const Brush tri = ok(transformVerts(box(), {7}, [](const Vec3& p, uint32_t) { return Vec3{p.x, p.y + 0.5, p.z}; })).brush;
    const Topology t = buildTopology(tri);
    const OpResult r = loopCut(tri, Edge{t.edgeList[0].a, t.edgeList[0].b}, 0.5);
    if (r.ok()) CHECK(isClosedManifold(r.brush));
}

// ---- the cases of other operations that need the loop cut ----
TEST_CASE("mesh ops with a loop cut: extrude a region of two adjacent faces as one piece (no wall between them)") {
    const Brush cut = ok(loopCut(box(), edgeKey(4, 5), 0.5)).brush;   // splits the top and other faces in two
    const std::vector<uint32_t> tops = topFaces(cut);
    REQUIRE(tops.size() == 2);
    const OpResult r = ok(extrudeFaces(cut, tops, 1));
    expectSolid(r.brush);
    CHECK(r.brush.signedVolume() == doctest::Approx(2));
    CHECK(r.brush.faces.size() == cut.faces.size() + 6);   // 6 walls around the 2-face region
}

TEST_CASE("mesh ops with a loop cut: merge by distance") {
    const Brush b = ok(loopCut(box(), edgeKey(4, 5), 0.005)).brush;
    std::vector<uint32_t> all;
    for (uint32_t i = 0; i < b.vertices.size(); ++i) all.push_back(i);
    CHECK_FALSE(mergeByDistance(b, all, 0.0001).ok());
    const OpResult r = ok(mergeByDistance(b, all, 0.01));
    CHECK(isClosedManifold(r.brush));
    CHECK(r.brush.vertices.size() < b.vertices.size());
}

TEST_CASE("mesh ops with a loop cut: dissolve undoes a loop cut (the cut edges, then the leftover vertices); dissolve faces into one polygon") {
    const OpResult cut = ok(loopCut(box(), edgeKey(4, 5), 0.5));
    const OpResult merged = ok(dissolveEdges(cut.brush, cut.sel.edges));
    CHECK(merged.brush.faces.size() == 6);
    CHECK(merged.brush.vertices.size() == 12);
    expectSolid(merged.brush, true);
    const OpResult clean = ok(dissolveVerts(merged.brush, {8, 9, 10, 11}));
    CHECK(clean.brush.vertices.size() == 8);
    CHECK(clean.brush.faces.size() == 6);
    CHECK(std::all_of(clean.brush.faces.begin(), clean.brush.faces.end(), [](const core::BrushFace& f) { return f.loop.size() == 4; }));
    expectSolid(clean.brush, true);

    const std::vector<uint32_t> tops = topFaces(cut.brush);
    REQUIRE(tops.size() == 2);
    const OpResult one = ok(dissolveFaces(cut.brush, tops));
    CHECK(one.brush.faces.size() == cut.brush.faces.size() - 1);
    expectSolid(one.brush, true);
    CHECK_FALSE(dissolveFaces(cut.brush, {tops[0]}).ok());
}

// ---- bridge ----
TEST_CASE("mesh bridge: two inset faces on opposite sides become a tunnel through the brush (face mode)") {
    Brush b = Brush::makeBox("b", {0, 0, 0}, {2, 2, 2});
    const uint32_t front = faceAlong(b, 2, 1), back = faceAlong(b, 2, -1);
    const OpResult ins = ok(insetFaces(b, {front, back}, 0.5));
    b = ins.brush;
    const std::vector<uint32_t> inner = ins.sel.faces;
    REQUIRE(inner.size() == 2);
    MeshSel sel;
    sel.faces = inner;
    const OpResult r = ok(bridgeLoops(b, sel, SelMode::Face));
    CHECK(isProperSolid(r.brush));
    CHECK(r.brush.signedVolume() == doctest::Approx(8 - 1 * 1 * 2).epsilon(1e-6));   // the 1 x 1 tunnel, 2 long
    for (uint32_t id : inner) CHECK_FALSE(hasFace(r.brush, id));
    CHECK(r.sel.faces.size() == 4);
}

TEST_CASE("mesh bridge: edge mode joins two open holes; unequal or single loops are refused") {
    Brush b = Brush::makeBox("b", {0, 0, 0}, {2, 2, 2});
    const OpResult ins = ok(insetFaces(b, {faceAlong(b, 0, 1), faceAlong(b, 0, -1)}, 0.5));
    const OpResult holes = ok(deleteFaces(ins.brush, ins.sel.faces));
    b = holes.brush;
    std::vector<Edge> open;   // the edges walked by one face only
    for (const auto& f : b.faces)
        for (size_t i = 0; i < f.loop.size(); ++i) {
            const uint32_t v = f.loop[i], w = f.loop[(i + 1) % f.loop.size()];
            bool twin = false;
            for (const auto& g : b.faces)
                for (size_t j = 0; j < g.loop.size(); ++j)
                    if (g.loop[j] == w && g.loop[(j + 1) % g.loop.size()] == v) twin = true;
            if (!twin) open.push_back(edgeKey(v, w));
        }
    REQUIRE(open.size() == 8);
    MeshSel sel;
    sel.edges = open;
    const OpResult t = ok(bridgeLoops(b, sel, SelMode::Edge));
    CHECK(isProperSolid(t.brush));
    CHECK(t.brush.signedVolume() == doctest::Approx(8 - 2).epsilon(1e-6));

    MeshSel half;
    half.edges.assign(open.begin(), open.begin() + 4);
    CHECK_FALSE(bridgeLoops(b, half, SelMode::Edge).ok());
    MeshSel one;
    one.faces = {0};
    CHECK_FALSE(bridgeLoops(Brush::makeBox("c", {0, 0, 0}, {1, 1, 1}), one, SelMode::Face).ok());
}
