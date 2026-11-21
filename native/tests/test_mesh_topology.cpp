// M11 item 1 (mesh mode), part 1: the topology, selection and clean-up of tools/mesh/topology, converted from shell/tests/meshOps.test.ts ("topology and selection") and
// the transformVerts clean-up cases. Headless, no window.
#include <doctest.h>

#include <algorithm>
#include <set>

#include "tools/mesh/topology.h"

using namespace native::tools::mesh;
using core::Brush;
using core::Vec3;

namespace {

// makeBox faces: 0 -Z, 1 +Z, 2 -X, 3 +X, 4 -Y, 5 +Y (top). Vertices 0..3 are z=0 (x0y0, x1y0, x1y1, x0y1), 4..7 z=1.
Brush box() { return Brush::makeBox("b", {0, 0, 0}, {1, 1, 1}, "wall/a"); }
constexpr uint32_t TOP = 5;

std::vector<uint32_t> sorted(std::vector<uint32_t> v) {
    std::sort(v.begin(), v.end());
    return v;
}

} // namespace

TEST_CASE("mesh topology: a box is a closed manifold; every edge has two faces") {
    const Brush b = box();
    const Topology t = buildTopology(b);
    CHECK(t.edgeList.size() == 12);
    CHECK(std::all_of(t.edgeList.begin(), t.edgeList.end(), [](const EdgeFaces& e) { return e.faces.size() == 2; }));
    CHECK(isClosedManifold(b));
    CHECK(t.vertexFaces.at(0).size() == 3);   // a corner belongs to three faces
    CHECK(t.half.at(Topology::halfKey(0, 3)) == 0);   // -Z walks 0 > 3 > 2 > 1

    Brush open = b;
    open.faces.pop_back();
    CHECK_FALSE(isClosedManifold(open));
    Brush flipped = b;   // a face walked the wrong way: its edges are shared in the same direction
    std::reverse(flipped.faces[TOP].loop.begin(), flipped.faces[TOP].loop.end());
    CHECK_FALSE(isClosedManifold(flipped));
}

TEST_CASE("mesh selection: modes convert the Blender way") {
    const Brush b = box();
    const MeshSel top = selFromFaces(b, {TOP});
    CHECK(top.verts.size() == 4);
    CHECK(top.edges.size() == 4);
    const MeshSel asVerts = convertSel(b, top, SelMode::Face, SelMode::Vertex);
    CHECK(asVerts.verts.size() == 4);
    CHECK(convertSel(b, asVerts, SelMode::Vertex, SelMode::Face).faces == std::vector<uint32_t>{TOP});   // all four corners selected -> the face
    const MeshSel oneEdge = selFromEdges(b, {edgeKey(0, 1)});
    CHECK(convertSel(b, oneEdge, SelMode::Edge, SelMode::Face).faces.empty());   // no face has all its edges selected
    CHECK(sorted(convertSel(b, oneEdge, SelMode::Edge, SelMode::Vertex).verts) == std::vector<uint32_t>{0, 1});
    const MeshSel fromEdges = convertSel(b, top, SelMode::Face, SelMode::Edge);
    CHECK(fromEdges.edges.size() == 4);
    CHECK(convertSel(b, fromEdges, SelMode::Edge, SelMode::Face).faces == std::vector<uint32_t>{TOP});
    CHECK(convertSel(b, top, SelMode::Face, SelMode::Face).faces == top.faces);

    CHECK(allSel(b).faces.size() == 6);
    CHECK(allSel(b).verts.size() == 8);
    CHECK(invertSel(b, top, SelMode::Face).faces.size() == 5);
    CHECK(invertSel(b, selFromVerts(b, {0}), SelMode::Vertex).verts.size() == 7);
}

TEST_CASE("mesh selection: grow and shrink") {
    const Brush b = box();
    const MeshSel v = selFromVerts(b, {0});
    CHECK(growSel(b, v, SelMode::Vertex).verts.size() == 4);   // itself + 3 neighbours
    const MeshSel g = growSel(b, selFromFaces(b, {TOP}), SelMode::Face);
    CHECK(g.faces.size() == 5);                               // the top and the four sides that touch it
    CHECK(shrinkSel(b, g, SelMode::Face).faces == std::vector<uint32_t>{TOP});   // only the top has all its neighbours selected
    const MeshSel e = growSel(b, selFromEdges(b, {edgeKey(0, 1)}), SelMode::Edge);
    CHECK(e.edges.size() == 5);   // the edge and the two others at each end
    CHECK(shrinkSel(b, e, SelMode::Edge).edges == std::vector<Edge>{edgeKey(0, 1)});
    CHECK(shrinkSel(b, growSel(b, v, SelMode::Vertex), SelMode::Vertex).verts == std::vector<uint32_t>{0});
}

