// tools/brush_ops -- Radiant-style whole-brush operations on convex brushes (port of shell/src/model/brushOps.ts): clip, hollow, merge (convex hull), snap to the grid,
// intersection and union volume. Also the pure helpers of the clipper tool (shell/src/tools/clipperPoints.ts) and "select inside / touching" (model/selectVolume.ts).
// Pure functions of core::Brush; face ids that survive an operation are kept, the cap of a clip gets a fresh id.
// Thread: any. Spec: 08_MILESTONES.md M11 item 2 (T11.2), 07_EDITOR_PORT.md B.
#pragma once

#include <core/brush.h>

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "render/camera.h"

namespace native::tools {

struct Plane {
    core::Vec3 n;
    double d = 0;   // points p with n.p = d; "front" is n.p > d
};
// The plane through three points oriented by the winding (counter-clockwise seen from the front); none for collinear points.
std::optional<Plane> planeFromPoints(const core::Vec3& a, const core::Vec3& b, const core::Vec3& c);
Plane facePlane(const core::Brush& b, const core::BrushFace& f);

enum class ClipKind { Unchanged, Clipped, Empty };
struct ClipResult {
    ClipKind kind = ClipKind::Empty;
    core::Brush brush;   // Unchanged: the input; Clipped: the part behind the plane (plus a cap face); Empty: unset
    bool has() const { return kind != ClipKind::Empty; }
};
// Keeps the part of the convex brush BEHIND `pl` (n.p <= d). Unchanged when nothing is cut away, Empty when nothing remains (or the result is not a proper solid).
ClipResult clipBrush(const core::Brush& b, const Plane& pl);

enum class Keep { Back, Front, Both };
struct ClipPair {
    std::optional<core::Brush> back, front;
};
ClipPair clipBy(const core::Brush& b, const Plane& pl, Keep keep);

// "Make Hollow": one wall brush per face, `t` thick, not overlapping. None when the walls would meet (nothing is left inside) or t <= 0.
std::optional<std::vector<core::Brush>> hollowBrush(const core::Brush& b, double t);
// The convex hull of the points as a brush (faces take `texture`); none for fewer than 4 non-coplanar points (or more than 120).
std::optional<core::Brush> hullBrush(const std::vector<core::Vec3>& points, const std::string& texture = "dev/grey");
// Snaps every vertex to the grid; none when that would break the solid (a vertex of a slanted face may leave its plane).
std::optional<core::Brush> snapBrush(const core::Brush& b, double step);
// Every vertex lies on or behind every face plane.
bool isConvex(const core::Brush& b);
// The intersection of two convex brushes (none when they only touch or are apart).
std::optional<core::Brush> intersectBrushes(const core::Brush& a, const core::Brush& b);
// Exact volume of the union of convex brushes (inclusion-exclusion over their intersections; up to 12 brushes, more are summed).
double unionVolume(const std::vector<core::Brush>& brushes);

// ---- select inside / touching (Radiant) ----
// Every vertex of `b` lies on or inside the convex `volume`.
bool brushInside(const core::Brush& volume, const core::Brush& b);
// The convex solids overlap or touch (separating axis test over face normals and edge pairs; contact counts).
bool brushesTouch(const core::Brush& a, const core::Brush& b);

// ---- clipper points ----
// Where the ray meets the plane through `planePoint` with normal `n`; none when parallel or behind the origin.
std::optional<core::Vec3> rayPlaneHit(const render::Ray& ray, const core::Vec3& planePoint, const core::Vec3& n);
// Snaps every coordinate to the grid except `keepAxis` (0..2; -1 = none), the depth axis of an orthographic drag.
core::Vec3 snapPoint(const core::Vec3& p, double step, int keepAxis = -1);
// Index of the point whose screen position is within `radius` px of (mx, my), the nearest first; none if there is none. `screen` gives each point's position (none = behind the camera).
std::optional<size_t> hitClipPoint(const std::vector<core::Vec3>& pts, const std::function<std::optional<std::pair<double, double>>(const core::Vec3&)>& screen, double mx, double my, double radius = 10);
// True when `q` coincides with another point than `skip` (a degenerate plane).
bool clashes(const std::vector<core::Vec3>& pts, size_t skip, const core::Vec3& q);
// The world axis (0..2) an orthographic view looking along `forward` is aligned with; -1 for a perspective view.
int depthAxis(const std::optional<core::Vec3>& forward);

} // namespace native::tools
