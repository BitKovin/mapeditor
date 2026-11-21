// tools/texturing/density -- see density.h.
#include "tools/texturing/density.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace native::tools::tex {

namespace {
double planeGradient(const V3& g, const V3& n) {
    const double d = g[0] * n[0] + g[1] * n[1] + g[2] * n[2];
    const V3 p{g[0] - n[0] * d, g[1] - n[1] * d, g[2] - n[2] * d};
    return std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
}
std::array<double, 3> lerp(const std::array<double, 3>& a, const std::array<double, 3>& b, double k) {
    return {a[0] + (b[0] - a[0]) * k, a[1] + (b[1] - a[1]) * k, a[2] + (b[2] - a[2]) * k};
}
} // namespace

V2 faceDensity(const core::Brush& b, const core::BrushFace& f, double width, double height, const InfoFn& info) {
    const core::texplan::UvAffine m = core::texplan::faceAffine(b, f, info);
    const V3 n = core::texplan::faceNormalOf(b, f);
    const double gu = planeGradient({m.u[0], m.u[1], m.u[2]}, n), gv = planeGradient({m.v[0], m.v[1], m.v[2]}, n);
    return {width * gu, height * gv};
}

std::optional<double> faceHeat(const core::Brush& b, const core::BrushFace& f, const core::texplan::TexInfo* texture, const InfoFn& info) {
    if (!texture) return std::nullopt;
    const V2 d = faceDensity(b, f, texture->width, texture->height, info);
    const double lo = std::min(d[0], d[1]);
    if (!(lo > 1e-9)) return -kHeatRange;
    return std::log2(lo / texture->ppm);
}

std::array<double, 3> heatColor(double e) {
    const double t = std::max(-1.0, std::min(1.0, e / kHeatRange));   // -1 stretched .. +1 dense
    const std::array<double, 3> red{0.9, 0.12, 0.1}, green{0.15, 0.8, 0.2}, blue{0.15, 0.3, 0.95};
    return t < 0 ? lerp(green, red, -t) : lerp(green, blue, t);
}

int heatBucket(double e) {
    const double c = std::max(-kHeatRange, std::min(kHeatRange, e));
    return static_cast<int>(std::lround((c + kHeatRange) / (2 * kHeatRange) * (kHeatBuckets - 1)));
}

std::array<double, 3> bucketColor(int i) { return heatColor(static_cast<double>(i) / (kHeatBuckets - 1) * 2 * kHeatRange - kHeatRange); }

std::string heatLabel(double e) {
    char buf[96];
    if (std::fabs(e) < 0.15) return "as authored";
    if (e < 0) std::snprintf(buf, sizeof buf, "stretched %.1fx (blurry)", std::pow(2, -e));
    else std::snprintf(buf, sizeof buf, "squeezed %.1fx (denser than needed)", std::pow(2, e));
    return buf;
}

} // namespace native::tools::tex
