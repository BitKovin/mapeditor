// tools/gizmo -- see gizmo.h.
#include "tools/gizmo.h"

#include <cmath>

namespace native::tools {

namespace {

using render::dot;
using render::length;

struct Params {
    double s, t;   // s along the ray, t along the line
};

// Parameters of the closest points between the ray and the line (o, d).
std::optional<Params> closestParams(const Ray& r, const Vec3& o, const Vec3& d) {
    const Vec3 w = r.origin - o;
    const double a = dot(r.dir, r.dir), b = dot(r.dir, d), c = dot(d, d);
    const double dd = dot(r.dir, w), e = dot(d, w);
    const double denom = a * c - b * b;
    if (std::fabs(denom) < 1e-9) return std::nullopt;   // the ray is parallel to the line
    return Params{(b * e - c * dd) / denom, (a * e - b * dd) / denom};
}

std::optional<Vec3> planeHit(const Ray& r, const Vec3& pivot, Axis axis) {
    const Vec3 n = axisDir(axis);
    const double denom = dot(r.dir, n);
    if (std::fabs(denom) < 1e-9) return std::nullopt;
    const double t = dot(pivot - r.origin, n) / denom;
    if (t < 0) return std::nullopt;
    return r.origin + r.dir * t;
}

} // namespace

double snapTo(double v, double step) { return step > 0 ? std::round(v / step) * step : v; }

std::optional<double> lineDistance(const Ray& ray, const Vec3& origin, const Vec3& dir) {
    const auto p = closestParams(ray, origin, dir);
    if (!p) return std::nullopt;
    return p->t;
}

std::optional<double> axisDistance(const Ray& ray, const Vec3& pivot, Axis axis) { return lineDistance(ray, pivot, axisDir(axis)); }

Axis hitAxis(const Ray& ray, const Vec3& pivot, double len, double tol) {
    Axis best = -1;
    double bestDist = 0;
    for (Axis axis = 0; axis < 3; ++axis) {
        const auto p = closestParams(ray, pivot, axisDir(axis));
        if (!p || p->s < 0) continue;
        const double t = std::max(0.0, std::min(len, p->t));   // clamp to the visible handle
        const Vec3 onAxis = pivot + axisDir(axis) * t;
        const Vec3 onRay = ray.origin + ray.dir * p->s;
        const double dist = length(onAxis - onRay);
        if (dist <= tol && (best < 0 || dist < bestDist)) {
            best = axis;
            bestDist = dist;
        }
    }
    return best;
}

bool hitDir(const Ray& ray, const Vec3& pivot, const Vec3& dir, double len, double tol) {
    const auto p = closestParams(ray, pivot, dir);
    if (!p || p->s < 0) return false;
    const double t = std::max(0.0, std::min(len, p->t));
    return length((pivot + dir * t) - (ray.origin + ray.dir * p->s)) <= tol;
}

std::pair<Vec3, Vec3> ringBasis(Axis axis) {
    if (axis == 0) return {{0, 1, 0}, {0, 0, 1}};
    if (axis == 1) return {{0, 0, 1}, {1, 0, 0}};
    return {{1, 0, 0}, {0, 1, 0}};
}

Axis hitRing(const Ray& ray, const Vec3& pivot, double radius, double tol) {
    Axis best = -1;
    double bestErr = 0;
    for (Axis axis = 0; axis < 3; ++axis) {
        const auto p = planeHit(ray, pivot, axis);
        if (!p) continue;
        const double err = std::fabs(length(*p - pivot) - radius);
        if (err <= tol && (best < 0 || err < bestErr)) {
            best = axis;
            bestErr = err;
        }
    }
    return best;
}

std::optional<double> ringAngle(const Ray& ray, const Vec3& pivot, Axis axis) {
    const auto p = planeHit(ray, pivot, axis);
    if (!p) return std::nullopt;
    const auto [u, v] = ringBasis(axis);
    const Vec3 q = *p - pivot;
    return std::atan2(dot(q, v), dot(q, u));
}

double angleDelta(double a, double b) {
    double d = b - a;
    const double pi = 3.14159265358979323846;
    while (d > pi) d -= 2 * pi;
    while (d < -pi) d += 2 * pi;
    return d;
}

std::vector<double> ringLines(const Vec3& pivot, Axis axis, double radius, int segments) {
    const auto [u, v] = ringBasis(axis);
    std::vector<double> out;
    out.reserve(static_cast<size_t>(segments) * 6);
    const auto at = [&](int k) {
        const double a = static_cast<double>(k) / segments * 2 * 3.14159265358979323846;
        return pivot + u * (std::cos(a) * radius) + v * (std::sin(a) * radius);
    };
    for (int k = 0; k < segments; ++k) {
        const Vec3 a = at(k), b = at(k + 1);
        out.insert(out.end(), {a.x, a.y, a.z, b.x, b.y, b.z});
    }
    return out;
}

Vec3 dragDelta(const Ray& ray, const Vec3& pivot, Axis axis, double startT, double step) {
    const auto t = axisDistance(ray, pivot, axis);
    if (!t) return {0, 0, 0};
    return axisDir(axis) * snapTo(*t - startT, step);
}

} // namespace native::tools
