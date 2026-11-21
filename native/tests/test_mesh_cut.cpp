// M11 item 1 (mesh mode), part 4a: connect vertices, subdivide / connect edges and knife, converted from shell/tests/meshCut.test.ts. Headless, no window.
#include <doctest.h>

#include <cmath>
#include <set>

#include "tools/mesh/cut.h"

using namespace native::tools::mesh;
using core::Brush;
using core::Vec3;

namespace {

Brush box() { return Brush::makeBox("b", {0, 0, 0}, {2, 1, 1}, "wall/a"); }
// makeBox: vertices 0..3 at z=0 (x0y0, x1y0, x1y1, x0y1), 4..7 at z=1; faces 0 -Z, 1 +Z, 2 -X, 3 +X, 4 -Y, 5 +Y.

OpResult ok(const OpResult& r) {
    INFO(r.error);
    REQUIRE(r.ok());
    return r;
}

void expectSolid(const Brush& b) {
    const auto problem = b.checkConvexSolid();
    INFO(problem.value_or(""));
    CHECK_FALSE(problem.has_value());   // still convex
    CHECK(isClosedManifold(b));
    CHECK(b.signedVolume() == doctest::Approx(2).epsilon(1e-9));   // nothing moved
    std::set<uint32_t> ids;
    for (const auto& f : b.faces) ids.insert(f.id);
    CHECK(ids.size() == b.faces.size());
}

std::vector<core::Vec2> projectFace(const Brush& b, const core::BrushFace& f) {
    namespace tp = core::texplan;
    const tp::Projection proj = tp::projectionOf(tp::faceKey(f, "projection"), nullptr);
    const tp::V3 n = tp::faceNormalOf(b, f);
    std::vector<core::Vec2> uvs;
    for (uint32_t v : f.loop) {
        const tp::V2 uv = tp::projectPoint(n, tp::v3(b.vertices[v]), proj);
        uvs.push_back({uv[0], uv[1]});
    }
    return uvs;
}

// the faces lying entirely at height z
std::vector<const core::BrushFace*> facesAt(const Brush& b, double z) {
    std::vector<const core::BrushFace*> out;
    for (const auto& f : b.faces) {
        bool all = true;
        for (uint32_t v : f.loop) all = all && std::abs(b.vertices[v].z - z) < 1e-9;
        if (all) out.push_back(&f);
    }
    return out;
}

KnifePoint mid(uint32_t a, uint32_t c) { return KnifePoint::onEdge(edgeKey(a, c), 0.5); }

} // namespace

TEST_CASE("mesh cut: splitFace cuts a quad along its diagonal and skips chords that are already edges") {
    uint32_t id = 10;
    core::BrushFace f;
    f.id = 3;
    f.loop = {0, 1, 2, 3};
    f.texture = "t";
    const SplitPieces r = splitFace(f, {{0, 1}, {0, 2}}, [&] { return id++; });
    REQUIRE(r.pieces.size() == 2);
    CHECK(r.pieces[0].loop == std::vector<uint32_t>{0, 1, 2});
    CHECK(r.pieces[1].loop == std::vector<uint32_t>{2, 3, 0});
    CHECK(r.pieces[0].id == 3);
    CHECK(r.pieces[1].id == 10);
    REQUIRE(r.made.size() == 1);
    CHECK(r.made[0] == std::pair<uint32_t, uint32_t>{0, 2});
}

TEST_CASE("mesh cut: connect vertices (J)") {
    SUBCASE("two opposite corners of the bottom face: two triangles, the brush stays a convex solid") {
        const OpResult r = ok(connectVerts(box(), {0, 2}));
        expectSolid(r.brush);
        CHECK(r.brush.faces.size() == 7);
        REQUIRE(r.sel.edges.size() == 1);
        CHECK(r.sel.edges[0] == edgeKey(0, 2));
    }
    SUBCASE("the pieces keep the texture where it was") {
        const Brush b = box();
        const core::BrushFace* bottom = facesAt(b, 0)[0];
        const std::vector<core::Vec2> before = projectFace(b, *bottom);
        size_t corner = 0;
        for (size_t i = 0; i < bottom->loop.size(); ++i)
            if (bottom->loop[i] == 2) corner = i;
        const OpResult r = ok(connectVerts(b, {0, 2}));
        int checked = 0;
        for (const core::BrushFace* piece : facesAt(r.brush, 0)) {
            const std::vector<core::Vec2> after = projectFace(r.brush, *piece);
            for (size_t i = 0; i < piece->loop.size(); ++i)
                if (piece->loop[i] == 2) {
                    CHECK(after[i].u == doctest::Approx(before[corner].u).epsilon(1e-9));
                    CHECK(after[i].v == doctest::Approx(before[corner].v).epsilon(1e-9));
                    ++checked;
                }
        }
        CHECK(checked >= 1);
    }
    SUBCASE("neighbours or no shared face: refused with a reason") {
        CHECK_FALSE(connectVerts(box(), {0, 1}).ok());
        CHECK_FALSE(connectVerts(box(), {0, 6}).ok());   // opposite corners of the box share no face
        CHECK_FALSE(connectVerts(box(), {0}).ok());
    }
}

