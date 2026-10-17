#pragma once
#include "core/brush.h"
#include "core/math3.h"
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace core {

// Smooth edges (REQUIREMENTS §33 H6). A brush lists edges the person marked smooth (`Brush::smoothEdges`: face id pairs, the Shell's `smooth` key). The faces on
// both sides of a smooth edge are shaded with normals that vary across the face: every corner gets the angle-weighted average of the normals of the
// faces meeting at that vertex through smooth edges, and a point inside the face interpolates its corners (mean value coordinates). Used by the
// light baker (texel sample normals), the lightmap packer (faces joined by a smooth edge share a chart) and the BSP export (vertex normals).
// Only the faces of one brush are smoothed with each other.

struct FaceShading {
    std::vector<Vec3> corners;  // the authored face's polygon (its loop), world space
    std::vector<Vec3> normals;  // unit corner normals, parallel to `corners`
    uint64_t hash = 0;          // of the corner normals: the light baker's change detection compares it
};

struct BrushSmoothing {
    std::vector<std::pair<Vec3, Vec3>> edges;          // the marked edges (end points)
    std::unordered_map<uint32_t, FaceShading> faces;   // by face id: faces with at least one corner normal that is not their own normal
    /// Whether the segment p-q lies on one of the marked edges (within `eps` metres).
    bool onSmoothEdge(const Vec3& p, const Vec3& q, double eps = 1e-4) const;
};

/// The smoothing of a brush, or null when it marks no valid smooth edge.
std::shared_ptr<const BrushSmoothing> brushSmoothing(const Brush& b);

/// The shading normal at `p` on the face (a point of its plane, normally inside it); `fallback` when the corners give none.
Vec3 shadingNormalAt(const FaceShading& s, const Vec3& p, const Vec3& fallback);

}  // namespace core
