// Baked visibility of the rBSP export (VISIBILITY_REWRITE.md).
#include "core/bsp_vis.h"
#include "core/lighting_classes.h"
#include "core/parallel.h"
#include "core/raytrace.h"
#include "gpu_bake.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <numeric>
#include <random>
#include <tuple>
#include <unordered_map>

namespace core {

namespace {

using Clock = std::chrono::steady_clock;
double secondsSince(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

constexpr double kPi = 3.14159265358979323846;

// Deterministic random numbers: a hash of three integers (the shaders repeat hashBits exactly).
uint32_t hashBits(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^ (c + 0x165667B1u) * 0xC2B2AE3Du;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}
double hash01(uint32_t a, uint32_t b, uint32_t c) { return (hashBits(a, b, c) >> 8) * (1.0 / 16777216.0); }
uint32_t hash32(uint32_t a, uint32_t b, uint32_t c) { return hashBits(a, b, c); }

struct Rng {
    uint32_t a, b, n = 0;
    double next() { return hash01(a, b, n++); }
};

using Poly = std::vector<Vec3>;
struct HS { Vec3 n; double d = 0; };  // inside: n . p <= d

// The part of the polygon with n . p <= d.
Poly clipPoly(const Poly& p, const Vec3& n, double d) {
    Poly out;
    if (p.empty()) return out;
    out.reserve(p.size() + 2);
    for (size_t i = 0; i < p.size(); ++i) {
        const Vec3& a = p[i];
        const Vec3& b = p[(i + 1) % p.size()];
        const double da = n.dot(a) - d, db = n.dot(b) - d;
        if (da <= 0) out.push_back(a);
        if ((da < 0 && db > 0) || (da > 0 && db < 0)) out.push_back(a + (b - a) * (da / (da - db)));
    }
    return out;
}

double polyArea(const Poly& p) {
    Vec3 s;
    for (size_t i = 0; i < p.size(); ++i) s = s + p[i].cross(p[(i + 1) % p.size()]);
    return 0.5 * s.length();
}

struct Basis { Vec3 u, v; };
Basis basisOf(const Vec3& n) {
    const Vec3 helper = std::fabs(n.x) < 0.6 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    const Vec3 u = n.cross(helper).normalized();
    return {u, n.cross(u)};
}

// The polygon each side of a convex region occupies (empty: it does not touch the region). The region lies within `half` of `center` on every axis.
std::vector<Poly> windingsOf(const std::vector<HS>& s, const Vec3& center, double half) {
    std::vector<Poly> out(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const Vec3& n = s[i].n;
        const Basis b = basisOf(n);
        const Vec3 c0 = center + n * (s[i].d - n.dot(center));
        Poly w = {c0 - b.u * half - b.v * half, c0 + b.u * half - b.v * half, c0 + b.u * half + b.v * half, c0 - b.u * half + b.v * half};
        for (size_t j = 0; j < s.size() && w.size() >= 3; ++j) {
            if (j == i) continue;
            if (s[j].n.dot(n) > 1 - 1e-9 && std::fabs(s[j].d - s[i].d) < 1e-7) {  // the same plane twice: only the first one keeps its polygon
                if (j < i) w.clear();
                continue;
            }
            w = clipPoly(w, s[j].n, s[j].d);
        }
        if (w.size() >= 3 && polyArea(w) > 1e-9) out[i] = std::move(w);
    }
    return out;
}

struct Geom {
    std::vector<HS> sides;
    std::vector<Poly> windows;  // parallel to sides
    std::vector<Vec3> verts;    // distinct corners
    Vec3 mn, mx, centroid;
    double volume = 0;
};

// Trims the sides that do not bound the region; false when it has no volume.
bool makeGeom(const std::vector<HS>& sides, const Vec3& center, double half, Geom& g) {
    const std::vector<Poly> w = windingsOf(sides, center, half);
    g = Geom{};
    double volume = 0;
    Vec3 sum;
    for (size_t i = 0; i < sides.size(); ++i) {
        if (w[i].empty()) continue;
        g.sides.push_back(sides[i]);
        g.windows.push_back(w[i]);
        volume += polyArea(w[i]) * (sides[i].d - sides[i].n.dot(center)) / 3.0;
        for (const Vec3& v : w[i]) {
            bool dup = false;
            for (const Vec3& u : g.verts) if (std::fabs(u.x - v.x) < 1e-6 && std::fabs(u.y - v.y) < 1e-6 && std::fabs(u.z - v.z) < 1e-6) { dup = true; break; }
            if (!dup) { g.verts.push_back(v); sum = sum + v; }
        }
    }
    if (g.sides.size() < 4 || volume <= 1e-9 || g.verts.size() < 4) return false;
    g.volume = volume;
    g.centroid = sum * (1.0 / double(g.verts.size()));
    g.mn = g.mx = g.verts[0];
    for (const Vec3& v : g.verts) {
        g.mn = {std::min(g.mn.x, v.x), std::min(g.mn.y, v.y), std::min(g.mn.z, v.z)};
        g.mx = {std::max(g.mx.x, v.x), std::max(g.mx.y, v.y), std::max(g.mx.z, v.z)};
    }
    return true;
}

// Whether the box touches the zone grown by `tol` (zones overlap or lie less than a metre apart: the cells must not leave a gap between them).
bool zoneHitsBox(const VisZone& z, const Vec3& a, const Vec3& b, double tol = 0) {
    if (a.x > z.mx.x + tol || b.x < z.mn.x - tol || a.y > z.mx.y + tol || b.y < z.mn.y - tol || a.z > z.mx.z + tol || b.z < z.mn.z - tol) return false;
    for (const auto& [n, d] : z.planes) {
        const double m = (n.x >= 0 ? a.x : b.x) * n.x + (n.y >= 0 ? a.y : b.y) * n.y + (n.z >= 0 ? a.z : b.z) * n.z;  // the smallest n . p over the box
        if (m > d + tol + 1e-6) return false;
    }
    return true;
}
constexpr double kZoneBridge = 0.5;  // m: zones closer than twice this are one area

// ---- splitter walls ---------------------------------------------------------------------------------------------------------------------------

struct Splitter {
    Vec3 n;
    double d = 0;
    Vec3 mn{1e300, 1e300, 1e300}, mx{-1e300, -1e300, -1e300};
    std::vector<Poly> polys;
};

double hullWidth(std::vector<std::array<double, 2>> pts) {
    if (pts.size() < 3) return 0;
    std::sort(pts.begin(), pts.end());
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    if (pts.size() < 3) return 0;
    auto cross = [](const std::array<double, 2>& o, const std::array<double, 2>& a, const std::array<double, 2>& b) {
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
    };
    std::vector<std::array<double, 2>> h(2 * pts.size());
    size_t k = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
        while (k >= 2 && cross(h[k - 2], h[k - 1], pts[i]) <= 0) --k;
        h[k++] = pts[i];
    }
    for (size_t i = pts.size() - 1, t = k + 1; i > 0; --i) {
        while (k >= t && cross(h[k - 2], h[k - 1], pts[i - 1]) <= 0) --k;
        h[k++] = pts[i - 1];
    }
    h.resize(k - 1);
    if (h.size() < 3) return 0;
    double best = 1e300;
    for (size_t i = 0; i < h.size(); ++i) {
        const auto& a = h[i];
        const auto& b = h[(i + 1) % h.size()];
        const double len = std::hypot(b[0] - a[0], b[1] - a[1]);
        if (len < 1e-9) continue;
        double far = 0;
        for (const auto& p : h) far = std::max(far, std::fabs(cross(a, b, p)) / len);
        best = std::min(best, far);
    }
    return best == 1e300 ? 0 : best;
}

std::vector<Splitter> buildSplitters(const std::vector<LightSurface>& occ, const std::vector<VisZone>& zones, double minWidth) {
    std::map<std::array<long long, 4>, std::vector<size_t>> groups;
    for (size_t s = 0; s < occ.size(); ++s) {
        const LightSurface& su = occ[s];
        if (su.verts.size() < 3) continue;
        const Vec3 n = su.normal.normalized();
        const double d = n.dot(su.verts[0]);
        groups[{std::llround(n.x * 1e4), std::llround(n.y * 1e4), std::llround(n.z * 1e4), std::llround(d * 500)}].push_back(s);
    }
    std::vector<Splitter> out;
    for (const auto& [key, idx] : groups) {
        const Vec3 n = occ[idx[0]].normal.normalized();
        const double d = n.dot(occ[idx[0]].verts[0]);
        const Basis b = basisOf(n);
        struct Piece { size_t s; double u0, u1, v0, v1; };
        std::vector<Piece> pieces;
        for (size_t s : idx) {
            Piece p{s, 1e300, -1e300, 1e300, -1e300};
            for (const Vec3& v : occ[s].verts) {
                const double u = v.dot(b.u), w = v.dot(b.v);
                p.u0 = std::min(p.u0, u); p.u1 = std::max(p.u1, u);
                p.v0 = std::min(p.v0, w); p.v1 = std::max(p.v1, w);
            }
            pieces.push_back(p);
        }
        std::sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& c) { return a.u0 < c.u0 || (a.u0 == c.u0 && a.s < c.s); });
        std::vector<size_t> parent(pieces.size());
        std::iota(parent.begin(), parent.end(), size_t(0));
        std::function<size_t(size_t)> find = [&](size_t i) { while (parent[i] != i) { parent[i] = parent[parent[i]]; i = parent[i]; } return i; };
        constexpr double eps = 0.01;
        std::vector<size_t> active;
        for (size_t i = 0; i < pieces.size(); ++i) {
            active.erase(std::remove_if(active.begin(), active.end(), [&](size_t a) { return pieces[a].u1 < pieces[i].u0 - eps; }), active.end());
            for (size_t a : active)
                if (pieces[a].v1 >= pieces[i].v0 - eps && pieces[i].v1 >= pieces[a].v0 - eps) parent[find(a)] = find(i);
            active.push_back(i);
        }
        std::map<size_t, std::vector<size_t>> comps;
        for (size_t i = 0; i < pieces.size(); ++i) comps[find(i)].push_back(i);
        for (const auto& [root, members] : comps) {
            std::vector<std::array<double, 2>> pts;
            Splitter sp;
            sp.n = n;
            sp.d = d;
            bool relevant = false;
            for (size_t m : members) {
                const LightSurface& su = occ[pieces[m].s];
                Vec3 pmn{1e300, 1e300, 1e300}, pmx{-1e300, -1e300, -1e300};
                for (const Vec3& v : su.verts) {
                    pts.push_back({v.dot(b.u), v.dot(b.v)});
                    pmn = {std::min(pmn.x, v.x), std::min(pmn.y, v.y), std::min(pmn.z, v.z)};
                    pmx = {std::max(pmx.x, v.x), std::max(pmx.y, v.y), std::max(pmx.z, v.z)};
                }
                for (const VisZone& z : zones) if (zoneHitsBox(z, pmn, pmx)) { relevant = true; break; }
                sp.polys.push_back(su.verts);
                sp.mn = {std::min(sp.mn.x, pmn.x), std::min(sp.mn.y, pmn.y), std::min(sp.mn.z, pmn.z)};
                sp.mx = {std::max(sp.mx.x, pmx.x), std::max(sp.mx.y, pmx.y), std::max(sp.mx.z, pmx.z)};
            }
            if (relevant && hullWidth(std::move(pts)) >= minWidth) out.push_back(std::move(sp));
        }
    }
    return out;
}

