#pragma once
#include "core/brush.h"
#include "core/document.h"
#include <vector>

namespace core {

// Non-destructive subtractive brushes (DESIGN.md §5.6). Authored brushes are never modified: this
// computes *derived* surface fragments, and Core is the single authority for them.
//
// Every output polygon lies on the plane of exactly one authored face and records which one
// (`source` brush + `faceId`). Cut walls, which appear where a subtractor bites into a solid, lie on
// the subtractor's faces and are tagged with the subtractor. That is what lets lightmap charts belong
// to authored faces instead of to fragments.
//
// Algorithm: BSP-tree polygon CSG (Naylor / csg.js style) on the brushes' faces. Concave faces are
// triangulated first because the BSP needs convex polygons; convex faces stay whole. Output polygons
// are convex but a face with a hole in it is necessarily several polygons -- the Shell hides the
// internal seams when it draws outlines.

struct CsgFragment {
    int source = 0;        // 0 = the brush being carved, k >= 1 = k-th subtractor passed in
    uint32_t faceId = 0;   // face id within that source brush
    std::vector<Vec3> verts;  // convex, CCW seen from outside the resulting solid
};

struct CsgLimits {
    size_t maxFragments = 20000;  // per carved brush; beyond this the brush is left uncarved
};

// `target` minus the union of `subtractors`. Returns false (leaving `out` empty) if the work exceeded
// `limits`. An empty `out` with true means the target was carved away completely.
bool csgSubtract(const Brush& target, const std::vector<const Brush*>& subtractors, std::vector<CsgFragment>& out,
                 const CsgLimits& limits = {});

// Axis-aligned bounds overlap (with a small margin): cheap test to skip pairs that cannot interact.
bool brushesMayOverlap(const Brush& a, const Brush& b, double margin = 1e-6);

// Which brushes a subtractor carves, in the scope rules of CarveScope (brush.h): own entity / structural
// entities / other non-structural entities.
bool carveScopeApplies(const CarveScope& scope, bool sameEntity, const Entity& target);

// The convex surface pieces of one authored brush: each face as-is when convex, triangulated when concave.
struct SurfacePiece {
    uint32_t faceId = 0;
    std::vector<Vec3> verts;  // CCW seen from outside
};
std::vector<SurfacePiece> brushSurfaces(const Brush& b);

}  // namespace core
