// The face's UV map and the repetition plan: ports of shell/src/model/uv.ts (projection), geometry.ts (faceNormal, triangulateFace) and uvmesh.ts (texplan.h).
#include "core/texplan.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace core::texplan {

using nlohmann::json;

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr int kMaxSegs = 1024;

V3 sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 normalize(const V3& a) {
    const double l = std::sqrt(dot(a, a));
    return l < 1e-12 ? V3{0, 0, 0} : V3{a[0] / l, a[1] / l, a[2] / l};
}
double jsSign(double v) { return v > 0 ? 1 : v < 0 ? -1 : 0; }
bool fin(const json& v) { return v.is_number() && std::isfinite(v.get<double>()); }
int parity(long long m) { return static_cast<int>(((m % 2) + 2) % 2); }
}  // namespace

// ---- projection (uv.ts) ------------------------------------------------------------------------------------------------------------------------------
Projection projectionOf(const json* raw, const TexInfo* info) {
    Projection d;
    if (info && info->width > 0 && info->height > 0) { const V2 n = naturalSize(*info); d.scale = {n[0], n[1]}; }
    auto pair = [](const json* v, V2 dflt) -> V2 {
        if (v && v->is_array() && v->size() == 2 && fin((*v)[0]) && fin((*v)[1])) return {(*v)[0].get<double>(), (*v)[1].get<double>()};
        return dflt;
    };
    const json* p = raw && raw->is_object() ? raw : nullptr;
    auto key = [&](const char* k) -> const json* { return p && p->contains(k) ? &(*p)[k] : nullptr; };
    Projection out;
    const V2 scale = pair(key("scale"), d.scale);
    out.scale = {std::fabs(scale[0]) < 1e-6 ? d.scale[0] : scale[0], std::fabs(scale[1]) < 1e-6 ? d.scale[1] : scale[1]};
    out.shift = pair(key("shift"), d.shift);
    out.rotate = key("rotate") && fin(*key("rotate")) ? key("rotate")->get<double>() : 0;
    if (key("shear") && fin(*key("shear")) && key("shear")->get<double>() != 0) out.shear = key("shear")->get<double>();
    // basis: two finite, non-parallel vectors
    const json* b = key("basis");
    std::optional<std::array<V3, 2>> basis;
    if (b && b->is_array() && b->size() == 2) {
        auto ok = [](const json& v) { return v.is_array() && v.size() == 3 && fin(v[0]) && fin(v[1]) && fin(v[2]); };
        if (ok((*b)[0]) && ok((*b)[1])) {
            const V3 a{(*b)[0][0].get<double>(), (*b)[0][1].get<double>(), (*b)[0][2].get<double>()};
            const V3 c{(*b)[1][0].get<double>(), (*b)[1][1].get<double>(), (*b)[1][2].get<double>()};
            const double l0 = std::sqrt(dot(a, a)), l1 = std::sqrt(dot(c, c));
            if (l0 >= 1e-6 && l1 >= 1e-6 && std::fabs(dot(a, c) / (l0 * l1)) <= 0.999) basis = std::array<V3, 2>{a, c};
        }
    }
    const json* k = key("kind");
    if (k && k->is_string() && basis) {
        const std::string kind = k->get<std::string>();
        if (kind == "ortho" || kind == "cam" || kind == "free") {
            out.kind = kind == "ortho" ? ProjKind::Ortho : kind == "cam" ? ProjKind::Cam : ProjKind::Free;
            out.basis = basis;
        }
    }
    return out;
}