constexpr double kCoarse = 8;  // m: the lookup grids

uint64_t packKey(int64_t x, int64_t y, int64_t z) {
    constexpr int64_t off = int64_t(1) << 20;
    return (uint64_t(x + off) << 42) | (uint64_t(y + off) << 21) | uint64_t(z + off);
}
int64_t coarseOf(double v) { return static_cast<int64_t>(std::floor(v / kCoarse)); }

struct SplitterIndex {
    std::vector<Splitter> list;
    std::unordered_map<uint64_t, std::vector<int>> grid;
    void build() {
        for (size_t i = 0; i < list.size(); ++i) {
            const Splitter& s = list[i];
            for (const Poly& p : s.polys) {
                Vec3 a{1e300, 1e300, 1e300}, b{-1e300, -1e300, -1e300};
                for (const Vec3& v : p) {
                    a = {std::min(a.x, v.x), std::min(a.y, v.y), std::min(a.z, v.z)};
                    b = {std::max(b.x, v.x), std::max(b.y, v.y), std::max(b.z, v.z)};
                }
                for (int64_t x = coarseOf(a.x); x <= coarseOf(b.x); ++x)
                    for (int64_t y = coarseOf(a.y); y <= coarseOf(b.y); ++y)
                        for (int64_t z = coarseOf(a.z); z <= coarseOf(b.z); ++z) {
                            auto& v = grid[packKey(x, y, z)];
                            if (v.empty() || v.back() != int(i)) v.push_back(int(i));
                        }
            }
        }
    }
};

