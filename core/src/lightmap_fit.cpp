#include "core/lightmap_fit.h"
#include <algorithm>
#include <cmath>

namespace core {

namespace {

inline size_t at(const FitLayer& l, int x, int y) { return (static_cast<size_t>(l.y0 + y) * l.atlas + (l.x0 + x)) * 3; }
inline bool baked(const FitLayer& l, int x, int y) { return !l.valid || l.valid[static_cast<size_t>(l.y0 + y) * l.atlas + (l.x0 + x)] == 1; }

// linear value -> 8-bit sRGB level (float) after the exposure
inline float level(float v, double exposure) {
    const double c = std::clamp(static_cast<double>(v) * exposure, 0.0, 1.0);
    return static_cast<float>((c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055) * 255.0);
}

// cubic B-spline weights of the four taps at ix - 1 .. ix + 2 for the fraction f
inline void splineWeights(double f, double w[4]) {
    const double f2 = f * f, f3 = f2 * f, g = 1 - f;
    w[0] = g * g * g / 6;
    w[1] = (3 * f3 - 6 * f2 + 4) / 6;
    w[2] = (-3 * f3 + 3 * f2 + 3 * f + 1) / 6;
    w[3] = f3 / 6;
}

// What the engine shows at the centre of every texel of an image stored as `img` (w x h x 3): the B-spline at the integer positions, weights 1/6 4/6 1/6.
std::vector<float> splineAtCentres(const std::vector<float>& img, int w, int h) {
    std::vector<float> out(img.size());
    auto px = [&](int x, int y, int c) { return img[(static_cast<size_t>(std::clamp(y, 0, h - 1)) * w + std::clamp(x, 0, w - 1)) * 3 + c]; };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c) {
                float s = 0;
                for (int j = -1; j <= 1; ++j) {
                    const float wy = j == 0 ? 4.f / 6.f : 1.f / 6.f;
                    for (int i = -1; i <= 1; ++i) s += wy * (i == 0 ? 4.f / 6.f : 1.f / 6.f) * px(x + i, y + j, c);
                }
                out[(static_cast<size_t>(y) * w + x) * 3 + c] = s;
            }
    return out;
}

std::vector<float> copyLayer(const FitLayer& l) {
    std::vector<float> out(static_cast<size_t>(l.w) * l.h * 3);
    for (int y = 0; y < l.h; ++y) std::copy_n(l.rgb + at(l, 0, y), static_cast<size_t>(l.w) * 3, out.begin() + static_cast<size_t>(y) * l.w * 3);
    return out;
}

struct Reference {
    std::vector<float> levels;  // sRGB levels of what the engine shows for the original, per texel
    std::vector<char> counts;   // 1 = a baked texel (judged)
    size_t judged = 0;
};

Reference referenceOf(const FitLayer& l, double exposure) {
    Reference r;
    const std::vector<float> shown = splineAtCentres(copyLayer(l), l.w, l.h);
    r.levels.resize(shown.size());
    for (size_t i = 0; i < shown.size(); ++i) r.levels[i] = level(shown[i], exposure);
    r.counts.assign(static_cast<size_t>(l.w) * l.h, 0);
    for (int y = 0; y < l.h; ++y)
        for (int x = 0; x < l.w; ++x)
            if (baked(l, x, y)) { r.counts[static_cast<size_t>(y) * l.w + x] = 1; ++r.judged; }
    return r;
}

FitError compare(const FitLayer& l, const Reference& ref, const std::vector<float>& small, int nw, int nh, double exposure, double tolerance) {
    FitError e;
    if (ref.judged == 0) return e;
    size_t over = 0;
    auto tap = [&](int x, int y, int c) { return fitTap(small, nw, nh, x, y, c); };
    std::vector<double> wx(static_cast<size_t>(l.w) * 4), wy(static_cast<size_t>(l.h) * 4);
    std::vector<int> ix(static_cast<size_t>(l.w)), iy(static_cast<size_t>(l.h));
    for (int x = 0; x < l.w; ++x) {
        const double p = (x + 0.5) * nw / l.w - 0.5;
        ix[x] = static_cast<int>(std::floor(p));
        splineWeights(p - ix[x], &wx[static_cast<size_t>(x) * 4]);
    }
    for (int y = 0; y < l.h; ++y) {
        const double p = (y + 0.5) * nh / l.h - 0.5;
        iy[y] = static_cast<int>(std::floor(p));
        splineWeights(p - iy[y], &wy[static_cast<size_t>(y) * 4]);
    }
    for (int y = 0; y < l.h; ++y)
        for (int x = 0; x < l.w; ++x) {
            const size_t i = static_cast<size_t>(y) * l.w + x;
            if (!ref.counts[i]) continue;
            double worst = 0;
            for (int c = 0; c < 3; ++c) {
                double s = 0;
                for (int j = 0; j < 4; ++j) {
                    double row = 0;
                    for (int k = 0; k < 4; ++k) row += wx[static_cast<size_t>(x) * 4 + k] * tap(ix[x] - 1 + k, iy[y] - 1 + j, c);
                    s += wy[static_cast<size_t>(y) * 4 + j] * row;
                }
                worst = std::max(worst, std::fabs(static_cast<double>(level(static_cast<float>(s), exposure)) - ref.levels[i * 3 + c]));
            }
            e.worst = std::max(e.worst, worst);
            if (worst > tolerance) ++over;
        }
    e.over = static_cast<double>(over) / static_cast<double>(ref.judged);
    return e;
}

bool acceptable(const FitError& e, double tolerance) { return e.over <= 0.01 && e.worst <= tolerance * 2.4; }

}  // namespace

