// tools/measure -- the Measure tool (10_UI_AND_INPUT.md U13): the distance between two points, its per-axis deltas and the length along the ground (Y is up), and the text the status
// bar shows. Pure.
// Thread: any (pure). Spec: 08_MILESTONES.md M11 item 11.
#pragma once

#include <string>

#include "core/math3.h"

namespace native::tools {

struct Measurement {
    double distance = 0;     // straight line
    double dx = 0, dy = 0, dz = 0;   // b - a
    double ground = 0;       // along the ground (X and Z)
};

Measurement measure(const core::Vec3& a, const core::Vec3& b);

// "5.39 m   dX 3.00   dY 2.00 (up)   dZ 4.00   ground 5.00 m": signed deltas, two decimals (trailing zeros are kept so the columns do not jump while moving).
std::string measureText(const Measurement& m);

} // namespace native::tools
