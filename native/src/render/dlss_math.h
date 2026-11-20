// render/dlss_math -- the small pure maths of the DLSS Super Resolution path: jitter sequence, jittered projection, texture LOD bias, render size.
//
// Owns: nothing that needs a GPU, so the tests check it directly. Matrices are the column-major double Mat4 of render/math3d.h (Vulkan clip space,
// y down); jitter offsets are in pixels of the render-resolution image, x to the right, y down. The scene is drawn with its content shifted by
// (+jx, +jy) pixels, and the same (jx, jy) goes to DLSS as `jitterOffset` (NVIDIA's convention: the Streamline sample adds 2 jx / w to the projection).
// Thread: any. Spec: 11_NVIDIA_TECH.md §2.2, 08_MILESTONES.md T6.8.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "render/math3d.h"
#include "render/viewport_settings.h"

namespace native::render {

// Quality renders at 2/3 of the output size in each direction (NVIDIA's "Quality" scale 0.667), DLAA at the full size.
inline double dlssScale(DlssMode m) { return m == DlssMode::Quality ? 2.0 / 3.0 : 1.0; }

inline void dlssRenderSize(DlssMode m, uint32_t outW, uint32_t outH, uint32_t* renderW, uint32_t* renderH) {
    const double s = dlssScale(m);
    *renderW = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(outW * s)));
    *renderH = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(outH * s)));
}

// Radical inverse of `index` in `base` (the Halton sequence, index from 1).
inline double halton(uint32_t index, uint32_t base) {
    double f = 1, r = 0;
    while (index > 0) {
        f /= base;
        r += f * (index % base);
        index /= base;
    }
    return r;
}

// NVIDIA's recommendation: 8 * (output / render)^2 phases.
inline uint32_t dlssPhaseCount(uint32_t renderW, uint32_t outW) {
    const double ratio = static_cast<double>(outW) / std::max<uint32_t>(1, renderW);
    return std::max<uint32_t>(8, static_cast<uint32_t>(std::lround(8.0 * ratio * ratio)));
}

// The jitter of frame `frame` in pixels, within [-0.5, 0.5).
inline void dlssJitter(uint32_t frame, uint32_t phases, double* jx, double* jy) {
    const uint32_t i = frame % std::max<uint32_t>(1, phases) + 1;
    *jx = halton(i, 2) - 0.5;
    *jy = halton(i, 3) - 0.5;
}

// The view-projection with the content shifted by (jx, jy) pixels of a renderW x renderH image: clip.xy += (2 jx / w, 2 jy / h) * clip.w.
inline Mat4 jitteredViewProj(const Mat4& viewProj, double jx, double jy, double renderW, double renderH) {
    Mat4 j = mat4Identity();
    j[12] = 2.0 * jx / renderW;   // column 3, row 0: x' = x + w * jxn
    j[13] = 2.0 * jy / renderH;
    return mat4Multiply(j, viewProj);
}

// Textures are sampled at the render resolution; DLSS wants the detail of the output resolution, so the LOD moves down by log2(render / output) - 1
// (NVIDIA: "a bias of log2(renderWidth / displayWidth) - 1.0"). 0 when nothing is upscaled.
inline float dlssMipBias(uint32_t renderW, uint32_t outW) {
    if (renderW == 0 || outW == 0 || renderW >= outW) return 0.0f;
    return static_cast<float>(std::log2(static_cast<double>(renderW) / outW) - 1.0);
}

} // namespace native::render
