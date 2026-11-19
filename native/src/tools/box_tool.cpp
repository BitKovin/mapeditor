// tools/box_tool -- see box_tool.h.
#include "tools/box_tool.h"

#include <cmath>
#include <cstdio>
#include <random>

namespace native::tools {

namespace {

constexpr Vec3 kAxes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

double snapValue(double v, double step) { return step > 0 ? std::round(v / step) * step : v; }

core::Vec3 clean(const Vec3& v) {   // no 3.0000000000000004 in map files
    const auto r = [](double x) { return std::round(x * 1e9) / 1e9; };
    return {r(v.x), r(v.y), r(v.z)};
}

} // namespace

DrawPlane planeThrough(const Vec3& point, const Vec3& normal) {
    const Vec3 n = render::normalize(normal);
    for (int k = 0; k < 3; ++k) {
        if (std::fabs(std::fabs(n[k]) - 1) < 1e-6) return {point, n, kAxes[(k + 1) % 3], kAxes[(k + 2) % 3]};
    }
    const Vec3 helper = std::fabs(n.y) < 0.9 ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    const Vec3 u = render::normalize(render::cross(helper, n));
    return {point, n, u, render::cross(n, u)};
}

DrawPlane fallbackPlane(const Ray& r, double distance, double maxDistance) {
    if (r.dir.y < -1e-6) {
        const double t = -r.origin.y / r.dir.y;
        if (t > 0 && t < maxDistance) return planeThrough(r.origin + r.dir * t, {0, 1, 0});
    }
    return planeThrough(r.origin + r.dir * distance, {0, 1, 0});
}

std::optional<Vec3> rayPlane(const Ray& r, const DrawPlane& p) {
    const double d = render::dot(p.normal, r.dir);
    if (std::fabs(d) < 1e-9) return std::nullopt;
    const double t = render::dot(p.normal, p.origin - r.origin) / d;
    if (t <= 0) return std::nullopt;
    return r.origin + r.dir * t;
}

Vec3 snapToPlane(const Vec3& p, const DrawPlane& plane, double step) {
    if (step <= 0) return p;
    const double a = render::dot(p, plane.u), b = render::dot(p, plane.v);
    return p + plane.u * (snapValue(a, step) - a) + plane.v * (snapValue(b, step) - b);
}

Rect rectFromDrag(const DrawPlane& plane, const Vec3& start, const Vec3& current, bool square) {
    const Vec3 d = current - start;
    double du = render::dot(d, plane.u), dv = render::dot(d, plane.v);
    if (square) {
        const double s = std::max(std::fabs(du), std::fabs(dv));
        du = (du < 0 ? -1 : 1) * s;
        dv = (dv < 0 ? -1 : 1) * s;
    }
    return {start, du, dv};
}

double heightFor(double raw, const Rect& rect, bool cube, double step) {
    if (cube) return (raw < 0 ? -1 : 1) * std::max(std::fabs(rect.du), std::fabs(rect.dv));
    return step > 0 ? snapValue(raw, step) : raw;
}

std::optional<core::Brush> buildBrush(const std::string& id, const DrawPlane& plane, const Rect& rect, double height, const std::string& texture) {
    if (std::fabs(rect.du) < kMinSize || std::fabs(rect.dv) < kMinSize || std::fabs(height) < kMinSize) return std::nullopt;
    const double a0 = std::min(0.0, rect.du), a1 = std::max(0.0, rect.du), b0 = std::min(0.0, rect.dv), b1 = std::max(0.0, rect.dv);
    const auto at = [&](double a, double b, double h) { return clean(rect.origin + plane.u * a + plane.v * b + plane.normal * h); };
    core::Brush brush;
    brush.id = id;
    brush.vertices = {at(a0, b0, 0), at(a1, b0, 0), at(a1, b1, 0), at(a0, b1, 0), at(a0, b0, height), at(a1, b0, height), at(a1, b1, height), at(a0, b1, height)};
    const uint32_t loops[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 4, 7, 3}, {1, 2, 6, 5}, {0, 1, 5, 4}, {3, 7, 6, 2}};
    for (uint32_t i = 0; i < 6; ++i) {
        core::BrushFace f;
        f.id = i;
        f.loop.assign(loops[i], loops[i] + 4);
        f.texture = texture;
        brush.faces.push_back(std::move(f));
    }
    if (brush.signedVolume() < 0)
        for (core::BrushFace& f : brush.faces) std::reverse(f.loop.begin(), f.loop.end());
    return brush;
}

std::vector<float> previewLines(const DrawPlane& plane, const Rect& rect, std::optional<double> height) {
    const auto brush = buildBrush("p", plane, rect, height.value_or(1e-3));
    std::vector<float> out;
    if (!brush) return out;
    static const int kFlat[4][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}};
    static const int kBox[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    const int count = height ? 12 : 4;
    for (int i = 0; i < count; ++i) {
        const int* e = height ? kBox[i] : kFlat[i];
        for (int k = 0; k < 2; ++k) {
            const core::Vec3& p = brush->vertices[static_cast<size_t>(e[k])];
            out.push_back(static_cast<float>(p.x));
            out.push_back(static_cast<float>(p.y));
            out.push_back(static_cast<float>(p.z));
        }
    }
    return out;
}

std::string newId(const std::string& prefix) {
    static std::mt19937 rng{std::random_device{}()};
    char buf[16];
    std::snprintf(buf, sizeof buf, "_%08x", static_cast<unsigned>(rng()));
    return prefix + buf;
}

} // namespace native::tools