namespace {
std::array<double, 2> planarCoords(const V3& n, const V3& p) {
    const double ax = std::fabs(n[0]), ay = std::fabs(n[1]), az = std::fabs(n[2]);
    if (ay >= ax && ay >= az) return {jsSign(n[1] != 0 ? n[1] : 1) * p[0], p[2]};
    if (ax >= az) return {-jsSign(n[0] != 0 ? n[0] : 1) * p[2], -p[1]};
    return {jsSign(n[2] != 0 ? n[2] : 1) * p[0], -p[1]};
}
std::array<double, 2> placedCoords(const V3& n, const V3& q, const Projection& p) {
    std::array<double, 2> ab;
    if (p.basis && p.kind != ProjKind::Axial) ab = {dot(q, (*p.basis)[0]), dot(q, (*p.basis)[1])};
    else ab = planarCoords(n, q);
    const double r = p.rotate * kPi / 180, c = std::cos(r), s = std::sin(r);
    const double a = ab[0] * c - ab[1] * s, b = ab[0] * s + ab[1] * c;
    return {a + p.shear * b, b};
}
}  // namespace

V2 projectPoint(const V3& n, const V3& q, const Projection& p) {
    const auto ac = placedCoords(n, q, p);
    return {ac[0] / p.scale[0] + p.shift[0], ac[1] / p.scale[1] + p.shift[1]};
}

UvAffine projectionAffine(const V3& n, const Projection& p) {
    const V2 o = projectPoint(n, {0, 0, 0}, p), x = projectPoint(n, {1, 0, 0}, p), y = projectPoint(n, {0, 1, 0}, p), z = projectPoint(n, {0, 0, 1}, p);
    return {{x[0] - o[0], y[0] - o[0], z[0] - o[0], o[0]}, {x[1] - o[1], y[1] - o[1], z[1] - o[1], o[1]}};
}

namespace {
std::optional<std::array<double, 4>> solve4(std::array<std::array<double, 5>, 4> A) {
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int i = c + 1; i < 4; ++i) if (std::fabs(A[i][c]) > std::fabs(A[p][c])) p = i;
        if (std::fabs(A[p][c]) < 1e-12) return std::nullopt;
        std::swap(A[c], A[p]);
        for (int i = 0; i < 4; ++i) {
            if (i == c) continue;
            const double f = A[i][c] / A[c][c];
            for (int j = c; j < 5; ++j) A[i][j] -= f * A[c][j];
        }
    }
    return std::array<double, 4>{A[0][4] / A[0][0], A[1][4] / A[1][1], A[2][4] / A[2][2], A[3][4] / A[3][3]};
}
}  // namespace

std::optional<UvAffine> affineFromCorners(const std::vector<V3>& pts, const std::vector<V2>& uvs, const V3& n, double tol) {
    if (pts.size() < 3 || uvs.size() != pts.size()) return std::nullopt;
    double best = -1;
    size_t bi = 1, bj = 2;
    for (size_t i = 1; i < pts.size(); ++i)
        for (size_t j = i + 1; j < pts.size(); ++j) {
            const V3 c = cross(sub(pts[i], pts[0]), sub(pts[j], pts[0]));
            const double l = std::sqrt(dot(c, c));
            if (l > best) { best = l; bi = i; bj = j; }
        }
    if (best < 1e-9) return std::nullopt;
    const size_t idx[3] = {0, bi, bj};
    auto solveFor = [&](int k) -> std::optional<Aff> {
        std::array<std::array<double, 5>, 4> A{};
        for (int r = 0; r < 3; ++r) A[r] = {pts[idx[r]][0], pts[idx[r]][1], pts[idx[r]][2], 1, uvs[idx[r]][static_cast<size_t>(k)]};
        A[3] = {n[0], n[1], n[2], 0, 0};
        const auto s = solve4(A);
        if (!s) return std::nullopt;
        return Aff{(*s)[0], (*s)[1], (*s)[2], (*s)[3]};
    };
    const auto u = solveFor(0), v = solveFor(1);
    if (!u || !v) return std::nullopt;
    for (size_t i = 0; i < pts.size(); ++i) {
        if (std::fabs(evalAff(*u, pts[i]) - uvs[i][0]) > tol * std::max(1.0, std::fabs(uvs[i][0])) ||
            std::fabs(evalAff(*v, pts[i]) - uvs[i][1]) > tol * std::max(1.0, std::fabs(uvs[i][1])))
            return std::nullopt;
    }
    return UvAffine{*u, *v};
}

