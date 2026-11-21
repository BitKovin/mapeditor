// tools/resize2d -- Radiant-style 2D resize of the selection in an orthographic view (port of shell/src/model/resize2d.ts): eight handles (4 sides, 4 corners) on the
// selection's bounding rectangle. Dragging one moves its side(s) while the opposite side stays fixed; every point is scaled linearly between them, so a rectangle resizes
// and a slanted brush keeps its shape. The map is applied to the document as it was when the drag began (tools/transform: transformSelections).
// Thread: any. Spec: 08_MILESTONES.md M11 item 2 (T11.2), 07_EDITOR_PORT.md B.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "tools/transform.h"

namespace native::tools {

// A screen axis expressed as a world axis: `axis` 0..2 and `sign` +1 when the screen axis points along it.
struct ScreenAxis {
    int axis = 0;
    int sign = 1;
};
struct Handle2D {
    std::string id;   // "h,v"
    int h = 0, v = 0;   // h: -1 left .. 1 right, v: -1 bottom .. 1 top
    V3 pos{0, 0, 0};
};

// The world axis a (unit, axis-aligned) screen direction points along.
ScreenAxis screenAxis(const V3& dir);
// The 8 handles around [min, max], positioned on the plane of the bounds' centre.
std::vector<Handle2D> handles2D(const V3& min, const V3& max, const ScreenAxis& right, const ScreenAxis& up);
// The point map for dragging handle (h, v) by (dr, du) metres along the screen axes. The moved side lands on the grid (`step` 0 = free) and keeps at least `minSize`
// thickness; nothing outside the dragged axes changes. A per-axis scale about the opposite side: an Affine.
Affine resizeMap(const V3& min, const V3& max, const ScreenAxis& right, const ScreenAxis& up, int h, int v, double dr, double du, double step, double minSize = 0.01);
// Pixel offset -> metres along the screen axes (screen y points down).
inline std::array<double, 2> pixelsToMetres(double dxPx, double dyPx, double metresPerPixel) { return {dxPx * metresPerPixel, -dyPx * metresPerPixel}; }

} // namespace native::tools
