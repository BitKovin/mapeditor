// tools/mesh/ops -- the direct mesh editing operations (port of shell/src/model/meshOps.ts, DESIGN section 8). All pure: (brush, selection, parameter) -> a new brush and
// selection, or an error message. The tools call them on every mouse move, always starting from the untouched brush, so a parameter of 0 gives back the input.
// Face ids that survive an operation are kept; new faces get fresh ids. Every result ends in `finish` (split non-planar faces, drop unused vertices, refresh UVs):
// `info` supplies texture sizes for the world projection (may be empty).
// Thread: any. Spec: 08_MILESTONES.md M11 item 1, 07_EDITOR_PORT.md B (meshOps.test.ts, bridge.test.ts).
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "tools/mesh/topology.h"

namespace native::tools::mesh {

struct OpResult {
    core::Brush brush;
    MeshSel sel;
    std::string error;   // empty = fine
    bool ok() const { return error.empty(); }
};
using PointFn = std::function<core::Vec3(const core::Vec3& p, uint32_t index)>;

// Closed, and every vertex has a single ring of faces around it (no bow-ties).
bool isProperSolid(const core::Brush& b);

// Moves the selected vertices with `fn`; faces that stop being planar are split into triangles. `lockUv`: textures stay attached to the faces (the toolbar's UV lock).
OpResult transformVerts(const core::Brush& b, const std::vector<uint32_t>& verts, const PointFn& fn, bool lockUv = false, const tp::InfoFn& info = {});
// Region extrude: the faces are pushed `dist` along their average normal (or by `along`) and side faces grow around the region's boundary; extruded faces keep their ids.
OpResult extrudeFaces(const core::Brush& b, const std::vector<uint32_t>& faceIds, double dist, const std::optional<core::Vec3>& along = std::nullopt, const tp::InfoFn& info = {});
// Each selected face gets an inner face `amount` metres inside its border, ringed by new quads.
OpResult insetFaces(const core::Brush& b, const std::vector<uint32_t>& faceIds, double amount, const tp::InfoFn& info = {});

// Welds the selected vertices into one (at their centre, or `at`).
OpResult mergeVerts(const core::Brush& b, const std::vector<uint32_t>& verts, const std::optional<core::Vec3>& at = std::nullopt, const tp::InfoFn& info = {});
// Merges vertices closer than `distance` (chains included), each cluster at its centre.
OpResult mergeByDistance(const core::Brush& b, const std::vector<uint32_t>& verts, double distance, const tp::InfoFn& info = {});

// Merges the faces on both sides of each selected edge into one polygon.
OpResult dissolveEdges(const core::Brush& b, const std::vector<Edge>& edges, const tp::InfoFn& info = {});
// A vertex with only two edges (left over by a loop cut) is taken out of its faces; any other vertex has the faces around it merged.
OpResult dissolveVerts(const core::Brush& b, const std::vector<uint32_t>& verts, const tp::InfoFn& info = {});
// Dissolves the edges between the selected faces, one polygon per connected group.
OpResult dissolveFaces(const core::Brush& b, const std::vector<uint32_t>& faceIds, const tp::InfoFn& info = {});

// Removes faces (leaving the mesh open); fillHole closes it again.
OpResult deleteFaces(const core::Brush& b, const std::vector<uint32_t>& faceIds, const tp::InfoFn& info = {});
// Makes faces from open edges among the selected vertices.
OpResult fillHole(const core::Brush& b, const std::vector<uint32_t>& verts, const tp::InfoFn& info = {});

OpResult flipFaces(const core::Brush& b, const std::vector<uint32_t>& faceIds);
// Makes the winding consistent across the surface, then points it outward (or inward).
OpResult recalcNormals(const core::Brush& b, bool outward = true);

// Mirrors the selected vertices across the plane through their centre with normal `axis` (0 x, 1 y, 2 z). Every vertex selected = a true mirror of the whole brush
// (faces re-wound, UVs mirrored); otherwise the vertices just move, and a result that would turn the brush inside out is refused.
OpResult mirrorVerts(const core::Brush& b, const std::vector<uint32_t>& verts, int axis, bool lockUv = true, const tp::InfoFn& info = {});

// Smooth falloff: 1 at distance 0, 0 at `radius` and beyond (smoothstep).
double falloff(double d, double radius);
// Like transformVerts, but every unselected vertex within `radius` of the selection follows by `falloff`: p + w (fn(p) - p). The selection stays what it was.
OpResult proportionalVerts(const core::Brush& b, const std::vector<uint32_t>& verts, const PointFn& fn, double radius, bool lockUv = false, const tp::InfoFn& info = {});

// ---- shared with the second half of the operations (bevel, loop cut, bridge: ops_bevel.cpp / ops_loop.cpp) ----
struct FanEntry {
    size_t face;
    uint32_t inV, outV;
};
// The faces around vertex v in rotational order; consecutive entries share the edge (v, outV). Empty when it is not a proper fan.
std::vector<FanEntry> fanOf(const core::Brush& b, const Topology& t, uint32_t v);
struct LoopsClosed {
    core::Brush brush;
    std::vector<uint32_t> created;
    std::string error;
};
// Adds a face for every closed chain of open edges (edges walked by one face only) whose vertices all satisfy `include`.
LoopsClosed closeOpenLoops(const core::Brush& b, const std::function<bool(uint32_t)>& include, uint32_t firstId, const core::BrushFace* tmpl = nullptr);

} // namespace native::tools::mesh