Extent extentOf(const UvAffine& m, const std::vector<V3>& pts) {
    Extent e;
    for (const V3& p : pts) {
        const double u = evalAff(m.u, p), v = evalAff(m.v, p);
        e.umin = std::min(e.umin, u); e.umax = std::max(e.umax, u); e.vmin = std::min(e.vmin, v); e.vmax = std::max(e.vmax, v);
    }
    return e;
}

V3 faceNormalOf(const Brush& b, const BrushFace& f) {
    if (f.loop.size() < 3) return {0, 0, 0};
    const V3 p0 = v3(b.vertices[f.loop[0]]);
    V3 sum{0, 0, 0};
    for (size_t i = 1; i + 1 < f.loop.size(); ++i) {
        const V3 c = cross(sub(v3(b.vertices[f.loop[i]]), p0), sub(v3(b.vertices[f.loop[i + 1]]), p0));
        sum = {sum[0] + c[0], sum[1] + c[1], sum[2] + c[2]};
    }
    return normalize(sum);
}

const json* faceKey(const BrushFace& f, const char* key) {
    if (!f.look || !f.look->is_object() || !f.look->contains(key)) return nullptr;
    return &(*f.look)[key];
}

namespace {
std::vector<V3> loopPoints(const Brush& b, const BrushFace& f) {
    std::vector<V3> out;
    for (uint32_t i : f.loop) out.push_back(v3(b.vertices[i]));
    return out;
}
std::vector<V2> storedUvs(const BrushFace& f) {
    std::vector<V2> out;
    for (const auto& uv : f.uvs) out.push_back({uv.u, uv.v});
    return out;
}
}  // namespace

UvAffine faceAffine(const Brush& b, const BrushFace& f, const InfoFn& info) {
    const V3 n = faceNormalOf(b, f);
    if (!f.uvs.empty() && f.uvs.size() == f.loop.size())
        if (const auto m = affineFromCorners(loopPoints(b, f), storedUvs(f), n)) return *m;
    return projectionAffine(n, projectionOf(faceKey(f, "projection"), info ? info(f.texture) : nullptr));
}

namespace {
std::array<double, 3> barycentric(const V3& p, const std::array<V3, 3>& t, const V3& n) {
    const double area = dot(cross(sub(t[1], t[0]), sub(t[2], t[0])), n);
    if (std::fabs(area) < 1e-14) return {1, 0, 0};
    const double w0 = dot(cross(sub(t[1], p), sub(t[2], p)), n) / area, w1 = dot(cross(sub(t[2], p), sub(t[0], p)), n) / area;
    return {w0, w1, 1 - w0 - w1};
}
}  // namespace

std::function<V2(const V3&)> faceUvAt(const Brush& b, const BrushFace& f, const InfoFn& info) {
    const V3 n = faceNormalOf(b, f);
    if (!f.uvs.empty() && f.uvs.size() == f.loop.size()) {
        if (const auto aff = affineFromCorners(loopPoints(b, f), storedUvs(f), n)) {
            const UvAffine m = *aff;
            return [m](const V3& p) { return V2{evalAff(m.u, p), evalAff(m.v, p)}; };
        }
        struct T { std::array<V3, 3> p; std::array<V2, 3> uv; };
        std::vector<T> tris;
        for (const auto& t : triangulateFace(b, f)) {
            T x;
            for (int k = 0; k < 3; ++k) {
                x.p[static_cast<size_t>(k)] = v3(b.vertices[t[static_cast<size_t>(k)]]);
                const size_t li = static_cast<size_t>(std::find(f.loop.begin(), f.loop.end(), t[static_cast<size_t>(k)]) - f.loop.begin());
                x.uv[static_cast<size_t>(k)] = {f.uvs[li].u, f.uvs[li].v};
            }
            tris.push_back(x);
        }
        return [tris, n](const V3& p) {
            const T* best = &tris[0];
            std::array<double, 3> bw{1, 0, 0};
            double bm = -1e300;
            for (const T& t : tris) {
                const auto w = barycentric(p, t.p, n);
                const double m = std::min({w[0], w[1], w[2]});
                if (m > bm) { bm = m; best = &t; bw = w; }
            }
            return V2{bw[0] * best->uv[0][0] + bw[1] * best->uv[1][0] + bw[2] * best->uv[2][0], bw[0] * best->uv[0][1] + bw[1] * best->uv[1][1] + bw[2] * best->uv[2][1]};
        };
    }
    const UvAffine m = projectionAffine(n, projectionOf(faceKey(f, "projection"), info ? info(f.texture) : nullptr));
    return [m](const V3& p) { return V2{evalAff(m.u, p), evalAff(m.v, p)}; };
}

