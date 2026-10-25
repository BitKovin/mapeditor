#pragma once
#include <cstdint>
#include <vector>

namespace core {

// Fitting a baked lightmap chart to fewer texels (export only). A chart that is flat, a plain gradient or just smooth does not need the density it was
// baked at: the engine reads a lightmap with a bicubic B-spline (4 bilinear fetches), so a few texels reproduce it. The size is searched by trying fewer
// texels per axis and comparing, texel by texel, what the engine would show: B(original) against B(smaller), as 8-bit sRGB after the exposure.

// One light style layer of one chart: the interior of the chart's rectangle in a page (the padding is not part of it).
struct FitLayer {
    const float* rgb = nullptr;      // the page: atlas * atlas * 3
    const uint8_t* valid = nullptr;  // the page: atlas * atlas; 1 = baked, 2 = gutter, 0 = empty (null = everything baked)
    int atlas = 0;
    int x0 = 0, y0 = 0;              // first interior texel
    int w = 0, h = 0;                // interior size
};

struct FitSettings {
    double exposure = 1;      // the display multiplier the error is judged after (the level's _cc_exposure)
    double tolerance = 2.5;   // allowed error in 8-bit sRGB levels: 99% of the texels within it, every texel within 2.4x it
    int minSide = 1;          // fewest texels per side
};

struct ChartFit {
    int w = 0, h = 0;         // interior size to store (<= the original)
    uint64_t hash = 0;        // of the stored texels of all layers: equal hashes (and sizes) are the same light
};

// The smallest size at which every layer still looks the same (max over layers). Never larger than the original.
ChartFit fitChart(const std::vector<FitLayer>& layers, const FitSettings& settings);

// Whether every layer, stored at w x h texels (as from resampleLayer) in 8 bits (`srgb`: through the sRGB curve, else linear), still looks the same by the
// fit's measure, and none of it is brighter than 1 (what a JPEG page can hold).
bool fitsEightBits(const std::vector<FitLayer>& layers, int w, int h, const FitSettings& settings, bool srgb);

// The 8-bit value of a light value (clamped to 0..1) and back.
uint8_t lightToByte(float v, bool srgb);
float byteToLight(uint8_t b, bool srgb);

// The value of a small image (w x h x 3, from resampleLayer) at texel (x, y), also outside it: past the edge the image continues linearly (clamped at 0), which
// is what the stored gutter holds, so a plain gradient stays a gradient up to the chart's edge.
float fitTap(const std::vector<float>& img, int w, int h, int x, int y, int channel);

// The layer at w x h texels (area weighted mean of the baked texels each new texel covers; w x h = the original size copies it): w * h * 3 floats.
std::vector<float> resampleLayer(const FitLayer& layer, int w, int h);

// The error of showing `small` (w x h, from resampleLayer) instead of the original: fraction of texels beyond 1/2 of the tolerance bound and the largest
// error, in sRGB levels. Exposed for tests.
struct FitError {
    double over = 0;   // fraction of baked texels whose error exceeds `tolerance`
    double worst = 0;  // largest error in levels
};
FitError fitError(const FitLayer& layer, const std::vector<float>& small, int w, int h, double exposure, double tolerance);

}  // namespace core
