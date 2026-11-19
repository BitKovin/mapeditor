// tools/gizmo -- the math of the transform gizmo (port of shell/src/render/gizmo.ts): hit-testing the three axis handles and rotation rings and turning a mouse
// ray into a distance along an axis or an angle about it. Drawing lives in tools/gizmo_overlay.
// Thread: any (pure).
// Spec: 08_MILESTONES.md T4.1, DESIGN section 8.
#pragma once

#include <optional>
#include <utility>
#include <vector>

#include "render/camera.h"
#include "render/math3d.h"

namespace native::tools {

using render::Ray;
using render::Vec3;

using Axis = int;   // 0 = x, 1 = y, 2 = z
constexpr Vec3 axisDir(Axis a) { return a == 0 ? Vec3{1, 0, 0} : (a == 1 ? Vec3{0, 1, 0} : Vec3{0, 0, 1}); }

// Signed distance along the line (origin, dir) of the point closest to the ray; nullopt when the ray is parallel to the line.
std::optional<double> lineDistance(const Ray& ray, const Vec3& origin, const Vec3& dir);
// Same along a world axis from `pivot`.
std::optional<double> axisDistance(const Ray& ray, const Vec3& pivot, Axis axis);
// Which axis handle (length `len`, pick radius `tol`, both in metres) is under the ray; -1 = none.
Axis hitAxis(const Ray& ray, const Vec3& pivot, double len, double tol);
// Is the single handle from `pivot` along unit `dir` (length `len`) under the ray?
bool hitDir(const Ray& ray, const Vec3& pivot, const Vec3& dir, double len, double tol);
// The two unit vectors the ring about `axis` is drawn in (u x v = the axis, so angles are right-handed about it).
std::pair<Vec3, Vec3> ringBasis(Axis axis);
// Which rotation ring (radius `radius`, tolerance `tol`) is under the ray: the one whose circle the ray passes closest to; -1 = none.
Axis hitRing(const Ray& ray, const Vec3& pivot, double radius, double tol);
// Angle (radians) of the ray's hit on the ring plane, measured about `axis`; nullopt when parallel / behind.
std::optional<double> ringAngle(const Ray& ray, const Vec3& pivot, Axis axis);
// Smallest signed difference between two angles.
double angleDelta(double a, double b);
// The ring as line endpoints (x, y, z per point, two points a line).
std::vector<double> ringLines(const Vec3& pivot, Axis axis, double radius, int segments = 48);
// Handle length that keeps the gizmo a constant apparent size.
inline double gizmoLength(double cameraDistance) { return cameraDistance * 0.18; }
// Movement along `axis` for a drag that started at `startT`, snapped to `step` (0 = free).
Vec3 dragDelta(const Ray& ray, const Vec3& pivot, Axis axis, double startT, double step);
double snapTo(double v, double step);

} // namespace native::tools