std::vector<std::array<uint32_t, 3>> triangulateFace(const Brush& b, const BrushFace& f) {
    const V3 nrm = faceNormalOf(b, f);
    const double ax = std::fabs(nrm[0]), ay = std::fabs(nrm[1]), az = std::fabs(nrm[2]);
    const int k = ax >= ay && ax >= az ? 0 : ay >= az ? 1 : 2;
    const int u = k == 0 ? 1 : k == 1 ? 2 : 0, v = k == 0 ? 2 : k == 1 ? 0 : 1;
    auto p = [&](uint32_t i) { const V3 q = v3(b.vertices[i]); return std::array<double, 2>{q[static_cast<size_t>(u)], q[static_cast<size_t>(v)]}; };
    auto cr = [](const std::array<double, 2>& o, const std::array<double, 2>& a, const std::array<double, 2>& c) {
        return (a[0] - o[0]) * (c[1] - o[1]) - (a[1] - o[1]) * (c[0] - o[0]);
    };
    std::vector<uint32_t> idx(f.loop.begin(), f.loop.end());
    double area = 0;
    for (size_t i = 0; i < idx.size(); ++i) { const auto a = p(idx[i]), c = p(idx[(i + 1) % idx.size()]); area += a[0] * c[1] - c[0] * a[1]; }
    const bool flipped = area < 0;
    if (flipped) std::reverse(idx.begin(), idx.end());
    std::vector<std::array<uint32_t, 3>> tris;
    auto inside = [&](const std::array<double, 2>& q, const std::array<double, 2>& a, const std::array<double, 2>& c, const std::array<double, 2>& d) {
        return cr(a, c, q) >= -1e-12 && cr(c, d, q) >= -1e-12 && cr(d, a, q) >= -1e-12;
    };
    long long guard = static_cast<long long>(idx.size()) * static_cast<long long>(idx.size());
    while (idx.size() > 3 && guard-- > 0) {
        bool clipped = false;
        for (size_t i = 0; i < idx.size(); ++i) {
            const uint32_t a = idx[(i + idx.size() - 1) % idx.size()], c = idx[i], d = idx[(i + 1) % idx.size()];
            if (cr(p(a), p(c), p(d)) <= 1e-12) continue;
            bool any = false;
            for (uint32_t q : idx) if (q != a && q != c && q != d && inside(p(q), p(a), p(c), p(d))) { any = true; break; }
            if (any) continue;
            tris.push_back({a, c, d});
            idx.erase(idx.begin() + static_cast<std::ptrdiff_t>(i));
            clipped = true;
            break;
        }
        if (!clipped) break;
    }
    for (size_t i = 1; i + 1 < idx.size(); ++i) tris.push_back({idx[0], idx[i], idx[i + 1]});
    if (flipped) for (auto& t : tris) std::swap(t[1], t[2]);
    return tris;
}

// ---- one axis ---------------------------------------------------------------------------------------------------------------------------------------
double edgeInsetTexels(double size) { return std::max(0.5, std::min(2.0, size * 0.01)); }

