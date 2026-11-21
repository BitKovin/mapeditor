// M11 item 1 (mesh mode), part 4b: loop / ring / path selection, what a click selects (shell/tests/meshSelect.test.ts) and the screen-space picking of tools/meshPick.ts. Headless.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include "tools/mesh/ops.h"
#include "tools/mesh/pick.h"
#include "tools/mesh/select.h"

using namespace native::tools::mesh;
using core::Brush;
using core::Vec3;

namespace {

Brush box() { return Brush::makeBox("b", {0, 0, 0}, {1, 1, 1}, "wall/a"); }
// makeBox: vertices 0..3 at z=0 (x0y0, x1y0, x1y1, x0y1), 4..7 at z=1; vertical edges 0-4, 1-5, 2-6, 3-7; faces 0 -Z, 1 +Z.

OpResult ok(const OpResult& r) {
    INFO(r.error);
    REQUIRE(r.ok());
    return r;
}

// A box cut once around its middle (z = 0.5): the cut's vertices have four edges, so loops run through them.
OpResult cutBox() { return ok(loopCut(box(), edgeKey(0, 4), 0.5)); }

uint32_t midVertex(const Brush& b) {
    for (size_t i = 0; i < b.vertices.size(); ++i)
        if (b.vertices[i].x == 0 && b.vertices[i].y == 0 && std::abs(b.vertices[i].z - 0.5) < 1e-9) return static_cast<uint32_t>(i);
    FAIL("no mid vertex");
    return 0;
}

bool shares(const Edge& a, const Edge& b) { return a.a == b.a || a.a == b.b || a.b == b.a || a.b == b.b; }
bool isChain(const std::vector<Edge>& e) {
    for (size_t i = 1; i < e.size(); ++i)
        if (!shares(e[i], e[i - 1])) return false;
    return true;
}
std::set<Edge> setOf(const std::vector<Edge>& e) { return {e.begin(), e.end()}; }
std::set<uint32_t> setOf(const std::vector<uint32_t>& e) { return {e.begin(), e.end()}; }

MeshSel emptySel() { return {}; }
const ClickMods plain{};

} // namespace

TEST_CASE("mesh select: edge loop (Alt+click)") {
    SUBCASE("stops at once on a box: every corner has three edges") {
        const auto l = edgeLoop(box(), edgeKey(0, 4));
        REQUIRE(l.size() == 1);
        CHECK(l[0] == edgeKey(0, 4));
    }
    SUBCASE("runs all the way round through the vertices of a loop cut") {
        const OpResult c = cutBox();
        REQUIRE(c.sel.edges.size() == 4);
        const auto loop = edgeLoop(c.brush, c.sel.edges[0]);
        CHECK(setOf(loop) == setOf(c.sel.edges));
        CHECK(loop.size() == 4);
        CHECK(isChain(loop));
    }
    SUBCASE("a vertical half edge continues through the cut vertex and stops at the box corners") {
        const OpResult c = cutBox();
        const uint32_t m = midVertex(c.brush);
        const auto loop = edgeLoop(c.brush, edgeKey(0, m));
        CHECK(setOf(loop) == std::set<Edge>{edgeKey(0, m), edgeKey(m, 4)});
    }
    SUBCASE("follows an open border") {
        const Brush open = ok(deleteFaces(box(), {1})).brush;   // no +Z face: the top rim is a border
        const auto rim = edgeLoop(open, edgeKey(4, 5));
        CHECK(rim.size() == 4);
        CHECK(isChain(rim));
        for (const Edge& e : rim) {
            CHECK(e.a >= 4);
            CHECK(e.b >= 4);
        }
    }
    SUBCASE("an unknown edge selects nothing") { CHECK(edgeLoop(box(), edgeKey(0, 6)).empty()); }
}

