// tools/mesh/topology -- the mesh editing foundation (port of shell/src/model/meshTopology.ts; DESIGN section 5.1, 8): the adjacency of a brush's polygon mesh
// (a half-edge view: every directed edge a>b belongs to the face that walks it), the mesh selection (vertices by index, edges as a < b pairs, faces by id), the
// mode conversions, and the clean-up every mesh tool runs at its end (split faces that stopped being planar, drop unused vertices, refresh UVs).
//
// Conventions (as the Shell): vertex indices are stable through an operation (tools append new vertices); `finish` then drops unused ones and remaps the
// selection. Face ids are stable: a face that survives an operation keeps its id, pieces of a split face and new faces get fresh ids (the lighting is keyed on them).
// Pure functions of core::Brush; the brush JSON keys the editor does not model are kept by the document layer (doc::VersionEditor::replaceBrush).
// Thread: any. Spec: 08_MILESTONES.md M11 item 1, 07_EDITOR_PORT.md B.
#pragma once

#include <core/brush.h>
#include <core/texplan.h>

#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

namespace native::tools::mesh {

namespace tp = core::texplan;

struct Edge {
    uint32_t a = 0, b = 0;   // a < b
    bool operator==(const Edge& o) const { return a == o.a && b == o.b; }
    bool operator<(const Edge& o) const { return a != o.a ? a < o.a : b < o.b; }
};
inline Edge edgeKey(uint32_t x, uint32_t y) { return x < y ? Edge{x, y} : Edge{y, x}; }
struct EdgeHash { size_t operator()(const Edge& e) const { return std::hash<uint64_t>()((uint64_t(e.a) << 32) | e.b); } };

struct MeshSel {
    std::vector<uint32_t> verts;   // vertex indices
    std::vector<Edge> edges;
    std::vector<uint32_t> faces;   // face ids
    bool empty() const { return verts.empty() && edges.empty() && faces.empty(); }
};
enum class SelMode { Vertex, Edge, Face };

struct EdgeFaces {
    uint32_t a = 0, b = 0;
    std::vector<size_t> faces;     // indices into Brush::faces of the faces touching the edge (2 for a closed manifold)
};
struct Topology {
    std::unordered_map<uint64_t, size_t> half;   // directed edge (a << 32 | b) -> index of the face that walks a then b
    std::vector<EdgeFaces> edgeList;             // in first-seen order
    std::unordered_map<Edge, size_t, EdgeHash> edgeIndex;
    std::unordered_map<uint32_t, std::vector<size_t>> vertexFaces;
    static uint64_t halfKey(uint32_t a, uint32_t b) { return (uint64_t(a) << 32) | b; }
    const EdgeFaces* find(const Edge& e) const {
        const auto it = edgeIndex.find(e);
        return it == edgeIndex.end() ? nullptr : &edgeList[it->second];
    }
};

Topology buildTopology(const core::Brush& b);
// Every edge shared by exactly two faces that walk it in opposite directions.
bool isClosedManifold(const core::Brush& b);

const core::BrushFace* faceById(const core::Brush& b, uint32_t id);
uint32_t nextFaceId(const core::Brush& b);
// A copy of `from` (texture, projection, anything else) with a new id and loop; stored UVs become zeros (recomputed later).
core::BrushFace cloneFace(const core::BrushFace& from, uint32_t id, const std::vector<uint32_t>& loop);

core::Vec3 centroid(const core::Brush& b, const std::vector<uint32_t>& verts);
core::Vec3 faceCentre(const core::Brush& b, const core::BrushFace& f);
// Area-weighted average normal of some faces (normalised; zero when they cancel out).
core::Vec3 averageNormal(const core::Brush& b, const std::vector<const core::BrushFace*>& faces);
double edgeLength(const core::Brush& b, const Edge& e);

// ---- selection ----
MeshSel selFromFaces(const core::Brush& b, const std::vector<uint32_t>& faceIds);
MeshSel selFromVerts(const core::Brush& b, const std::vector<uint32_t>& verts);
MeshSel selFromEdges(const core::Brush& b, const std::vector<Edge>& edges);
// Blender's rule when switching select mode: keep what is still fully selected in the new mode.
MeshSel convertSel(const core::Brush& b, const MeshSel& sel, SelMode from, SelMode to);
MeshSel allSel(const core::Brush& b);
MeshSel invertSel(const core::Brush& b, const MeshSel& sel, SelMode mode);
// One ring of neighbours more (vertex: connected vertices; edge / face: touching ones).
MeshSel growSel(const core::Brush& b, const MeshSel& sel, SelMode mode);
// Drops the elements that touch anything unselected.
MeshSel shrinkSel(const core::Brush& b, const MeshSel& sel, SelMode mode);
// Drops entries that no longer exist (vertices out of range, edges not in the mesh, faces gone).
MeshSel cleanSel(const core::Brush& b, const MeshSel& sel);

// ---- the clean-up every tool ends with ----
inline constexpr double kPlanarEps = 1e-4;
struct SplitResult {
    core::Brush brush;
    std::map<uint32_t, std::vector<uint32_t>> pieces;   // old face id -> the ids now covering it (the first keeps the old id)
};
// Splits non-planar faces into triangles (faces stay planar, DESIGN section 14). The first triangle keeps the face's id.
SplitResult splitNonPlanar(const core::Brush& b, double eps = kPlanarEps);
// Faces with stored UVs get them re-projected (the projection is world aligned, so textures stay put in the world). With `before` (UV lock, REQUIREMENTS section 6 B4)
// a face keeps the UV map it had before the edit: the old affine map is evaluated at the new corner positions; `parentOf` maps the faces made by splitting a
// non-planar face to the face they came from. Faces without stored UVs whose corners moved get UVs baked this way. `info` gives texture sizes (may be empty).
core::Brush refreshUvs(const core::Brush& b, const core::Brush* before, const std::map<uint32_t, uint32_t>& parentOf, const tp::InfoFn& info);
struct Cleaned {
    core::Brush brush;
    MeshSel sel;
};
// Drops vertices no face uses; remaps the loops and the selection.
Cleaned compact(const core::Brush& b, const MeshSel& sel);
// split non-planar faces, drop unused vertices, refresh UVs. `splitFrom` (with `before`): faces the tool cut out of an existing face (new id -> that face's id).
Cleaned finish(const core::Brush& b, const MeshSel& sel, const tp::InfoFn& info, const core::Brush* before = nullptr, const std::map<uint32_t, uint32_t>& splitFrom = {});

} // namespace native::tools::mesh