namespace {
using Push = std::function<bool(double, double, double, double, bool)>;

std::optional<std::vector<Seg>> extension(const AxisRepeat& cfg, const AltRatio* alt, double xlo, double xhi, bool low, std::vector<Seg>& out, const Push& push) {
    auto T = [low](double x) { return low ? -x : 1 + x; };
    auto seg = [&](double xa, double xb, double s, double c, bool role) { return low ? push(T(xb), T(xa), s, c, role) : push(T(xa), T(xb), s, c, role); };
    if (alt) {
        const double edge = low ? 0 : 1, k = alt->k, off = alt->off;
        if (!(k > 1e-9)) return std::nullopt;
        if (cfg.fill == Fill::Repeat) {
            if (!seg(xlo, xhi, k, off - k * edge, true)) return std::nullopt;
            return out;
        }
        auto tbOf = [&](double x) { return k * (T(x) - edge) + off; };
        const double a = tbOf(xlo), b = tbOf(xhi);
        const long long mLo = static_cast<long long>(std::floor(std::min(a, b))), mHi = static_cast<long long>(std::floor(std::max(a, b)));
        if (mHi - mLo > kMaxSegs) return std::nullopt;
        for (long long m = mLo; m <= mHi; ++m) {
            const double t0 = (static_cast<double>(m) - off) / k + edge, t1 = (static_cast<double>(m) + 1 - off) / k + edge;
            const double ta = std::max(std::min(t0, t1), low ? T(xhi) : T(xlo)), tb = std::min(std::max(t0, t1), low ? T(xlo) : T(xhi));
            const bool even = parity(m) == 0;
            if (!push(ta, tb, even ? k : -k, even ? off - k * edge - static_cast<double>(m) : static_cast<double>(m) + 1 + k * edge - off, true)) return std::nullopt;
        }
        return out;
    }
    const double split = cfg.split;
    const double bw = low ? split : 1 - split;
    if (!(bw > 1e-6)) return std::nullopt;
    const long long jLo = static_cast<long long>(std::floor(xlo / bw)), jHi = static_cast<long long>(std::floor(xhi / bw));
    if (jHi - jLo > kMaxSegs) return std::nullopt;
    for (long long j = jLo; j <= jHi; ++j) {
        const double jd = static_cast<double>(j);
        const double xa = std::max(xlo, jd * bw), xb = std::min(xhi, (jd + 1) * bw);
        double s, c;
        if (low) {
            if (cfg.fill == Fill::Mirror && parity(j) == 0) { s = -1; c = -jd * bw; } else { s = 1; c = (jd + 1) * bw; }
        } else {
            if (cfg.fill == Fill::Mirror && parity(j) == 0) { s = -1; c = 2 + jd * bw; } else { s = 1; c = split - 1 - jd * bw; }
        }
        if (!seg(xa, xb, s, c, false)) return std::nullopt;
    }
    return out;
}

std::optional<std::vector<Seg>> axisSegmentsRaw(const AxisRepeat& cfg, const AltRatio* alt, double lo, double hi) {
    if (!(hi >= lo)) return std::vector<Seg>{};
    std::vector<Seg> out;
    const Push push = [&](double a, double b, double s, double c, bool role) {
        if (b - a <= 1e-12 && !(a == lo && b == hi)) return true;
        out.push_back({a, b, s, c, role, std::nullopt});
        return static_cast<int>(out.size()) <= kMaxSegs;
    };
    if (cfg.mode == AxisMode::Repeat) return std::vector<Seg>{{lo, hi, 1, 0, false, std::nullopt}};
    if (cfg.mode == AxisMode::Mirror) {
        const long long flo = static_cast<long long>(std::floor(lo)), fhi = static_cast<long long>(std::floor(hi));
        for (long long k = flo; k <= fhi && k <= flo + kMaxSegs + 1; ++k) {
            const double kd = static_cast<double>(k);
            const double a = std::max(lo, kd), b = std::min(hi, kd + 1);
            if (!push(a, b, parity(k) == 0 ? 1 : -1, parity(k) == 0 ? -kd : kd + 1, false)) return std::nullopt;
        }
        if (static_cast<int>(out.size()) > kMaxSegs) return std::nullopt;
        return out;
    }
    if (cfg.anchor == AnchorSide::End) {
        if (hi > 0 && !push(std::max(lo, 0.0), hi, 1, 0, false)) return std::nullopt;
        if (lo >= 0) return out;
        const double xlo = -std::min(hi, 0.0), xhi = -lo;
        auto ext = extension(cfg, alt, xlo, xhi, true, out, push);
        if (!ext) return std::nullopt;
        std::stable_sort(ext->begin(), ext->end(), [](const Seg& a, const Seg& b) { return a.lo < b.lo; });
        return ext;
    }
    if (lo < 1 && !push(lo, std::min(hi, 1.0), 1, 0, false)) return std::nullopt;
    if (hi <= 1) return out;
    const double xlo = std::max(lo, 1.0) - 1, xhi = hi - 1;
    return extension(cfg, alt, xlo, xhi, false, out, push);
}

std::optional<std::vector<Seg>> splitAtTiles(const std::vector<Seg>& segs, bool tMain, bool tAlt) {
    std::vector<Seg> out;
    for (const Seg& g : segs) {
        if (!(g.alt ? tAlt : tMain) || std::fabs(g.s) < 1e-12) { out.push_back(g); continue; }
        const double a = g.s * g.lo + g.c, b = g.s * g.hi + g.c;
        const long long mLo = static_cast<long long>(std::floor(std::min(a, b) + 1e-9)), mHi = static_cast<long long>(std::floor(std::max(a, b) - 1e-9));
        if (mHi - mLo > kMaxSegs) return std::nullopt;
        for (long long m = mLo; m <= mHi; ++m) {
            const double t0 = (static_cast<double>(m) - g.c) / g.s, t1 = (static_cast<double>(m) + 1 - g.c) / g.s;
            const double lo = std::max(g.lo, std::min(t0, t1)), hi = std::min(g.hi, std::max(t0, t1));
            if (hi - lo > 1e-9 || (g.hi - g.lo <= 1e-9 && lo == g.lo)) { Seg s = g; s.lo = lo; s.hi = hi; s.tile = static_cast<int>(m); out.push_back(s); }
        }
        if (static_cast<int>(out.size()) > kMaxSegs) return std::nullopt;
    }
    return out;
}
}  // namespace