TEST_CASE("mesh select: edge ring (Ctrl+Alt+click) and face loop") {
    SUBCASE("ring of a vertical box edge = the four vertical edges") {
        CHECK(setOf(edgeRingEdges(box(), edgeKey(0, 4))) == std::set<Edge>{edgeKey(0, 4), edgeKey(1, 5), edgeKey(2, 6), edgeKey(3, 7)});
    }
    SUBCASE("face loop across a vertical edge of a lower quad = the four lower side quads") {
        const OpResult c = cutBox();
        const uint32_t m = midVertex(c.brush);
        const core::BrushFace* lowerQuad = nullptr;
        for (const auto& f : c.brush.faces)
            if (std::find(f.loop.begin(), f.loop.end(), 0u) != f.loop.end() && std::find(f.loop.begin(), f.loop.end(), m) != f.loop.end()) lowerQuad = &f;
        REQUIRE(lowerQuad);
        const auto loop = faceLoop(c.brush, lowerQuad->id, edgeKey(0, m));
        CHECK(loop.size() == 4);
        for (uint32_t id : loop)
            for (uint32_t v : faceById(c.brush, id)->loop) CHECK(c.brush.vertices[v].z <= 0.5 + 1e-9);
    }
    SUBCASE("face loop across the cut edge goes over the bottom and the top: six faces") {
        const OpResult c = cutBox();
        const Edge cut = c.sel.edges[0];
        const core::BrushFace* quad = nullptr;
        for (const auto& f : c.brush.faces)
            for (size_t i = 0; i < f.loop.size(); ++i)
                if (edgeKey(f.loop[i], f.loop[(i + 1) % f.loop.size()]) == cut) quad = &f;
        REQUIRE(quad);
        CHECK(faceLoop(c.brush, quad->id, cut).size() == 6);
    }
}

TEST_CASE("mesh select: shortest path (Ctrl+click)") {
    SUBCASE("vertex path between opposite corners of a box takes three edges") {
        const Brush b = box();
        const auto p = shortestVertexPath(b, 0, 6);
        REQUIRE(p.size() == 4);
        CHECK(p.front() == 0);
        CHECK(p.back() == 6);
        const Topology t = buildTopology(b);
        for (size_t i = 1; i < p.size(); ++i) CHECK(t.find(edgeKey(p[i - 1], p[i])) != nullptr);
    }
    SUBCASE("vertex path prefers the shorter way by length") {
        Brush b = box();
        for (auto& v : b.vertices) v.x *= 10;   // long in x
        const auto p = shortestVertexPath(b, 0, 7);   // 0 = (0,0,0), 7 = (0,1,1): stay at x = 0
        for (uint32_t v : p) CHECK(b.vertices[v].x == 0);
    }
    SUBCASE("edge path joins two edges with a connected chain") {
        const auto p = shortestEdgePath(box(), edgeKey(0, 1), edgeKey(6, 7));
        REQUIRE_FALSE(p.empty());
        CHECK(p.front() == edgeKey(0, 1));
        CHECK(p.back() == edgeKey(6, 7));
        CHECK(isChain(p));
    }
    SUBCASE("face path from bottom to top crosses one side face") {
        const auto p = shortestFacePath(box(), 0, 1);
        REQUIRE(p.size() == 3);
        CHECK(p[0] == 0);
        CHECK(p[2] == 1);
    }
    SUBCASE("same element = itself") {
        CHECK(shortestVertexPath(box(), 3, 3) == std::vector<uint32_t>{3});
        CHECK(shortestFacePath(box(), 2, 2) == std::vector<uint32_t>{2});
        CHECK(shortestEdgePath(box(), edgeKey(0, 1), edgeKey(0, 1)) == std::vector<Edge>{edgeKey(0, 1)});
    }
}

