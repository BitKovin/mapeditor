// tools/texturing/ftt -- see ftt.h.
#include "tools/texturing/ftt.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include "tools/texturing/flow.h"

namespace native::tools::tex {

namespace {
constexpr double kPi = 3.14159265358979323846;
namespace tp = core::texplan;

double snapTo(double v, double step) { return step > 0 ? std::round(v / step) * step : v; }
double keepSign(double v, double min = 0.01) { return std::fabs(v) < min ? (v < 0 ? -min : min) : v; }
const core::BrushFace* faceById(const core::Brush& b, uint32_t id) {
    for (const core::BrushFace& f : b.faces)
        if (f.id == id) return &f;
    return nullptr;
}
} // namespace

// ---- transform algebra -------------------------------------------------------------------------------------------------------------------------------

Xform translateXform(double du, double dv) { return {1, 0, 0, 1, du, dv}; }
Xform scaleXformAbout(double sx, double sy, const V2& p) { return {sx, 0, 0, sy, p[0] - sx * p[0], p[1] - sy * p[1]}; }
Xform rotateXformAbout(double deg, const V2& p) {
    const double r = deg * kPi / 180, c = std::cos(r), s = std::sin(r);
    return {c, -s, s, c, p[0] - c * p[0] + s * p[1], p[1] - s * p[0] - c * p[1]};
}
Xform mirrorXformAbout(char axis, double at) { return axis == 'u' ? scaleXformAbout(-1, 1, {at, 0}) : scaleXformAbout(1, -1, {0, at}); }
Xform composeXform(const Xform& second, const Xform& first) {
    const auto& [a2, b2, c2, d2, x2, y2] = second;
    const auto& [a1, b1, c1, d1, x1, y1] = first;
    return {a2 * a1 + b2 * c1, a2 * b1 + b2 * d1, c2 * a1 + d2 * c1, c2 * b1 + d2 * d1, a2 * x1 + b2 * y1 + x2, c2 * x1 + d2 * y1 + y2};
}
V2 applyXform(const Xform& t, const V2& p) { return {t[0] * p[0] + t[1] * p[1] + t[4], t[2] * p[0] + t[3] * p[1] + t[5]}; }

// ---- reading an island -------------------------------------------------------------------------------------------------------------------------------

std::vector<V2> faceUvPolygon(const core::Brush& b, const core::BrushFace& f, const InfoFn& info) {
    const tp::UvAffine m = tp::faceAffine(b, f, info);
    std::vector<V2> out;
    for (uint32_t i : f.loop) {
        const V3 v = tp::v3(b.vertices[i]);
        out.push_back({tp::evalAff(m.u, v), tp::evalAff(m.v, v)});
    }
    return out;
}

std::optional<UvBounds> polygonsBounds(const std::vector<std::vector<V2>>& polys) {
    const double inf = std::numeric_limits<double>::infinity();
    double u0 = inf, v0 = inf, u1 = -inf, v1 = -inf;
    for (const auto& p : polys)
        for (const V2& q : p) {
            u0 = std::min(u0, q[0]);
            u1 = std::max(u1, q[0]);
            v0 = std::min(v0, q[1]);
            v1 = std::max(v1, q[1]);
        }
    if (!std::isfinite(u0)) return std::nullopt;
    return UvBounds{{u0, v0}, {u1, v1}};
}

V2 boundsCentre(const UvBounds& b) { return {(b.min[0] + b.max[0]) / 2, (b.min[1] + b.max[1]) / 2}; }

std::vector<IslandFace> islandPolygons(const core::Brush& b, const std::vector<uint32_t>& ids, const InfoFn& info) {
    std::vector<IslandFace> out;
    for (uint32_t id : ids)
        if (const core::BrushFace* f = faceById(b, id)) out.push_back({id, faceUvPolygon(b, *f, info)});
    return out;
}

V2 idealSize(const core::Brush& b, const core::BrushFace& f, const InfoFn& info) {
    const auto poly = faceUvPolygon(b, f, info);
    const tp::TexInfo* ti = info ? info(f.texture.empty() ? "dev/grey" : f.texture) : nullptr;
    const V2 nat = ti ? tp::naturalSize(*ti) : V2{1, 1};
    const tp::UvAffine m = tp::faceAffine(b, f, info);
    const double gu = std::sqrt(m.u[0] * m.u[0] + m.u[1] * m.u[1] + m.u[2] * m.u[2]), gv = std::sqrt(m.v[0] * m.v[0] + m.v[1] * m.v[1] + m.v[2] * m.v[2]);
    const auto b2 = polygonsBounds({poly});
    if (!b2 || gu < 1e-12 || gv < 1e-12) return {1, 1};
    return {((b2->max[0] - b2->min[0]) / gu) / nat[0], ((b2->max[1] - b2->min[1]) / gv) / nat[1]};
}

// ---- applying a transform ----------------------------------------------------------------------------------------------------------------------------

FttResult transformFaces(const core::Brush& b, const std::vector<uint32_t>& ids, const Xform& t, const InfoFn& info) {
    const std::set<uint32_t> set(ids.begin(), ids.end());
    FttResult out;
    out.brush = b;
    for (size_t i = 0; i < b.faces.size(); ++i) {
        const core::BrushFace& f = b.faces[i];
        if (!set.count(f.id)) continue;
        const tp::UvAffine m = tp::faceAffine(b, f, info);
        const double a = t[0], bb = t[1], c = t[2], d = t[3], tx = t[4], ty = t[5];
        const V3 gu{a * m.u[0] + bb * m.v[0], a * m.u[1] + bb * m.v[1], a * m.u[2] + bb * m.v[2]};
        const V3 gv{c * m.u[0] + d * m.v[0], c * m.u[1] + d * m.v[1], c * m.u[2] + d * m.v[2]};
        const double du = a * m.u[3] + bb * m.v[3] + tx, dv = c * m.u[3] + d * m.v[3] + ty;
        const auto p = projectionFromAffine(tp::faceNormalOf(b, f), gu, gv, du, dv);
        if (!p) continue;
        ++out.changed;
        out.brush.faces[i] = withProjection(b, f, *p);
    }
    if (!out.changed) out.brush = b;
    return out;
}

std::optional<Xform> alignEdgeXform(const V2& a, const V2& b) {
    const double du = b[0] - a[0], dv = b[1] - a[1];
    if (std::hypot(du, dv) < 1e-12) return std::nullopt;
    const double phi = std::atan2(dv, du) * 180 / kPi;
    const double delta = std::round(phi / 90) * 90 - phi;
    if (std::fabs(delta) < 1e-9) return kIdentity;
    return rotateXformAbout(delta, {(a[0] + b[0]) / 2, (a[1] + b[1]) / 2});
}

FttResult unwrapFaces(const core::Brush& b, const std::vector<uint32_t>& ids, UnwrapMode mode, const InfoFn& info) {
    FttResult out;
    out.brush = b;
    if (mode == UnwrapMode::Follow) {
        if (ids.size() < 2) return out;
        const auto r = flowFaces(b, ids[0], std::vector<uint32_t>(ids.begin() + 1, ids.end()), info);
        if (!r) return out;
        out.brush = r->brush;
        out.changed = r->flowed.size();
        return out;
    }
    const std::set<uint32_t> set(ids.begin(), ids.end());
    for (size_t i = 0; i < b.faces.size(); ++i) {
        const core::BrushFace& f = b.faces[i];
        if (!set.count(f.id)) continue;
        const Projection cur = projectionOfFace(f, info);
        const Projection p = mode == UnwrapMode::Ortho ? projectOrtho(b, f, cur) : projectAxial(cur);
        out.brush.faces[i] = withProjection(b, f, p);
        ++out.changed;
    }
    if (!out.changed) out.brush = b;
    return out;
}

// ---- direct manipulation -----------------------------------------------------------------------------------------------------------------------------

V2 handlePoint(const UvBounds& bounds, Handle h, double rotateGap) {
    const double u0 = bounds.min[0], v0 = bounds.min[1], u1 = bounds.max[0], v1 = bounds.max[1], um = (u0 + u1) / 2, vm = (v0 + v1) / 2;
    switch (h) {
        case Handle::NW: return {u0, v0};
        case Handle::N: return {um, v0};
        case Handle::NE: return {u1, v0};
        case Handle::E: return {u1, vm};
        case Handle::SE: return {u1, v1};
        case Handle::S: return {um, v1};
        case Handle::SW: return {u0, v1};
        case Handle::W: return {u0, vm};
        case Handle::Rotate: return {um, v0 - rotateGap};
        default: return {um, vm};
    }
}

Xform dragXform(Handle kind, const V2& start, const V2& cur, const UvBounds& bounds, const DragOpts& o) {
    if (kind == Handle::Move) {
        // snap the island's top-left corner, not the pointer, so the island lands on the grid whatever was grabbed
        const double du = snapTo(bounds.min[0] + (cur[0] - start[0]), o.snap) - bounds.min[0];
        const double dv = snapTo(bounds.min[1] + (cur[1] - start[1]), o.snap) - bounds.min[1];
        return translateXform(du, dv);
    }
    const V2 c = boundsCentre(bounds);
    if (kind == Handle::Rotate) {
        const double a0 = std::atan2(start[1] - c[1], start[0] - c[0]), a1 = std::atan2(cur[1] - c[1], cur[0] - c[0]);
        double deg = (a1 - a0) * 180 / kPi;
        if (o.shift) deg = std::round(deg / 15) * 15;
        return rotateXformAbout(deg, c);
    }
    if (kind == Handle::None) return kIdentity;
    Handle opposite = Handle::SE;
    switch (kind) {
        case Handle::NW: opposite = Handle::SE; break;
        case Handle::N: opposite = Handle::S; break;
        case Handle::NE: opposite = Handle::SW; break;
        case Handle::E: opposite = Handle::W; break;
        case Handle::SE: opposite = Handle::NW; break;
        case Handle::S: opposite = Handle::N; break;
        case Handle::SW: opposite = Handle::NE; break;
        default: opposite = Handle::E; break;
    }
    const V2 pivot = handlePoint(bounds, opposite), h0 = handlePoint(bounds, kind);
    const bool moveU = kind != Handle::N && kind != Handle::S, moveV = kind != Handle::E && kind != Handle::W;
    const double cu = snapTo(h0[0] + (cur[0] - start[0]), o.snap), cv = snapTo(h0[1] + (cur[1] - start[1]), o.snap);
    double sx = moveU && std::fabs(h0[0] - pivot[0]) > 1e-12 ? (cu - pivot[0]) / (h0[0] - pivot[0]) : 1;
    double sy = moveV && std::fabs(h0[1] - pivot[1]) > 1e-12 ? (cv - pivot[1]) / (h0[1] - pivot[1]) : 1;
    sx = keepSign(sx);
    sy = keepSign(sy);
    if (o.shift && moveU && moveV) {
        const double k = std::fabs(sx) > std::fabs(sy) ? sx : sy;
        sx = k;
        sy = k;
    }
    return scaleXformAbout(sx, sy, pivot);
}

Handle hitTest(const UvBounds& bounds, const std::vector<std::vector<V2>>& polys, const V2& p, double tol, double rotateGap) {
    std::vector<Handle> hs(std::begin(kScaleHandles), std::end(kScaleHandles));
    hs.push_back(Handle::Rotate);
    for (Handle h : hs) {
        const V2 q = handlePoint(bounds, h, rotateGap);
        if (std::hypot(q[0] - p[0], q[1] - p[1]) <= tol) return h;
    }
    for (const auto& poly : polys)
        if (pointInPolygon(p, poly)) return Handle::Move;
    return Handle::None;
}

bool pointInPolygon(const V2& p, const std::vector<V2>& poly) {
    bool inside = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const double xi = poly[i][0], yi = poly[i][1], xj = poly[j][0], yj = poly[j][1];
        if ((yi > p[1]) != (yj > p[1]) && p[0] < ((xj - xi) * (p[1] - yi)) / (yj - yi) + xi) inside = !inside;
    }
    return inside;
}

std::optional<std::array<V2, 2>> nearestEdge(const std::vector<std::vector<V2>>& polys, const V2& p, double tol) {
    std::optional<std::array<V2, 2>> best;
    double bd = tol;
    for (const auto& poly : polys)
        for (size_t i = 0; i < poly.size(); ++i) {
            const V2 a = poly[i], b = poly[(i + 1) % poly.size()];
            const double dx = b[0] - a[0], dy = b[1] - a[1], l2 = dx * dx + dy * dy;
            if (l2 < 1e-18) continue;
            const double t = std::max(0.0, std::min(1.0, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / l2));
            const double d = std::hypot(p[0] - (a[0] + t * dx), p[1] - (a[1] + t * dy));
            if (d <= bd) {
                bd = d;
                best = std::array<V2, 2>{a, b};
            }
        }
    return best;
}

} // namespace native::tools::tex
