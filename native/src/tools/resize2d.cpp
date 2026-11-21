#include "tools/resize2d.h"

#include <algorithm>
#include <cmath>

namespace native::tools {

ScreenAxis screenAxis(const V3& dir) {
    int a = 0;
    for (int i = 1; i < 3; ++i)
        if (std::abs(dir[static_cast<size_t>(i)]) > std::abs(dir[static_cast<size_t>(a)])) a = i;
    return {a, dir[static_cast<size_t>(a)] >= 0 ? 1 : -1};
}

std::vector<Handle2D> handles2D(const V3& min, const V3& max, const ScreenAxis& right, const ScreenAxis& up) {
    std::vector<Handle2D> out;
    const auto side = [&](const ScreenAxis& ax, int k) {
        const double lo = min[static_cast<size_t>(ax.axis)], hi = max[static_cast<size_t>(ax.axis)];
        return k == 0 ? (lo + hi) / 2 : (k * ax.sign > 0 ? hi : lo);
    };
    for (int v : {1, 0, -1})
        for (int h : {-1, 0, 1}) {
            if (h == 0 && v == 0) continue;
            V3 pos{(min[0] + max[0]) / 2, (min[1] + max[1]) / 2, (min[2] + max[2]) / 2};
            pos[static_cast<size_t>(right.axis)] = side(right, h);
            pos[static_cast<size_t>(up.axis)] = side(up, v);
            out.push_back({std::to_string(h) + "," + std::to_string(v), h, v, pos});
        }
    return out;
}

Affine resizeMap(const V3& min, const V3& max, const ScreenAxis& right, const ScreenAxis& up, int h, int v, double dr, double du, double step, double minSize) {
    struct Axis {
        ScreenAxis a;
        int k;
        double d;
    };
    const Axis axes[2] = {{right, h, dr}, {up, v, du}};
    V3 scaleBy{1, 1, 1}, anchor{0, 0, 0};
    for (const Axis& ax : axes) {
        if (ax.k == 0) continue;
        const size_t i = static_cast<size_t>(ax.a.axis);
        const double lo = min[i], hi = max[i];
        const bool movesHigh = ax.k * ax.a.sign > 0;   // the dragged side is the max side of this world axis
        double moved = (movesHigh ? hi : lo) + ax.d * ax.a.sign;   // screen delta -> world delta along the axis
        if (step > 0) moved = std::round(moved / step) * step;
        const double size = movesHigh ? moved - lo : hi - moved;
        const double newSize = std::max(minSize, size);
        anchor[i] = movesHigh ? lo : hi;
        scaleBy[i] = hi - lo > 1e-9 ? newSize / (hi - lo) : 1;
    }
    Affine a;
    a.m = {scaleBy[0], 0, 0, 0, scaleBy[1], 0, 0, 0, scaleBy[2]};
    for (size_t i = 0; i < 3; ++i) a.t[i] = scaleBy[i] == 1 ? 0 : anchor[i] * (1 - scaleBy[i]);
    return a;
}

} // namespace native::tools