TEST_CASE("mesh select: what a click selects (clickSelect)") {
    const Brush b = box();
    MeshPickAt none;
    SUBCASE("nothing under the cursor = nullopt (the viewport starts a box selection)") {
        CHECK_FALSE(clickSelect(b, SelMode::Vertex, none, plain, emptySel(), std::nullopt).has_value());
    }
    SUBCASE("plain click selects the element and makes it active; Shift toggles") {
        MeshPickAt at = none;
        at.vertex = 2;
        auto r = clickSelect(b, SelMode::Vertex, at, plain, emptySel(), std::nullopt);
        REQUIRE(r);
        CHECK(r->sel.verts == std::vector<uint32_t>{2});
        REQUIRE(r->active);
        CHECK(r->active->mode == SelMode::Vertex);
        CHECK(r->active->id.n == 2);
        at.vertex = 5;
        ClickMods shift;
        shift.shift = true;
        auto r2 = clickSelect(b, SelMode::Vertex, at, shift, r->sel, r->active);
        REQUIRE(r2);
        CHECK(setOf(r2->sel.verts) == std::set<uint32_t>{2, 5});
        at.vertex = 2;
        auto r3 = clickSelect(b, SelMode::Vertex, at, shift, r2->sel, r2->active);
        REQUIRE(r3);
        CHECK(r3->sel.verts == std::vector<uint32_t>{5});
    }
    SUBCASE("Ctrl+click adds the shortest path from the active element") {
        MeshPickAt at = none;
        at.vertex = 6;
        ClickMods ctrl;
        ctrl.ctrl = true;
        auto r = clickSelect(b, SelMode::Vertex, at, ctrl, selFromVerts(b, {0}), ActiveElem{SelMode::Vertex, ElemId{0, {}}});
        REQUIRE(r);
        CHECK(r->sel.verts.size() == 4);
        CHECK(setOf(r->sel.verts).count(0) == 1);
        CHECK(setOf(r->sel.verts).count(6) == 1);
        REQUIRE(r->active);
        CHECK(r->active->id.n == 6);
    }
    SUBCASE("Ctrl+click with no active element in this mode selects just the element") {
        MeshPickAt at = none;
        at.face = 1;
        ClickMods ctrl;
        ctrl.ctrl = true;
        auto r = clickSelect(b, SelMode::Face, at, ctrl, emptySel(), ActiveElem{SelMode::Vertex, ElemId{0, {}}});
        REQUIRE(r);
        CHECK(r->sel.faces == std::vector<uint32_t>{1});
    }
    SUBCASE("Alt+click in edge mode = loop, Ctrl+Alt = ring; vertex mode takes the loop vertices") {
        MeshPickAt at = none;
        at.edge = edgeKey(0, 4);
        ClickMods alt;
        alt.alt = true;
        ClickMods ctrlAlt = alt;
        ctrlAlt.ctrl = true;
        auto l = clickSelect(b, SelMode::Edge, at, alt, emptySel(), std::nullopt);
        REQUIRE(l);
        CHECK(l->sel.edges == std::vector<Edge>{edgeKey(0, 4)});
        auto ring = clickSelect(b, SelMode::Edge, at, ctrlAlt, emptySel(), std::nullopt);
        REQUIRE(ring);
        CHECK(setOf(ring->sel.edges) == std::set<Edge>{edgeKey(0, 4), edgeKey(1, 5), edgeKey(2, 6), edgeKey(3, 7)});
        auto v = clickSelect(b, SelMode::Vertex, at, ctrlAlt, emptySel(), std::nullopt);
        REQUIRE(v);
        CHECK(v->sel.verts.size() == 8);
    }
    SUBCASE("loop by double-click (loop flag without Alt) in face mode; Shift adds it to the selection") {
        const OpResult c = cutBox();
        const uint32_t m = midVertex(c.brush);
        const core::BrushFace* q = nullptr;
        for (const auto& f : c.brush.faces)
            if (std::find(f.loop.begin(), f.loop.end(), 0u) != f.loop.end() && std::find(f.loop.begin(), f.loop.end(), m) != f.loop.end()) q = &f;
        REQUIRE(q);
        MeshPickAt at = none;
        at.face = q->id;
        at.faceEdge = edgeKey(0, m);
        auto r = clickSelect(c.brush, SelMode::Face, at, plain, emptySel(), std::nullopt, true);
        REQUIRE(r);
        CHECK(r->sel.faces.size() == 4);
        ClickMods shift;
        shift.shift = true;
        MeshSel cur;
        cur.faces = {0};
        auto r2 = clickSelect(c.brush, SelMode::Face, at, shift, cur, std::nullopt, true);
        REQUIRE(r2);
        CHECK(r2->sel.faces.size() == 5);
    }
}

