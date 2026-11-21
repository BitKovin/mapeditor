// tools/mesh/select -- loop, ring and path selection for mesh mode and what a click selects (port of shell/src/model/meshSelect.ts and the click rules of tools/meshPick.ts;
// REQUIREMENTS section 21 E1). Pure: brush + picked element -> the elements to select. Thread: any. Spec: 08_MILESTONES.md M11 item 1.
#pragma once

#include <optional>
#include <vector>

#include "tools/mesh/topology.h"

namespace native::tools::mesh {

// The edge loop through `edge`: from each end, walk on through a vertex with exactly four edges to the edge that shares no face with the one we arrived on (Blender's rule); along
// an open border, walk on to the other border edge. Stops anywhere else. Empty when `edge` is not an edge of the mesh.
std::vector<Edge> edgeLoop(const core::Brush& b, const Edge& edge);
// The edges a loop cut through `edge` would cross (the edge ring).
std::vector<Edge> edgeRingEdges(const core::Brush& b, const Edge& edge);
// The face loop that crosses `edge` of face `faceId`: the quads its ring passes through (the face itself always).
std::vector<uint32_t> faceLoop(const core::Brush& b, uint32_t faceId, const Edge& edge);
// The shortest chain of vertices from `from` to `to` along edges (by length), both ends included; empty when not connected.
std::vector<uint32_t> shortestVertexPath(const core::Brush& b, uint32_t from, uint32_t to);
// The edges of the shortest connection between two edges: both edges and the shortest vertex chain joining their nearest ends.
std::vector<Edge> shortestEdgePath(const core::Brush& b, const Edge& from, const Edge& to);
// The fewest faces from `from` to `to`, stepping across shared edges, both ends included; empty when not connected.
std::vector<uint32_t> shortestFacePath(const core::Brush& b, uint32_t from, uint32_t to);

// What is under the cursor: the vertex / edge near it (screen space), the face hit, and that face's edge nearest to the cursor.
struct MeshPickAt {
    std::optional<uint32_t> vertex;
    std::optional<Edge> edge;
    std::optional<uint32_t> face;
    std::optional<Edge> faceEdge;
};
// An element of the current mode: a vertex index or a face id (n), or an edge (e).
struct ElemId {
    uint32_t n = 0;
    Edge e;
    bool operator==(const ElemId& o) const { return n == o.n && e == o.e; }
};
// The last element clicked (the start of a Ctrl+click path).
struct ActiveElem {
    SelMode mode = SelMode::Vertex;
    ElemId id;
};
struct ClickMods {
    bool shift = false, ctrl = false, alt = false;
};
struct ClickResult {
    MeshSel sel;
    std::optional<ActiveElem> active;
};
// The selection after a click. `loop` = Alt held or a double-click. Alt: loop (Ctrl+Alt: ring), replacing the selection unless Shift; Ctrl: shortest path from the active element,
// added to the selection; plain: the element (Shift toggles it). Nothing usable under the cursor: nullopt (the caller then starts a box selection).
std::optional<ClickResult> clickSelect(const core::Brush& b, SelMode mode, const MeshPickAt& at, const ClickMods& mods, const MeshSel& cur, const std::optional<ActiveElem>& active, bool loop);
inline std::optional<ClickResult> clickSelect(const core::Brush& b, SelMode mode, const MeshPickAt& at, const ClickMods& mods, const MeshSel& cur, const std::optional<ActiveElem>& active) {
    return clickSelect(b, mode, at, mods, cur, active, mods.alt);
}

} // namespace native::tools::mesh
