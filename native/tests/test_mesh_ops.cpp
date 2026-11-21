// M11 item 1 (mesh mode), part 2: tools/mesh/ops, converted from shell/tests/meshOps.test.ts (transformVerts, extrude, inset, merge, dissolve, delete / fill, normals).
// The cases that need the loop cut or the bevel come with them (test_mesh_ops_loop.cpp). Headless, no window.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <set>

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

// Every tool result must be a valid closed solid with outward faces.
void expectSolid(const Brush& b, bool convex = false) {
    const auto problem = convex ? b.checkConvexSolid() : b.checkStructure();
    INFO(problem.value_or(""));
    CHECK_FALSE(problem.has_value());
    CHECK(isClosedManifold(b));
    CHECK(b.signedVolume() > 0);
    std::set<uint32_t> ids;
    for (const auto& f : b.faces) ids.insert(f.id);
    CHECK(ids.size() == b.faces.size());   // ids unique
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

bool uvsEqual(const std::vector<core::Vec2>& a, const std::vector<core::Vec2>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::abs(a[i].u - b[i].u) > 1e-9 || std::abs(a[i].v - b[i].v) > 1e-9) return false;
    return true;
}

bool hasFace(const Brush& b, uint32_t id) { return faceById(b, id) != nullptr; }

} // namespace

TEST_CASE("mesh ops: transformVerts keeps faces planar when a whole face moves; a single corner splits the faces that stop being planar") {
    const Brush b = box();
    const MeshSel top = selFromFaces(b, {TOP});
    const OpResult& moved = ok(transformVerts(b, top.verts, [](const Vec3& p, uint32_t) { return Vec3{p.x, p.y + 1, p.z}; }));
    CHECK(moved.brush.faces.size() == 6);
    expectSolid(moved.brush, true);
    CHECK(moved.brush.signedVolume() == doctest::Approx(2));

    const OpResult corner = transformVerts(b, {7}, [](const Vec3& p, uint32_t) { return Vec3{p.x, p.y + 0.5, p.z}; });
    ok(corner);
    CHECK(corner.brush.faces.size() > 6);
    expectSolid(corner.brush);
    for (uint32_t id : {TOP, 1u, 2u}) CHECK(hasFace(corner.brush, id));

    // a pure function of the input: moving back gives the original planar faces
    const OpResult back = transformVerts(b, {7}, [](const Vec3& p, uint32_t) { return p; });
    CHECK(ok(back).brush.faces.size() == 6);
    CHECK(transformVerts(b, {}, [](const Vec3& p, uint32_t) { return p; }).ok() == false);
}

TEST_CASE("mesh ops: transformVerts re-projects stored UVs so textures stay put in the world") {
    Brush b = box();
    for (auto& f : b.faces) f.uvs = projectFace(b, f);
    const MeshSel top = selFromFaces(b, {TOP});
    const Brush r = ok(transformVerts(b, top.verts, [](const Vec3& p, uint32_t) { return Vec3{p.x + 3, p.y, p.z}; })).brush;
    const core::BrushFace& face = *faceById(r, TOP);
    CHECK(uvsEqual(face.uvs, projectFace(r, face)));
    CHECK_FALSE(uvsEqual(face.uvs, faceById(b, TOP)->uvs));
}

TEST_CASE("mesh ops: extrude pulls a face out - closed, right volume, the face keeps its id, four new sides") {
    const Brush b = box();
    const OpResult& r = ok(extrudeFaces(b, {TOP}, 2));
    expectSolid(r.brush, true);
    CHECK(r.brush.signedVolume() == doctest::Approx(3));
    CHECK(r.brush.faces.size() == 10);
    CHECK(r.sel.faces == std::vector<uint32_t>{TOP});
    CHECK(hasFace(r.brush, TOP));
    CHECK(std::count_if(r.brush.faces.begin(), r.brush.faces.end(), [](const core::BrushFace& f) { return f.texture == "wall/a"; }) == 10);   // sides take the look of what they grow from

    const OpResult cut = extrudeFaces(box(), {TOP}, -0.5);   // a negative distance cuts into the brush (valid, no longer convex)
    ok(cut);
    expectSolid(cut.brush);
    CHECK(cut.brush.signedVolume() == doctest::Approx(0.5));

    CHECK(ok(extrudeFaces(b, {TOP}, 0)).brush.vertices.size() == b.vertices.size());   // distance 0 is the identity
    CHECK_FALSE(extrudeFaces(b, {}, 1).ok());
    std::vector<uint32_t> all;
    for (const auto& f : b.faces) all.push_back(f.id);
    CHECK_FALSE(extrudeFaces(b, all, 1).ok());   // faces that point every which way have no single direction

    // `along` offsets by a vector instead of along the normal
    const OpResult sideways = extrudeFaces(b, {TOP}, 0, Vec3{0, 1, 0});
    ok(sideways);
    CHECK(sideways.brush.signedVolume() == doctest::Approx(2));
}