namespace {

// A simple orthographic top view: x, y map to the screen, z is the depth (so a box looks like a square from above).
Project topView(double scale = 100) {
    return [scale](const Vec3& p) -> std::optional<Screen> { return Screen{p.x * scale, p.y * scale, p.z}; };
}

} // namespace

TEST_CASE("mesh pick: vertices, edges, faces and rectangles") {
    const Brush b = box();
    const Project view = topView();
    SUBCASE("pickVertex takes the nearest within the radius") {
        const auto v = pickVertex(b, view, 99, 3);   // near (1,0,*): vertices 1 and 5 project to the same point
        REQUIRE(v);
        CHECK((*v == 1 || *v == 5));
        CHECK_FALSE(pickVertex(b, view, 50, 50).has_value());
    }
    SUBCASE("pickEdge measures the distance to the segment") {
        const auto e = pickEdge(b, view, 50, 3);   // above the middle of the y=0 side
        REQUIRE(e);
        CHECK((*e == edgeKey(0, 1) || *e == edgeKey(4, 5)));
        CHECK_FALSE(pickEdge(b, view, 50, 50).has_value());
        CHECK(pickEdge(b, view, 50, 50, std::numeric_limits<double>::infinity()).has_value());
    }
    SUBCASE("nearestFaceEdge picks the closest edge of one face however far") {
        const auto e = nearestFaceEdge(b, 1, view, 90, 40);   // top face (+Z): the x = 1 edge is nearest
        REQUIRE(e);
        CHECK(*e == edgeKey(5, 6));
        CHECK_FALSE(nearestFaceEdge(b, 99, view, 0, 0).has_value());
    }
    SUBCASE("pickFaceId returns the front-most face along the ray") {
        Ray down{{0.5, 0.5, 5}, {0, 0, -1}};
        CHECK(pickFaceId(b, down) == std::optional<uint32_t>(1));   // the +Z face is hit first
        Ray up{{0.5, 0.5, -5}, {0, 0, 1}};
        CHECK(pickFaceId(b, up) == std::optional<uint32_t>(0));
        Ray miss{{3, 3, 5}, {0, 0, -1}};
        CHECK_FALSE(pickFaceId(b, miss).has_value());
    }
    SUBCASE("boxSelect takes everything inside the rectangle") {
        const MeshSel v = boxSelect(b, view, SelMode::Vertex, -10, -10, 110, 50);   // the y = 0 side
        CHECK(setOf(v.verts) == std::set<uint32_t>{0, 1, 4, 5});
        const MeshSel e = boxSelect(b, view, SelMode::Edge, -10, -10, 110, 50);
        CHECK(setOf(e.edges) == std::set<Edge>{edgeKey(0, 1), edgeKey(4, 5), edgeKey(0, 4), edgeKey(1, 5)});
        const MeshSel f = boxSelect(b, view, SelMode::Face, -10, -10, 110, 50);
        CHECK(f.faces == std::vector<uint32_t>{4});
        const MeshSel all = boxSelect(b, view, SelMode::Face, -10, -10, 110, 110);
        CHECK(all.faces.size() == 6);
    }
    SUBCASE("combineSel adds, or toggles with Shift") {
        const MeshSel a = selFromVerts(b, {0, 1});
        const MeshSel c = selFromVerts(b, {1, 2});
        CHECK(setOf(combineSel(b, a, c, SelMode::Vertex, false).verts) == std::set<uint32_t>{0, 1, 2});
        CHECK(setOf(combineSel(b, a, c, SelMode::Vertex, true).verts) == std::set<uint32_t>{0, 2});
    }
}
