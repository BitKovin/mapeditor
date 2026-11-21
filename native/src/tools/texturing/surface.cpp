// tools/texturing/surface -- see surface.h.
#include "tools/texturing/surface.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

#include "doc/json_write.h"

namespace native::tools::tex {

using nlohmann::json;

namespace {

constexpr double kPi = 3.14159265358979323846;

V3 sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
double dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
std::optional<V3> unit(const V3& v) {
    const double l = std::sqrt(dot(v, v));
    if (l < 1e-9) return std::nullopt;
    return V3{v[0] / l, v[1] / l, v[2] / l};
}
V3 neg(const V3& v) { return {-v[0], -v[1], -v[2]}; }
double jsSign(double v) { return v > 0 ? 1 : v < 0 ? -1 : 0; }

std::array<double, 2> planarCoords(const V3& n, const V3& p) {
    const double ax = std::fabs(n[0]), ay = std::fabs(n[1]), az = std::fabs(n[2]);
    if (ay >= ax && ay >= az) return {jsSign(n[1] != 0 ? n[1] : 1) * p[0], p[2]};   // floor / ceiling
    if (ax >= az) return {-jsSign(n[0] != 0 ? n[0] : 1) * p[2], -p[1]};              // X facing walls
    return {jsSign(n[2] != 0 ? n[2] : 1) * p[0], -p[1]};                             // Z facing walls
}

V3 faceNormal(const core::Brush& b, const core::BrushFace& f) { return core::texplan::faceNormalOf(b, f); }

// The new kind keeps stretch and mirroring; rotation, shift and shear restart (they were relative to the old axes).
Projection rekind(const Projection& p, core::texplan::ProjKind kind, const std::array<V3, 2>& basis) {
    Projection out;
    out.scale = p.scale;
    out.shift = {0, 0};
    out.rotate = 0;
    out.shear = 0;
    out.kind = kind;
    out.basis = basis;
    return out;
}

V2 withAxis(const V2& v, int axis, double x) {
    V2 out = v;
    out[static_cast<size_t>(axis)] = x;
    return out;
}

} // namespace

// ---- the face's projection ---------------------------------------------------------------------------------------------------------------------------

Projection defaultProjectionFor(const std::string& texture, const InfoFn& info) {
    Projection d;
    const core::texplan::TexInfo* i = info ? info(texture.empty() ? "dev/grey" : texture) : nullptr;
    if (i && i->width > 0 && i->height > 0) d.scale = core::texplan::naturalSize(*i);
    return d;
}

Projection projectionOfFace(const core::BrushFace& f, const InfoFn& info) {
    return core::texplan::projectionOf(core::texplan::faceKey(f, "projection"), info ? info(f.texture) : nullptr);
}

bool hasProjection(const core::BrushFace& f) { return core::texplan::faceKey(f, "projection") != nullptr; }

json projectionJson(const Projection& p) {
    json j = json::object();
    j["scale"] = json::array({doc::numberJson(p.scale[0]), doc::numberJson(p.scale[1])});
    j["shift"] = json::array({doc::numberJson(p.shift[0]), doc::numberJson(p.shift[1])});
    j["rotate"] = doc::numberJson(p.rotate);
    if (std::isfinite(p.shear) && p.shear != 0) j["shear"] = doc::numberJson(p.shear);
    if (p.basis && p.kind != core::texplan::ProjKind::Axial) {
        j["kind"] = p.kind == core::texplan::ProjKind::Ortho ? "ortho" : p.kind == core::texplan::ProjKind::Cam ? "cam" : "free";
        j["basis"] = json::array();
        for (const V3& v : *p.basis) j["basis"].push_back(json::array({doc::numberJson(v[0]), doc::numberJson(v[1]), doc::numberJson(v[2])}));
    }
    return j;
}

V2 placedCoords(const V3& n, const V3& q, const Projection& p) {
    std::array<double, 2> ab;
    if (p.basis && p.kind != core::texplan::ProjKind::Axial) ab = {dot(q, (*p.basis)[0]), dot(q, (*p.basis)[1])};
    else ab = planarCoords(n, q);
    const double r = p.rotate * kPi / 180, c = std::cos(r), s = std::sin(r);
    const double a = ab[0] * c - ab[1] * s, b = ab[0] * s + ab[1] * c;
    return {a + p.shear * b, b};
}

std::vector<V2> projectFace(const core::Brush& b, const core::BrushFace& f, const Projection& p) {
    const V3 n = faceNormal(b, f);
    std::vector<V2> out;
    for (uint32_t i : f.loop) {
        const V2 ac = placedCoords(n, core::texplan::v3(b.vertices[i]), p);
        out.push_back({ac[0] / p.scale[0] + p.shift[0], ac[1] / p.scale[1] + p.shift[1]});
    }
    return out;
}

void refreshLookHash(core::BrushFace& f) {
    std::string look;
    const auto add = [&look](const char* key, const json& v) {
        look += key;
        look += '=';
        look += v.dump();
        look += ';';
    };
    if (const json* p = core::texplan::faceKey(f, "projection")) add("projection", *p);
    if (!f.uvs.empty()) {
        json uvs = json::array();
        for (const core::Vec2& uv : f.uvs) uvs.push_back(json::array({doc::numberJson(uv.u), doc::numberJson(uv.v)}));
        add("uvs", uvs);
    }
    if (const json* r = core::texplan::faceKey(f, "repeat")) add("repeat", *r);
    if (const json* l = core::texplan::faceKey(f, "layout")) add("layout", *l);
    f.lookHash = look.empty() ? 0 : static_cast<uint64_t>(std::hash<std::string>{}(look)) | 1u;   // never 0 when something is set
}

core::BrushFace withProjection(const core::Brush& b, const core::BrushFace& f, const Projection& p) {
    return withProjectionUvs(f, p, projectFace(b, f, p));
}

core::BrushFace withProjectionUvs(const core::BrushFace& f, const Projection& p, const std::vector<V2>& uvs) {
    core::BrushFace nf = f;
    json look = f.look && f.look->is_object() ? *f.look : json::object();
    look["projection"] = projectionJson(p);
    nf.look = std::make_shared<const json>(std::move(look));
    nf.projection.scaleU = p.scale[0];
    nf.projection.scaleV = p.scale[1];
    nf.projection.shiftU = p.shift[0];
    nf.projection.shiftV = p.shift[1];
    nf.projection.rotate = p.rotate;
    nf.uvs.clear();
    for (const V2& uv : uvs) nf.uvs.push_back({uv[0], uv[1]});
    refreshLookHash(nf);
    return nf;
}

// ---- the inspector's fields --------------------------------------------------------------------------------------------------------------------------

SurfaceBasis surfaceBasis(const core::BrushFace& f, const InfoFn& info) {
    const core::texplan::TexInfo* i = info ? info(f.texture.empty() ? "dev/grey" : f.texture) : nullptr;
    SurfaceBasis out;
    if (!i) return out;
    out.texels = {static_cast<double>(i->width), static_cast<double>(i->height)};
    out.natural = core::texplan::naturalSize(*i);
    return out;
}

V2 shiftTexels(const Projection& p, const V2& texels) { return {p.shift[0] * texels[0], p.shift[1] * texels[1]}; }
V2 stretchOf(const Projection& p, const V2& natural) { return {p.scale[0] / natural[0], p.scale[1] / natural[1]}; }

Projection withShiftTexels(const Projection& p, int axis, double texels, const V2& size) {
    Projection out = p;
    out.shift = withAxis(p.shift, axis, texels / size[static_cast<size_t>(axis)]);
    return out;
}

Projection withStretch(const Projection& p, int axis, double stretch, const V2& natural) {
    if (!std::isfinite(stretch) || std::fabs(stretch) < 1e-4) return p;
    Projection out = p;
    out.scale = withAxis(p.scale, axis, stretch * natural[static_cast<size_t>(axis)]);
    return out;
}

Projection mirrorAxis(const Projection& p, int axis) {
    Projection out = p;
    out.scale = withAxis(p.scale, axis, -p.scale[static_cast<size_t>(axis)]);
    return out;
}

Projection withShear(const Projection& p, double shear) {
    Projection out = p;
    out.shear = std::isfinite(shear) && std::fabs(shear) > 1e-9 ? shear : 0;
    return out;
}

// ---- placement ---------------------------------------------------------------------------------------------------------------------------------------

Bounds faceBounds(const core::Brush& b, const core::BrushFace& f, const Projection& p) {
    const V3 n = faceNormal(b, f);
    const double inf = std::numeric_limits<double>::infinity();
    Bounds r{{inf, inf}, {-inf, -inf}};
    for (uint32_t i : f.loop) {
        const V2 q = placedCoords(n, core::texplan::v3(b.vertices[i]), p);
        r.min = {std::min(r.min[0], q[0]), std::min(r.min[1], q[1])};
        r.max = {std::max(r.max[0], q[0]), std::max(r.max[1], q[1])};
    }
    return r;
}

Projection justify(const core::Brush& b, const core::BrushFace& f, const Projection& p, JustifyEdge edge) {
    const Bounds bd = faceBounds(b, f, p);
    V2 shift = p.shift;
    const auto apply = [&](int axis, int where) {   // where: 0 start, 1 end, 2 centre
        const size_t a = static_cast<size_t>(axis);
        const double s = p.scale[a], x0 = bd.min[a] / s, x1 = bd.max[a] / s;
        const double lo = std::min(x0, x1), hi = std::max(x0, x1);
        shift[a] = where == 0 ? -lo : where == 1 ? 1 - hi : 0.5 - (lo + hi) / 2;
    };
    switch (edge) {
        case JustifyEdge::Left: apply(0, 0); break;
        case JustifyEdge::Right: apply(0, 1); break;
        case JustifyEdge::Top: apply(1, 0); break;
        case JustifyEdge::Bottom: apply(1, 1); break;
        case JustifyEdge::Center:
            apply(0, 2);
            apply(1, 2);
            break;
    }
    Projection out = p;
    out.shift = shift;
    return out;
}

Projection bottomLeft(const core::Brush& b, const core::BrushFace& f, const Projection& p) {
    return justify(b, f, justify(b, f, p, JustifyEdge::Left), JustifyEdge::Bottom);
}

Projection fitAxis(const core::Brush& b, const core::BrushFace& f, const Projection& p, int axis, double count) {
    const size_t a = static_cast<size_t>(axis);
    const Bounds bd = faceBounds(b, f, p);
    const double extent = std::max(bd.max[a] - bd.min[a], 1e-6), k = std::max(count, 1e-3);
    const double sign = p.scale[a] < 0 ? -1 : 1;
    Projection out = p;
    out.scale[a] = sign * extent / k;
    const double x0 = bd.min[a] / out.scale[a], x1 = bd.max[a] / out.scale[a];
    const double lo = std::min(x0, x1), hi = std::max(x0, x1);
    out.shift[a] = axis == 0 ? -lo : 1 - hi;   // u: left edge = texture start; v grows downwards: the face's bottom (largest v) = texture bottom (1)
    return out;
}

Projection fitBoth(const core::Brush& b, const core::BrushFace& f, const Projection& p, const V2& counts) {
    return fitAxis(b, f, fitAxis(b, f, p, 0, counts[0]), 1, counts[1]);
}

Projection matchGrid(const Projection& p, double grid) {
    if (!(grid > 0)) return p;
    const auto snap = [grid](double v) { return (v == 0 ? 1.0 : jsSign(v)) * std::max(1.0, std::round(std::fabs(v) / grid)) * grid; };
    Projection out = p;
    out.scale = {snap(p.scale[0]), snap(p.scale[1])};
    return out;
}

// ---- projection kinds --------------------------------------------------------------------------------------------------------------------------------

Projection projectOrtho(const core::Brush& b, const core::BrushFace& f, const Projection& p) {
    const V3 n = faceNormal(b, f);
    const V3 up = std::fabs(n[1]) > 0.999 ? V3{0, 0, -1} : V3{0, 1, 0};
    const auto right = unit(cross(up, n));
    if (!right) return p;
    const auto upInPlane = unit(cross(n, *right));
    if (!upInPlane) return p;
    return bottomLeft(b, f, rekind(p, core::texplan::ProjKind::Ortho, {*right, neg(*upInPlane)}));
}

std::optional<Projection> projectCam(const core::Brush& b, const core::BrushFace& f, const Projection& p, const V3& camRight, const V3& camUp) {
    const V3 n = faceNormal(b, f);
    const auto inPlane = [&n](const V3& v) { return unit({v[0] - n[0] * dot(v, n), v[1] - n[1] * dot(v, n), v[2] - n[2] * dot(v, n)}); };
    const auto r = inPlane(camRight), u = inPlane(camUp);
    if (!r || !u || std::fabs(dot(*r, *u)) > 0.999) return std::nullopt;
    return bottomLeft(b, f, rekind(p, core::texplan::ProjKind::Cam, {*r, neg(*u)}));
}

Projection projectAxial(const Projection& p) {
    Projection out;
    out.scale = p.scale;
    out.shift = {0, 0};
    out.rotate = 0;
    return out;
}

Projection resetSurface(const core::Brush& b, const core::BrushFace& f, const InfoFn& info) {
    return bottomLeft(b, f, defaultProjectionFor(f.texture, info));
}

std::optional<Projection> projectionFromAffine(const V3& n, const V3& gu, const V3& gv, double du, double dv) {
    const double nl = std::sqrt(dot(n, n));
    if (nl < 1e-9) return std::nullopt;
    const V3 nn{n[0] / nl, n[1] / nl, n[2] / nl};
    // an in-plane frame (right, down) that reads like the axial one: world up projected into the plane gives "down"
    const V3 up = std::fabs(nn[1]) > 0.999 ? V3{0, 0, -1} : V3{0, 1, 0};
    const auto e1 = unit(cross(up, nn));
    if (!e1) return std::nullopt;
    const auto upIn = unit(cross(nn, *e1));
    if (!upIn) return std::nullopt;
    const V3 e2 = neg(*upIn);
    const double m11 = dot(gu, *e1), m12 = dot(gu, e2), m21 = dot(gv, *e1), m22 = dot(gv, e2);
    const double r = std::hypot(m21, m22);
    if (r < 1e-9) return std::nullopt;
    const double c = m22 / r, s = m21 / r;
    const double A = m11 * c - m12 * s, B = m11 * s + m12 * c;
    if (std::fabs(A) < 1e-9) return std::nullopt;
    Projection out;
    out.scale = {1 / A, 1 / r};
    out.shift = {du, dv};
    out.rotate = std::atan2(s, c) * 180 / kPi;
    out.kind = core::texplan::ProjKind::Free;
    out.basis = std::array<V3, 2>{*e1, e2};
    if (std::fabs(B / A) > 1e-9) out.shear = B / A;
    return out;
}

// ---- baked default -----------------------------------------------------------------------------------------------------------------------------------

core::BrushFace withBottomLeftDefault(const core::Brush& b, const core::BrushFace& f, const InfoFn& info) {
    if (hasProjection(f) || !info || !info(f.texture.empty() ? "dev/grey" : f.texture)) return f;
    return withProjection(b, f, bottomLeft(b, f, defaultProjectionFor(f.texture, info)));
}

core::Brush alignBrush(const core::Brush& b, const InfoFn& info) {
    core::Brush out = b;
    for (size_t i = 0; i < out.faces.size(); ++i) out.faces[i] = withBottomLeftDefault(b, b.faces[i], info);
    return out;
}

} // namespace native::tools::tex
