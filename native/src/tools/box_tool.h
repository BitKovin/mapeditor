// tools/box_tool -- the maths of the box-draw tool (port of shell/src/tools/boxTool.ts). Interaction: press on a surface (or an imaginary plane when the ray hits
// nothing) and drag to draw a rectangle on that plane; release, move the pointer to set the height along the plane's normal, click to finish. Shift keeps the
// proportions (a square, then a cube), grid snapping is optional.
// Thread: any (pure).
// Spec: 08_MILESTONES.md T4.2, DESIGN section 7.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/brush.h"
#include "render/math3d.h"
#include "render/picking.h"

namespace native::tools {

using render::Ray;
using render::Vec3;

struct DrawPlane {
    Vec3 origin, normal, u, v;
};

// Plane through `point` with `normal`. Axis-aligned normals get world-axis basis vectors.
DrawPlane planeThrough(const Vec3& point, const Vec3& normal);
// The plane to draw on when the ray hits no geometry: the ground (y = 0) if the ray reaches it within `maxDistance`, else a horizontal plane `distance` out.
DrawPlane fallbackPlane(const Ray& ray, double distance = 10, double maxDistance = 200);
// Where the ray meets the plane in front of it.
std::optional<Vec3> rayPlane(const Ray& ray, const DrawPlane& plane);
// Snaps a point on `plane` to the world grid measured along the plane's own axes from the world origin; the offset along the normal is left alone.
Vec3 snapToPlane(const Vec3& p, const DrawPlane& plane, double step);

struct Rect {
    Vec3 origin;
    double du = 0, dv = 0;
};
// The rectangle from the drag start to the current point; `square` (Shift) forces |du| = |dv|.
Rect rectFromDrag(const DrawPlane& plane, const Vec3& start, const Vec3& current, bool square);
// Height of phase 2: `raw` = signed distance of the pointer along the normal; `cube` makes it the square's side.
double heightFor(double raw, const Rect& rect, bool cube, double step);

constexpr double kMinSize = 1e-4;

// An oriented box brush: the rectangle extruded `height` along the plane normal. nullopt when degenerate.
std::optional<core::Brush> buildBrush(const std::string& id, const DrawPlane& plane, const Rect& rect, double height, const std::string& texture = "dev/grey");
// Line endpoints (x y z, two points a line) previewing the rectangle, or the whole box once `height` is set.
std::vector<float> previewLines(const DrawPlane& plane, const Rect& rect, std::optional<double> height);

// A fresh id "<prefix>_<8 hex digits>" (like the Shell's newId).
std::string newId(const std::string& prefix);

} // namespace native::tools