TEST_CASE("mesh cut: subdivide / connect edges") {
    SUBCASE("one edge: both faces on it get the new vertex, nothing else changes") {
        const OpResult r = ok(subdivideEdges(box(), {edgeKey(0, 1)}));
        expectSolid(r.brush);
        REQUIRE(r.brush.vertices.size() == 9);
        CHECK(r.brush.vertices[8].x == doctest::Approx(1));
        CHECK(r.brush.vertices[8].y == doctest::Approx(0));
        CHECK(r.brush.vertices[8].z == doctest::Approx(0));
        CHECK(r.brush.faces.size() == 6);
        CHECK(r.sel.edges.size() == 2);   // the two halves
    }
    SUBCASE("two opposite edges of a face: the face is cut across (Scythe connect edges); 3 cuts give 4 strips") {
        const OpResult r = ok(subdivideEdges(box(), {edgeKey(4, 5), edgeKey(6, 7)}));
        expectSolid(r.brush);
        CHECK(facesAt(r.brush, 1).size() == 2);
        CHECK(r.sel.edges.size() == 1);
        const OpResult r3 = ok(subdivideEdges(box(), {edgeKey(4, 5), edgeKey(6, 7)}, 3));
        expectSolid(r3.brush);
        CHECK(facesAt(r3.brush, 1).size() == 4);
        CHECK(r3.sel.edges.size() == 3);
    }
    SUBCASE("all four edges of a face: midpoints joined around a middle face (4 corners + 1)") {
        const OpResult r = ok(subdivideEdges(box(), {edgeKey(4, 5), edgeKey(5, 6), edgeKey(6, 7), edgeKey(4, 7)}));
        expectSolid(r.brush);
        CHECK(facesAt(r.brush, 1).size() == 5);
    }
    SUBCASE("nothing selected is refused") { CHECK_FALSE(subdivideEdges(box(), {}).ok()); }
}

TEST_CASE("mesh cut: knife (K)") {
    SUBCASE("from the middle of a bottom edge to the middle of the top edge above it: that side is cut in two") {
        const OpResult r = ok(knifeCut(box(), {mid(0, 1), mid(4, 5)}));
        expectSolid(r.brush);
        CHECK(r.brush.vertices.size() == 10);
        CHECK(r.brush.faces.size() == 7);
        REQUIRE(r.sel.edges.size() == 1);
        CHECK(r.brush.vertices[r.sel.edges[0].a].x == doctest::Approx(1).epsilon(1e-12));
        CHECK(r.brush.vertices[r.sel.edges[0].b].x == doctest::Approx(1).epsilon(1e-12));
    }
    SUBCASE("a closed cut all the way round splits four faces") {
        const OpResult r = ok(knifeCut(box(), {mid(0, 1), mid(4, 5), mid(6, 7), mid(2, 3), mid(0, 1)}));
        expectSolid(r.brush);
        CHECK(r.brush.faces.size() == 10);
        CHECK(r.sel.edges.size() == 4);
    }
    SUBCASE("vertex to vertex across a face, and positions along an edge") {
        const OpResult r = ok(knifeCut(box(), {KnifePoint::at(0), KnifePoint::at(2)}));
        expectSolid(r.brush);
        CHECK(r.brush.faces.size() == 7);
        const Vec3 q = knifePos(box(), KnifePoint::onEdge(edgeKey(0, 1), 0.25));
        CHECK(q.x == doctest::Approx(0.5));
        CHECK(q.y == doctest::Approx(0));
        CHECK(q.z == doctest::Approx(0));
        CHECK(knifePos(box(), KnifePoint::onEdge(edgeKey(0, 1), 0)).x == doctest::Approx(0));
    }
    SUBCASE("points with no face in common cut nothing and are refused; one point is not a cut") {
        CHECK_FALSE(knifeCut(box(), {KnifePoint::at(0), KnifePoint::at(6)}).ok());
        CHECK_FALSE(knifeCut(box(), {mid(0, 1)}).ok());
    }
}
