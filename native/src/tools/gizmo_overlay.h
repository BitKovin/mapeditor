// tools/gizmo_overlay -- the transform gizmo as coloured lines (the drawing half of shell/src/render/renderer.ts: axis arrows, rotation rings, scale handles).
// Thread: any (pure).
// Spec: 08_MILESTONES.md T4.1.
#pragma once

#include <vector>

#include "render/line_vertex.h"
#include "render/viewport_settings.h"
#include "tools/gizmo.h"

namespace native::tools {

// The gizmo for `mode` at `pivot` with handles of `length` metres; `active` (-1 = none) is the axis under the cursor or being dragged (drawn yellow).
// Two vertices a line.
std::vector<render::LineVertex> gizmoLines(const Vec3& pivot, double length, render::GizmoMode mode, Axis active);

// The diamond at the tip of a selected point entity's direction arrow (REQUIREMENTS 33 H2); while dragged it is white and the line from the arrow's base is drawn.
std::vector<render::LineVertex> angleHandleLines(const Vec3& base, const Vec3& tip, const Vec3& right, const Vec3& up, double half, bool active);
// The origin handle of a brush entity with a custom origin: a square of half-size `half` metres in the view plane (`right`, `up`), plus a cross; white when
// `active` (the origin is the thing being edited), else amber.
std::vector<render::LineVertex> originHandleLines(const Vec3& origin, const Vec3& right, const Vec3& up, double half, bool active);

} // namespace native::tools