TEST_CASE("mesh ops: inset makes an inner face (same id) and a ring of four quads; inset then extrude builds a raised panel") {
    const Brush b = box();
    const OpResult& r = ok(insetFaces(b, {TOP}, 0.25));
    expectSolid(r.brush, true);
    CHECK(r.brush.faces.size() == 10);
    CHECK(r.sel.faces == std::vector<uint32_t>{TOP});
    const core::BrushFace& inner = *faceById(r.brush, TOP);
    double lo = 1e9, hi = -1e9;
    for (uint32_t v : inner.loop) {
        lo = std::min(lo, r.brush.vertices[v].x);
        hi = std::max(hi, r.brush.vertices[v].x);
    }
    CHECK(lo == doctest::Approx(0.25));
    CHECK(hi == doctest::Approx(0.75));
    CHECK(r.brush.signedVolume() == doctest::Approx(1));

    CHECK_FALSE(insetFaces(b, {TOP}, 0.6).ok());   // bigger than the face
    CHECK_FALSE(insetFaces(b, {TOP}, -0.1).ok());
    CHECK(ok(insetFaces(b, {TOP}, 0)).brush.faces.size() == 6);

    const OpResult a = insetFaces(box(), {TOP}, 0.25);
    ok(a);
    const OpResult raised = extrudeFaces(a.brush, a.sel.faces, 0.2);
    ok(raised);
    expectSolid(raised.brush);   // a bump on a box is not convex
    CHECK(raised.brush.signedVolume() == doctest::Approx(1 + 0.5 * 0.5 * 0.2));
}

TEST_CASE("mesh ops: merge welds vertices; refuses pinching and too few") {
    const OpResult& r = ok(mergeVerts(box(), {6, 7}));
    CHECK(isClosedManifold(r.brush));
    CHECK(r.brush.vertices.size() == 7);
    CHECK(r.brush.signedVolume() < 1);
    const OpResult at = mergeVerts(box(), {6, 7}, Vec3{0.5, 1, 1});
    ok(at);
    CHECK(std::any_of(at.brush.vertices.begin(), at.brush.vertices.end(), [](const Vec3& p) { return p.x == 0.5 && p.y == 1 && p.z == 1; }));
    CHECK_FALSE(mergeVerts(box(), {0, 6}).ok());   // opposite corners of the brush
    CHECK_FALSE(mergeVerts(box(), {0}).ok());
}

TEST_CASE("mesh ops: merge by distance clusters close vertices (chains included)") {
    // a box with a second, nearly coincident copy of the top edge x: move vertex 5 next to vertex 4 -> they are 0.005 apart after the loop
    Brush b = box();
    b.vertices[5].x = 0.005;   // vertices 4 and 5 now 0.005 apart: the +Z and -Y faces degenerate, but the weld still must give a closed solid
    std::vector<uint32_t> all;
    for (uint32_t i = 0; i < b.vertices.size(); ++i) all.push_back(i);
    CHECK_FALSE(mergeByDistance(b, all, 0.0001).ok());
    const OpResult r = mergeByDistance(b, all, 0.01);
    ok(r);
    CHECK(isClosedManifold(r.brush));
    CHECK(r.brush.vertices.size() < b.vertices.size());
}

TEST_CASE("mesh ops: dissolve refuses nothing selected; a corner vertex merges its faces") {
    CHECK_FALSE(dissolveEdges(box(), {}).ok());
    const OpResult r = dissolveVerts(box(), {6});
    if (r.ok()) CHECK(r.brush.vertices.size() == 7);
    CHECK_FALSE(dissolveVerts(box(), {}).ok());
    CHECK_FALSE(dissolveFaces(box(), {TOP}).ok());   // at least two faces
    // two adjacent faces of a box dissolve into one L-shaped polygon (non planar -> split again); the edge between a face and itself is refused
    const OpResult two = dissolveFaces(box(), {TOP, 1});
    if (two.ok()) CHECK(isClosedManifold(two.brush));
    CHECK_FALSE(dissolveEdges(box(), {edgeKey(0, 6)}).ok());   // not an edge of the mesh
}