// area of the splitter's polygons inside the box
double splitterAreaIn(const Splitter& s, const Vec3& a, const Vec3& b) {
    double area = 0;
    for (const Poly& p : s.polys) {
        Poly w = p;
        w = clipPoly(w, {1, 0, 0}, b.x);
        w = clipPoly(w, {-1, 0, 0}, -a.x);
        w = clipPoly(w, {0, 1, 0}, b.y);
        w = clipPoly(w, {0, -1, 0}, -a.y);
        w = clipPoly(w, {0, 0, 1}, b.z);
        w = clipPoly(w, {0, 0, -1}, -a.z);
        if (w.size() >= 3) area += polyArea(w);
    }
    return area;
}

// ---- cells ------------------------------------------------------------------------------------------------------------------------------------

using CellId = std::array<int, 3>;

// The cells (cubes of edge c on a grid anchored at the origin) that touch a zone; false when there would be more than `limit` candidates.
bool enumerateCells(const std::vector<VisZone>& zones, double c, size_t limit, std::vector<CellId>& out) {
    out.clear();
    double total = 0;
    const double t = kZoneBridge;
    for (const VisZone& z : zones) {
        const double nx = std::floor((z.mx.x + t) / c) - std::floor((z.mn.x - t) / c) + 1, ny = std::floor((z.mx.y + t) / c) - std::floor((z.mn.y - t) / c) + 1,
                     nz = std::floor((z.mx.z + t) / c) - std::floor((z.mn.z - t) / c) + 1;
        total += nx * ny * nz;
    }
    if (total > double(limit)) return false;
    for (const VisZone& z : zones)
        for (int k = int(std::floor((z.mn.z - t) / c)); k <= int(std::floor((z.mx.z + t) / c)); ++k)
            for (int j = int(std::floor((z.mn.y - t) / c)); j <= int(std::floor((z.mx.y + t) / c)); ++j)
                for (int i = int(std::floor((z.mn.x - t) / c)); i <= int(std::floor((z.mx.x + t) / c)); ++i) {
                    const Vec3 a{i * c, j * c, k * c};
                    if (zoneHitsBox(z, a, a + Vec3{c, c, c}, t)) out.push_back({i, j, k});
                }
    std::sort(out.begin(), out.end(), [](const CellId& p, const CellId& q) { return std::tie(p[2], p[1], p[0]) < std::tie(q[2], q[1], q[0]); });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return true;
}

struct FragLeaf {
    Geom g;
    bool solid = true;
    std::vector<Vec3> pts;
};
struct FragNode { Vec3 n; double d = 0; int32_t child[2] = {0, 0}; Vec3 mn, mx; };
struct Frag {
    std::vector<FragNode> nodes;
    std::vector<FragLeaf> leaves;
    int32_t root = -1;  // >= 0 a node, < 0 leaf -(i + 1)
    bool ok = false;
};

// Sample points of a region: its corners, points on its boundary and inside; none inside a solid. No point at all = the region is solid.
void makeSamples(const Geom& g, const SolidSet* solids, int pool, Rng rng, std::vector<Vec3>& out) {
    constexpr double pull = 0.01;
    auto valid = [&](const Vec3& p) { return !(solids && solids->inside(p)); };
    const size_t half = size_t(std::max(2, pool / 2));
    for (const Vec3& v : g.verts) {
        if (out.size() >= half) break;
        Vec3 toC = g.centroid - v;
        const double l = toC.length();
        const Vec3 p = l > 2 * pull ? v + toC * (pull / l) : g.centroid;
        if (valid(p)) out.push_back(p);
    }
    std::vector<double> cum;
    double total = 0;
    for (const Poly& w : g.windows) { total += polyArea(w); cum.push_back(total); }
    const int maxAttempts = std::max(8 * pool, 64);
    int attempts = 0;
    while (out.size() < half && attempts++ < maxAttempts && total > 0) {
        const double t = rng.next() * total;
        const size_t wi = size_t(std::lower_bound(cum.begin(), cum.end(), t) - cum.begin());
        if (wi >= g.windows.size()) continue;
        const Poly& w = g.windows[wi];
        // a random triangle of the fan, area weighted, and a random point of it
        std::vector<double> tc;
        double ta = 0;
        for (size_t i = 1; i + 1 < w.size(); ++i) { ta += 0.5 * (w[i] - w[0]).cross(w[i + 1] - w[0]).length(); tc.push_back(ta); }
        const double tt = rng.next() * ta;
        const size_t ti = std::min(size_t(std::lower_bound(tc.begin(), tc.end(), tt) - tc.begin()), tc.size() - 1);
        double r1 = rng.next(), r2 = rng.next();
        if (r1 + r2 > 1) { r1 = 1 - r1; r2 = 1 - r2; }
        const Vec3 p = w[0] + (w[ti + 1] - w[0]) * r1 + (w[ti + 2] - w[0]) * r2 - g.sides[wi].n * pull;
        if (valid(p)) out.push_back(p);
    }
    attempts = 0;
    while (out.size() < size_t(pool) && attempts++ < maxAttempts) {
        const Vec3 p{g.mn.x + rng.next() * (g.mx.x - g.mn.x), g.mn.y + rng.next() * (g.mx.y - g.mn.y), g.mn.z + rng.next() * (g.mx.z - g.mn.z)};
        bool in = true;
        for (const HS& s : g.sides) if (s.n.dot(p) > s.d - 0.005) { in = false; break; }
        if (in && valid(p)) out.push_back(p);
    }
}

