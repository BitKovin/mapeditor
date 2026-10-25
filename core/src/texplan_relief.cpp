// Reliefs over faces and the polygon entry point: ports of shell/src/model/reliefApply.ts and the fragment texturing of model/derived.ts
// (buildCarvedMesh), plus the environment that loads textures and texture groups from the assets folder (texplan.h).
#include "core/albedo.h"
#include "core/profile.h"
#include "core/texgroups.h"
#include "core/textures.h"
#include "core/texplan.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

namespace core::texplan {

namespace {
constexpr double kEps = 1e-9;
V3 sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
std::optional<V3> unit(const V3& a) { const double l = std::sqrt(dot(a, a)); if (l < 1e-12) return std::nullopt; return V3{a[0] / l, a[1] / l, a[2] / l}; }

struct P { double u, v; };
double polyArea(const std::vector<P>& p) { double s = 0; for (size_t i = 0; i < p.size(); ++i) { const P& a = p[i]; const P& b = p[(i + 1) % p.size()]; s += a.u * b.v - b.u * a.v; } return std::fabs(s) / 2; }
std::vector<P> clip(const std::vector<P>& poly, double a, double b, double c) {
    std::vector<P> out;
    for (size_t i = 0; i < poly.size(); ++i) {
        const P& p = poly[i];
        const P& q = poly[(i + 1) % poly.size()];
        const double fp = a * p.u + b * p.v + c, fq = a * q.u + b * q.v + c;
        if (fp >= 0) out.push_back(p);
        if ((fp >= 0) != (fq >= 0)) { const double t = fp / (fp - fq); out.push_back({p.u + (q.u - p.u) * t, p.v + (q.v - p.v) * t}); }
    }
    return out;
}

struct RTri { std::array<V3, 3> v; bool wall; };
std::vector<RTri> reliefTris(const Relief& r) {
    Brush b;
    b.id = "relief";
    for (const V3& v : r.vertices) b.vertices.push_back({v[0], v[1], v[2]});
    for (size_t i = 0; i < r.faces.size(); ++i) {
        BrushFace f;
        f.id = static_cast<uint32_t>(i);
        for (int k : r.faces[i]) f.loop.push_back(static_cast<uint32_t>(k));
        b.faces.push_back(f);
    }
    std::vector<RTri> out;
    for (const auto& f : b.faces)
        for (const auto& t : triangulateFace(b, f)) {
            const V3 a = v3(b.vertices[t[0]]), c = v3(b.vertices[t[1]]), d = v3(b.vertices[t[2]]);
            const double area = (c[0] - a[0]) * (d[1] - a[1]) - (d[0] - a[0]) * (c[1] - a[1]);
            out.push_back({{a, c, d}, std::fabs(area) < 1e-9});
        }
    return out;
}

// the relief's triangles, worked out once per relief
std::shared_ptr<const std::vector<RTri>> trisOf(const Relief& relief) {
    static std::mutex cacheM;
    static std::map<const Relief*, std::shared_ptr<const std::vector<RTri>>> cache;
    std::lock_guard<std::mutex> lock(cacheM);
    auto& slot = cache[&relief];
    if (!slot) slot = std::make_shared<const std::vector<RTri>>(reliefTris(relief));
    return slot;
}

// ---- connecting walls (reliefApply.ts) ----------------------------------------------------------------------------------------------------------------
// The surface of a relief ends at the border of its repeat and at the outline of the face. Where the height there is not 0, the neighbouring repeat does
// not continue it: the gap between the two edges is closed by a wall standing on the line, from one height to the other (between repeats) or down to the
// surface (the face's outline). Heights come from the relief's own surface triangles, sampled along the line.
using Span = ReliefSpan;
constexpr double kSideShift = 1e-7;

// the heights of the relief along the UV line a -> b as seen from its `side` (a unit UV vector pointing off the line): the line is moved a hair to that side
std::vector<Span> profile(const std::vector<const RTri*>& surface, const V2& a, const V2& b, const V2& side) {
    const double ax = a[0] + side[0] * kSideShift, ay = a[1] + side[1] * kSideShift, bx = b[0] + side[0] * kSideShift, by = b[1] + side[1] * kSideShift;
    const double dx = bx - ax, dy = by - ay;
    std::vector<Span> out;
    const long long i0 = static_cast<long long>(std::floor(std::min(ax, bx))), i1 = static_cast<long long>(std::ceil(std::max(ax, bx))) - 1;
    const long long j0 = static_cast<long long>(std::floor(std::min(ay, by))), j1 = static_cast<long long>(std::ceil(std::max(ay, by))) - 1;
    auto range = [](double p, double d, double lo, double hi, double* r) {
        if (std::fabs(d) < 1e-15) return p >= lo && p <= hi;
        double t0 = (lo - p) / d, t1 = (hi - p) / d;
        if (t0 > t1) std::swap(t0, t1);
        r[0] = std::max(r[0], t0);
        r[1] = std::min(r[1], t1);
        return r[0] < r[1];
    };
    for (long long j = j0; j <= j1; ++j)
        for (long long i = i0; i <= i1; ++i) {
            double cell[2] = {0, 1};
            if (!range(ax, dx, static_cast<double>(i), static_cast<double>(i) + 1, cell) || !range(ay, dy, static_cast<double>(j), static_cast<double>(j) + 1, cell)) continue;
            for (const RTri* rt : surface) {
                std::array<V3, 3> Q;
                for (size_t m = 0; m < 3; ++m) Q[m] = {static_cast<double>(i) + rt->v[m][0], static_cast<double>(j) + 1 - rt->v[m][1], rt->v[m][2]};
                const double so = ((Q[1][0] - Q[0][0]) * (Q[2][1] - Q[0][1]) - (Q[2][0] - Q[0][0]) * (Q[1][1] - Q[0][1])) > 0 ? 1 : -1;
                double r[2] = {cell[0], cell[1]};
                bool hit = true;
                for (size_t k = 0; k < 3 && hit; ++k) {
                    const V3 p = Q[k], q = Q[(k + 1) % 3];
                    const double ex = q[0] - p[0], ey = q[1] - p[1];
                    const double ha = -ey * so, hb = ex * so, hc = (ey * p[0] - ex * p[1]) * so;
                    const double f0 = ha * ax + hb * ay + hc, f1 = ha * bx + hb * by + hc;
                    if (f0 < 0 && f1 < 0) { hit = false; break; }
                    if (f0 < 0) r[0] = std::max(r[0], f0 / (f0 - f1));
                    else if (f1 < 0) r[1] = std::min(r[1], f0 / (f0 - f1));
                    if (r[1] - r[0] < 1e-9) hit = false;
                }
                if (!hit) continue;
                const double d = (Q[1][0] - Q[0][0]) * (Q[2][1] - Q[0][1]) - (Q[2][0] - Q[0][0]) * (Q[1][1] - Q[0][1]);
                auto zAt = [&](double t) {
                    const double x = ax + dx * t - Q[0][0], y = ay + dy * t - Q[0][1];
                    const double s = (x * (Q[2][1] - Q[0][1]) - y * (Q[2][0] - Q[0][0])) / d, u = ((Q[1][0] - Q[0][0]) * y - (Q[1][1] - Q[0][1]) * x) / d;
                    return Q[0][2] + (Q[1][2] - Q[0][2]) * s + (Q[2][2] - Q[0][2]) * u;
                };
                out.push_back({r[0], r[1], zAt(r[0]), zAt(r[1])});
            }
        }
    std::stable_sort(out.begin(), out.end(), [](const Span& x, const Span& y) { return x.t0 < y.t0; });
    return out;
}

// the height of `spans` over the interval ta..tb (inside one span) as [at ta, at tb]; false where the relief has no surface
bool heightsOver(const std::vector<Span>& spans, double ta, double tb, double* h) {
    for (const Span& s : spans) {
        if (s.t0 > ta + 1e-9 || s.t1 < tb - 1e-9) continue;
        const double k = (s.z1 - s.z0) / (s.t1 - s.t0);
        h[0] = s.z0 + k * (ta - s.t0);
        h[1] = s.z0 + k * (tb - s.t0);
        return true;
    }
    return false;
}

// pieces over t where the height of f runs from f0 to f1 and that of g (0 when null) from g0 to g1, one of them above the other throughout
struct Band { double ta, tb, f0, f1, g0, g1; };
std::vector<Band> bands(const std::vector<Span>& f, const std::vector<Span>* g, bool bridge = false) {
    std::vector<double> ts;
    for (const Span& s : f) { ts.push_back(s.t0); ts.push_back(s.t1); }
    if (g) for (const Span& s : *g) { ts.push_back(s.t0); ts.push_back(s.t1); }
    std::sort(ts.begin(), ts.end());
    ts.erase(std::unique(ts.begin(), ts.end()), ts.end());
    std::vector<Band> out;
    auto add = [&](double ta, double tb, double f0, double f1, double g0, double g1) {
        if (bridge ? std::max({std::fabs(f0), std::fabs(g0), std::fabs(f1), std::fabs(g1)}) < 1e-6
                   : std::fabs(f0 - g0) < 1e-6 && std::fabs(f1 - g1) < 1e-6) return;
        out.push_back({ta, tb, f0, f1, g0, g1});
    };
    for (size_t k = 0; k + 1 < ts.size(); ++k) {
        const double ta = ts[k], tb = ts[k + 1];
        if (tb - ta < 1e-9) continue;
        double hf[2], hg[2] = {0, 0};
        if (!heightsOver(f, ta, tb, hf)) continue;
        // A missing span means there is no relief surface on that side of this interval.
        // Do not substitute z=0 here: doing so creates a fake connector to the underlying
        // brush wall at every end of a disconnected relief segment.
        if (g && !heightsOver(*g, ta, tb, hg)) continue;
        const double d0 = hf[0] - hg[0], d1 = hf[1] - hg[1];
        if (!bridge && d0 * d1 < 0) {  // the two heights cross: two bands, each with one of them on top
            const double s = d0 / (d0 - d1), tm = ta + (tb - ta) * s, fm = hf[0] + (hf[1] - hf[0]) * s;
            add(ta, tm, hf[0], fm, hg[0], fm);
            add(tm, tb, fm, hf[1], fm, hg[1]);
        } else add(ta, tb, hf[0], hf[1], hg[0], hg[1]);
    }
    return out;
}
}  // namespace

std::shared_ptr<const Relief> reliefOf(const InfoFn& info, const std::string& texture) {
    const TexInfo* i = info ? info(texture.empty() ? std::string("dev/grey") : texture) : nullptr;
    if (!i || !i->props.relief || isFlatRelief(*i->props.relief)) return nullptr;
    return i->props.relief;
}

std::optional<std::vector<TexturedTri>> reliefTriangle(const std::array<V3, 3>& tri, const std::array<V2, 3>& uv, const V3& n, const Relief& relief,
                                                       const std::string& texture, ReliefBudget* budget, int border, const NeighbourFn* neighbour) {
    const V2 a0 = uv[0], a1 = uv[1], a2 = uv[2];
    const double det = (a1[0] - a0[0]) * (a2[1] - a0[1]) - (a2[0] - a0[0]) * (a1[1] - a0[1]);
    if (std::fabs(det) < kEps) return std::nullopt;
    const double sb = det > 0 ? 1 : -1;
    const double u0 = std::min({a0[0], a1[0], a2[0]}), u1 = std::max({a0[0], a1[0], a2[0]}), v0 = std::min({a0[1], a1[1], a2[1]}), v1 = std::max({a0[1], a1[1], a2[1]});
    const long long i0 = static_cast<long long>(std::floor(u0 + 1e-9)), i1 = static_cast<long long>(std::ceil(u1 - 1e-9)) - 1;
    const long long j0 = static_cast<long long>(std::floor(v0 + 1e-9)), j1 = static_cast<long long>(std::ceil(v1 - 1e-9)) - 1;
    if ((i1 - i0 + 1) * (j1 - j0 + 1) > kMaxReliefCells) return std::nullopt;
    const V3 e1 = sub(tri[1], tri[0]), e2 = sub(tri[2], tri[0]);
    const V2 du{a1[0] - a0[0], a1[1] - a0[1]}, dv{a2[0] - a0[0], a2[1] - a0[1]};
    auto world = [&](double u, double v) -> V3 {
        const double x = u - a0[0], y = v - a0[1];
        const double s = (x * dv[1] - y * dv[0]) / det, t = (du[0] * y - du[1] * x) / det;
        return {tri[0][0] + e1[0] * s + e2[0] * t, tri[0][1] + e1[1] * s + e2[1] * t, tri[0][2] + e1[2] * s + e2[2] * t};
    };
    auto at = [&](double u, double v, double z) -> V3 { const V3 w = world(u, v); return {w[0] + n[0] * z, w[1] + n[1] * z, w[2] + n[2] * z}; };
    const V3 wu = sub(world(1, 0), world(0, 0)), wv = sub(world(0, 1), world(0, 0));
    const double mapSign = dot(cross(wu, {-wv[0], -wv[1], -wv[2]}), n) >= 0 ? 1 : -1;
    const V2 U[3] = {a0, a1, a2};
    std::array<double, 3> edges[3];
    for (int k = 0; k < 3; ++k) {
        const V2 p = U[k], q = U[(k + 1) % 3];
        const double ex = q[0] - p[0], ey = q[1] - p[1];
        edges[k] = {-ey * sb, ex * sb, (ey * p[0] - ex * p[1]) * sb};
    }
    const std::shared_ptr<const std::vector<RTri>> rtsPtr = trisOf(relief);
    const auto& rts = *rtsPtr;
    std::vector<TexturedTri> out;
    auto push3 = [&](const std::array<V3, 3>& p, const V3& nn, const std::array<V2, 3>& t) {
        TexturedTri tt;
        tt.texture = texture;
        tt.pos = p;
        tt.uv = t;
        tt.normal = {nn, nn, nn};
        out.push_back(tt);
    };
    const std::vector<P> base = {{a0[0], a0[1]}, {a1[0], a1[1]}, {a2[0], a2[1]}};
    for (long long j = j0; j <= j1; ++j)
        for (long long i = i0; i <= i1; ++i) {
            const double id = static_cast<double>(i), jd = static_cast<double>(j);
            std::vector<P> cell = clip(clip(clip(clip(base, 1, 0, -id), -1, 0, id + 1), 0, 1, -jd), 0, -1, jd + 1);
            if (cell.size() < 3) cell.clear();
            for (const RTri& rt : rts) {
                std::array<V3, 3> Q;
                for (int m = 0; m < 3; ++m) Q[static_cast<size_t>(m)] = {id + rt.v[static_cast<size_t>(m)][0], jd + 1 - rt.v[static_cast<size_t>(m)][1], rt.v[static_cast<size_t>(m)][2]};
                if (!rt.wall) {
                    if (cell.empty()) continue;
                    const double so = ((Q[1][0] - Q[0][0]) * (Q[2][1] - Q[0][1]) - (Q[2][0] - Q[0][0]) * (Q[1][1] - Q[0][1])) > 0 ? 1 : -1;
                    std::vector<P> poly = cell;
                    for (int k = 0; k < 3 && poly.size() >= 3; ++k) {
                        const V3 p = Q[static_cast<size_t>(k)], q = Q[static_cast<size_t>((k + 1) % 3)];
                        const double ex = q[0] - p[0], ey = q[1] - p[1];
                        poly = clip(poly, -ey * so, ex * so, (ey * p[0] - ex * p[1]) * so);
                    }
                    if (poly.size() < 3 || polyArea(poly) < 1e-12) continue;
                    const double d = (Q[1][0] - Q[0][0]) * (Q[2][1] - Q[0][1]) - (Q[2][0] - Q[0][0]) * (Q[1][1] - Q[0][1]);
                    auto zAt = [&](double u, double v) {
                        const double x = u - Q[0][0], y = v - Q[0][1];
                        const double s = (x * (Q[2][1] - Q[0][1]) - y * (Q[2][0] - Q[0][0])) / d, t = ((Q[1][0] - Q[0][0]) * y - (Q[1][1] - Q[0][1]) * x) / d;
                        return Q[0][2] + (Q[1][2] - Q[0][2]) * s + (Q[2][2] - Q[0][2]) * t;
                    };
                    const V3 c0 = at(Q[0][0], Q[0][1], Q[0][2]), c1 = at(Q[1][0], Q[1][1], Q[1][2]), c2 = at(Q[2][0], Q[2][1], Q[2][2]);
                    const auto raw = unit(cross(sub(c1, c0), sub(c2, c0)));
                    if (!raw) continue;
                    const V3 nn = so * sb > 0 ? *raw : V3{-(*raw)[0], -(*raw)[1], -(*raw)[2]};
                    const long long tris = static_cast<long long>(poly.size()) - 2;
                    if (budget) { if (budget->left < tris) return std::nullopt; budget->left -= tris; }
                    for (size_t k = 1; k + 1 < poly.size(); ++k) {
                        const P q3[3] = {poly[0], poly[k], poly[k + 1]};
                        std::array<V3, 3> pos;
                        std::array<V2, 3> uvs;
                        for (int m = 0; m < 3; ++m) { pos[static_cast<size_t>(m)] = at(q3[m].u, q3[m].v, zAt(q3[m].u, q3[m].v)); uvs[static_cast<size_t>(m)] = {q3[m].u, q3[m].v}; }
                        push3(pos, nn, uvs);
                    }
                } else {
                    int L0 = 0, L1 = 0;
                    double best = -1;
                    for (int x = 0; x < 3; ++x)
                        for (int y = x + 1; y < 3; ++y) {
                            const double l = std::hypot(Q[static_cast<size_t>(x)][0] - Q[static_cast<size_t>(y)][0], Q[static_cast<size_t>(x)][1] - Q[static_cast<size_t>(y)][1]);
                            if (l > best) { best = l; L0 = x; L1 = y; }
                        }
                    if (best < 1e-9) continue;
                    const V3 P0 = Q[static_cast<size_t>(L0)], P1 = Q[static_cast<size_t>(L1)];
                    // A relief-data wall on an integer UV line is a tile-boundary side face. Never
                    // emit it independently for every repeat: the seam pass below compares the
                    // actual relief surfaces on both sides and creates a connector only when needed.
                    const auto onGridLine = [](double x, double y) { return std::fabs(x - y) < 1e-9 && std::fabs(x - std::round(x)) < 1e-9; };
                    if (onGridLine(P0[0], P1[0]) || onGridLine(P0[1], P1[1])) continue;
                    double slo = 0, shi = 1;
                    for (const auto& e : edges) {
                        const double f0 = e[0] * P0[0] + e[1] * P0[1] + e[2], f1 = e[0] * P1[0] + e[1] * P1[1] + e[2];
                        if (std::fabs(f1 - f0) < 1e-12) { if (f0 < 0) { slo = 1; shi = 0; } continue; }
                        const double s = -f0 / (f1 - f0);
                        if (f1 > f0) slo = std::max(slo, s); else shi = std::min(shi, s);
                    }
                    if (shi - slo < 1e-9) continue;
                    std::vector<P> poly;
                    for (const V3& q : Q) poly.push_back({((q[0] - P0[0]) * (P1[0] - P0[0]) + (q[1] - P0[1]) * (P1[1] - P0[1])) / (best * best), q[2]});
                    poly = clip(clip(poly, 1, 0, -slo), -1, 0, shi);
                    if (poly.size() < 3) continue;
                    const long long tris = static_cast<long long>(poly.size()) - 2;
                    if (budget) { if (budget->left < tris) return std::nullopt; budget->left -= tris; }
                    auto pos = [&](const P& p) { return at(P0[0] + (P1[0] - P0[0]) * p.u, P0[1] + (P1[1] - P0[1]) * p.u, p.v); };
                    for (size_t k = 1; k + 1 < poly.size(); ++k) {
                        std::array<P, 3> t3 = {poly[0], poly[k], poly[k + 1]};
                        if (mapSign < 0) std::swap(t3[1], t3[2]);
                        const std::array<V3, 3> w = {pos(t3[0]), pos(t3[1]), pos(t3[2])};
                        const auto nn = unit(cross(sub(w[1], w[0]), sub(w[2], w[0])));
                        if (!nn) continue;
                        std::array<V2, 3> uvs;
                        for (int m = 0; m < 3; ++m) uvs[static_cast<size_t>(m)] = {P0[0] + (P1[0] - P0[0]) * t3[static_cast<size_t>(m)].u, P0[1] + (P1[1] - P0[1]) * t3[static_cast<size_t>(m)].u};
                        push3(w, *nn, uvs);
                    }
                }
            }
        }

    // connecting walls: along the borders between repeats inside the triangle, and along the triangle's edges that are the face's outline
    std::vector<const RTri*> surface;
    for (const RTri& rt : rts) if (!rt.wall) surface.push_back(&rt);
    bool over = false;
    // a wall standing on the UV line a -> b between the heights of f (seen from sideF, a UV direction) and g (the other side of the line; null: the surface, 0)
    auto wall = [&](const V2& a, const V2& b, const std::vector<Span>& f, const std::vector<Span>* g, const V2& sideF, const std::optional<Neighbour>& nb = std::nullopt) {
        for (const Band& band : bands(f, nb && !g ? &nb->spans : g, nb.has_value())) {
            if (over) return;
            // the wall belongs to the higher edge and looks at the lower one's side (the outline: the open air outside the face)
            const bool higherF = band.f0 + band.f1 >= band.g0 + band.g1;
            const V2 fd = g ? (higherF ? V2{-sideF[0], -sideF[1]} : sideF) : V2{-sideF[0], -sideF[1]};
            auto lp = [&](double t) -> V2 { return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t}; };
            const V2 mid = lp(0.5);
            // at a corner the wall looks away from both faces
            const V3 facing = nb ? V3{n[0] + nb->n[0], n[1] + nb->n[1], n[2] + nb->n[2]} : sub(world(mid[0] + fd[0], mid[1] + fd[1]), world(mid[0], mid[1]));
            struct Corner { V3 w; V2 uv; };
            auto corner = [&](double t, double z, const V3& along) -> Corner { const V2 p = lp(t); const V3 e = at(p[0], p[1], 0); return {V3{e[0] + along[0] * z, e[1] + along[1] * z, e[2] + along[2] * z}, p}; };
            const V3 low = nb ? nb->n : n;  // the second edge of the wall stands on the surface (g = 0) or on the neighbour's relief
            std::vector<Corner> ring = {corner(band.ta, band.f0, n), corner(band.ta, band.g0, low), corner(band.tb, band.g1, low), corner(band.tb, band.f1, n)};
            if (!nb) {  // the two edges stand on one line: they meet where the heights do
                if (std::fabs(band.f0 - band.g0) < 1e-6) ring.erase(ring.begin() + 1);
                if (std::fabs(band.f1 - band.g1) < 1e-6) ring.pop_back();
            } else {  // a bevel: it shrinks to a triangle where an end of the wall lies on the edge itself
                if (std::hypot(band.f0, band.g0) < 1e-6) ring.erase(ring.begin() + 1);
                if (std::hypot(band.f1, band.g1) < 1e-6) ring.pop_back();
            }
            if (ring.size() < 3) continue;
            const long long tris = static_cast<long long>(ring.size()) - 2;
            if (budget) { if (budget->left < tris) { over = true; return; } budget->left -= tris; }
            for (size_t k = 1; k + 1 < ring.size(); ++k) {
                std::array<Corner, 3> t3 = {ring[0], ring[k], ring[k + 1]};
                auto nn = unit(cross(sub(t3[1].w, t3[0].w), sub(t3[2].w, t3[0].w)));
                if (!nn) continue;
                if (dot(*nn, facing) < 0) { std::swap(t3[1], t3[2]); nn = V3{-(*nn)[0], -(*nn)[1], -(*nn)[2]}; }
                push3({t3[0].w, t3[1].w, t3[2].w}, *nn, {t3[0].uv, t3[1].uv, t3[2].uv});
            }
        }
    };
    // the part of the UV line a -> b (as t) that lies inside the base triangle
    auto inside = [&](const V2& a, const V2& b) -> std::array<double, 2> {
        double lo = 0, hi = 1;
        for (const auto& e : edges) {
            const double f0 = e[0] * a[0] + e[1] * a[1] + e[2], f1 = e[0] * b[0] + e[1] * b[1] + e[2];
            if (std::fabs(f1 - f0) < 1e-12) { if (f0 < 0) { lo = 1; hi = 0; } continue; }
            const double s = -f0 / (f1 - f0);
            if (f1 > f0) lo = std::max(lo, s); else hi = std::min(hi, s);
        }
        return {lo, hi};
    };
    auto seam = [&](const V2& a, const V2& b, const V2& side) {
        const auto [lo, hi] = inside(a, b);
        if (hi - lo < 1e-9) return;
        const V2 p{a[0] + (b[0] - a[0]) * lo, a[1] + (b[1] - a[1]) * lo}, q{a[0] + (b[0] - a[0]) * hi, a[1] + (b[1] - a[1]) * hi};
        const std::vector<Span> f = profile(surface, p, q, side), g = profile(surface, p, q, V2{-side[0], -side[1]});
        wall(p, q, f, &g, side);
    };
    for (long long i = i0 + 1; i <= i1; ++i) seam({static_cast<double>(i), static_cast<double>(j0)}, {static_cast<double>(i), static_cast<double>(j1 + 1)}, {-1, 0});
    for (long long j = j0 + 1; j <= j1; ++j) seam({static_cast<double>(i0), static_cast<double>(j)}, {static_cast<double>(i1 + 1), static_cast<double>(j)}, {0, -1});
    for (size_t k = 0; k < 3 && !over; ++k) {
        if (!(border & (1 << k))) continue;
        const V2 p = U[k], q = U[(k + 1) % 3];
        const double len = std::hypot(edges[k][0], edges[k][1]);
        if (len < 1e-12) continue;
        const auto nb = neighbour && *neighbour ? (*neighbour)(tri[k], tri[(k + 1) % 3]) : std::nullopt;
        // Do not close relief ends against the host brush face. Outline connectors are only
        // meaningful at an edge backed by another relief surface.
        if (!nb || nb->spans.empty()) continue;
        const V2 inward{edges[k][0] / len, edges[k][1] / len};  // the edge's half-plane points into the triangle
        wall(p, q, profile(surface, p, q, inward), nullptr, inward, nb);
    }
    if (over) return std::nullopt;
    return out;
}