std::optional<std::vector<Seg>> axisSegments(const AxisRepeat& cfg, const AltRatio* alt, double lo, double hi, bool tilesMain, bool tilesAlt) {
    auto segs = axisSegmentsRaw(cfg, alt, lo, hi);
    if (segs && (tilesMain || tilesAlt)) return splitAtTiles(*segs, tilesMain, tilesAlt);
    return segs;
}

// ---- a face plan -------------------------------------------------------------------------------------------------------------------------------------
namespace {
struct CV { double u, v; V3 w; };
std::vector<CV> clipCV(const std::vector<CV>& poly, int k, double bound, bool keepGE) {
    if (!std::isfinite(bound)) return poly;
    std::vector<CV> out;
    auto val = [k](const CV& p) { return k == 0 ? p.u : p.v; };
    auto inside = [&](const CV& p) { return keepGE ? val(p) >= bound : val(p) <= bound; };
    for (size_t i = 0; i < poly.size(); ++i) {
        const CV& a = poly[i];
        const CV& b = poly[(i + 1) % poly.size()];
        const bool ia = inside(a), ib = inside(b);
        if (ia) out.push_back(a);
        if (ia != ib) {
            const double t = (bound - val(a)) / (val(b) - val(a));
            CV p{a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t, {a.w[0] + (b.w[0] - a.w[0]) * t, a.w[1] + (b.w[1] - a.w[1]) * t, a.w[2] + (b.w[2] - a.w[2]) * t}};
            (k == 0 ? p.u : p.v) = bound;
            out.push_back(p);
        }
    }
    return out;
}
double areaCV(const std::vector<CV>& p) {
    double s = 0;
    for (size_t i = 0; i < p.size(); ++i) { const CV& a = p[i]; const CV& b = p[(i + 1) % p.size()]; s += a.u * b.v - b.u * a.v; }
    return std::fabs(s) / 2;
}
}  // namespace

