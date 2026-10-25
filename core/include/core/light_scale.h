#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace core::lightscale {

// HDR light from 8-bit lightmaps (DESIGN §12 "Light scale"). A lightmap page only holds 0..1 in 8 bits (.jpg), and a flat 0.02 uses 5 of its 255 steps. So a
// chart's texels are stored NORMALISED, divided by one colour `C` per chart (the peak of every channel over all its style layers and texels), and the
// drawvert colour carries `C`:
//
//     light = sum over style slots ( lightmap_slot * styleColor_slot ) * C            C = rgb / 255 * multiplier(A)
//
// rgb (linear, 0..1) is the mantissa, A is a shared exponent: multiplier(A) = 2^(5 * (255 - A) / 255), 1 .. 32. A = 255 is the multiplier 1, so the
// opaque white every older rBSP v2 file carries still means "no scale" and the shader needs no version switch. Peaks above 1 (bright lights) take
// a multiplier above 1 and rgb <= 1; dark charts keep multiplier 1 and a small rgb, so the page always uses its full 8 bits.
//
// C is quantised BEFORE the texels are divided by it (the compiler divides by the decoded value), so the quantisation of C costs no accuracy.
//
// The same encoding holds the light of a vertex lit surface (no lightmap: the engine binds a white lightmap and the light is only in the colour). There
// every vertex has its own rgb and the whole surface shares ONE A, because the GPU interpolates rgb and A separately and the decode (a non linear
// function of A) happens after: with one A per surface the interpolated value is exactly the interpolation of the light.

inline constexpr double kLog2Range = 5.0;                    // multiplier 1 .. 2^5 = 32
inline constexpr double kMaxMultiplier = 32.0;
inline constexpr uint8_t kNeutralAlpha = 255;                // multiplier 1

inline double multiplierOf(uint8_t a) { return std::exp2(kLog2Range * (255 - a) / 255.0); }

/// The smallest exponent byte whose multiplier is >= `peak` (255 when peak <= 1; 0 when peak exceeds 32: `clipped`).
inline uint8_t alphaFor(double peak, bool* clipped = nullptr) {
    if (clipped) *clipped = false;
    if (!(peak > 1.0)) return kNeutralAlpha;
    const double steps = std::ceil(255.0 * std::log2(peak) / kLog2Range - 1e-9);
    if (steps >= 255.0) {
        if (clipped && peak > kMaxMultiplier * (1 + 1e-9)) *clipped = true;
        return 0;
    }
    return static_cast<uint8_t>(255 - static_cast<int>(steps));
}

/// What the shader computes from a vertex colour (as read: bytes).
inline void decode(const uint8_t rgba[4], double out[3]) {
    const double m = multiplierOf(rgba[3]);
    for (int q = 0; q < 3; ++q) out[q] = rgba[q] / 255.0 * m;
}

struct Scale {
    uint8_t rgba[4] = {255, 255, 255, kNeutralAlpha};  // as written to the file
    double value[3] = {1, 1, 1};                        // what it decodes to: the divisor of the chart's texels
    bool clipped = false;                               // the peak was above 32 (the brightest texels will clip)
    uint32_t code() const { return uint32_t(rgba[0]) | uint32_t(rgba[1]) << 8 | uint32_t(rgba[2]) << 16 | uint32_t(rgba[3]) << 24; }
};

/// The colour for a chart whose brightest light is `peak` (per channel, linear): decodes to >= peak wherever the range allows (rounded UP, so the
/// normalised texels stay <= 1), and every channel with light has a non zero byte.
inline Scale encodePeak(const double peak[3]) {
    Scale s;
    const double top = std::max({peak[0], peak[1], peak[2], 0.0});
    s.rgba[3] = alphaFor(top, &s.clipped);
    const double m = multiplierOf(s.rgba[3]);
    for (int q = 0; q < 3; ++q) {
        const double v = std::max(peak[q], 0.0);
        int b = static_cast<int>(std::ceil(v / m * 255.0 - 1e-9));
        if (v > 0 && b < 1) b = 1;
        s.rgba[q] = static_cast<uint8_t>(std::clamp(b, 0, 255));
    }
    decode(s.rgba, s.value);
    return s;
}

/// Vertex lit surface: one exponent for all vertices (from the brightest channel of any of them), and each vertex's own rgb, rounded to nearest.
inline uint8_t sharedAlpha(double brightest, bool* clipped = nullptr) { return alphaFor(brightest, clipped); }
inline void encodeVertex(const double v[3], uint8_t alpha, uint8_t rgba[4]) {
    const double m = multiplierOf(alpha);
    for (int q = 0; q < 3; ++q) rgba[q] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(std::max(v[q], 0.0) / m * 255.0)), 0, 255));
    rgba[3] = alpha;
}

}  // namespace core::lightscale
