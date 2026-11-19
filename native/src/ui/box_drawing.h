// ui/box_drawing -- the state of a box / shape being drawn (T4.2): phase 1 drags the rectangle on a plane, phase 2 chooses the height, a click commits.
// Thread: UI thread.
#pragma once

#include "render/math3d.h"
#include "tools/box_tool.h"

namespace native::render { class ViewportWindow; }

namespace native::ui {

struct BoxDrawing {
    enum class Phase { Rect, Height };
    render::ViewportWindow* viewport = nullptr;
    Phase phase = Phase::Rect;
    tools::DrawPlane plane;
    render::Vec3 start, current;   // phase 1: the drag's start and the point under the pointer on the plane
    tools::Rect rect;              // phase 2: the finished rectangle
    double height = 0;
};

} // namespace native::ui