TEST_CASE("mesh ops: delete makes a hole; fill closes it with a new face") {
    const Brush b = box();
    const Brush open = ok(deleteFaces(b, {TOP})).brush;
    CHECK(open.faces.size() == 5);
    CHECK_FALSE(isClosedManifold(open));
    const MeshSel top = selFromFaces(b, {TOP});
    const OpResult& filled = ok(fillHole(open, top.verts));
    CHECK(filled.brush.faces.size() == 6);
    expectSolid(filled.brush, true);
    CHECK(filled.sel.faces.size() == 1);

    CHECK_FALSE(fillHole(box(), {0, 1, 2, 3}).ok());   // nothing open
    std::vector<uint32_t> all;
    for (const auto& f : b.faces) all.push_back(f.id);
    CHECK_FALSE(deleteFaces(box(), all).ok());
    CHECK_FALSE(fillHole(open, {4, 5}).ok());   // fewer than three
}

TEST_CASE("mesh ops: flip makes the volume negative; recalculate repairs a mixed-up mesh in both directions") {
    const Brush b = box();
    std::vector<uint32_t> all;
    for (const auto& f : b.faces) all.push_back(f.id);
    const Brush allFlipped = ok(flipFaces(b, all)).brush;
    CHECK(allFlipped.signedVolume() < 0);
    CHECK(ok(recalcNormals(allFlipped, true)).brush.signedVolume() > 0);
    const Brush mixed = ok(flipFaces(b, {0, 3})).brush;
    CHECK_FALSE(isClosedManifold(mixed));   // inconsistent winding shows up as a broken edge pairing
    expectSolid(ok(recalcNormals(mixed, true)).brush, true);
    CHECK(ok(recalcNormals(b, false)).brush.signedVolume() < 0);   // inward on request
    CHECK_FALSE(flipFaces(b, {}).ok());
}

TEST_CASE("mesh ops: flipping keeps UVs matched to their corners") {
    Brush b = box();
    for (auto& f : b.faces)
        for (size_t i = 0; i < f.loop.size(); ++i) f.uvs.push_back({static_cast<double>(i), 0});
    const Brush flipped = ok(flipFaces(b, {0})).brush;
    const core::BrushFace& f = *faceById(flipped, 0);
    REQUIRE(f.uvs.size() == 4);
    CHECK(std::vector<double>{f.uvs[0].u, f.uvs[1].u, f.uvs[2].u, f.uvs[3].u} == std::vector<double>{3, 2, 1, 0});
}

TEST_CASE("mesh ops: mirror of every vertex is a true mirror (still outward); a part of the vertices must not turn the brush inside out") {
    const Brush b = box();
    std::vector<uint32_t> all;
    for (uint32_t i = 0; i < b.vertices.size(); ++i) all.push_back(i);
    const OpResult whole = mirrorVerts(b, all, 0);
    ok(whole);
    expectSolid(whole.brush, true);
    CHECK(whole.brush.signedVolume() == doctest::Approx(1));
    CHECK_FALSE(mirrorVerts(b, {}, 0).ok());
    // mirroring four vertices of one face about their own centre plane moves nothing along that plane; the top face mirrored in y stays where it is
    const OpResult part = mirrorVerts(b, selFromFaces(b, {TOP}).verts, 1);
    ok(part);
    expectSolid(part.brush, true);
    // mirroring a single corner through the centre of... itself: its own plane, so nothing changes
    CHECK(ok(mirrorVerts(b, {6}, 2)).brush.vertices[6].z == doctest::Approx(1));
}

TEST_CASE("mesh ops: proportional editing moves neighbours by a smooth falloff and keeps the selection") {
    CHECK(falloff(0, 1) == doctest::Approx(1));
    CHECK(falloff(1, 1) == 0);
    CHECK(falloff(2, 1) == 0);
    CHECK(falloff(0.5, 1) == doctest::Approx(0.5));
    CHECK(falloff(0.3, 0) == 0);
    Brush b = Brush::makeBox("b", {0, 0, 0}, {4, 1, 1}, "wall/a");   // 4 m long; vertices 2 and 6 are the x = 4, y = 1 edge
    const OpResult r = proportionalVerts(b, {2, 6}, [](const Vec3& p, uint32_t) { return Vec3{p.x, p.y + 1, p.z}; }, 2.5);
    ok(r);
    expectSolid(r.brush);
    CHECK(r.sel.verts == std::vector<uint32_t>{2, 6});
    CHECK(r.brush.vertices[2].y == doctest::Approx(2));   // selected: the full move
    CHECK(r.brush.vertices[1].y == doctest::Approx(falloff(1, 2.5)));   // 1 m from the selection (x = 4, y = 0)
    CHECK(r.brush.vertices[3].y == doctest::Approx(1));   // 4 m away: out of reach
    CHECK(r.brush.vertices[0].y == doctest::Approx(0));
    CHECK_FALSE(proportionalVerts(b, {}, [](const Vec3& p, uint32_t) { return p; }, 1).ok());
}
