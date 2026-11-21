// render/frustum -- the view frustum of a Vulkan clip-space matrix and the box test the draw culling uses (GpuScene::buildDraws, ModelRenderer::buildDraws).
// Thread: any (pure).
#pragma once

#include <array>
#include <cmath>

#include "doc/changeset.h"
#include "render/math3d.h"

namespace native::render {

struct Plane {
    double a, b, c, d;
};

// Frustum planes (inside: a x + b y + c z + d >= 0) of a Vulkan clip-space matrix (column-major, depth 0..1).
inline void frustumPlanes(const Mat4& m, Plane out[6]) {
    const auto row = [&](int r) { return std::array<double, 4>{m[r], m[4 + r], m[8 + r], m[12 + r]}; };
    const auto r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    const auto make = [](const std::array<double, 4>& v) {
        const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        const double k = l > 0 ? 1.0 / l : 1.0;
        return Plane{v[0] * k, v[1] * k, v[2] * k, v[3] * k};
    };
    out[0] = make({r3[0] + r0[0], r3[1] + r0[1], r3[2] + r0[2], r3[3] + r0[3]});   // left
    out[1] = make({r3[0] - r0[0], r3[1] - r0[1], r3[2] - r0[2], r3[3] - r0[3]});   // right
    out[2] = make({r3[0] + r1[0], r3[1] + r1[1], r3[2] + r1[2], r3[3] + r1[3]});   // bottom / top (y is flipped, both are tested)
    out[3] = make({r3[0] - r1[0], r3[1] - r1[1], r3[2] - r1[2], r3[3] - r1[3]});
    out[4] = make(r2);                                                                // near (z >= 0)
    out[5] = make({r3[0] - r2[0], r3[1] - r2[1], r3[2] - r2[2], r3[3] - r2[3]});   // far
}

inline bool boxInFrustum(const Plane planes[6], const doc::Box& b) {
    for (int i = 0; i < 6; ++i) {
        const Plane& p = planes[i];
        const double x = p.a >= 0 ? b.mx.x : b.mn.x;
        const double y = p.b >= 0 ? b.mx.y : b.mn.y;
        const double z = p.c >= 0 ? b.mx.z : b.mn.z;
        if (p.a * x + p.b * y + p.c * z + p.d < 0) return false;
    }
    return true;
}

} // namespace native::render