float fitTap(const std::vector<float>& img, int w, int h, int x, int y, int channel) {
    auto line = [&](int yy) {  // row yy (inside), column x (anywhere)
        auto v = [&](int xx) { return img[(static_cast<size_t>(yy) * w + xx) * 3 + channel]; };
        if (x >= 0 && x < w) return v(x);
        if (w < 2) return v(0);
        return x < 0 ? v(0) + (v(0) - v(1)) * static_cast<float>(-x) : v(w - 1) + (v(w - 1) - v(w - 2)) * static_cast<float>(x - w + 1);
    };
    float r;
    if (y >= 0 && y < h) r = line(y);
    else if (h < 2) r = line(0);
    else if (y < 0) r = line(0) + (line(0) - line(1)) * static_cast<float>(-y);
    else r = line(h - 1) + (line(h - 1) - line(h - 2)) * static_cast<float>(y - h + 1);
    return std::max(0.0f, r);
}

std::vector<float> resampleLayer(const FitLayer& l, int nw, int nh) {
    if (nw == l.w && nh == l.h) return copyLayer(l);
    std::vector<double> sum(static_cast<size_t>(nw) * nh * 3, 0.0), wsum(static_cast<size_t>(nw) * nh, 0.0);
    std::vector<double> sumAny(static_cast<size_t>(nw) * nh * 3, 0.0), wAny(static_cast<size_t>(nw) * nh, 0.0);  // gutter / empty texels, used when a cell has no baked one
    {
        for (int y = 0; y < l.h; ++y) {
            const double y0 = static_cast<double>(y) * nh / l.h, y1 = static_cast<double>(y + 1) * nh / l.h;
            for (int x = 0; x < l.w; ++x) {
                const double x0 = static_cast<double>(x) * nw / l.w, x1 = static_cast<double>(x + 1) * nw / l.w;
                const bool good = baked(l, x, y);
                const float* p = l.rgb + at(l, x, y);
                for (int cy = static_cast<int>(y0); cy < nh && cy < y1; ++cy) {
                    const double oy = std::min<double>(cy + 1, y1) - std::max<double>(cy, y0);
                    if (oy <= 1e-12) continue;
                    for (int cx = static_cast<int>(x0); cx < nw && cx < x1; ++cx) {
                        const double ox = std::min<double>(cx + 1, x1) - std::max<double>(cx, x0);
                        if (ox <= 1e-12) continue;
                        const size_t cell = static_cast<size_t>(cy) * nw + cx;
                        const double wgt = ox * oy;
                        auto& s = good ? sum : sumAny;
                        auto& ws = good ? wsum : wAny;
                        for (int c = 0; c < 3; ++c) s[cell * 3 + c] += wgt * p[c];
                        ws[cell] += wgt;
                    }
                }
            }
        }
    }
    std::vector<float> out(static_cast<size_t>(nw) * nh * 3, 0.0f);
    std::vector<char> filled(static_cast<size_t>(nw) * nh, 0);
    for (size_t i = 0; i < filled.size(); ++i) {
        if (wsum[i] > 0) { for (int c = 0; c < 3; ++c) out[i * 3 + c] = static_cast<float>(sum[i * 3 + c] / wsum[i]); filled[i] = 1; }
        else if (wAny[i] > 0) { for (int c = 0; c < 3; ++c) out[i * 3 + c] = static_cast<float>(sumAny[i * 3 + c] / wAny[i]); filled[i] = 1; }
    }
    // cells with nothing at all (no texel covers them): take the mean of the filled neighbours, repeatedly
    for (bool again = true; again;) {
        again = false;
        std::vector<char> next = filled;
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x) {
                const size_t i = static_cast<size_t>(y) * nw + x;
                if (filled[i]) continue;
                double s[3] = {0, 0, 0};
                int n = 0;
                const int dx[4] = {-1, 1, 0, 0}, dy[4] = {0, 0, -1, 1};
                for (int d = 0; d < 4; ++d) {
                    const int qx = x + dx[d], qy = y + dy[d];
                    if (qx < 0 || qy < 0 || qx >= nw || qy >= nh) continue;
                    const size_t q = static_cast<size_t>(qy) * nw + qx;
                    if (!filled[q]) continue;
                    for (int c = 0; c < 3; ++c) s[c] += out[q * 3 + c];
                    ++n;
                }
                if (!n) { again = true; continue; }
                for (int c = 0; c < 3; ++c) out[i * 3 + c] = static_cast<float>(s[c] / n);
                next[i] = 1;
            }
        if (next == filled) break;
        filled = std::move(next);
    }
    return out;
}

