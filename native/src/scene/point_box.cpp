// scene/point_box -- see point_box.h.
#include "scene/point_box.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace native::scene {

namespace {

constexpr double kDeg = 3.14159265358979323846 / 180.0;
double clean(double n) { return n == 0 ? 0 : n; }   // no -0

bool usable(const std::array<double, 6>& b) {
    for (double v : b)
        if (!std::isfinite(v)) return false;
    return b[3] >= b[0] && b[4] >= b[1] && b[5] >= b[2];
}

double sizeKey(const core::Entity& e, const char* key, double dflt) {
    const auto it = e.keyvalues.find(key);
    if (it == e.keyvalues.end()) return dflt;
    char* end = nullptr;
    const double n = std::strtod(it->second.c_str(), &end);
    return end != it->second.c_str() && std::isfinite(n) && n > 1e-6 ? n : dflt;
}

} // namespace

const std::array<int, 24> kBoxEdges = {0, 1, 1, 2, 2, 3, 3, 0, 4, 5, 5, 6, 6, 7, 7, 4, 0, 4, 1, 5, 2, 6, 3, 7};

V3 quakeToEditor(const V3& v) { return {clean(v[0] / kQuakeUnitsPerMeter), clean(v[2] / kQuakeUnitsPerMeter), clean(-v[1] / kQuakeUnitsPerMeter)}; }
V3 editorToQuake(const V3& v) { return {clean(v[0] * kQuakeUnitsPerMeter), clean(-v[2] * kQuakeUnitsPerMeter), clean(v[1] * kQuakeUnitsPerMeter)}; }

Box boxFromQuake(const std::optional<std::array<double, 6>>& box) {
    const std::array<double, 6> b = box && usable(*box) ? *box : std::array<double, 6>{-8, -8, -8, 8, 8, 8};
    const V3 a = quakeToEditor({b[0], b[1], b[2]}), c = quakeToEditor({b[3], b[4], b[5]});
    Box out;
    for (size_t i = 0; i < 3; ++i) {
        out.min[i] = std::min(a[i], c[i]);
        out.max[i] = std::max(a[i], c[i]);
    }
    return out;
}

BoxOf classBoxes(const core::EntityDefinitions* defs) {
    return [defs](const std::string& classname) {
        std::optional<std::array<double, 6>> box;
        if (defs)
            if (const core::EntClass* c = defs->find(classname))
                if (c->box) {
                    const auto& q = *c->box;
                    box = std::array<double, 6>{q[0], q[1], q[2], q[3], q[4], q[5]};
                }
        return boxFromQuake(box);
    };
}

Box boxAt(const Box& box, const V3& o) {
    Box b;
    for (size_t i = 0; i < 3; ++i) {
        b.min[i] = o[i] + box.min[i];
        b.max[i] = o[i] + box.max[i];
    }
    return b;
}

Mat3 rotationFromAngles(const V3& a) {
    const double p = a[0] * kDeg, y = a[1] * kDeg, r = a[2] * kDeg;
    const double cp = std::cos(p), sp = std::sin(p), cy = std::cos(y), sy = std::sin(y), cr = std::cos(r), sr = std::sin(r);
    return {cy * cr + sy * sp * sr,  -cy * sr + sy * sp * cr, sy * cp,
            cp * sr,                 cp * cr,                 -sp,
            -sy * cr + cy * sp * sr, sy * sr + cy * sp * cr,  cy * cp};
}

V3 apply3(const Mat3& m, const V3& v) {
    return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2], m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
}

V3 unrotate(const Mat3& m, const V3& v) {
    return {m[0] * v[0] + m[3] * v[1] + m[6] * v[2], m[1] * v[0] + m[4] * v[1] + m[7] * v[2], m[2] * v[0] + m[5] * v[1] + m[8] * v[2]};
}

bool isModernLightClass(const std::string& c) {
    return c == "light_point" || c == "light_spot" || c == "light_directional" || c == "light_rect" || c == "light_sky";
}

bool drawsBox(const core::Entity& e) { return !isModernLightClass(e.classname) && e.classname != "decal"; }

Box decalLocalBox(const core::Entity& e) {
    const double hw = sizeKey(e, "width", 1) / 2, hh = sizeKey(e, "height", 1) / 2, hd = sizeKey(e, "depth", 0.5) / 2;
    return {{-hw, -hd, -hh}, {hw, hd, hh}};
}

OrientedBox pointBox(const core::Entity& e, const BoxOf& boxOf) {
    const Box local = e.classname == "decal" ? decalLocalBox(e) : boxOf(e.classname);
    OrientedBox b;
    b.origin = {e.origin.x, e.origin.y, e.origin.z};
    b.rot = rotationFromAngles({e.angles.x, e.angles.y, e.angles.z});
    b.min = local.min;
    b.max = local.max;
    return b;
}

V3 toWorld(const OrientedBox& b, const V3& v) {
    const V3 r = apply3(b.rot, v);
    return {b.origin[0] + r[0], b.origin[1] + r[1], b.origin[2] + r[2]};
}

V3 toLocal(const OrientedBox& b, const V3& p) { return unrotate(b.rot, {p[0] - b.origin[0], p[1] - b.origin[1], p[2] - b.origin[2]}); }

std::array<V3, 8> cornersOf(const V3& mn, const V3& mx) {
    return {V3{mn[0], mn[1], mn[2]}, V3{mx[0], mn[1], mn[2]}, V3{mx[0], mx[1], mn[2]}, V3{mn[0], mx[1], mn[2]},
            V3{mn[0], mn[1], mx[2]}, V3{mx[0], mn[1], mx[2]}, V3{mx[0], mx[1], mx[2]}, V3{mn[0], mx[1], mx[2]}};
}

std::array<V3, 8> worldCorners(const OrientedBox& b) {
    std::array<V3, 8> c = cornersOf(b.min, b.max);
    for (V3& p : c) p = toWorld(b, p);
    return c;
}

Box boundsOfBox(const OrientedBox& b) {
    Box out{{1e300, 1e300, 1e300}, {-1e300, -1e300, -1e300}};
    for (const V3& c : worldCorners(b))
        for (size_t i = 0; i < 3; ++i) {
            out.min[i] = std::min(out.min[i], c[i]);
            out.max[i] = std::max(out.max[i], c[i]);
        }
    return out;
}

} // namespace native::scene