NeighbourFn neighbourFn(const Brush& brush, const BrushFace& face, const InfoFn& info, const GroupFn& groups) {
    struct Other { V3 n; std::vector<V3> loop; std::function<V2(const V3&)> uvAt; V3 centre; std::shared_ptr<const std::vector<RTri>> tris; std::vector<const RTri*> surface; };
    const V3 nA = faceNormalOf(brush, face);
    const V3 base = v3(brush.vertices[face.loop[0]]);
    auto others = std::make_shared<std::optional<std::vector<Other>>>();
    return [&brush, &face, info, groups, nA, base, others](const V3& a, const V3& b) -> std::optional<Neighbour> {
        if (!others->has_value()) {
            std::vector<Other> out;
            for (const BrushFace& o : brush.faces) {
                if (o.id == face.id) continue;
                const auto r = reliefOf(info, o.texture.empty() ? std::string("dev/grey") : o.texture);
                if (!r || planForFace(brush, o, info, groups)) continue;
                Other x;
                for (uint32_t i : o.loop) x.loop.push_back(v3(brush.vertices[i]));
                x.centre = {0, 0, 0};
                for (const V3& v : x.loop) for (size_t k = 0; k < 3; ++k) x.centre[k] += v[k] / static_cast<double>(x.loop.size());
                if (dot(nA, sub(x.centre, base)) > -1e-9) continue;  // not behind the face: a flat or concave corner
                x.n = faceNormalOf(brush, o);
                x.uvAt = faceUvAt(brush, o, info);
                x.tris = trisOf(*r);
                for (const RTri& rt : *x.tris) if (!rt.wall) x.surface.push_back(&rt);
                out.push_back(std::move(x));
            }
            *others = std::move(out);
        }
        auto onSegment = [](const V3& p, const V3& s, const V3& e) {
            const V3 d = sub(e, s);
            const double len2 = dot(d, d);
            if (len2 < 1e-12) return false;
            const double t = dot(sub(p, s), d) / len2;
            if (t < -1e-6 || t > 1 + 1e-6) return false;
            const V3 c = sub(V3{s[0] + d[0] * t, s[1] + d[1] * t, s[2] + d[2] * t}, p);
            return dot(c, c) < 1e-10;
        };
        for (const Other& o : **others)
            for (size_t k = 0; k < o.loop.size(); ++k) {
                const V3& p = o.loop[k];
                const V3& q = o.loop[(k + 1) % o.loop.size()];
                if (!onSegment(a, p, q) || !onSegment(b, p, q)) continue;
                const V2 uv0 = o.uvAt(a), uv1 = o.uvAt(b), c = o.uvAt(o.centre);
                const double dx = uv1[0] - uv0[0], dy = uv1[1] - uv0[1], len = std::hypot(dx, dy);
                if (len < 1e-12) return std::nullopt;
                // the side of the edge the neighbour's face is on, in its UV
                V2 side{-dy / len, dx / len};
                if ((c[0] - uv0[0]) * side[0] + (c[1] - uv0[1]) * side[1] < 0) side = {-side[0], -side[1]};
                return Neighbour{o.n, profile(o.surface, uv0, uv1, side)};
            }
        return std::nullopt;
    };
}

