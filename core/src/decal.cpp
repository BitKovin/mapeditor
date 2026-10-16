#include "core/decal.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace core {

namespace {
constexpr double kDeg = 3.14159265358979323846 / 180.0;

double numberKey(const Entity& e, const char* key, double fallback) {
    const auto it = e.keyvalues.find(key);
    if (it == e.keyvalues.end()) return fallback;
    char* end = nullptr;
    const double v = std::strtod(it->second.c_str(), &end);
    return (end != it->second.c_str() && std::isfinite(v) && v > 1e-6) ? v : fallback;
}

// Rotation applied to a vector: Ry(yaw) * Rx(pitch) * Rz(roll)
Vec3 rotate(const Vec3& v, const Vec3& anglesDeg) {
    const double p = anglesDeg.x * kDeg, y = anglesDeg.y * kDeg, r = anglesDeg.z * kDeg;
    // Rz
    Vec3 a{v.x * std::cos(r) - v.y * std::sin(r), v.x * std::sin(r) + v.y * std::cos(r), v.z};
    // Rx
    Vec3 b{a.x, a.y * std::cos(p) - a.z * std::sin(p), a.y * std::sin(p) + a.z * std::cos(p)};
    // Ry
    return {b.x * std::cos(y) + b.z * std::sin(y), b.y, -b.x * std::sin(y) + b.z * std::cos(y)};
}
}  // namespace

bool isDecalEntity(const Entity& e) { return e.classname == "decal" && e.brushes.empty(); }

DecalProjector decalFromEntity(const Entity& e) {
    DecalProjector d;
    d.id = e.id;
    d.origin = e.origin;
    d.dir = rotate({0, -1, 0}, e.angles).normalized();
    d.up = rotate({0, 0, -1}, e.angles).normalized();
    d.right = d.dir.cross(d.up).normalized();
    d.width = numberKey(e, "width", 1.0);
    d.height = numberKey(e, "height", 1.0);
    d.depth = numberKey(e, "depth", 0.5);
    const auto t = e.keyvalues.find("texture");
    if (t != e.keyvalues.end() && !t->second.empty()) d.texture = t->second;
    return d;
}

namespace {
// Sutherland-Hodgman against one plane: keep points with n.p - w <= 0
std::vector<Vec3> clipPlane(const std::vector<Vec3>& in, const Vec3& n, double w) {
    std::vector<Vec3> out;
    const size_t c = in.size();
    for (size_t i = 0; i < c; ++i) {
        const Vec3 &a = in[i], &b = in[(i + 1) % c];
        const double da = n.dot(a) - w, db = n.dot(b) - w;
        if (da <= 1e-9) out.push_back(a);
        if ((da < -1e-9 && db > 1e-9) || (da > 1e-9 && db < -1e-9)) out.push_back(a + (b - a) * (da / (da - db)));
    }
    return out;
}
}  // namespace

DecalPolygon clipToDecal(const DecalProjector& d, const SurfaceInput& s) {
    DecalPolygon out;
    if (s.verts.size() < 3) return out;
    Vec3 n;  // Newell
    for (size_t i = 0; i < s.verts.size(); ++i) {
        const Vec3 &p = s.verts[i], &q = s.verts[(i + 1) % s.verts.size()];
        n = n + Vec3{(p.y - q.y) * (p.z + q.z), (p.z - q.z) * (p.x + q.x), (p.x - q.x) * (p.y + q.y)};
    }
    n = n.normalized();
    if (n.length() < 0.5 || n.dot(d.dir) > -0.05) return out;  // faces away or is (nearly) edge-on
    std::vector<Vec3> poly = s.verts;
    const struct { Vec3 axis; double half; } box[3] = {{d.right, d.width / 2}, {d.up, d.height / 2}, {d.dir, d.depth / 2}};
    for (const auto& b : box) {
        const double c = b.axis.dot(d.origin);
        poly = clipPlane(poly, b.axis, c + b.half);
        if (poly.size() >= 3) poly = clipPlane(poly, b.axis * -1.0, -c + b.half);
        if (poly.size() < 3) return out;
    }
    for (const auto& p : poly) {
        const Vec3 r = p - d.origin;
        out.verts.push_back(p);
        out.uv.push_back({r.dot(d.right) / d.width + 0.5, 0.5 - r.dot(d.up) / d.height});  // image rows run DOWN (v = 0 is the top): +up is -v, else the picture is upside down (= rotated 180 + mirrored)
    }
    return out;
}

}  // namespace core