PlanPtr makePlan(const TexInfo& A, const InfoFn& info, const UvAffine& map, const Extent& ext) {
    const AxisRepeat cu = A.props.u, cv = A.props.v;
    if (!(A.width > 0 && A.height > 0 && A.ppm > 0)) return nullptr;
    const bool clampU = edgeClamp(A, 0), clampV = edgeClamp(A, 1);
    if (isPlainProps(A.props) && !clampU && !clampV) return nullptr;
    const V2 natA = naturalSize(A);

    struct AltOf { TexInfo info; AltRatio ratio, cross; };
    auto altOf = [&](const AxisRepeat& cfg, int axis) -> std::optional<AltOf> {
        if (cfg.mode != AxisMode::Anchor || !cfg.alt) return std::nullopt;
        const TexInfo* B = info ? info(cfg.alt->texture) : nullptr;
        if (!B || !(B->width > 0 && B->height > 0 && B->ppm > 0)) return std::nullopt;
        const V2 nb = naturalSize(*B);
        const int along = axis, across = 1 - axis;
        return AltOf{*B, {natA[static_cast<size_t>(along)] / nb[static_cast<size_t>(along)], cfg.alt->offset[static_cast<size_t>(along)]},
                     {natA[static_cast<size_t>(across)] / nb[static_cast<size_t>(across)], cfg.alt->offset[static_cast<size_t>(across)]}};
    };
    const auto altU = altOf(cu, 0), altV = altOf(cv, 1);
    const bool tUm = clampU, tUa = altU && edgeClamp(altU->info, 0), tVm = clampV, tVa = altV && edgeClamp(altV->info, 1);
    const double shiftU = cu.mode == AxisMode::Anchor ? (cu.anchor == AnchorSide::End ? 1 - ext.umax : -ext.umin) : 0;
    const double shiftV = cv.mode == AxisMode::Anchor ? (cv.anchor == AnchorSide::End ? 1 - ext.vmax : -ext.vmin) : 0;
    const AltRatio* rU = altU ? &altU->ratio : nullptr;
    const AltRatio* rV = altV ? &altV->ratio : nullptr;
    const auto su0 = axisSegments(cu, rU, ext.umin + shiftU, ext.umax + shiftU, tUm, tUa);
    const auto sv0 = axisSegments(cv, rV, ext.vmin + shiftV, ext.vmax + shiftV, tVm, tVa);
    if (!su0 || !sv0 || static_cast<long long>(su0->size()) * static_cast<long long>(sv0->size()) > kMaxCells) return nullptr;

    auto plan = std::make_shared<FacePlan>();
    plan->texture = A.path;
    plan->textures.push_back(A.path);
    auto addTex = [&](const std::string& t) { if (std::find(plan->textures.begin(), plan->textures.end(), t) == plan->textures.end()) plan->textures.push_back(t); };
    if (altU) addTex(altU->info.path);
    if (altV) addTex(altV->info.path);

    // everything the pieces need, by value (the plan outlives this call)
    struct Ctx {
        TexInfo A; AxisRepeat cu, cv; std::optional<AltOf> altU, altV; bool tUm, tUa, tVm, tVa; double shiftU, shiftV; UvAffine map;
    };
    auto c = std::make_shared<Ctx>(Ctx{A, cu, cv, altU, altV, tUm, tUa, tVm, tVa, shiftU, shiftV, map});
    plan->pieces = [c](const std::array<V3, 3>& tri) {
        auto inset = [](double x, const std::optional<int>& tile, double size) {
            if (!tile) return x;
            const double e = edgeInsetTexels(size);
            return *tile + e / size + (x - *tile) * (1 - (2 * e) / size);
        };
        auto resolve = [&](const Seg& su, const Seg& sv, double tu, double tv, std::string& texture) -> V2 {
            const double u1 = su.s * tu + su.c, v1 = sv.s * tv + sv.c;
            if (sv.alt && c->altV) {
                const V2 o = c->cv.alt->offset;
                texture = c->altV->info.path;
                return {su.alt ? c->altV->cross.k * tu + o[0] : c->altV->cross.k * u1 + o[0], inset(v1, sv.tile, c->altV->info.height)};
            }
            if (su.alt && c->altU) {
                const V2 o = c->cu.alt->offset;
                texture = c->altU->info.path;
                return {inset(u1, su.tile, c->altU->info.width), c->altU->cross.k * v1 + o[1]};
            }
            texture = c->A.path;
            return {inset(u1, su.tile, c->A.width), inset(v1, sv.tile, c->A.height)};
        };
        std::vector<CV> verts;
        for (int i = 0; i < 3; ++i)
            verts.push_back({evalAff(c->map.u, tri[static_cast<size_t>(i)]) + c->shiftU, evalAff(c->map.v, tri[static_cast<size_t>(i)]) + c->shiftV,
                             {i == 0 ? 1.0 : 0.0, i == 1 ? 1.0 : 0.0, i == 2 ? 1.0 : 0.0}});
        double umin = 1e300, umax = -1e300, vmin = 1e300, vmax = -1e300;
        for (const CV& q : verts) { umin = std::min(umin, q.u); umax = std::max(umax, q.u); vmin = std::min(vmin, q.v); vmax = std::max(vmax, q.v); }
        const auto segsU = axisSegments(c->cu, c->altU ? &c->altU->ratio : nullptr, umin, umax, c->tUm, c->tUa);
        const auto segsV = axisSegments(c->cv, c->altV ? &c->altV->ratio : nullptr, vmin, vmax, c->tVm, c->tVa);
        std::vector<Piece> out;
        if (!segsU || !segsV) {
            Piece p{c->A.path, {}};
            for (const CV& q : verts) p.verts.push_back({q.w, {q.u - c->shiftU, q.v - c->shiftV}});
            out.push_back(p);
            return out;
        }
        for (const Seg& su : *segsU) {
            const auto strip = clipCV(clipCV(verts, 0, su.lo, true), 0, su.hi, false);
            if (strip.size() < 3 || areaCV(strip) < 1e-10) continue;
            for (const Seg& sv : *segsV) {
                const auto cell = clipCV(clipCV(strip, 1, sv.lo, true), 1, sv.hi, false);
                if (cell.size() < 3 || areaCV(cell) < 1e-10) continue;
                Piece p;
                resolve(su, sv, cell[0].u, cell[0].v, p.texture);
                for (const CV& q : cell) { std::string t; p.verts.push_back({q.w, resolve(su, sv, q.u, q.v, t)}); }
                out.push_back(std::move(p));
            }
        }
        return out;
    };
    return plan;
}

PlanPtr planForFace(const Brush& b, const BrushFace& f, const InfoFn& info, const GroupFn& groups) {
    const json* rep = faceKey(f, "repeat");
    const auto over = rep ? parseFaceRepeat(*rep) : std::nullopt;
    const json* lay = faceKey(f, "layout");
    const auto layout = lay ? parseFaceLayout(*lay) : std::nullopt;
    if (layout && f.loop.size() >= 3)
        if (auto lp = layoutPlanFor(b, f, *layout, info, groups)) return lp;
    const std::string path = f.texture.empty() ? std::string("dev/grey") : f.texture;
    const TexInfo* base = info ? info(path) : nullptr;
    if (!base || f.loop.size() < 3) return nullptr;
    const TexInfo A = withFaceRepeat(*base, over);
    if (isPlainProps(A.props) && !edgeClamp(A, 0) && !edgeClamp(A, 1)) return nullptr;
    const UvAffine map = faceAffine(b, f, info);
    return makePlan(A, info, map, extentOf(map, loopPoints(b, f)));
}

}  // namespace core::texplan