FitError fitError(const FitLayer& layer, const std::vector<float>& small, int w, int h, double exposure, double tolerance) {
    return compare(layer, referenceOf(layer, exposure), small, w, h, exposure, tolerance);
}

ChartFit fitChart(const std::vector<FitLayer>& layers, const FitSettings& st) {
    ChartFit fit;
    if (layers.empty()) return fit;
    const int W = layers[0].w, H = layers[0].h;
    fit.w = W;
    fit.h = H;
    const int minSide = std::max(1, st.minSide);
    if (st.tolerance > 0 && W * H > 1) {
        std::vector<Reference> refs;
        for (const FitLayer& l : layers) refs.push_back(referenceOf(l, st.exposure));
        auto ok = [&](int nw, int nh) {
            for (size_t k = 0; k < layers.size(); ++k) {
                const std::vector<float> small = resampleLayer(layers[k], nw, nh);
                if (!acceptable(compare(layers[k], refs[k], small, nw, nh, st.exposure, st.tolerance), st.tolerance)) return false;
            }
            return true;
        };
        if (ok(minSide, minSide)) {
            fit.w = std::min(W, minSide);
            fit.h = std::min(H, minSide);
        } else {
            // fewer texels along one axis at a time: halve, then take a quarter off, until neither is accepted
            bool changed = true;
            while (changed) {
                changed = false;
                for (const double keep : {0.5, 0.75}) {
                    const int nw = std::max(minSide, static_cast<int>(std::ceil(fit.w * keep)));
                    if (nw < fit.w && ok(nw, fit.h)) { fit.w = nw; changed = true; break; }
                    const int nh = std::max(minSide, static_cast<int>(std::ceil(fit.h * keep)));
                    if (nh < fit.h && ok(fit.w, nh)) { fit.h = nh; changed = true; break; }
                }
            }
        }
    }
    // the hash of what is stored: the texels of every layer, rounded to 1/16384 (identical light only: flat black, flat ambient, ...)
    uint64_t hsh = 1469598103934665603ull ^ (static_cast<uint64_t>(fit.w) << 20) ^ static_cast<uint64_t>(fit.h);
    for (const FitLayer& l : layers) {
        const std::vector<float> data = resampleLayer(l, fit.w, fit.h);
        for (float v : data) hsh = (hsh ^ static_cast<uint64_t>(static_cast<int64_t>(std::lround(static_cast<double>(v) * 16384.0)))) * 1099511628211ull;
        hsh = (hsh ^ 0xFFull) * 1099511628211ull;
    }
    fit.hash = hsh;
    return fit;
}

}  // namespace core