struct CellBuilder {
    const SolidSet* solids;
    const SplitterIndex* splitters;
    const VisSettings* cfg;
    double c;

    Frag build(const CellId& id) const {
        Frag f;
        const Vec3 mn{id[0] * c, id[1] * c, id[2] * c}, mx = mn + Vec3{c, c, c};
        // the splitters through the cell, the biggest part first
        std::vector<int> ids;
        for (int64_t x = coarseOf(mn.x); x <= coarseOf(mx.x); ++x)
            for (int64_t y = coarseOf(mn.y); y <= coarseOf(mx.y); ++y)
                for (int64_t z = coarseOf(mn.z); z <= coarseOf(mx.z); ++z)
                    if (const auto it = splitters->grid.find(packKey(x, y, z)); it != splitters->grid.end()) ids.insert(ids.end(), it->second.begin(), it->second.end());
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        std::vector<std::pair<double, int>> cand;
        for (int s : ids) {
            const Splitter& sp = splitters->list[size_t(s)];
            if (sp.mn.x > mx.x || sp.mx.x < mn.x || sp.mn.y > mx.y || sp.mx.y < mn.y || sp.mn.z > mx.z || sp.mx.z < mn.z) continue;
            const double lo = (sp.n.x >= 0 ? mn.x : mx.x) * sp.n.x + (sp.n.y >= 0 ? mn.y : mx.y) * sp.n.y + (sp.n.z >= 0 ? mn.z : mx.z) * sp.n.z;
            const double hi = (sp.n.x >= 0 ? mx.x : mn.x) * sp.n.x + (sp.n.y >= 0 ? mx.y : mn.y) * sp.n.y + (sp.n.z >= 0 ? mx.z : mn.z) * sp.n.z;
            if (hi <= sp.d + 1e-4 || lo >= sp.d - 1e-4) continue;  // the plane does not cut the cell
            const double a = splitterAreaIn(sp, mn, mx);
            if (a > 1e-4) cand.push_back({a, s});
        }
        std::sort(cand.begin(), cand.end(), [](const auto& p, const auto& q) { return p.first > q.first || (p.first == q.first && p.second < q.second); });
        if (cand.size() > size_t(std::max(0, cfg->maxSplitsPerCell))) cand.resize(size_t(std::max(0, cfg->maxSplitsPerCell)));

        std::vector<HS> box = {{{1, 0, 0}, mx.x}, {{-1, 0, 0}, -mn.x}, {{0, 1, 0}, mx.y}, {{0, -1, 0}, -mn.y}, {{0, 0, 1}, mx.z}, {{0, 0, -1}, -mn.z}};
        Geom g;
        const Vec3 center = (mn + mx) * 0.5;
        if (!makeGeom(box, center, 2 * c, g)) return f;
        const double minVolume = 0.01 * c * c * c;
        const uint32_t cellHash = hash32(uint32_t(id[0]), uint32_t(id[1]), uint32_t(id[2]));

        std::function<int32_t(const Geom&, size_t)> rec = [&](const Geom& r, size_t next) -> int32_t {
            for (size_t p = next; p < cand.size(); ++p) {
                const Splitter& sp = splitters->list[size_t(cand[p].second)];
                double fmin = 1e300, fmax = -1e300;
                for (const Vec3& v : r.verts) { const double t = sp.n.dot(v) - sp.d; fmin = std::min(fmin, t); fmax = std::max(fmax, t); }
                if (fmax <= 1e-4 || fmin >= -1e-4) continue;
                std::vector<HS> fs = r.sides, bs = r.sides;
                fs.push_back({sp.n * -1.0, -sp.d});
                bs.push_back({sp.n, sp.d});
                Geom fg, bg;
                if (!makeGeom(fs, center, 2 * c, fg) || fg.volume < minVolume) continue;
                if (!makeGeom(bs, center, 2 * c, bg) || bg.volume < minVolume) continue;
                const int32_t me = int32_t(f.nodes.size());
                f.nodes.push_back({});
                const int32_t front = rec(fg, p + 1), back = rec(bg, p + 1);
                FragNode& nd = f.nodes[size_t(me)];
                nd.n = sp.n;
                nd.d = sp.d;
                nd.child[0] = front;
                nd.child[1] = back;
                nd.mn = mn;
                nd.mx = mx;
                return me;
            }
            FragLeaf leaf;
            leaf.g = r;
            makeSamples(r, solids, cfg->samplePool, Rng{cellHash, uint32_t(f.leaves.size())}, leaf.pts);
            leaf.solid = leaf.pts.empty();
            f.leaves.push_back(std::move(leaf));
            return -int32_t(f.leaves.size());
        };
        f.root = rec(g, 0);
        f.ok = true;
        return f;
    }
};

// ---- the tree ---------------------------------------------------------------------------------------------------------------------------------

struct Built {
    VisResult r;
    std::vector<Vec3> pts;                  // sample points, cluster after cluster
    std::vector<uint32_t> ptsFirst, ptsCount;  // per cluster
    std::vector<int> leafOfCluster;
};

