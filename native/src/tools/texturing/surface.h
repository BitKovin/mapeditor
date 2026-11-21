// tools/texturing/surface -- the Surface Inspector's model (port of shell/src/model/surface.ts and the projection parts of uv.ts): pure functions over a face's
// `Projection` (core::texplan; scale = metres per repeat, shift in repeats, rotate in degrees, shear, kind + basis). Shift is shown in TEXELS, stretch as a multiplier of
// the texture's natural size. Every function returns a new projection (or face); the actions apply them to every target face and write `uvs` with the projection.
// The texture's size comes from an `InfoFn` (the texture environment); without it a texture counts as 64 x 64 px / 1 m.
// Thread: any (pure). Spec: 07_EDITOR_PORT.md B (model/surface.ts), 08_MILESTONES.md M11 item 3.
#pragma once

#include <core/brush.h>
#include <core/texplan.h>

#include <optional>
#include <string>
#include <vector>

namespace native::tools::tex {

using core::texplan::InfoFn;
using core::texplan::Projection;
using core::texplan::V2;
using core::texplan::V3;

enum class JustifyEdge { Left, Right, Top, Bottom, Center };

// ---- the face's projection ---------------------------------------------------------------------------------------------------------------------------
/// The projection of a face without parameters: the texture at its natural size (the fixed 2 m while the texture is not loaded).
Projection defaultProjectionFor(const std::string& texture, const InfoFn& info);
/// The face's projection as the Shell reads it (`look.projection` with the defaults of its texture).
Projection projectionOfFace(const core::BrushFace& f, const InfoFn& info);
bool hasProjection(const core::BrushFace& f);
/// The raw `projection` object of a projection: scale, shift, rotate, and shear / kind / basis when set.
nlohmann::json projectionJson(const Projection& p);

/// In-plane coordinates (right, down the image) of a world point under the projection's kind, then rotated and sheared (metres, before scale / shift).
V2 placedCoords(const V3& n, const V3& q, const Projection& p);
/// The UVs of the corners of a face under a projection.
std::vector<V2> projectFace(const core::Brush& b, const core::BrushFace& f, const Projection& p);
/// The face with projection `p`: `look.projection` and the typed projection are set and `uvs` hold the result (so Core and the exporters never need the projection).
core::BrushFace withProjection(const core::Brush& b, const core::BrushFace& f, const Projection& p);
/// The same with the corner UVs given (they may be a map that is not exactly the projection's, e.g. unfolded by Flow); `p` is stored so the inspector can edit the face.
core::BrushFace withProjectionUvs(const core::BrushFace& f, const Projection& p, const std::vector<V2>& uvs);
/// Recomputes BrushFace::lookHash from the face's `look` and `uvs` (what the loader does), after anything of them changed.
void refreshLookHash(core::BrushFace& f);

// ---- the inspector's fields --------------------------------------------------------------------------------------------------------------------------
struct SurfaceBasis {
    V2 texels{64, 64};    // texture size in pixels
    V2 natural{1, 1};     // natural size in metres
};
SurfaceBasis surfaceBasis(const core::BrushFace& f, const InfoFn& info);
V2 shiftTexels(const Projection& p, const V2& texels);
V2 stretchOf(const Projection& p, const V2& natural);
Projection withShiftTexels(const Projection& p, int axis, double texels, const V2& size);
/// Sets one axis' stretch; 0 (or a non-finite value) is refused (the projection is returned as it was).
Projection withStretch(const Projection& p, int axis, double stretch, const V2& natural);
Projection mirrorAxis(const Projection& p, int axis);
Projection withShear(const Projection& p, double shear);

// ---- placement ---------------------------------------------------------------------------------------------------------------------------------------
struct Bounds { V2 min, max; };
/// Extent of the face along the projection's placed axes (rotation, shear, kind) in metres.
Bounds faceBounds(const core::Brush& b, const core::BrushFace& f, const Projection& p);
/// Shifts the texture so its left / right / top / bottom edge touches the same edge of the face, or centres it on both axes. Stretch and rotation are kept.
Projection justify(const core::Brush& b, const core::BrushFace& f, const Projection& p, JustifyEdge edge);
/// Moves the texture so its bottom-left corner touches the face's bottom-left corner (stretch and rotation kept).
Projection bottomLeft(const core::Brush& b, const core::BrushFace& f, const Projection& p);
/// Width / Height / Fit: stretches the texture so `count` repeats cover the face along the axis (other axis and rotation kept); a fractional count leaves the partial repeat
/// at the top / right.
Projection fitAxis(const core::Brush& b, const core::BrushFace& f, const Projection& p, int axis, double count = 1);
Projection fitBoth(const core::Brush& b, const core::BrushFace& f, const Projection& p, const V2& counts);
/// Match Grid: every repeat is a whole number of grid cells wide (at least one).
Projection matchGrid(const Projection& p, double grid);

// ---- projection kinds --------------------------------------------------------------------------------------------------------------------------------
/// Project > Ortho: the texture axes lie in the face (v down the steepest slope, u across it); keeps stretch and mirroring, restarts rotation / shift / shear.
Projection projectOrtho(const core::Brush& b, const core::BrushFace& f, const Projection& p);
/// Project > Cam: the camera's right / up projected into the face, frozen now. Nullopt when the camera looks along the face.
std::optional<Projection> projectCam(const core::Brush& b, const core::BrushFace& f, const Projection& p, const V3& camRight, const V3& camUp);
/// Project > Axial: the standard projection along the dominant axis again (rotation and shift cleared), the face's stretch kept.
Projection projectAxial(const Projection& p);
/// Reset: the texture at its natural size, unrotated, its bottom-left corner on the face's bottom-left corner.
Projection resetSurface(const core::Brush& b, const core::BrushFace& f, const InfoFn& info);
/// The projection parameters of an arbitrary (u, v) map on a plane: a frozen in-plane frame (kind Free) plus scale / shift / rotate / shear. `gu` / `gv` are the gradients of u
/// and v, `du` / `dv` their values at the world origin. Nullopt when the map is degenerate.
std::optional<Projection> projectionFromAffine(const V3& n, const V3& gu, const V3& gv, double du, double dv);

// ---- baked default -----------------------------------------------------------------------------------------------------------------------------------
/// A face with no projection yet whose texture size is known gets one: natural size, bottom-left corner on the face's bottom-left corner. Otherwise the face is returned as it is.
core::BrushFace withBottomLeftDefault(const core::Brush& b, const core::BrushFace& f, const InfoFn& info);
core::Brush alignBrush(const core::Brush& b, const InfoFn& info);

} // namespace native::tools::tex
