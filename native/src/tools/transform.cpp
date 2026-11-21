// tools/transform -- see transform.h.
#include "tools/transform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "doc/edit.h"
#include "tools/parametric.h"

namespace native::tools {

namespace {

constexpr double kPi = 3.14159265358979323846;

V3 add(const V3& a, const V3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 mul(const V3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double len3(const V3& v) { return std::sqrt(dot(v, v)); }
V3 col(const Mat3& m, int i) { return {m[static_cast<size_t>(i)], m[static_cast<size_t>(3 + i)], m[static_cast<size_t>(6 + i)]}; }
bool nearly(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }
V3 vec(const core::Vec3& v) { return {v.x, v.y, v.z}; }
core::Vec3 cvec(const V3& v) { return {v[0], v[1], v[2]}; }

double tidy(double x) {
    const double r = std::round(x * 1e5) / 1e5;
    return r == 0 ? 0.0 : r;   // no -0
}
std::string fmt(double x) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.15g", tidy(x));
    return buf;
}
// The number a key holds, `dflt` when it is missing, blank or not a finite number.
double numKey(const core::Entity& e, const std::string& key, double dflt) {
    const auto it = e.keyvalues.find(key);
    if (it == e.keyvalues.end()) return dflt;
    const char* s = it->second.c_str();
    while (*s == ' ' || *s == '\t') ++s;
    if (*s == '\0') return dflt;
    char* end = nullptr;
    const double v = std::strtod(s, &end);
    while (end && (*end == ' ' || *end == '\t')) ++end;
    return end && *end == '\0' && std::isfinite(v) ? v : dflt;
}
bool numeric(const std::string& s, double& out) {
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end != s.c_str() && std::isfinite(out);
}

bool isIdentity(const Mat3& m) {
    for (int i = 0; i < 9; ++i)
        if (!nearly(m[static_cast<size_t>(i)], i % 4 == 0 ? 1 : 0)) return false;
    return true;
}

struct LocalAxes {
    V3 right, up, dir;
};
// Frame of the keys a classname understands: which local axis `width`, `height`, `depth` run along (lights: core/lights.h, decals: core/decal.h).
LocalAxes localAxes(const Mat3& R, bool decal) {
    if (decal) {   // dir = R * (0, -1, 0), up = R * (0, 0, -1), right = dir x up
        const V3 dir = scene::apply3(R, {0, -1, 0}), up = scene::apply3(R, {0, 0, -1});
        return {cross(dir, up), up, dir};
    }
    return {scene::apply3(R, {1, 0, 0}), scene::apply3(R, {0, 1, 0}), scene::apply3(R, {0, 0, 1})};
}

enum class Kind { None, Point, Spot, Directional, Rect, Sky };
Kind lightKind(const std::string& c) {
    if (c == "light_point") return Kind::Point;
    if (c == "light_spot") return Kind::Spot;
    if (c == "light_directional") return Kind::Directional;
    if (c == "light_rect") return Kind::Rect;
    if (c == "light_sky") return Kind::Sky;
    return Kind::None;
}

} // namespace

V3 Affine::operator()(const V3& p) const {
    return {m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + t[0], m[3] * p[0] + m[4] * p[1] + m[5] * p[2] + t[1], m[6] * p[0] + m[7] * p[1] + m[8] * p[2] + t[2]};
}

Affine rotateAbout(const V3& pivot, const V3& axis, double angle) {
    const double l = len3(axis);
    const V3 k = l > 0 ? mul(axis, 1 / l) : V3{0, 0, 1};
    const double c = std::cos(angle), s = std::sin(angle), oc = 1 - c;
    Affine a;
    a.m = {c + k[0] * k[0] * oc,        k[0] * k[1] * oc - k[2] * s, k[0] * k[2] * oc + k[1] * s,
           k[1] * k[0] * oc + k[2] * s, c + k[1] * k[1] * oc,        k[1] * k[2] * oc - k[0] * s,
           k[2] * k[0] * oc - k[1] * s, k[2] * k[1] * oc + k[0] * s, c + k[2] * k[2] * oc};
    a.t = sub(pivot, scene::apply3(a.m, pivot));
    return a;
}

Affine scaleAbout(const V3& pivot, const V3& f) {
    Affine a;
    a.m = {f[0], 0, 0, 0, f[1], 0, 0, 0, f[2]};
    a.t = {pivot[0] - pivot[0] * f[0], pivot[1] - pivot[1] * f[1], pivot[2] - pivot[2] * f[2]};
    return a;
}

Affine translateBy(const V3& d) {
    Affine a;
    a.t = d;
    return a;
}

bool mirrors(const V3& f) { return (static_cast<int>(f[0] < 0) + static_cast<int>(f[1] < 0) + static_cast<int>(f[2] < 0)) % 2 == 1; }

Mat3 mul3(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r[static_cast<size_t>(i * 3 + j)] = a[static_cast<size_t>(i * 3)] * b[static_cast<size_t>(j)] + a[static_cast<size_t>(i * 3 + 1)] * b[static_cast<size_t>(3 + j)] +
                                                a[static_cast<size_t>(i * 3 + 2)] * b[static_cast<size_t>(6 + j)];
    return r;
}

double det3(const Mat3& m) {
    return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
}

V3 anglesFromRotation(const Mat3& R) {
    const double sp = std::max(-1.0, std::min(1.0, -R[5]));
    const double pitch = std::asin(sp);
    double yaw, roll;
    if (std::fabs(sp) < 1 - 1e-9) {
        yaw = std::atan2(R[2], R[8]);
        roll = std::atan2(R[3], R[4]);
    } else {   // gimbal lock: all in yaw
        roll = 0;
        yaw = sp > 0 ? std::atan2(R[1], R[0]) : std::atan2(-R[1], R[0]);
    }
    const double deg = 180.0 / kPi;
    return {pitch * deg, yaw * deg, roll * deg};
}

core::Brush mapBrushPoints(const core::Brush& b, const Affine& map, bool flip, bool lockUv) {
    core::Brush moved = b;
    for (core::Vec3& v : moved.vertices) v = cvec(map(vec(v)));
    for (size_t fi = 0; fi < moved.faces.size(); ++fi) {
        const core::BrushFace& old = b.faces[fi];
        core::BrushFace& nf = moved.faces[fi];
        const bool base = old.uvs.size() == old.loop.size() && !old.loop.empty();
        std::vector<core::Vec2> uvs;
        if (lockUv) {
            if (base) {
                uvs = old.uvs;
            } else {   // bake: the UVs as they look before the move
                const core::Vec3 n = b.faceNormal(old);
                for (uint32_t i : old.loop) uvs.push_back(core::projectAlbedoUv(n, b.vertices[i], old.projection));
            }
            if (flip) std::reverse(uvs.begin(), uvs.end());
        }
        if (flip) std::reverse(nf.loop.begin(), nf.loop.end());
        if (lockUv) {
            nf.uvs = std::move(uvs);
        } else if (base) {   // unlocked: the world-aligned projection again at the new place
            nf.uvs.clear();
            const core::Vec3 n = moved.faceNormal(nf);
            for (uint32_t i : nf.loop) nf.uvs.push_back(core::projectAlbedoUv(n, moved.vertices[i], nf.projection));
        } else {
            nf.uvs.clear();
        }
    }
    return moved;
}

core::Entity mapEntityProps(const core::Entity& e, const Affine& map) {
    const Mat3& M = map.m;
    if (isIdentity(M)) return e;
    const Mat3 R = scene::rotationFromAngles(vec(e.angles));
    const double det = det3(M);
    const bool orthonormal = nearly(std::fabs(det), 1, 1e-6) && nearly(len3(col(M, 0)), 1, 1e-6) && nearly(len3(col(M, 1)), 1, 1e-6) && nearly(len3(col(M, 2)), 1, 1e-6) &&
                             nearly(dot(col(M, 0), col(M, 1)), 0, 1e-6) && nearly(dot(col(M, 0), col(M, 2)), 0, 1e-6);
    core::Entity out = e;
    if (orthonormal) {
        if (det > 0) {
            const V3 a = anglesFromRotation(mul3(M, R));
            const V3 next{tidy(a[0]), tidy(a[1]), tidy(a[2])};
            const V3 cur = vec(e.angles);
            if (!(nearly(next[0], cur[0], 1e-5) && nearly(next[1], cur[1], 1e-5) && nearly(next[2], cur[2], 1e-5))) out.angles = cvec(next);
        }
        return out;
    }
    const double uni = std::cbrt(std::fabs(det));
    if (!(uni > 1e-6)) return e;
    const std::string& cls = e.classname;
    const auto set = [&](const std::string& key, double v, double lo, double hi) { out.keyvalues[key] = fmt(std::min(hi, std::max(lo, v))); };
    const Kind kind = lightKind(cls);
    const bool decal = cls == "decal";
    const LocalAxes ax = localAxes(R, decal);
    const auto along = [&](const V3& v) { return len3(scene::apply3(M, v)); };
    if (kind != Kind::None) {
        if (kind != Kind::Directional && kind != Kind::Sky) set("radius", numKey(e, "radius", 10) * uni, 0.1, 1000);
        if (kind == Kind::Point || kind == Kind::Spot) {
            const double s = numKey(e, "source_size", 0);
            if (s > 0) set("source_size", s * uni, 0, 10);
        }
        if (kind == Kind::Rect) {
            set("width", numKey(e, "width", 1) * along(ax.right), 0.01, 100);
            set("height", numKey(e, "height", 1) * along(ax.up), 0.01, 100);
        }
    } else if (decal) {
        set("width", numKey(e, "width", 1) * along(ax.right), 0.001, 1000);
        set("height", numKey(e, "height", 1) * along(ax.up), 0.001, 1000);
        set("depth", numKey(e, "depth", 0.5) * along(ax.dir), 0.001, 1000);
    } else {
        double v = 0;
        const auto radius = out.keyvalues.find("radius");
        if (radius != out.keyvalues.end() && numeric(radius->second, v)) set("radius", v * uni, 0, 1e6);
        const auto ms = out.keyvalues.find("modelscale");
        const bool hasVec = out.keyvalues.count("modelscale_vec") > 0;
        if (ms != out.keyvalues.end() && !hasVec && numeric(ms->second, v)) {
            set("modelscale", v * uni, 1e-4, 1e4);
        } else {
            std::vector<double> cur;
            {
                std::istringstream in(hasVec ? out.keyvalues["modelscale_vec"] : std::string("1 1 1"));
                std::string tok;
                bool bad = false;
                while (in >> tok) {
                    double x;
                    if (!numeric(tok, x)) bad = true;
                    cur.push_back(x);
                }
                if (bad) cur.clear();
            }
            V3 base{1, 1, 1};
            if (cur.size() == 3 && cur[0] != 0 && cur[1] != 0 && cur[2] != 0) base = {cur[0], cur[1], cur[2]};
            const V3 fac{along(scene::apply3(R, {1, 0, 0})), along(scene::apply3(R, {0, 1, 0})), along(scene::apply3(R, {0, 0, 1}))};
            const V3 next{tidy(base[0] * fac[0]), tidy(base[1] * fac[1]), tidy(base[2] * fac[2])};
            if (next[0] == 1 && next[1] == 1 && next[2] == 1) out.keyvalues.erase("modelscale_vec");
            else out.keyvalues["modelscale_vec"] = fmt(next[0]) + " " + fmt(next[1]) + " " + fmt(next[2]);
        }
    }
    return out;
}

core::Entity mapEntityPoints(const core::Entity& e, bool hasBrushes, const Affine& map, double yawDeg) {
    core::Entity out = hasBrushes ? e : mapEntityProps(e, map);   // brush entities only turn their yaw (legacy) when asked to
    if (hasBrushes && yawDeg != 0) out.angles = {e.angles.x, e.angles.y + yawDeg, e.angles.z};
    out.origin = cvec(map(vec(e.origin)));
    return out;
}

core::PrefabInstance mapInstancePoints(const core::PrefabInstance& p, const Affine& map) {
    const V3 o = vec(p.origin);
    core::PrefabInstance out = p;
    out.origin = cvec(map(o));
    const Mat3& M = map.m;
    if (isIdentity(M)) return out;
    const bool off = nearly(M[1], 0) && nearly(M[2], 0) && nearly(M[3], 0) && nearly(M[5], 0) && nearly(M[6], 0) && nearly(M[7], 0);
    if (off) {   // pure (axis-aligned) scale
        const V3 f{M[0], M[4], M[8]};
        const bool uniform = nearly(f[0], f[1]) && nearly(f[1], f[2]);
        const bool rotated = p.angles.x != 0 || p.angles.y != 0 || p.angles.z != 0;
        if (uniform || !rotated) {
            const core::Vec3 s = p.scale ? *p.scale : core::Vec3{1, 1, 1};
            out.scale = core::Vec3{s.x * f[0], s.y * f[1], s.z * f[2]};
        }
        return out;
    }
    const bool orthonormal = nearly(det3(M), 1) && nearly(M[0] * M[0] + M[3] * M[3] + M[6] * M[6], 1);
    if (orthonormal) out.angles = cvec(anglesFromRotation(mul3(M, scene::rotationFromAngles(vec(p.angles)))));
    return out;
}

Targets selectionTargets(const std::vector<doc::SelRef>& sels) {
    Targets t;
    for (const doc::SelRef& s : sels)
        if (s.isInstance()) t.instances.insert(s.instancePath.front());
    for (const doc::SelRef& s : sels)
        if (s.instancePath.empty() && !s.entityId.empty() && s.brushId.empty()) t.entities.insert(s.entityId);
    for (const doc::SelRef& s : sels)
        if (s.instancePath.empty() && !s.entityId.empty() && !s.brushId.empty() && !t.entities.count(s.entityId)) t.brushes[s.entityId].insert(s.brushId);
    return t;
}

namespace {
// A parametric shape's box moves with its parts (REQUIREMENTS 22 P1); reconcileShapes then finds the parts still matching their record.
void mapShapeKey(doc::VersionEditor& ed, const std::string& entityId, const doc::BrushNode& b, const Affine& map) {
    if (!b.raw.is_object() || !b.raw.contains("shape")) return;
    const parametric::Json mapped = parametric::mapShapeRecord(b.raw["shape"], [&map](const Vec3& p) {
        const V3 q = map({p.x, p.y, p.z});
        return Vec3{q[0], q[1], q[2]};
    });
    ed.setBrushKey(entityId, b.brush.id, "shape", mapped);
}
} // namespace

std::shared_ptr<const doc::DocumentVersion> transformSelections(const std::shared_ptr<const doc::DocumentVersion>& base, const std::vector<doc::SelRef>& sels, const Affine& map,
                                                                bool flip, double yawDeg, bool lockUv) {
    if (!base) return base;
    const Targets t = selectionTargets(sels);
    if (t.empty()) return base;
    doc::VersionEditor ed(base);
    for (const auto& node : base->entities) {
        const std::string& id = node->typed.id;
        if (t.entities.count(id)) {
            ed.replaceEntity(mapEntityPoints(node->typed, !node->brushes.empty(), map, yawDeg));
            for (const auto& b : node->brushes) {
                ed.replaceBrush(id, mapBrushPoints(b->brush, map, flip, lockUv));
                mapShapeKey(ed, id, *b, map);
            }
            continue;
        }
        const auto it = t.brushes.find(id);
        if (it == t.brushes.end()) continue;
        for (const auto& b : node->brushes)
            if (it->second.count(b->brush.id)) {
                ed.replaceBrush(id, mapBrushPoints(b->brush, map, flip, lockUv));
                mapShapeKey(ed, id, *b, map);
            }
    }
    for (const auto& inst : base->instances)
        if (t.instances.count(inst->typed.id)) ed.replaceInstance(mapInstancePoints(inst->typed, map));
    return ed.finish();
}

std::shared_ptr<const doc::DocumentVersion> moveEntityOrigin(const std::shared_ptr<const doc::DocumentVersion>& base, const std::string& entityId, const V3& delta) {
    if (!base) return base;
    const int i = base->findEntity(entityId);
    if (i < 0) return base;
    core::Entity e = base->entities[static_cast<size_t>(i)]->typed;
    e.origin = {e.origin.x + delta[0], e.origin.y + delta[1], e.origin.z + delta[2]};
    doc::VersionEditor ed(base);
    ed.replaceEntity(e);
    return ed.finish();
}

std::optional<V3> gizmoPivot(const doc::DocumentVersion& doc, const std::vector<doc::SelRef>& sels) {
    if (sels.empty()) return std::nullopt;
    if (sels.size() > 1) return selectionPivot(doc, sels);
    const doc::SelRef& s = sels[0];
    if (s.isInstance()) {
        for (const auto& inst : doc.instances)
            if (inst->typed.id == s.instancePath.front()) return vec(inst->typed.origin);
        return std::nullopt;
    }
    if (!s.instancePath.empty() || s.entityId.empty()) return std::nullopt;
    const int ei = doc.findEntity(s.entityId);
    if (ei < 0) return std::nullopt;
    const auto& node = *doc.entities[static_cast<size_t>(ei)];
    const bool brushEntity = !node.brushes.empty() || node.typed.classname == "worldspawn";
    if (s.brushId.empty() && (!brushEntity || node.typed.showsCustomOriginGizmo())) return vec(node.typed.origin);
    V3 mn{1e300, 1e300, 1e300}, mx{-1e300, -1e300, -1e300};
    bool any = false;
    for (const auto& b : node.brushes) {
        if (!s.brushId.empty() && b->brush.id != s.brushId) continue;
        core::Vec3 a, c;
        if (!b->brush.bounds(a, c)) continue;
        any = true;
        for (size_t i = 0; i < 3; ++i) {
            mn[i] = std::min(mn[i], vec(a)[i]);
            mx[i] = std::max(mx[i], vec(c)[i]);
        }
    }
    if (!any) return vec(node.typed.origin);
    return V3{(mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, (mn[2] + mx[2]) / 2};
}

std::optional<V3> selectionPivot(const doc::DocumentVersion& doc, const std::vector<doc::SelRef>& sels) {
    V3 mn{1e300, 1e300, 1e300}, mx{-1e300, -1e300, -1e300};
    bool any = false;
    const auto grow = [&](const V3& p) {
        any = true;
        for (size_t i = 0; i < 3; ++i) {
            mn[i] = std::min(mn[i], p[i]);
            mx[i] = std::max(mx[i], p[i]);
        }
    };
    const auto growBrush = [&](const core::Brush& b) {
        core::Vec3 a, c;
        if (!b.bounds(a, c)) return false;
        grow(vec(a));
        grow(vec(c));
        return true;
    };
    const Targets t = selectionTargets(sels);
    for (const auto& inst : doc.instances)
        if (t.instances.count(inst->typed.id)) grow(vec(inst->typed.origin));
    for (const auto& node : doc.entities) {
        const auto ids = t.brushes.find(node->typed.id);
        if (t.entities.count(node->typed.id)) {
            bool boxed = false;
            for (const auto& b : node->brushes) boxed = growBrush(b->brush) || boxed;
            if (!boxed) grow(vec(node->typed.origin));
        } else if (ids != t.brushes.end()) {
            for (const auto& b : node->brushes)
                if (ids->second.count(b->brush.id)) growBrush(b->brush);
        }
    }
    if (!any) return std::nullopt;
    return V3{(mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, (mn[2] + mx[2]) / 2};
}

} // namespace native::tools
