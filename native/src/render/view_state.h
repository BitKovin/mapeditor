// render/view_state -- what the render thread needs to draw one viewport's view (filled by CameraRig, consumed by the passes).
// Qt-free, GPU-free. Spec: 05_RENDERER.md §3, 08_MILESTONES.md T1.4.
#pragma once

#include "render/math3d.h"

namespace native::render {

// Everything the render thread needs to draw one viewport's view.
struct ViewState {
    Mat4 viewProj = mat4Identity();
    double gridSpacing = 1;          // metres between minor grid lines (adaptive)
    int gridPlane = 0;               // 0 = XZ (ground), 1 = XY (front), 2 = ZY (side)
    double gridCenterA = 0, gridCenterB = 0;   // in-plane coordinates the grid is centred on
    bool showGrid = true;
    bool showAxes = true;
};

} // namespace native::render
