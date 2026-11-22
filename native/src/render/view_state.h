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
    double eye[3] = {0, 0, 0};       // camera position (sky faces are drawn by view direction)
    bool perspective = true;         // false: an orthographic view (no sky background)
    bool backfaceCulling = true;     // translucent surfaces and volumes show their front faces only (the Shell's default; both sides blended look like a mess)
    int renderMode = 0;              // 0 = Unlit / Wireframe, 1 = Lit, 2 = Lighting only (the lit modes ask the bake for light)
    // DLSS Super Resolution (T6.8): the viewport's mode (a DlssMode as int, 0 = off) and what the upscaler needs to know about the camera.
    int dlss = 0;
    Mat4 proj = mat4Identity();      // the unjittered projection (perspective views only)
    double fovY = 0, nearPlane = 0, farPlane = 0;
    double right[3] = {1, 0, 0}, up[3] = {0, 1, 0}, forward[3] = {0, 0, -1};
    unsigned cut = 0;                // grows when the camera jumped (frame selection, camera mode change, pose load): the upscaler drops its history
};

} // namespace native::render