TEST_CASE("mesh clean-up: a corner moved out of plane splits the faces; the first piece keeps the id; ids stay unique") {
    Brush b = box();
    b.vertices[7].y += 0.5;   // x0 y1 z1 moves along +Z and -X (still planar) but out of the +Y plane
    const SplitResult s = splitNonPlanar(b);
    CHECK(s.brush.faces.size() > 6);
    std::set<uint32_t> ids;
    for (const auto& f : s.brush.faces) ids.insert(f.id);
    CHECK(ids.size() == s.brush.faces.size());
    for (uint32_t id : {TOP, 1u, 2u}) CHECK(ids.count(id) == 1);
    CHECK(s.pieces.size() == 1);   // only the top (+Y) bends
    for (const auto& [old, pieces] : s.pieces) {
        CHECK(pieces.front() == old);
        CHECK(pieces.size() == 2);   // a quad -> two triangles
    }
    CHECK(isClosedManifold(s.brush));
    // a brush that is planar everywhere comes back untouched
    CHECK(splitNonPlanar(box()).pieces.empty());
    CHECK(splitNonPlanar(box()).brush.faces.size() == 6);
}

TEST_CASE("mesh clean-up: compact drops unused vertices and remaps the selection; cleanSel drops what is gone") {
    Brush b = box();
    b.vertices.insert(b.vertices.begin(), Vec3{9, 9, 9});   // vertex 0 is unused now: shift every loop by one
    for (auto& f : b.faces)
        for (uint32_t& v : f.loop) ++v;
    MeshSel sel;
    sel.verts = {0, 1, 8};
    sel.edges = {edgeKey(1, 2), edgeKey(0, 1)};
    sel.faces = {TOP, 77};
    const Cleaned c = compact(b, sel);
    CHECK(c.brush.vertices.size() == 8);
    CHECK(c.brush.faces[0].loop == std::vector<uint32_t>{0, 3, 2, 1});   // as the original box
    CHECK(c.sel.verts == std::vector<uint32_t>{0, 7});                  // the unused vertex went, the others moved down
    CHECK(c.sel.edges == std::vector<Edge>{edgeKey(0, 1)});             // (1,2) -> (0,1); the edge to the dropped vertex is gone
    CHECK(c.sel.faces == std::vector<uint32_t>{TOP});                   // face 77 does not exist
    const MeshSel cleaned = cleanSel(box(), sel);
    CHECK(cleaned.verts == std::vector<uint32_t>{0, 1});
    CHECK(cleaned.faces == std::vector<uint32_t>{TOP});
}

TEST_CASE("mesh clean-up: finish splits, compacts and keeps the selection on the pieces; stored UVs follow the world projection or the old map (UV lock)") {
    Brush b = box();
    for (auto& f : b.faces) f.uvs.assign(f.loop.size(), core::Vec2{0, 0});
    Brush bent = b;
    bent.vertices[7].z += 0.5;   // +Z (z = 1) is the only face that bends
    MeshSel sel;
    sel.faces = {1};
    const native::tools::mesh::tp::InfoFn none;
    const Cleaned c = finish(bent, sel, none);
    CHECK(c.brush.faces.size() > 6);
    CHECK(c.sel.faces.size() == 2);   // +Z became two pieces, both selected
    CHECK(std::find(c.sel.faces.begin(), c.sel.faces.end(), 1u) != c.sel.faces.end());
    for (const auto& f : c.brush.faces) CHECK(f.uvs.size() == f.loop.size());
    // refreshed by the (default) world projection: the +Y face at vertices with x = 1 differs from x = 0
    const auto& side = *faceById(c.brush, 3);   // +X
    CHECK((side.uvs[0].u != side.uvs[1].u || side.uvs[0].v != side.uvs[1].v));

    // UV lock: the +Z face keeps the map it had before; moving a corner of the face along its plane leaves the other corners' uvs where they were
    Brush locked = b;
    {
        // give +Z an explicit affine map u = x, v = y
        auto& f = locked.faces[1];
        f.uvs.clear();
        for (uint32_t v : f.loop) f.uvs.push_back(core::Vec2{locked.vertices[v].x, locked.vertices[v].y});
    }
    Brush moved = locked;
    moved.vertices[6].x += 0.25;   // x1 y1 z1 -> x 1.25
    const Brush withLock = refreshUvs(moved, &locked, {}, none);
    const auto& top = *faceById(withLock, 1);
    for (size_t k = 0; k < top.loop.size(); ++k) {
        CHECK(top.uvs[k].u == doctest::Approx(moved.vertices[top.loop[k]].x));
        CHECK(top.uvs[k].v == doctest::Approx(moved.vertices[top.loop[k]].y));
    }
}