// Fills `b` for cell size c; false when the clusters do not fit `maxClusters`.
bool buildTree(const VisInput& in, const SplitterIndex& splitters, double c, int maxClusters, Built& b, std::vector<CellId>& cells, VisStats& stats) {
    const VisSettings& cfg = in.settings;
    std::vector<Frag> frags(cells.size());
    CellBuilder cb{in.solids, &splitters, &cfg, c};
    parallelFor(cells.size(), 16, [&](size_t i0, size_t i1) {
        for (size_t i = i0; i < i1; ++i) frags[i] = cb.build(cells[i]);
    });
    size_t regions = 0;
    for (const Frag& f : frags) for (const FragLeaf& l : f.leaves) regions += l.solid ? 0 : 1;
    if (regions + 1 > size_t(maxClusters)) return false;

    VisResult& r = b.r;
    r = VisResult{};
    r.cellSize = c;
    VisLeaf outside;
    outside.cluster = 0;
    r.leafs.push_back(outside);  // leaf 0
    b.ptsFirst.assign(1, 0);
    b.ptsCount.assign(1, 0);
    b.leafOfCluster.assign(1, 0);
    stats.splitPlanes = 0;
    stats.solidRegions = 0;

    // bounds of the cells
    Vec3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
    for (const CellId& id : cells) {
        lo = {std::min(lo.x, id[0] * c), std::min(lo.y, id[1] * c), std::min(lo.z, id[2] * c)};
        hi = {std::max(hi.x, (id[0] + 1) * c), std::max(hi.y, (id[1] + 1) * c), std::max(hi.z, (id[2] + 1) * c)};
    }
    if (cells.empty()) { lo = {0, 0, 0}; hi = {c, c, c}; }

    // six nodes around the cells (outside the box: cluster 0), then the tree of the cells
    r.nodes.resize(6);
    const Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const double los[3] = {lo.x, lo.y, lo.z}, his[3] = {hi.x, hi.y, hi.z};
    for (int k = 0; k < 6; ++k) {
        const int a = k / 2;
        VisNode& nd = r.nodes[size_t(k)];
        nd.normal = axes[a];
        nd.dist = (k % 2 == 0) ? his[a] : los[a];
        nd.mn = lo - Vec3{1, 1, 1};
        nd.mx = hi + Vec3{1, 1, 1};
        if (k % 2 == 0) { nd.children[0] = -1; nd.children[1] = k + 1; }
        else { nd.children[0] = k + 1; nd.children[1] = -1; }
    }
    int nextCluster = 1;
    auto attach = [&](size_t ci) -> int32_t {
        const Frag& f = frags[ci];
        if (!f.ok) return -1;
        const int32_t nodeBase = int32_t(r.nodes.size());
        std::vector<int32_t> leafRef(f.leaves.size());
        for (size_t li = 0; li < f.leaves.size(); ++li) {
            const FragLeaf& fl = f.leaves[li];
            VisLeaf leaf;
            leaf.mn = fl.g.mn;
            leaf.mx = fl.g.mx;
            leaf.centroid = fl.g.centroid;
            if (fl.solid) {
                leaf.cluster = -1;
                ++stats.solidRegions;
            } else {
                leaf.cluster = nextCluster++;
                for (const HS& s : fl.g.sides) leaf.sides.push_back({s.n, s.d});
                b.ptsFirst.push_back(uint32_t(b.pts.size()));
                b.ptsCount.push_back(uint32_t(fl.pts.size()));
                b.pts.insert(b.pts.end(), fl.pts.begin(), fl.pts.end());
                b.leafOfCluster.push_back(int(r.leafs.size()));
            }
            leafRef[li] = -int32_t(r.leafs.size()) - 1;
            r.leafs.push_back(std::move(leaf));
        }
        auto remap = [&](int32_t ref) { return ref >= 0 ? nodeBase + ref : leafRef[size_t(-ref - 1)]; };
        for (const FragNode& fn : f.nodes) {
            VisNode nd;
            nd.normal = fn.n;
            nd.dist = fn.d;
            nd.children[0] = remap(fn.child[0]);
            nd.children[1] = remap(fn.child[1]);
            nd.mn = fn.mn;
            nd.mx = fn.mx;
            r.nodes.push_back(nd);
            ++stats.splitPlanes;
        }
        return remap(f.root);
    };
    std::function<int32_t(std::vector<size_t>&, CellId, CellId)> kd = [&](std::vector<size_t>& ids, CellId a, CellId z) -> int32_t {
        if (ids.empty()) return -1;
        if (a == z) return attach(ids[0]);
        int axis = 0;
        for (int k = 1; k < 3; ++k) if (z[k] - a[k] > z[axis] - a[axis]) axis = k;
        const int mid = a[axis] + (z[axis] - a[axis] + 1) / 2;
        std::vector<size_t> front, back;
        for (size_t i : ids) (cells[i][axis] >= mid ? front : back).push_back(i);
        const int32_t me = int32_t(r.nodes.size());
        r.nodes.push_back({});
        CellId fa = a, bz = z;
        fa[axis] = mid;
        bz[axis] = mid - 1;
        const int32_t fc = kd(front, fa, z), bc = kd(back, a, bz);
        VisNode& nd = r.nodes[size_t(me)];
        nd.normal = axes[axis];
        nd.dist = mid * c;
        nd.children[0] = fc;
        nd.children[1] = bc;
        nd.mn = {a[0] * c, a[1] * c, a[2] * c};
        nd.mx = {(z[0] + 1) * c, (z[1] + 1) * c, (z[2] + 1) * c};
        return me;
    };
    int32_t inner = -1;
    if (!cells.empty()) {
        CellId a = cells[0], z = cells[0];
        for (const CellId& id : cells) for (int k = 0; k < 3; ++k) { a[k] = std::min(a[k], id[k]); z[k] = std::max(z[k], id[k]); }
        std::vector<size_t> ids(cells.size());
        std::iota(ids.begin(), ids.end(), size_t(0));
        inner = kd(ids, a, z);
    }
    r.nodes[5].children[0] = inner;
    r.clusters = nextCluster;
    r.leafs[0].mn = lo;
    r.leafs[0].mx = hi;
    stats.regions = size_t(nextCluster - 1);
    stats.cells = cells.size();
    return true;
}

}  // namespace

// ---- VisResult --------------------------------------------------------------------------------------------------------------------------------

int VisResult::leafAt(const Vec3& p) const {
    int32_t i = 0;
    for (size_t guard = 0; i >= 0 && guard < nodes.size() + 8; ++guard) {
        const VisNode& n = nodes[size_t(i)];
        i = n.children[n.normal.dot(p) - n.dist >= 0 ? 0 : 1];
    }
    return i < 0 ? int(-i - 1) : 0;
}

void VisResult::buildIndex() {
    std::map<uint64_t, std::vector<int>> grid;
    for (size_t li = 0; li < leafs.size(); ++li) {
        const VisLeaf& l = leafs[li];
        if (l.cluster < 1) continue;
        for (int64_t x = coarseOf(l.mn.x - 0.1); x <= coarseOf(l.mx.x + 0.1); ++x)
            for (int64_t y = coarseOf(l.mn.y - 0.1); y <= coarseOf(l.mx.y + 0.1); ++y)
                for (int64_t z = coarseOf(l.mn.z - 0.1); z <= coarseOf(l.mx.z + 0.1); ++z) grid[packKey(x, y, z)].push_back(int(li));
    }
    index.assign(grid.begin(), grid.end());
}

