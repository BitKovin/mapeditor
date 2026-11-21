// tools/texturing/ftt -- the Fast Texture Tool model (port of shell/src/model/ftt.ts, REQUIREMENTS section 6 B5, Scythe's "Fast Texture Tool"): a UV editor for the selected
// faces of ONE brush. Everything here is pure; the window only draws and turns pointer movement into the transforms below.
//
// A face's UV is one affine map of the plane (its Projection), so every edit is a 2D transform of UV space applied to the whole selection as ONE island (the faces keep their
// positions relative to each other, so a texture that continues over an edge keeps continuing). The result is written back as a Projection of kind Free with a frozen frame
// (projectionFromAffine), exactly like Flow, so the Surface Inspector keeps working on the face afterwards.
// UV space as drawn: u to the right, v DOWN (image convention). Angles are therefore clockwise on screen.
// Thread: any (pure). Spec: 07_EDITOR_PORT.md B, 08_MILESTONES.md M11 item 3.
#pragma once

#include <core/brush.h>

#include <array>
#include <optional>
#include <vector>

#include "tools/texturing/surface.h"

namespace native::tools::tex {

/// (u, v) -> (a u + b v + tx, c u + d v + ty)
using Xform = std::array<double, 6>;
constexpr Xform kIdentity{1, 0, 0, 1, 0, 0};
Xform translateXform(double du, double dv);
Xform scaleXformAbout(double sx, double sy, const V2& p);
Xform rotateXformAbout(double degrees, const V2& p);
/// The mirror feature: flips the island left-right ('u') or top-bottom ('v') about `at` (a u or v value).
Xform mirrorXformAbout(char axis, double at);
/// `second` after `first`.
Xform composeXform(const Xform& second, const Xform& first);
V2 applyXform(const Xform& t, const V2& p);

// ---- reading an island -------------------------------------------------------------------------------------------------------------------------------
/// The UV corners of a face (what the texture does at each corner of its loop).
std::vector<V2> faceUvPolygon(const core::Brush& b, const core::BrushFace& f, const InfoFn& info);
struct UvBounds {
    V2 min, max;
};
std::optional<UvBounds> polygonsBounds(const std::vector<std::vector<V2>>& polys);
V2 boundsCentre(const UvBounds& b);
struct IslandFace {
    uint32_t id;
    std::vector<V2> poly;
};
std::vector<IslandFace> islandPolygons(const core::Brush& b, const std::vector<uint32_t>& ids, const InfoFn& info);
/// The shape a face would have in UV space at the texture's own pixels per metre (Scythe's blue rectangle): its extent in metres along the face's current u and v directions
/// over the natural size.
V2 idealSize(const core::Brush& b, const core::BrushFace& f, const InfoFn& info);

// ---- applying a transform ----------------------------------------------------------------------------------------------------------------------------
struct FttResult {
    core::Brush brush;
    size_t changed = 0;
};
/// Applies `t` to the UV map of every face in `ids` (all of one brush). A face whose result would be degenerate is left as it was; `changed` counts the faces that moved.
FttResult transformFaces(const core::Brush& b, const std::vector<uint32_t>& ids, const Xform& t, const InfoFn& info);
/// Rotation that makes the UV edge a -> b horizontal or vertical (whichever is nearer), about its midpoint. Nullopt for a zero-length edge.
std::optional<Xform> alignEdgeXform(const V2& a, const V2& b);

enum class UnwrapMode { Follow, Ortho, Axial };
/// Re-projects the faces. Follow = Follow Quads (Flow): the first id's UVs continue over the shared edges onto the others; Ortho / Axial project each face on its own.
FttResult unwrapFaces(const core::Brush& b, const std::vector<uint32_t>& ids, UnwrapMode mode, const InfoFn& info);

// ---- direct manipulation -----------------------------------------------------------------------------------------------------------------------------
enum class Handle { None, Move, NW, N, NE, E, SE, S, SW, W, Rotate };
constexpr Handle kScaleHandles[] = {Handle::NW, Handle::N, Handle::NE, Handle::E, Handle::SE, Handle::S, Handle::SW, Handle::W};
/// Where a handle sits in UV space (the rotate handle is above the top edge by `rotateGap`).
V2 handlePoint(const UvBounds& bounds, Handle h, double rotateGap = 0);
struct DragOpts {
    double snap = 0;      // snap step in repeats for move and scale; 0 = none
    bool shift = false;   // keep the aspect ratio of a scale; rotate in 15 degree steps
};
/// The transform a drag means. `start` = pointer in UV space when the drag began, `cur` = now, `bounds` = the island's bounds at the start. Scale handles keep the opposite
/// edge / corner fixed.
Xform dragXform(Handle kind, const V2& start, const V2& cur, const UvBounds& bounds, const DragOpts& o);
/// Which handle (or the body) is under a UV point; `tol` is the pick radius in UV units. Handle::None when nothing.
Handle hitTest(const UvBounds& bounds, const std::vector<std::vector<V2>>& polys, const V2& p, double tol, double rotateGap);
bool pointInPolygon(const V2& p, const std::vector<V2>& poly);
/// The polygon edge nearest to a UV point within `tol`, as its two UV endpoints (for Align edge).
std::optional<std::array<V2, 2>> nearestEdge(const std::vector<std::vector<V2>>& polys, const V2& p, double tol);

} // namespace native::tools::tex
