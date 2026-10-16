#pragma once
#include <cmath>

namespace core {

// Double precision throughout: the Shell (JS numbers) is double too, and
// coordinates are metric (meters, Y-up -- DESIGN.md §4), so there is no
// reason to lose precision at the boundary.
struct Vec2 {
    double u = 0, v = 0;
};

struct Vec3 {
    double x = 0, y = 0, z = 0;

    Vec3() = default;
    Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    Vec3 mul(const Vec3& o) const { return {x * o.x, y * o.y, z * o.z}; }  // component-wise

    double dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    double length() const { return std::sqrt(dot(*this)); }
    Vec3 normalized() const {
        double l = length();
        if (l < 1e-12) return {0, 0, 0};
        return {x / l, y / l, z / l};
    }
};

}  // namespace core
