// scene/light_gizmo -- see light_gizmo.h.
#include "scene/light_gizmo.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace native::scene {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr double kIcon = 0.35;   // size of the always-visible icon part, metres
constexpr int kSeg = 48;
const V3 kX{1, 0, 0}, kY{0, 1, 0}, kZ{0, 0, 1};

V3 add(const V3& a, const V3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 scale(const V3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
V3 cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
V3 normalize(const V3& a) {
    const double l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    return l < 1e-12 ? V3{0, 0, 0} : scale(a, 1 / l);
}

std::optional<double> num(const std::string* v) {
    if (!v) return std::nullopt;
    char* end = nullptr;
    const double n = std::strtod(v->c_str(), &end);
    if (end == v->c_str() || !std::isfinite(n)) return std::nullopt;
    return n;
}

struct Lines {
    std::vector<double>& out;
    void seg(const V3& p, const V3& q) { out.insert(out.end(), {p[0], p[1], p[2], q[0], q[1], q[2]}); }
};
V3 at(const V3& c, const V3& a, const V3& b, double x, double y) { return add(c, add(scale(a, x), scale(b, y))); }

// A circle (or arc from `from` to `to` radians) around c in the plane spanned by the unit vectors a, b.
void arc(Lines& l, const V3& c, const V3& a, const V3& b, double r, double from = 0, double to = kPi * 2, int seg = kSeg) {
    V3 prev = at(c, a, b, std::cos(from) * r, std::sin(from) * r);
    for (int i = 1; i <= seg; ++i) {
        const double t = from + (to - from) * i / seg;
        const V3 p = at(c, a, b, std::cos(t) * r, std::sin(t) * r);
        l.seg(prev, p);
        prev = p;
    }
}

void head(Lines& l, const V3& tip, const V3& dir, double len) {
    const V3 u = normalize(std::fabs(dir[1]) < 0.9 ? cross(dir, kY) : cross(dir, kX));
    const V3 v = cross(dir, u);
    const V3 back = add(tip, scale(dir, -len));
    for (const V3& s : {u, v, scale(u, -1), scale(v, -1)}) l.seg(tip, add(back, scale(s, len * 0.4)));
}

void sphere(Lines& l, const V3& c, double r) {
    arc(l, c, kX, kY, r);
    arc(l, c, kY, kZ, r);
    arc(l, c, kX, kZ, r);
}

} // namespace

std::optional<LightParams> lightParams(const core::Entity& e, const core::EntClass* def) {
    LightParams p;
    if (e.classname == "light_point") p.kind = LightKind::Point;
    else if (e.classname == "light_spot") p.kind = LightKind::Spot;
    else if (e.classname == "light_directional") p.kind = LightKind::Directional;
    else if (e.classname == "light_rect") p.kind = LightKind::Rect;
    else if (e.classname == "light_sky") p.kind = LightKind::Sky;
    else return std::nullopt;
    // the entity's value, else the .ent default, else the baker's fallback
    const auto defaultOf = [&](const std::string& key) -> const std::string* {
        if (!def) return nullptr;
        for (const core::EntKey& k : def->keys)
            if (k.key == key && k.defaultValue) return &*k.defaultValue;
        return nullptr;
    };
    const auto kv = [&](const std::string& key) -> const std::string* {
        const auto it = e.keyvalues.find(key);
        return it == e.keyvalues.end() ? nullptr : &it->second;
    };
    const auto n = [&](const char* key, double fallback) { return num(kv(key)).value_or(num(defaultOf(key)).value_or(fallback)); };
    const std::string* colorText = kv("color") ? kv("color") : defaultOf("color");
    if (colorText) {
        std::istringstream is(*colorText);
        double r, g, b;
        if (is >> r >> g >> b) p.color = {r, g, b};
    }
    const double outer = std::min(89.9, std::max(0.1, n("outer_angle", 35)));
    double inner = std::min(89.0, std::max(0.0, n("inner_angle", 20)));
    if (inner >= outer) inner = outer * 0.5;
    p.brightness = n("brightness", 1);
    p.radius = std::max(1e-3, n("radius", 10));
    p.bounce = std::max(0.0, n("bounce", 1));
    p.castShadows = n("cast_shadows", 1) != 0;
    p.style = static_cast<int>(std::min(63.0, std::max(0.0, std::trunc(n("style", 0)))));
    p.sourceSize = std::max(0.0, n("source_size", 0));
    p.innerAngle = inner;
    p.outerAngle = outer;
    p.width = std::max(1e-3, n("width", 1));
    p.height = std::max(1e-3, n("height", 1));
    return p;
}

bool isDarkLight(const LightParams& p) { return p.brightness < 0; }

V3 lightGizmoColor(const LightParams& p) {
    V3 c;
    for (size_t i = 0; i < 3; ++i) c[i] = 0.25 + 0.75 * std::min(1.0, std::max(0.0, p.color[i]));
    if (!isDarkLight(p)) return c;
    const V3 tint{0.6, 0.35, 1};   // a dark light is pulled towards violet so it never looks like a normal one
    for (size_t i = 0; i < 3; ++i) c[i] = 0.45 * c[i] + 0.55 * tint[i];
    return c;
}

std::vector<double> lightGizmo(const core::Entity& e, const LightParams& p, bool selected) {
    std::vector<double> out;
    Lines l{out};
    // the baker's frame: forward = R * (0, 0, 1), right = R * (1, 0, 0), up = R * (0, 1, 0)
    const Mat3 R = rotationFromAngles({e.angles.x, e.angles.y, e.angles.z});
    const V3 o{e.origin.x, e.origin.y, e.origin.z};
    const V3 f = normalize(apply3(R, kZ)), r = normalize(apply3(R, kX)), u = normalize(apply3(R, kY));
    switch (p.kind) {
        case LightKind::Point: {
            // a diamond (octahedron) as the icon
            const double s = kIcon * 0.5;
            arc(l, o, kX, kY, s, 0, kPi * 2, 4);
            arc(l, o, kY, kZ, s, 0, kPi * 2, 4);
            arc(l, o, kX, kZ, s, 0, kPi * 2, 4);
            if (selected) {
                sphere(l, o, p.radius);
                if (p.sourceSize > 0) sphere(l, o, p.sourceSize);
            }
            break;
        }
        case LightKind::Spot: {
            const double len = selected ? p.radius : std::min(p.radius, 1.2);
            const auto tipOf = [&](double deg) { return std::pair<V3, double>{add(o, scale(f, len)), len * std::tan(deg * kDeg)}; };
            const auto [oc, orad] = tipOf(p.outerAngle);
            const auto [ic, irad] = tipOf(p.innerAngle);
            arc(l, oc, r, u, orad);
            for (const auto& [x, y] : {std::pair<int, int>{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) l.seg(o, at(oc, r, u, x * orad, y * orad));
            arc(l, ic, r, u, irad, 0, kPi * 2, 32);
            l.seg(o, oc);
            arc(l, o, kX, kY, kIcon * 0.25, 0, kPi * 2, 8);
            arc(l, o, kY, kZ, kIcon * 0.25, 0, kPi * 2, 8);
            if (selected) {
                // the reach: an arc of the attenuation sphere across the cone, in two planes
                const double a = p.outerAngle * kDeg;
                arc(l, o, f, r, p.radius, -a, a, 16);
                arc(l, o, f, u, p.radius, -a, a, 16);
                if (p.sourceSize > 0) sphere(l, o, p.sourceSize);
            }
            break;
        }
        case LightKind::Directional: {
            // parallel rays: a ring of arrows along the direction (the sun has no position and no range)
            const double len = selected ? 4 : 1.6, ring = kIcon;
            l.seg(o, add(o, scale(f, len)));
            head(l, add(o, scale(f, len)), f, std::min(0.4, len * 0.25));
            for (int i = 0; i < 8; ++i) {
                const double t = i / 8.0 * kPi * 2;
                const V3 off = at(o, r, u, std::cos(t) * ring, std::sin(t) * ring);
                l.seg(off, add(off, scale(f, len * 0.75)));
            }
            arc(l, o, r, u, ring, 0, kPi * 2, 24);
            if (p.sourceSize > 0) {   // angular diameter: a cone of that opening around the direction, at the arrow's end
                const double rad = len * std::tan(p.sourceSize / 2 * kDeg);
                const V3 c = add(o, scale(f, len));
                arc(l, c, r, u, rad, 0, kPi * 2, 24);
                for (const auto& [x, y] : {std::pair<int, int>{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) l.seg(o, at(c, r, u, x * rad, y * rad));
            }
            break;
        }
        case LightKind::Rect: {
            // the real emitting rectangle, its front (emitting) side marked by the direction arrow
            const double hw = p.width / 2, hh = p.height / 2;
            const V3 corners[4] = {at(o, r, u, -hw, -hh), at(o, r, u, hw, -hh), at(o, r, u, hw, hh), at(o, r, u, -hw, hh)};
            for (int i = 0; i < 4; ++i) l.seg(corners[i], corners[(i + 1) % 4]);
            l.seg(corners[0], corners[2]);   // the diagonals: an emitting panel, not a frame
            l.seg(corners[1], corners[3]);
            const double len = std::max({0.5, std::min(p.width, p.height) * 0.5, selected ? 1.0 : 0.0});
            l.seg(o, add(o, scale(f, len)));
            head(l, add(o, scale(f, len)), f, std::min(0.3, len * 0.3));
            if (selected) {   // the reach: the attenuation sphere's front half in two planes, and the rays from the corners
                arc(l, o, r, f, p.radius, 0, kPi, 24);
                arc(l, o, u, f, p.radius, 0, kPi, 24);
                for (const V3& c : corners) l.seg(c, add(c, scale(f, p.radius)));
            }
            break;
        }
        case LightKind::Sky: {
            // a dome over the position: the upper hemisphere the light comes from
            const double s = selected ? 2 : kIcon * 1.4;
            arc(l, o, kX, kZ, s, 0, kPi * 2, 32);                 // the horizon
            arc(l, o, kX, kY, s, 0, kPi, 24);                     // two meridians
            arc(l, o, kZ, kY, s, 0, kPi, 24);
            for (const auto& [x, z] : {std::pair<int, int>{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {   // rays coming down onto the position
                const V3 top = add(o, {x * s * 0.5, s * 0.87, z * s * 0.5});
                l.seg(top, add(o, {x * s * 0.12, s * 0.2, z * s * 0.12}));
            }
            head(l, add(o, {0, s * 0.05, 0}), {0, -1, 0}, s * 0.25);
            l.seg(add(o, {0, s, 0}), add(o, {0, s * 0.1, 0}));
            break;
        }
    }
    if (isDarkLight(p)) {   // a dark light: the four body diagonals of a small cube through the icon, a cross over it
        const double s = kIcon * 0.7;
        for (const auto& [x, y, z] : {std::array<int, 3>{1, 1, 1}, {1, 1, -1}, {1, -1, 1}, {1, -1, -1}})
            l.seg(add(o, {x * s, y * s, z * s}), add(o, {-x * s, -y * s, -z * s}));
    }
    return out;
}

} // namespace native::scene