std::vector<std::vector<char>> polygonBorders(const std::vector<std::vector<V3>>& polys) {
    constexpr double Q = 1e4;
    using Key = std::array<long long, 3>;
    auto key = [&](const V3& p) -> Key { return {std::llround(p[0] * Q), std::llround(p[1] * Q), std::llround(p[2] * Q)}; };
    std::map<Key, V3> all;
    for (const auto& p : polys) for (const V3& v : p) all[key(v)] = v;
    // every directed edge, cut at the vertices lying on it, so a neighbour that splits an edge in two still matches it
    auto pieces = [&](const V3& a, const V3& b) {
        const V3 d = sub(b, a);
        const double len2 = dot(d, d);
        std::vector<std::pair<double, V3>> on;
        if (len2 > 1e-12)
            for (const auto& [k, p] : all) {
                const double t = dot(sub(p, a), d) / len2;
                if (t <= 1e-6 || t >= 1 - 1e-6) continue;
                const V3 c = sub(V3{a[0] + d[0] * t, a[1] + d[1] * t, a[2] + d[2] * t}, p);
                if (dot(c, c) < 1e-10) on.push_back({t, p});
            }
        std::sort(on.begin(), on.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
        std::vector<std::pair<Key, Key>> out;
        V3 prev = a;
        for (const auto& [t, p] : on) { out.push_back({key(prev), key(p)}); prev = p; }
        out.push_back({key(prev), key(b)});
        return out;
    };
    std::set<std::pair<Key, Key>> have;
    std::vector<std::vector<std::vector<std::pair<Key, Key>>>> cut;
    for (const auto& p : polys) {
        cut.emplace_back();
        for (size_t k = 0; k < p.size(); ++k) cut.back().push_back(pieces(p[k], p[(k + 1) % p.size()]));
    }
    for (const auto& edges : cut) for (const auto& e : edges) for (const auto& [x, y] : e) if (x != y) have.insert({x, y});
    std::vector<std::vector<char>> out;
    for (const auto& edges : cut) {
        out.emplace_back();
        for (const auto& e : edges) {
            bool outline = false;
            for (const auto& [x, y] : e) if (x != y && !have.count({y, x})) outline = true;
            out.back().push_back(outline ? 1 : 0);
        }
    }
    return out;
}

size_t OutlineIndex::add(const std::string& faceKey, std::vector<V3> poly, const std::string& texture) {
    V3 nrm{0, 0, 0};  // Newell
    for (size_t i = 0; i < poly.size(); ++i) {
        const V3 c = cross(poly[i], poly[(i + 1) % poly.size()]);
        nrm = {nrm[0] + c[0], nrm[1] + c[1], nrm[2] + c[2]};
    }
    normals_.push_back(unit(nrm).value_or(V3{0, 0, 0}));
    textures_.push_back(texture);
    gridBuilt_ = false;
    polys_.push_back(std::move(poly));
    keys_.push_back(faceKey);
    groups_[faceKey].push_back(polys_.size() - 1);
    return polys_.size() - 1;
}

const std::vector<char>& OutlineIndex::of(size_t polygon) {
    const auto it = done_.find(polygon);
    if (it != done_.end()) return it->second;
    const std::vector<size_t>& g = groups_[keys_[polygon]];
    std::vector<std::vector<V3>> face;
    for (size_t k : g) face.push_back(polys_[k]);
    auto flags = polygonBorders(face);
    for (size_t m = 0; m < g.size(); ++m) {
        const std::vector<V3>& p = polys_[g[m]];
        for (size_t k = 0; k < flags[m].size(); ++k)
            if (flags[m][k] && continued(g[m], p[k], p[(k + 1) % p.size()])) flags[m][k] = 0;
        done_[g[m]] = std::move(flags[m]);
    }
    return done_[polygon];
}

int fanBorder(size_t i, size_t n, const std::vector<char>& edge) {
    return (i == 1 && edge[0] ? 1 : 0) | (edge[i] ? 2 : 0) | (i + 2 == n && edge[n - 1] ? 4 : 0);
}

int pieceBorder(const std::vector<PieceVert>& verts, int triBorder) {
    if (!triBorder) return 0;
    int bits = 0;
    for (size_t m = 0; m < verts.size(); ++m) {
        const V3& a = verts[m].w;
        const V3& b = verts[(m + 1) % verts.size()].w;
        // the triangle's edge k runs from corner k to k + 1: the weight of corner k + 2 is 0 all along it
        for (size_t k = 0; k < 3; ++k) if ((triBorder & (1 << k)) && std::fabs(a[(k + 2) % 3]) < 1e-9 && std::fabs(b[(k + 2) % 3]) < 1e-9) bits |= 1 << m;
    }
    return bits;
}

namespace {
V3 lerp3(const std::array<V3, 3>& t, const V3& w) {
    return {t[0][0] * w[0] + t[1][0] * w[1] + t[2][0] * w[2], t[0][1] * w[0] + t[1][1] * w[1] + t[2][1] * w[2], t[0][2] * w[0] + t[1][2] * w[1] + t[2][2] * w[2]};
}
bool planHasRelief(const FacePlan& plan, const InfoFn& info) {
    for (const auto& t : plan.textures) if (reliefOf(info, t)) return true;
    return false;
}
void flatTri(std::vector<TexturedTri>& out, const std::string& tex, const std::array<V3, 3>& p, const std::array<V2, 3>& uv, const V3& n) {
    TexturedTri t;
    t.texture = tex;
    t.pos = p;
    t.uv = uv;
    t.normal = {n, n, n};
    out.push_back(t);
}
}  // namespace

std::vector<TexturedTri> texturePolygon(const std::vector<V3>& pts, const V3& normal, const Brush* pb, const BrushFace* pf, const InfoFn& info,
                                        const GroupFn& groups, ReliefBudget* budget, const std::string& defaultTexture, const std::vector<char>* border) {
    std::vector<TexturedTri> out;
    const size_t n = pts.size();
    if (n < 3) return out;
    const bool parent = pb && pf;
    const std::string tex = parent && !pf->texture.empty() ? pf->texture : defaultTexture;
    const auto relief = reliefOf(info, tex);
    const PlanPtr plan = parent ? planForFace(*pb, *pf, info, groups) : nullptr;
    std::vector<V2> uv;
    if (!plan) {
        if (parent) { const auto at = faceUvAt(*pb, *pf, info); for (const V3& p : pts) uv.push_back(at(p)); }
        else { const Projection def; for (const V3& p : pts) uv.push_back(projectPoint(normal, p, def)); }
    }
    const bool withRelief = plan && planHasRelief(*plan, info);
    // what stands next to the face's outline edges (a corner with a face that has a relief too is bevelled): only worked out when an edge is on the outline
    NeighbourFn neighbour;
    if (border && parent && (relief || withRelief) && std::find(border->begin(), border->end(), 1) != border->end()) neighbour = neighbourFn(*pb, *pf, info, groups);
    for (size_t i = 1; i + 1 < n; ++i) {
        const std::array<V3, 3> tri = {pts[0], pts[i], pts[i + 1]};
        const int edges = border && border->size() == n ? fanBorder(i, n, *border) : 0;
        if (relief && !plan) {
            if (auto got = reliefTriangle(tri, {uv[0], uv[i], uv[i + 1]}, normal, *relief, tex, budget, edges, &neighbour)) { out.insert(out.end(), got->begin(), got->end()); continue; }
        }
        if (plan) {
            for (const Piece& piece : plan->pieces(tri)) {
                for (size_t k = 1; k + 1 < piece.verts.size(); ++k) {
                    const PieceVert* v[3] = {&piece.verts[0], &piece.verts[k], &piece.verts[k + 1]};
                    const std::array<V3, 3> pos = {lerp3(tri, v[0]->w), lerp3(tri, v[1]->w), lerp3(tri, v[2]->w)};
                    const std::array<V2, 3> tuv = {v[0]->uv, v[1]->uv, v[2]->uv};
                    if (withRelief) {
                        const auto r = reliefOf(info, piece.texture);
                        if (r) if (auto got = reliefTriangle(pos, tuv, normal, *r, piece.texture, budget, pieceBorder(piece.verts, edges), &neighbour)) { out.insert(out.end(), got->begin(), got->end()); continue; }
                    }
                    flatTri(out, piece.texture, pos, tuv, normal);
                }
            }
        } else {
            flatTri(out, tex, tri, {uv[0], uv[i], uv[i + 1]}, normal);
        }
    }
    return out;
}

// ---- the environment --------------------------------------------------------------------------------------------------------------------------------
struct TexEnvironment::Impl {
    std::filesystem::path root;
    GameProfile profile;
    std::mutex m;
    std::unordered_map<std::string, std::unique_ptr<TexInfo>> infos;  // null = not available
    std::unordered_map<std::string, GroupPtr> groups;
    std::unordered_map<std::string, bool> groupTried;
};

TexEnvironment::TexEnvironment(std::filesystem::path assetsRoot, const GameProfile& profile) : d_(std::make_unique<Impl>()) {
    d_->root = std::move(assetsRoot);
    d_->profile = profile;
}
TexEnvironment::~TexEnvironment() = default;

const TexInfo* TexEnvironment::info(const std::string& path) {
    std::lock_guard<std::mutex> lock(d_->m);
    const auto it = d_->infos.find(path);
    if (it != d_->infos.end()) return it->second.get();
    std::unique_ptr<TexInfo> ti;
    if (const auto file = readTexture(d_->root, d_->profile, path)) {
        if (const auto img = decodeImage(file->bytes, file->ext)) {
            ti = std::make_unique<TexInfo>();
            ti->path = path;
            ti->width = img->width;
            ti->height = img->height;
            const auto props = readTextureProps(d_->root, d_->profile, path);
            ti->props = props ? parseProps(*props, path) : defaultProps();
            ti->ppm = ti->props.pixelsPerMeter ? *ti->props.pixelsPerMeter : d_->profile.pixelsPerMeter;
            ti->seamless = analyseSeams(img->rgba.data(), img->width, img->height);
        }
    }
    const TexInfo* out = ti.get();
    d_->infos[path] = std::move(ti);
    return out;
}

GroupPtr TexEnvironment::group(const std::string& name) {
    std::lock_guard<std::mutex> lock(d_->m);
    if (d_->groupTried[name]) { const auto it = d_->groups.find(name); return it == d_->groups.end() ? nullptr : it->second; }
    d_->groupTried[name] = true;
    if (!readTexGroup(d_->root, d_->profile, name)) return nullptr;  // validates the name and the file
    std::ifstream in(d_->root / d_->profile.texGroupRoot / (name + ".texgroup.json"), std::ios::binary);
    const nlohmann::ordered_json j = nlohmann::ordered_json::parse(in, nullptr, false);
    if (j.is_discarded()) return nullptr;
    const GroupParse p = parseGroup(j);
    if (p.group) d_->groups[name] = p.group;
    return p.group;
}

void TexEnvironment::setInfo(const TexInfo& i) {
    std::lock_guard<std::mutex> lock(d_->m);
    d_->infos[i.path] = std::make_unique<TexInfo>(i);
}
void TexEnvironment::setGroup(const std::string& name, GroupPtr g) {
    std::lock_guard<std::mutex> lock(d_->m);
    d_->groups[name] = std::move(g);
    d_->groupTried[name] = true;
}

// ---- coplanar neighbours of other faces --------------------------------------------------------------------------------------------------------------
namespace {
constexpr double kOutlineCell = 4.0;
long long outlineCell(long long x, long long y, long long z) { return (x * 73856093LL) ^ (y * 19349663LL) ^ (z * 83492791LL); }
}  // namespace

void OutlineIndex::buildGrid() {
    grid_.clear();
    big_.clear();
    for (size_t i = 0; i < polys_.size(); ++i) {
        V3 mn{1e300, 1e300, 1e300}, mx{-1e300, -1e300, -1e300};
        for (const V3& v : polys_[i]) for (int k = 0; k < 3; ++k) { mn[static_cast<size_t>(k)] = std::min(mn[static_cast<size_t>(k)], v[static_cast<size_t>(k)]); mx[static_cast<size_t>(k)] = std::max(mx[static_cast<size_t>(k)], v[static_cast<size_t>(k)]); }
        long long lo[3], hi[3];
        for (size_t k = 0; k < 3; ++k) { lo[k] = static_cast<long long>(std::floor((mn[k] - 1e-3) / kOutlineCell)); hi[k] = static_cast<long long>(std::floor((mx[k] + 1e-3) / kOutlineCell)); }
        if ((hi[0] - lo[0] + 1) * (hi[1] - lo[1] + 1) * (hi[2] - lo[2] + 1) > 4096) { big_.push_back(i); continue; }
        for (long long x = lo[0]; x <= hi[0]; ++x) for (long long y = lo[1]; y <= hi[1]; ++y) for (long long z = lo[2]; z <= hi[2]; ++z) grid_[outlineCell(x, y, z)].push_back(i);
    }
    gridBuilt_ = true;
}

// Is the edge a -> b of `polygon` carried on by other faces: polygons of the same texture in the same plane, facing the same way, that run along the edge in the
// opposite direction and together cover all of it? Then the surface goes on there and a relief needs no wall (the neighbour's relief meets it).
bool OutlineIndex::continued(size_t polygon, const V3& a, const V3& b) {
    if (!gridBuilt_) buildGrid();
    const V3 n = normals_[polygon];
    const V3 d = sub(b, a);
    const double len2 = dot(d, d);
    if (len2 < 1e-12 || dot(n, n) < 0.5) return false;
    std::vector<std::pair<double, double>> cover;
    auto consider = [&](size_t q) {
        if (q == polygon || keys_[q] == keys_[polygon] || textures_[q] != textures_[polygon]) return;
        if (dot(normals_[q], n) < 0.9998) return;                          // about 1 degree
        if (std::fabs(dot(sub(polys_[q][0], a), n)) > 1e-4) return;       // the same plane
        const std::vector<V3>& P = polys_[q];
        for (size_t k = 0; k < P.size(); ++k) {
            double t[2], off[2];
            for (size_t e = 0; e < 2; ++e) {
                const V3 c = sub(P[(k + e) % P.size()], a);
                t[e] = dot(c, d) / len2;
                const V3 r = sub(c, V3{d[0] * t[e], d[1] * t[e], d[2] * t[e]});
                off[e] = dot(r, r);
            }
            if (off[0] > 1e-8 || off[1] > 1e-8 || t[1] >= t[0]) continue;  // on the line, and running the other way
            const double lo = std::max(0.0, t[1]), hi = std::min(1.0, t[0]);
            if (hi > lo) cover.push_back({lo, hi});
        }
    };
    const V3 lo{std::min(a[0], b[0]) - 1e-3, std::min(a[1], b[1]) - 1e-3, std::min(a[2], b[2]) - 1e-3};
    const V3 hi{std::max(a[0], b[0]) + 1e-3, std::max(a[1], b[1]) + 1e-3, std::max(a[2], b[2]) + 1e-3};
    long long c0[3], c1[3];
    for (size_t k = 0; k < 3; ++k) { c0[k] = static_cast<long long>(std::floor(lo[k] / kOutlineCell)); c1[k] = static_cast<long long>(std::floor(hi[k] / kOutlineCell)); }
    if ((c1[0] - c0[0] + 1) * (c1[1] - c0[1] + 1) * (c1[2] - c0[2] + 1) > 4096) {
        for (size_t q = 0; q < polys_.size(); ++q) consider(q);
    } else {
        for (size_t q : big_) consider(q);
        for (long long x = c0[0]; x <= c1[0]; ++x) for (long long y = c0[1]; y <= c1[1]; ++y) for (long long z = c0[2]; z <= c1[2]; ++z) {
            const auto it = grid_.find(outlineCell(x, y, z));
            if (it != grid_.end()) for (size_t q : it->second) consider(q);
        }
    }
    std::sort(cover.begin(), cover.end());
    double reach = 0;
    for (const auto& [lo2, hi2] : cover) {
        if (lo2 > reach + 1e-6) return false;  // a gap: part of the edge is still open
        reach = std::max(reach, hi2);
    }
    return reach >= 1 - 1e-6;
}

}  // namespace core::texplan
