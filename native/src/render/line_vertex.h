// render/line_vertex -- one vertex of the coloured overlay lines (selection outline, rubber band).
#pragma once

#include <algorithm>
#include <cstdint>

namespace native::render {

struct LineVertex {
    float pos[3];
    uint32_t rgba;   // r | g << 8 | b << 16 | a << 24
};
static_assert(sizeof(LineVertex) == 16);

inline uint32_t packColor(float r, float g, float b, float a = 1.0f) {
    const auto u = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return u(r) | (u(g) << 8) | (u(b) << 16) | (u(a) << 24);
}

} // namespace native::render
