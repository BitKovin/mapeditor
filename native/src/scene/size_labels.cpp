// scene/size_labels -- see size_labels.h.
#include "scene/size_labels.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace native::scene {

namespace {

constexpr double kPx = 14;   // how far a label sits outside its edge, in pixels

double len(const V3& v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
V3 sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 add(const V3& a, const V3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 scale(const V3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
V3 normalize(const V3& a) {
    const double l = len(a);
    return l < 1e-12 ? V3{0, 0, 0} : scale(a, 1 / l);
}
V3 toV3(const core::Vec3& v) { return {v.x, v.y, v.z}; }

} // namespace

std::string fmtSize(double metres) {
    // +m.toFixed(3): three decimals, trailing zeros dropped
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.3f", metres);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s + " m";
}

std::vector<SizeLabel> withoutOverlaps(const std::vector<SizeLabel>& labels, const ScreenOf& screen) {
    struct Kept {
        const SizeLabel* label;
        double x, y, w;
    };
    std::vector<Kept> kept;
    for (const SizeLabel& l : labels) {
        const auto s = screen(l.at);
        if (!s) continue;
        const double w = static_cast<double>(l.text.size()) * 7 + 10;
        bool overlaps = false;
        for (const Kept& k : kept)
            if (std::fabs(k.x - s->x) * 2 < k.w + w && std::fabs(k.y - s->y) < 18) {
                overlaps = true;
                break;
            }
        if (!overlaps) kept.push_back({&l, s->x, s->y, w});
    }
    std::vector<SizeLabel> out;
    out.reserve(kept.size());
    for (const Kept& k : kept) out.push_back(*k.label);
    return out;
}

std::vector<SizeLabel> boxSizeLabels(const V3& mn, const V3& mx, const ScreenOf& screen, const MetresPerPixel& mpp, int skipAxis) {
    const V3 c{(mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, (mn[2] + mx[2]) / 2};
    const auto sc = screen(c);
    if (!sc) return {};
    std::vector<SizeLabel> out;
    for (size_t i = 0; i < 3; ++i) {
        const double size = mx[i] - mn[i];
        if (static_cast<int>(i) == skipAxis || size < 1e-6) continue;
        const size_t j = (i + 1) % 3, k = (i + 2) % 3;
        bool have = false;
        V3 bestAt{0, 0, 0};
        double bestScore = 0;
        for (const double a : {mn[j], mx[j]})
            for (const double b : {mn[k], mx[k]}) {
                V3 m{0, 0, 0};
                m[i] = c[i];
                m[j] = a;
                m[k] = b;
                const auto s = screen(m);
                if (!s) continue;
                // furthest from the centre on screen; ties (orthographic views) go to the bottom, then the left
                const double score = std::hypot(s->x - sc->x, s->y - sc->y) + s->y * 1e-6 - s->x * 1e-9;
                if (!have || score > bestScore + 1e-9) {
                    have = true;
                    bestScore = score;
                    bestAt = m;
                }
            }
        if (!have) continue;
        V3 away = sub(bestAt, c);
        away[i] = 0;
        const V3 d = len(away) > 1e-12 ? normalize(away) : V3{0, 0, 0};
        out.push_back({fmtSize(size), add(bestAt, scale(d, kPx * mpp(bestAt)))});
    }
    return withoutOverlaps(out, screen);
}

std::vector<SizeLabel> faceSizeLabels(const core::Brush& b, const core::BrushFace& f, const MetresPerPixel& mpp) {
    std::vector<V3> pts;
    for (uint32_t i : f.loop) {
        if (i >= b.vertices.size()) return {};
        pts.push_back(toV3(b.vertices[i]));
    }
    if (pts.size() < 3) return {};
    V3 u{1, 0, 0};
    double longest = -1;
    for (size_t i = 0; i < pts.size(); ++i) {
        const V3 e = sub(pts[(i + 1) % pts.size()], pts[i]);
        const double l = len(e);
        if (l > longest) {
            longest = l;
            u = scale(e, 1 / l);
        }
    }
    const V3 n = toV3(b.faceNormal(f));
    const V3 v = normalize(cross(n, u));
    double u0 = 1e300, u1 = -1e300, v0 = 1e300, v1 = -1e300;
    for (const V3& p : pts) {
        const double a = dot(p, u), c = dot(p, v);
        u0 = std::min(u0, a); u1 = std::max(u1, a);
        v0 = std::min(v0, c); v1 = std::max(v1, c);
    }
    const double d0 = dot(pts[0], n);
    const auto point = [&](double a, double c) { return add(add(scale(u, a), scale(v, c)), scale(n, d0)); };
    std::vector<SizeLabel> out;
    const V3 bottom = point((u0 + u1) / 2, v0), right = point(u1, (v0 + v1) / 2);
    if (u1 - u0 > 1e-6) out.push_back({fmtSize(u1 - u0), add(bottom, scale(v, -kPx * mpp(bottom)))});
    if (v1 - v0 > 1e-6) out.push_back({fmtSize(v1 - v0), add(right, scale(u, kPx * mpp(right)))});
    return out;
}

} // namespace native::scene