const std::vector<int>* VisResult::indexAt(int64_t x, int64_t y, int64_t z) const {
    const uint64_t key = packKey(x, y, z);
    const auto it = std::lower_bound(index.begin(), index.end(), key, [](const auto& e, uint64_t k) { return e.first < k; });
    return it != index.end() && it->first == key ? &it->second : nullptr;
}

void VisResult::leavesForBox(const Vec3& mn, const Vec3& mx, std::vector<int>& out) const {
    out.clear();
    constexpr double g = 0.03;
    for (int64_t x = coarseOf(mn.x - g); x <= coarseOf(mx.x + g); ++x)
        for (int64_t y = coarseOf(mn.y - g); y <= coarseOf(mx.y + g); ++y)
            for (int64_t z = coarseOf(mn.z - g); z <= coarseOf(mx.z + g); ++z)
                if (const auto* v = indexAt(x, y, z)) out.insert(out.end(), v->begin(), v->end());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    out.erase(std::remove_if(out.begin(), out.end(), [&](int li) {
        const VisLeaf& l = leafs[size_t(li)];
        return l.mn.x > mx.x + g || l.mx.x < mn.x - g || l.mn.y > mx.y + g || l.mx.y < mn.y - g || l.mn.z > mx.z + g || l.mx.z < mn.z - g;
    }), out.end());
}

void VisResult::leavesFor(const std::vector<Vec3>& polygon, std::vector<int>& out, bool& outside) const {
    outside = false;
    out.clear();
    if (polygon.size() < 3) return;
    Vec3 mn = polygon[0], mx = polygon[0];
    for (const Vec3& v : polygon) {
        mn = {std::min(mn.x, v.x), std::min(mn.y, v.y), std::min(mn.z, v.z)};
        mx = {std::max(mx.x, v.x), std::max(mx.y, v.y), std::max(mx.z, v.z)};
    }
    // which leaves it touches
    std::vector<int> cand;
    leavesForBox(mn, mx, cand);
    constexpr double g = 0.03;
    for (int li : cand) {
        const VisLeaf& l = leafs[size_t(li)];
        Poly w = polygon;
        for (const VisHalfSpace& s : l.sides) {
            w = clipPoly(w, s.n, s.d + g);
            if (w.empty()) break;
        }
        if (!w.empty()) out.push_back(li);
    }
    // does part of it lie outside the cells: points over the polygon (a lattice per fan triangle) in a leaf of cluster 0
    const double spacing = std::max(0.5, cellSize * 0.5);
    for (size_t t = 1; t + 1 < polygon.size() && !outside; ++t) {
        const Vec3 &a = polygon[0], &b = polygon[t], &c = polygon[t + 1];
        const double longest = std::max({(b - a).length(), (c - b).length(), (a - c).length()});
        const int k = std::clamp(int(std::ceil(longest / spacing)), 1, 48);
        for (int i = 0; i <= k && !outside; ++i)
            for (int j = 0; i + j <= k; ++j) {
                const double u = double(i) / k, v = double(j) / k;
                const Vec3 p = a + (b - a) * u + (c - a) * v;
                if (leafs[size_t(leafAt(p))].cluster == 0) { outside = true; break; }
            }
    }
}

// ---- the passes -------------------------------------------------------------------------------------------------------------------------------

namespace {

struct Matrix {
    size_t n = 0, words = 0;
    std::vector<uint64_t> bits;
    void init(size_t clusters) { n = clusters; words = (clusters + 63) / 64; bits.assign(n * words, 0); }
    void set(size_t r, size_t c) { bits[r * words + c / 64] |= uint64_t(1) << (c % 64); }
    bool get(size_t r, size_t c) const { return (bits[r * words + c / 64] >> (c % 64)) & 1; }
    void setAtomic(size_t r, size_t c) { std::atomic_ref<uint64_t>(bits[r * words + c / 64]).fetch_or(uint64_t(1) << (c % 64), std::memory_order_relaxed); }
};

// V |= V^T
void symmetrize(Matrix& m) {
    for (size_t r = 0; r < m.n; ++r)
        for (size_t w = 0; w < m.words; ++w) {
            uint64_t x = m.bits[r * m.words + w];
            while (x) {
                const size_t c = w * 64 + size_t(std::countr_zero(x));
                x &= x - 1;
                if (c < m.n && c != r) m.set(c, r);
            }
        }
}

bool segmentClear(const TriangleBvh& bvh, Vec3 p, Vec3 q) {
    const Vec3 d = q - p;
    const double len = d.length();
    if (len < 1e-6) return true;
    const double e = std::min(0.002 / len, 0.25);
    return !bvh.occluded(p + d * e, d * (1 - 2 * e), 1.0);
}

}  // namespace

