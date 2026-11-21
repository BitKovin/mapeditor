// tools/mesh/cut -- cutting faces apart without moving anything (port of shell/src/model/meshCut.ts; REQUIREMENTS section 21 E2, E5): connect vertices (J), subdivide /
// connect edges, knife. A planar convex face cut along a chord between two of its corners gives two planar convex faces, so a valid brush stays valid. The first piece of a
// face keeps its id; the others get new ids and keep the face's UV map. Thread: any. Spec: 08_MILESTONES.md M11 item 1.
#pragma once

#include <functional>
#include <vector>

#include "tools/mesh/ops.h"

namespace native::tools::mesh {

struct SplitPieces {
    std::vector<core::BrushFace> pieces;                         // the first keeps the face's id
    std::vector<std::pair<uint32_t, uint32_t>> made;             // the chords that were applied
};
// Cuts a face along chords (pairs of vertex indices of its loop). Chords must not cross; each is applied to the piece holding both ends. Chords between neighbours of a
// piece (already an edge) are skipped.
SplitPieces splitFace(const core::BrushFace& f, const std::vector<std::pair<uint32_t, uint32_t>>& chords, const std::function<uint32_t()>& newId);

// J: every face holding two or more selected vertices that are not all neighbours is cut between them (between cyclically consecutive selected corners; three or more give
// a middle face). The new edges are selected.
OpResult connectVerts(const core::Brush& b, const std::vector<uint32_t>& verts, const tp::InfoFn& info = {});
// Each selected edge gets `cuts` evenly spaced new vertices (every face on it takes them). Inside a face where two or more selected edges were split, the new vertices are
// joined: two edges = parallel cuts; more edges with one cut = their midpoints joined around a middle face. The new edges are selected; with nothing to join, the pieces of
// the split edges are.
OpResult subdivideEdges(const core::Brush& b, const std::vector<Edge>& edges, int cuts = 1, const tp::InfoFn& info = {});

// A knife point: an existing vertex, or a point on an edge at `t` (0..1 from the edge's lower vertex index to its higher).
struct KnifePoint {
    bool onVertex = true;
    uint32_t vertex = 0;
    Edge edge;
    double t = 0;
    static KnifePoint at(uint32_t v) { KnifePoint p; p.onVertex = true; p.vertex = v; return p; }
    static KnifePoint onEdge(const Edge& e, double t) { KnifePoint p; p.onVertex = false; p.edge = e; p.t = t; return p; }
};
core::Vec3 knifePos(const core::Brush& b, const KnifePoint& p);
// Cuts the brush along the clicked points: points on edges become new vertices (every face on that edge takes them), and each pair of consecutive points that lie on one face cuts
// that face. Consecutive points without a common face are skipped. The new edges are selected.
OpResult knifeCut(const core::Brush& b, const std::vector<KnifePoint>& points, const tp::InfoFn& info = {});

} // namespace native::tools::mesh