VisResult buildVisibility(const VisInput& in, const VisProgress& progress) {
    const auto t0 = Clock::now();
    auto stage = [&](const char* s, double f) { if (progress) progress(s, f); };
    const VisSettings& cfg = in.settings;
    VisStats stats;

    // the biggest cluster count whose visibility table fits
    int maxClusters = 2;
    while (8 + size_t(maxClusters + 1) * size_t((maxClusters + 8) / 8) <= cfg.maxBytes && maxClusters < 65000) ++maxClusters;

    std::vector<LightSurface> noOcc;
    const std::vector<LightSurface>& occ = in.occluders ? *in.occluders : noOcc;
    SplitterIndex splitters;
    splitters.list = buildSplitters(occ, in.zones, cfg.splitMinWidth);
    splitters.build();

    Built b;
    std::vector<CellId> cells;
    double c = std::max(cfg.cell, 0.25);
    stage("cells", 0.0);
    for (int attempt = 0; attempt < 80; ++attempt) {
        if (!enumerateCells(in.zones, c, size_t(maxClusters) * 6, cells)) { c *= 1.15; continue; }
        if (buildTree(in, splitters, c, maxClusters, b, cells, stats)) break;
        c *= 1.15;
    }
    stats.cell = c;
    stats.secondsTree = secondsSince(t0);
    VisResult& r = b.r;
    if (r.nodes.empty()) {  // could not fit even at the biggest size: a single outside cluster
        r.leafs.assign(1, VisLeaf{});
        r.leafs[0].cluster = 0;
        r.nodes.resize(1);
        r.nodes[0].children[0] = r.nodes[0].children[1] = -1;
        r.clusters = 1;
    }
    r.buildIndex();
    const size_t N = size_t(r.clusters);
    Matrix m;
    m.init(N);

    if (!in.trace || N <= 1) {
        std::fill(m.bits.begin(), m.bits.end(), ~uint64_t(0));
    } else {
        // the occluders
        stage("visibility", 0.1);
        LightingClasses occClasses;
        occClasses.castShadows = {"worldspawn"};
        std::vector<LightSurface> named = occ;
        for (LightSurface& s : named) s.classname = "worldspawn";
        const OccluderMesh mesh = buildOccluders(named, occClasses);
        const TriangleBvh bvh(mesh);
        stats.pairs = N * (N - 1) / 2;
        // the occluders on the GPU when there is enough work for it (and a device)
        std::unique_ptr<gpu::VisGpu> gpuVis;
        std::string gpuNote;
        {
            const char* envDevice = std::getenv("MAPEDITOR_VIS_DEVICE");
            VisDevice device = cfg.device;
            if (envDevice) {
                const std::string e = envDevice;
                device = e == "cpu" ? VisDevice::Cpu : e == "compute" ? VisDevice::Compute : e == "rayquery" ? VisDevice::RayQuery : VisDevice::Auto;
            }
            if (device != VisDevice::Cpu && (device != VisDevice::Auto || stats.pairs >= cfg.gpuMinPairs)) {
                std::vector<float> nodes, bvhTris;
                if (device != VisDevice::RayQuery) bvh.exportGpu(nodes, bvhTris);
                gpuVis = gpu::createVisGpu(mesh.tris, nodes, bvhTris, device != VisDevice::Compute, gpuNote);
            }
        }
        // closest hits along rays (8 floats each: origin, tmax, direction, 0) on the GPU or the CPU
        auto traceClosest = [&](const std::vector<float>& rays, size_t count, std::vector<float>& t) {
            if (gpuVis && gpuVis->closest(rays, count, t)) return;
            t.assign(count, -1.f);
            parallelFor(count, 2048, [&](size_t i0, size_t i1) {
                for (size_t i = i0; i < i1; ++i) {
                    const float* q = &rays[i * 8];
                    t[i] = float(bvh.closestHit(Vec3{q[0], q[1], q[2]}, Vec3{q[4], q[5], q[6]}, q[3]).t);
                }
            });
        };

        // clusters that share an open boundary see each other (and later pass their view on: the dilation)
        const auto tAdj = Clock::now();
        std::vector<std::vector<int>> adjacent(N);
        parallelFor(N - 1, 8, [&](size_t i0, size_t i1) {
            std::vector<int> cand;
            for (size_t ci = i0 + 1; ci < i1 + 1; ++ci) {
                const VisLeaf& A = r.leafs[size_t(b.leafOfCluster[ci])];
                r.leavesForBox(A.mn, A.mx, cand);
                const std::vector<Poly> win = windingsOf([&] { std::vector<HS> s; for (const auto& h : A.sides) s.push_back({h.n, h.d}); return s; }(), A.centroid, 2 * r.cellSize);
                for (int li : cand) {
                    const VisLeaf& B = r.leafs[size_t(li)];
                    if (B.cluster == A.cluster) continue;
                    bool open = false;
                    for (size_t si = 0; si < win.size() && !open; ++si) {
                        if (win[si].empty()) continue;
                        Poly w = win[si];
                        for (const VisHalfSpace& s : B.sides) { w = clipPoly(w, s.n, s.d + 0.02); if (w.empty()) break; }
                        if (w.size() < 3 || polyArea(w) < 1e-4) continue;
                        Vec3 cen;
                        for (const Vec3& v : w) cen = cen + v;
                        cen = cen * (1.0 / double(w.size()));
                        std::vector<Vec3> tests = {cen};
                        for (const Vec3& v : w) tests.push_back((cen + v) * 0.5);
                        const Vec3 nrm = A.sides[si].n;
                        for (const Vec3& t : tests) {
                            const Vec3 p1 = t - nrm * 0.01, p2 = t + nrm * 0.01;
                            if (in.solids && (in.solids->inside(p1) || in.solids->inside(p2))) continue;
                            if (!segmentClear(bvh, p1, p2)) continue;
                            open = true;
                            break;
                        }
                    }
                    if (open) adjacent[ci].push_back(B.cluster);
                }
            }
        });
        for (size_t ci = 1; ci < N; ++ci) {
            std::sort(adjacent[ci].begin(), adjacent[ci].end());
            adjacent[ci].erase(std::unique(adjacent[ci].begin(), adjacent[ci].end()), adjacent[ci].end());
            for (int o : adjacent[ci]) { m.set(ci, size_t(o)); m.set(size_t(o), ci); ++stats.adjacent; }
        }
        stats.secondsDilate += secondsSince(tAdj);

        // all pairs: segments between sample points
        stage("visibility", 0.3);
        const auto tPairs = Clock::now();
        const int M = std::max(1, cfg.pairSegments);
        bool onGpu = false;
        if (gpuVis) {
            std::vector<float> fpts(b.pts.size() * 4, 0.f);
            for (size_t i = 0; i < b.pts.size(); ++i) { fpts[i * 4] = float(b.pts[i].x); fpts[i * 4 + 1] = float(b.pts[i].y); fpts[i * 4 + 2] = float(b.pts[i].z); }
            onGpu = gpuVis->pairs(fpts, b.ptsFirst, b.ptsCount, uint32_t(M), m.words, m.bits.data());
            if (onGpu) stats.device = gpuVis->device();
            else gpuNote = gpuVis->error();
        }
        if (!onGpu) {
            if (!gpuNote.empty()) r.warnings.push_back("visibility: the GPU path failed (" + gpuNote + "), the CPU traced the segments");
            parallelFor(N - 1, 1, [&](size_t i0, size_t i1) {
                for (size_t ai = i0 + 1; ai < i1 + 1; ++ai) {
                    const uint32_t na = b.ptsCount[ai];
                    if (!na) continue;
                    const Vec3* pa = b.pts.data() + b.ptsFirst[ai];
                    for (size_t bi = ai + 1; bi < N; ++bi) {
                        const uint32_t nb = b.ptsCount[bi];
                        if (!nb || m.get(ai, bi)) continue;
                        const Vec3* pb = b.pts.data() + b.ptsFirst[bi];
                        for (int k = 0; k < M; ++k) {
                            const Vec3& p = pa[hash32(uint32_t(ai), uint32_t(bi), uint32_t(k)) % na];
                            const Vec3& q = pb[hash32(uint32_t(bi), uint32_t(ai), uint32_t(k)) % nb];
                            if (segmentClear(bvh, p, q)) { m.set(ai, bi); break; }
                        }
                    }
                }
            });
            stats.device = "cpu";
        }
        stats.secondsPairs = secondsSince(tPairs);

        // view of the neighbours: symmetric, one ring of open neighbours, symmetric again
        stage("visibility", 0.9);
        const auto tDil = Clock::now();
        symmetrize(m);
        Matrix d = m;
        parallelFor(N, 16, [&](size_t i0, size_t i1) {
            for (size_t ci = i0; ci < i1; ++ci)
                for (int o : adjacent[ci])
                    for (size_t w = 0; w < m.words; ++w) d.bits[ci * m.words + w] |= m.bits[size_t(o) * m.words + w];
        });
        m = std::move(d);
        symmetrize(m);
        stats.secondsDilate += secondsSince(tDil);
        for (size_t ci = 0; ci < N; ++ci) m.set(ci, ci);

        // a check of the result: random segments between points of the zones that nothing blocks must join clusters that see each other
        size_t checks = cfg.selfCheck;
        if (const char* e = std::getenv("MAPEDITOR_VIS_CHECK")) checks = size_t(std::strtoull(e, nullptr, 10));
        if (checks > 0) {
            std::mt19937_64 rng(12345);
            std::vector<double> volume;
            double total = 0;
            for (const VisZone& z : in.zones) {
                total += std::max(1e-6, (z.mx.x - z.mn.x) * (z.mx.y - z.mn.y) * (z.mx.z - z.mn.z));
                volume.push_back(total);
            }
            auto uni = [&]() { return std::uniform_real_distribution<double>(0.0, 1.0)(rng); };
            auto pointInZones = [&](Vec3& p) {
                for (int tries = 0; tries < 64; ++tries) {
                    const double t = uni() * total;
                    const size_t zi = size_t(std::lower_bound(volume.begin(), volume.end(), t) - volume.begin());
                    if (zi >= in.zones.size()) continue;
                    const VisZone& z = in.zones[zi];
                    p = {z.mn.x + uni() * (z.mx.x - z.mn.x), z.mn.y + uni() * (z.mx.y - z.mn.y), z.mn.z + uni() * (z.mx.z - z.mn.z)};
                    bool ok = true;
                    for (const auto& [n, d] : z.planes) if (n.dot(p) > d) { ok = false; break; }
                    if (ok && !(in.solids && in.solids->inside(p))) return true;
                }
                return false;
            };
            auto inZones = [&](const Vec3& p) {
                for (const VisZone& z : in.zones) {
                    if (p.x < z.mn.x || p.x > z.mx.x || p.y < z.mn.y || p.y > z.mx.y || p.z < z.mn.z || p.z > z.mx.z) continue;
                    bool ok = true;
                    for (const auto& [n, d] : z.planes) if (n.dot(p) > d) { ok = false; break; }
                    if (ok) return !(in.solids && in.solids->inside(p));
                }
                return false;
            };
            std::vector<std::pair<Vec3, Vec3>> segs;
            segs.reserve(checks);
            for (size_t i = 0; i < checks; ++i) {
                Vec3 p, q;
                if (!pointInZones(p)) continue;
                if (i % 2 == 0) {  // anywhere in the zones
                    if (!pointInZones(q)) continue;
                } else {           // a segment of 3 to 43 m in a random direction (the pairs that are more often clear)
                    const double z = 2 * uni() - 1, phi = 2 * kPi * uni(), rr = std::sqrt(std::max(0.0, 1 - z * z)), len = 3 + 40 * uni();
                    q = p + Vec3{rr * std::cos(phi), z, rr * std::sin(phi)} * len;
                    if (!inZones(q)) continue;
                }
                const int a = r.leafs[size_t(r.leafAt(p))].cluster, o = r.leafs[size_t(r.leafAt(q))].cluster;
                if (a < 1 || o < 1 || a == o || m.get(size_t(a), size_t(o))) continue;  // only the pairs that are reported hidden can be wrong
                segs.push_back({p, q});
            }
            std::vector<char> clear(segs.size(), 0);
            parallelFor(segs.size(), 256, [&](size_t i0, size_t i1) {
                for (size_t i = i0; i < i1; ++i) clear[i] = segmentClear(bvh, segs[i].first, segs[i].second) ? 1 : 0;
            });
            stats.checked = checks;
            for (size_t i = 0; i < segs.size(); ++i)
                if (clear[i]) {
                    ++stats.misses;
                    if (stats.misses <= 5) {
                        const Vec3 &p = segs[i].first, &q = segs[i].second;
                        r.warnings.push_back("visibility check: (" + std::to_string(p.x) + " " + std::to_string(p.y) + " " + std::to_string(p.z) + ") sees (" + std::to_string(q.x) + " " +
                                             std::to_string(q.y) + " " + std::to_string(q.z) + ") but their clusters do not");
                    }
                }
        }
    }
    // the outside sees and is seen by every cluster
    for (size_t ci = 0; ci < N; ++ci) { m.set(0, ci); m.set(ci, 0); }

    r.words = m.words;
    r.bits = std::move(m.bits);
    stats.bytes = 8 + N * ((N + 7) / 8);
    stats.seconds = secondsSince(t0);
    if (stats.device.empty()) stats.device = "none";
    r.stats = stats;
    return r;
}

}  // namespace core
