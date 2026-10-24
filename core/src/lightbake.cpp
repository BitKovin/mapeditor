#include "core/lightbake.h"
#include "core/denoise.h"
#include "core/relief_geometry.h"
#include "core/smoothing.h"
#include "core/parallel.h"
#include "gpu_bake.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_map>

namespace core {

namespace {

// ---- rasterisation --------------------------------------------------------------------------------------------------------
// World distance a conservatively rasterised sample is pulled in from the fragment edge.
constexpr double kEdgeInset = 0.005;

// What the inside test of a fragment needs per fan triangle, computed once per fragment (the arithmetic is the same as evaluating it per
// texel and per sub position, so the masks are bit for bit the ones of the straightforward version, see tests/texel_reference.h).
struct FanEdge {
    double e0u = 0, e0v = 0, ex = 0, ey = 0, len = 0;
};
struct FanTri {
    bool valid = false;  // false: degenerate (area below 1e-12), contributes nothing
    double sign = 1;
    FanEdge e[3];
    bool polyEdge[3] = {false, true, false};  // polygon edges get the margin, the internal diagonal does not
};
struct PieceCtx {
    const LightSurface* surface = nullptr;
    const std::vector<Vec2>* t = nullptr;  // texel coordinates of the fragment's vertices
    std::vector<FanTri> fan;               // fan triangle kk (0, kk, kk + 1) at index kk - 1
};

PieceCtx makePiece(const LightSurface& s, const std::vector<Vec2>& t) {
    PieceCtx c;
    c.surface = &s;
    c.t = &t;
    const size_t n = s.verts.size();
    for (size_t kk = 1; kk + 1 < n; ++kk) {
        FanTri f;
        const Vec2 p[3] = {t[0], t[kk], t[kk + 1]};
        const double area = (p[1].u - p[0].u) * (p[2].v - p[0].v) - (p[1].v - p[0].v) * (p[2].u - p[0].u);
        if (std::fabs(area) >= 1e-12) {
            f.valid = true;
            f.sign = area > 0 ? 1.0 : -1.0;
            f.polyEdge[0] = kk == 1;
            f.polyEdge[1] = true;
            f.polyEdge[2] = kk + 2 == n;
            for (int e = 0; e < 3; ++e) {
                const Vec2 e0 = p[e], e1 = p[(e + 1) % 3];
                f.e[e] = {e0.u, e0.v, e1.u - e0.u, e1.v - e0.v, 0};
                f.e[e].len = std::sqrt(f.e[e].ex * f.e[e].ex + f.e[e].ey * f.e[e].ey);
            }
        }
        c.fan.push_back(f);
    }
    return c;
}

// 12 floats, layout in TexelSamples::sub. All zero (mask 0) when there is no triangle to extrapolate from.
void subInfo(const PieceCtx* c, size_t k, int x, int y, float* out) {
    float v[12] = {};
    if (c && c->surface && c->t) {
        const auto& verts = c->surface->verts;
        const auto& t = *c->t;
        const Vec2 a = t[0], b = t[k], cc = t[k + 1];
        const double den = (b.v - cc.v) * (a.u - cc.u) + (cc.u - b.u) * (a.v - cc.v);
        if (std::fabs(den) > 1e-12) {
            // The texel -> world map is affine on the chart's plane, so the claiming triangle also extrapolates to texel centres outside it.
            auto world = [&](double px, double py) {
                const double w0 = ((b.v - cc.v) * (px - cc.u) + (cc.u - b.u) * (py - cc.v)) / den;
                const double w1 = ((cc.v - a.v) * (px - cc.u) + (a.u - cc.u) * (py - cc.v)) / den;
                return verts[0] * w0 + verts[k] * w1 + verts[k + 1] * (1.0 - w0 - w1);
            };
            const double cx = x + 0.5, cy = y + 0.5;
            const Vec3 C = world(cx, cy), U = world(cx + 1, cy) - C, V = world(cx, cy + 1) - C;
            const double texelSize = 0.5 * (U.length() + V.length());
            const double margin = texelSize > 1e-9 ? kEdgeInset / texelSize : 0.0;  // texels: keeps sub positions off the polygon's border
            // is the texel-space point (qx, qy) inside the polygon (union of its fan triangles), clear of its border?
            auto insideAt = [&](double qx, double qy) {
                for (const FanTri& f : c->fan) {
                    if (!f.valid) continue;
                    bool in = true;
                    for (int e = 0; e < 3 && in; ++e) {
                        const FanEdge& ed = f.e[e];
                        if (ed.len < 1e-12) { in = false; break; }
                        const double d = f.sign * (ed.ex * (qy - ed.e0v) - ed.ey * (qx - ed.e0u)) / ed.len;  // distance inside, in texels
                        in = d >= (f.polyEdge[e] ? margin : -1e-9);
                    }
                    if (in) return true;
                }
                return false;
            };
            uint32_t mask = 0;
            for (int j = 0; j < kMaxSuperSamples; ++j)
                if (insideAt(cx + kSuperOffsets[j][0], cy + kSuperOffsets[j][1])) mask |= 1u << j;
            // the same test for the cells of the 2x2 and 4x4 grids the global illumination is gathered on (giGridOffset)
            uint32_t grid2 = 0, grid4 = 0;
            for (int j = 0; j < 4; ++j)
                if (insideAt(cx + giGridOffset(2, j % 2), cy + giGridOffset(2, j / 2))) grid2 |= 1u << j;
            for (int j = 0; j < 16; ++j)
                if (insideAt(cx + giGridOffset(4, j % 4), cy + giGridOffset(4, j / 4))) grid4 |= 1u << j;
            v[0] = static_cast<float>(C.x); v[1] = static_cast<float>(C.y); v[2] = static_cast<float>(C.z);
            std::memcpy(&v[3], &mask, sizeof(mask));
            v[4] = static_cast<float>(U.x); v[5] = static_cast<float>(U.y); v[6] = static_cast<float>(U.z);
            std::memcpy(&v[7], &grid2, sizeof(grid2));
            v[8] = static_cast<float>(V.x); v[9] = static_cast<float>(V.y); v[10] = static_cast<float>(V.z);
            std::memcpy(&v[11], &grid4, sizeof(grid4));
        }
    }
    std::memcpy(out, v, sizeof v);
}

}  // namespace

uint32_t TexelSamples::subMask(size_t i) const {
    uint32_t m = 0;
    std::memcpy(&m, &sub[i * 12 + 3], sizeof(m));
    return m;
}

uint32_t TexelSamples::gridMask(size_t i, int scale) const {
    uint32_t m = 0;
    std::memcpy(&m, &sub[i * 12 + (scale == 2 ? 7 : 11)], sizeof(m));
    return m;
}

Vec3 TexelSamples::gridPosition(size_t i, int scale, int cell) const {
    const float* v = &sub[i * 12];
    const double ox = giGridOffset(scale, cell % scale), oy = giGridOffset(scale, cell / scale);
    return {v[0] + v[4] * ox + v[8] * oy, v[1] + v[5] * ox + v[9] * oy, v[2] + v[6] * ox + v[10] * oy};
}

Vec3 TexelSamples::subPosition(size_t i, int k) const {
    const float* v = &sub[i * 12];
    const double ox = kSuperOffsets[k][0], oy = kSuperOffsets[k][1];
    return {v[0] + v[4] * ox + v[8] * oy, v[1] + v[5] * ox + v[9] * oy, v[2] + v[6] * ox + v[10] * oy};
}

void TexelSamples::appendFrom(const TexelSamples& src, size_t i) {
    pos.insert(pos.end(), src.pos.begin() + static_cast<std::ptrdiff_t>(i * 4), src.pos.begin() + static_cast<std::ptrdiff_t>(i * 4 + 4));
    nrm.insert(nrm.end(), src.nrm.begin() + static_cast<std::ptrdiff_t>(i * 4), src.nrm.begin() + static_cast<std::ptrdiff_t>(i * 4 + 4));
    texel.push_back(src.texel[i]);
    if (src.hasSub()) sub.insert(sub.end(), src.sub.begin() + static_cast<std::ptrdiff_t>(i * 12), src.sub.begin() + static_cast<std::ptrdiff_t>(i * 12 + 12));
}

// ---- texel samples, cached per surface piece ---------------------------------------------------------------------------------
namespace {
constexpr double kClaimTolerance = -1e-9;

// Everything one fragment contributes, independent of what other fragments do: the texels it would claim in the centre pass (pass 0) and in the
// conservative pass (pass 1), in the order the straightforward rasteriser visits them, with their sub-texel data. Which candidate really becomes a
// sample is decided by the assembly (first claim wins, buried texels dropped).
struct TexelCandidate {
    int32_t x = 0, y = 0;
    Vec3 p;
    float sub[12] = {};
};
}  // namespace

struct TexelSampleCache::Entry {
    std::vector<double> sig;  // everything the candidates depend on; compared exactly
    Vec3 normal;
    std::vector<TexelCandidate> pass0, pass1;
    bool hasTiny = false;     // pass 0 fallback: a piece smaller than a texel claims the texel under its centroid
    int32_t tinyX = 0, tinyY = 0;
    Vec3 centroid;
    uint64_t used = 0;        // build generation that last used the entry
};

namespace {
std::vector<double> signatureOf(const LightSurface& s, const LightmapFace& f, int page, int atlas) {
    std::vector<double> sig;
    sig.reserve(s.verts.size() * 3 + 16);
    for (const auto& v : s.verts) { sig.push_back(v.x); sig.push_back(v.y); sig.push_back(v.z); }
    for (double d : {s.normal.x, s.normal.y, s.normal.z, f.su.x, f.su.y, f.su.z, f.sv.x, f.sv.y, f.sv.z, f.so, f.to, static_cast<double>(page), static_cast<double>(atlas)}) sig.push_back(d);
    return sig;
}
uint64_t hashOf(const std::vector<double>& sig) {
    uint64_t h = 1469598103934665603ull;
    for (double d : sig) {
        uint64_t b;
        std::memcpy(&b, &d, sizeof b);
        h ^= b + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        h *= 0xFF51AFD7ED558CCDull;
    }
    return h;
}
}  // namespace

void TexelSampleCache::generate(const LightSurface& s, const LightmapFace& f, int atlas, Entry& out) {
    out.normal = s.normal;
    std::vector<Vec2> t;
    Vec3 centroid;
    for (const auto& v : s.verts) { t.push_back(f.texel(v)); centroid = centroid + v; }
    centroid = centroid * (1.0 / static_cast<double>(s.verts.size()));
    const PieceCtx piece = makePiece(s, t);
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<TexelCandidate>& list = pass == 0 ? out.pass0 : out.pass1;
        for (size_t k = 1; k + 1 < s.verts.size(); ++k) {
            const Vec2 a = t[0], b = t[k], c = t[k + 1];
            const double den = (b.v - c.v) * (a.u - c.u) + (c.u - b.u) * (a.v - c.v);
            if (std::fabs(den) < 1e-12) continue;
            const int x0 = static_cast<int>(std::floor(std::min({a.u, b.u, c.u}) - 0.5)) - pass;
            const int x1 = static_cast<int>(std::ceil(std::max({a.u, b.u, c.u}) - 0.5)) + pass;
            const int y0 = static_cast<int>(std::floor(std::min({a.v, b.v, c.v}) - 0.5)) - pass;
            const int y1 = static_cast<int>(std::ceil(std::max({a.v, b.v, c.v}) - 0.5)) + pass;
            auto bary = [&](double px, double py, double& w0, double& w1, double& w2) {
                w0 = ((b.v - c.v) * (px - c.u) + (c.u - b.u) * (py - c.v)) / den;
                w1 = ((c.v - a.v) * (px - c.u) + (a.u - c.u) * (py - c.v)) / den;
                w2 = 1.0 - w0 - w1;
            };
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const double cx = x + 0.5, cy = y + 0.5;
                    double w0, w1, w2;
                    bary(cx, cy, w0, w1, w2);
                    Vec3 p;
                    if (w0 >= kClaimTolerance && w1 >= kClaimTolerance && w2 >= kClaimTolerance) {
                        if (pass != 0) continue;  // handled by pass 0
                        p = s.verts[0] * w0 + s.verts[k] * w1 + s.verts[k + 1] * w2;
                    } else {
                        if (pass != 1) continue;
                        // nearest point of the triangle to the texel centre; the texel is covered when it lies within the texel square
                        const Vec2 pts[3] = {a, b, c};
                        double best = 1e300, qx = cx, qy = cy;
                        for (int e = 0; e < 3; ++e) {
                            const Vec2 e0 = pts[e], e1 = pts[(e + 1) % 3];
                            const double dx = e1.u - e0.u, dy = e1.v - e0.v, len2 = dx * dx + dy * dy;
                            const double u = len2 > 1e-18 ? std::clamp(((cx - e0.u) * dx + (cy - e0.v) * dy) / len2, 0.0, 1.0) : 0.0;
                            const double ex = e0.u + dx * u, ey = e0.v + dy * u;
                            const double d2 = (ex - cx) * (ex - cx) + (ey - cy) * (ey - cy);
                            if (d2 < best) { best = d2; qx = ex; qy = ey; }
                        }
                        if (std::fabs(qx - cx) > 0.5 - 1e-6 || std::fabs(qy - cy) > 0.5 - 1e-6) continue;  // touching only is not enough
                        bary(qx, qy, w0, w1, w2);
                        w0 = std::max(0.0, w0); w1 = std::max(0.0, w1); w2 = std::max(0.0, w2);
                        const double sum = w0 + w1 + w2;
                        if (sum < 1e-12) continue;
                        p = (s.verts[0] * w0 + s.verts[k] * w1 + s.verts[k + 1] * w2) * (1.0 / sum);
                        // pull the point off the edge so the shadow ray origin does not sit on a crease / coplanar neighbour
                        const Vec3 toC = centroid - p;
                        const double dl = toC.length();
                        if (dl > 1e-9) p = p + toC * (std::min(dl, kEdgeInset) / dl);
                    }
                    if (x < 0 || y < 0 || x >= atlas || y >= atlas) continue;  // such a claim never succeeds
                    TexelCandidate cand;
                    cand.x = x;
                    cand.y = y;
                    cand.p = p;
                    subInfo(&piece, k, x, y, cand.sub);
                    list.push_back(cand);
                }
        }
    }
    double u = 0, v = 0;
    for (size_t i = 0; i < s.verts.size(); ++i) { u += t[i].u; v += t[i].v; }
    const double inv = 1.0 / static_cast<double>(s.verts.size());
    out.hasTiny = true;
    out.tinyX = static_cast<int32_t>(std::floor(u * inv));
    out.tinyY = static_cast<int32_t>(std::floor(v * inv));
    out.centroid = centroid;
}

TexelSampleCache::TexelSampleCache() = default;
TexelSampleCache::~TexelSampleCache() = default;
void TexelSampleCache::clear() { entries_.clear(); stats_ = {}; }

TexelSamples TexelSampleCache::build(const LightmapLayout& layout, const std::vector<LightSurface>& surfaces, const SolidSet* solids, size_t* hiddenOut) {
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    ++generation_;
    TexelSamples out;
    out.atlas = layout.settings.atlasSize;
    out.pageCount = layout.pageCount;
    const int atlas = out.atlas;

    // 1. which surfaces take part, and their cache entries (missing ones are created empty and filled in parallel below)
    struct Use { Entry* entry = nullptr; int page = 0; const ReliefMesh* relief = nullptr; const FaceShading* shading = nullptr; };
    std::vector<Use> uses(surfaces.size());
    struct Miss { Entry* entry; size_t surface; const LightmapFace* face; };
    std::vector<Miss> misses;
    stats_.surfaces = 0;
    stats_.reused = 0;
    for (size_t i = 0; i < surfaces.size(); ++i) {
        const LightSurface& s = surfaces[i];
        if (s.verts.size() < 3) continue;
        const LightmapFace* f = layout.findForFragment(s.instance, s.entityId, s.brushInstance, s.brushId, s.faceId, s.verts);
        if (!f || f->chart < 0) continue;
        const int page = layout.charts[static_cast<size_t>(f->chart)].page;
        std::vector<double> sig = signatureOf(s, *f, page, atlas);
        const uint64_t h = hashOf(sig);
        auto it = entries_.find(h);
        ++stats_.surfaces;
        if (it != entries_.end() && it->second->sig == sig) {
            ++stats_.reused;
            it->second->used = generation_;
        } else {
            std::unique_ptr<Entry>& slot = entries_[h];  // a hash collision replaces the other entry (it is regenerated when it is used again)
            slot = std::make_unique<Entry>();
            slot->sig = std::move(sig);
            slot->used = generation_;
            misses.push_back({slot.get(), i, f});
            it = entries_.find(h);
        }
        uses[i] = {it->second.get(), page, s.relief.get(), s.shading};
    }
    const size_t pageCount = static_cast<size_t>(std::max(0, layout.pageCount));
    {
        // identical surfaces share one entry: generate each entry once
        std::vector<Miss> unique;
        std::unordered_map<Entry*, bool> seen;
        for (const Miss& m : misses) if (seen.emplace(m.entry, true).second) unique.push_back(m);
        parallelFor(unique.size(), 4, [&](size_t b, size_t e) {
            for (size_t i = b; i < e; ++i) generate(surfaces[unique[i].surface], *unique[i].face, atlas, *unique[i].entry);
        });
    }
    const auto t1 = Clock::now();

    // 2. assembly: first claim wins, texels buried in a solid are dropped. The samples come out in the order a serial walk would claim them (pass 0 over
    // every piece in order, then pass 1), whatever the way they are worked out here, in three steps:
    //  a) which candidates win: a texel slot is on one page, and a claim only depends on the earlier claims of its page, so the pages run on all cores
    //  b) where every winner lands in the output: a prefix sum over the pieces, in the serial order
    //  c) the winners' data (relief / smooth shading applied) written to those places, the pieces on all cores
    // (the serial walk spent 1+ s on a level of 12 million texels, most of it in the solid test of every candidate)
    const size_t pageN = pageCount;
    std::vector<std::vector<int8_t>> owner(pageN, std::vector<int8_t>(static_cast<size_t>(atlas) * atlas, 0));  // 0 free, 1 a sample, 2 buried in a solid
    std::vector<std::vector<size_t>> usesOfPage(pageN);
    std::vector<size_t> firstOf[2];  // per piece: index of its first candidate in won[pass]
    for (int pass = 0; pass < 2; ++pass) {
        firstOf[pass].assign(uses.size() + 1, 0);
        for (size_t i = 0; i < uses.size(); ++i)
            firstOf[pass][i + 1] = firstOf[pass][i] + (uses[i].entry ? (pass == 0 ? uses[i].entry->pass0 : uses[i].entry->pass1).size() : 0);
    }
    std::vector<char> won[2] = {std::vector<char>(firstOf[0].back(), 0), std::vector<char>(firstOf[1].back(), 0)};
    std::vector<uint32_t> wonCount[2] = {std::vector<uint32_t>(uses.size(), 0), std::vector<uint32_t>(uses.size(), 0)};
    std::vector<char> tinyWon(uses.size(), 0);
    for (size_t i = 0; i < uses.size(); ++i)
        if (uses[i].entry && uses[i].page >= 0 && static_cast<size_t>(uses[i].page) < pageN) usesOfPage[static_cast<size_t>(uses[i].page)].push_back(i);
    std::vector<size_t> hiddenOfPage(pageN, 0);
    parallelFor(pageN, 1, [&](size_t pageBegin, size_t pageEnd) {
        SolidSet::Probe probe;
        for (size_t pg = pageBegin; pg < pageEnd; ++pg) {
            std::vector<int8_t>& own = owner[pg];
            size_t hidden = 0;
            // Claims one slot; true when the candidate became a sample, `taken` when the slot already was one
            auto claim = [&](int x, int y, const Vec3& p, const Vec3& n, bool& taken) {
                taken = false;
                if (x < 0 || y < 0 || x >= atlas || y >= atlas) return false;
                int8_t& o = own[static_cast<size_t>(y) * atlas + x];
                if (o == 1) { taken = true; return false; }
                if (o != 0) return false;  // known to be hidden
                if (solids && solids->inside(p + n * 0.02, probe)) { o = 2; ++hidden; return false; }  // buried in a solid: invisible, counted once
                o = 1;
                return true;
            };
            // Pass 0 bakes every texel whose centre lies inside a fragment. Pass 1 (conservative rasterisation) bakes the texels that only partly overlap a
            // fragment, at the nearest point inside it (see generate()). Centre-inside texels always win over conservative ones.
            for (int pass = 0; pass < 2; ++pass)
                for (size_t ui : usesOfPage[pg]) {
                    const Entry& e = *uses[ui].entry;
                    const std::vector<TexelCandidate>& list = pass == 0 ? e.pass0 : e.pass1;
                    bool any = false;
                    uint32_t count = 0;
                    for (size_t k = 0; k < list.size(); ++k) {
                        bool taken = false;
                        if (claim(list[k].x, list[k].y, list[k].p, e.normal, taken)) { won[pass][firstOf[pass][ui] + k] = 1; ++count; any = true; }
                        else if (taken) any = true;
                    }
                    wonCount[pass][ui] = count;
                    if (pass == 0 && !any && e.hasTiny) {
                        bool taken = false;
                        if (claim(e.tinyX, e.tinyY, e.centroid, e.normal, taken)) tinyWon[ui] = 1;
                    }
                }
            hiddenOfPage[pg] = hidden;
        }
    });
    size_t hiddenCount = 0;
    for (size_t h : hiddenOfPage) hiddenCount += h;

    std::vector<size_t> firstSample[2];  // per piece: index in the output of its first winner of that pass
    firstSample[0].assign(uses.size(), 0);
    firstSample[1].assign(uses.size(), 0);
    size_t total = 0;
    for (size_t i = 0; i < uses.size(); ++i) { firstSample[0][i] = total; total += wonCount[0][i] + (tinyWon[i] ? 1 : 0); }
    for (size_t i = 0; i < uses.size(); ++i) { firstSample[1][i] = total; total += wonCount[1][i]; }
    out.texel.resize(total);
    out.pos.resize(total * 4);
    out.nrm.resize(total * 4);
    out.sub.resize(total * 12);
    parallelFor(uses.size(), 64, [&](size_t useBegin, size_t useEnd) {
        for (size_t ui = useBegin; ui < useEnd; ++ui) {
            const Use& u = uses[ui];
            if (!u.entry) continue;
            const Entry& e = *u.entry;
            // A relief moves the sample onto the displayed surface (after the solid test: a pit lies inside the brush, the texel does not)
            auto write = [&](size_t at, int x, int y, const Vec3& p, const float* sub) {
                Vec3 pp = p, nn = e.normal;
                float reliefSub[12];
                const float* useSub = sub;
                if (u.relief) {
                    Vec3 dp, dn;
                    if (displaceSample(*u.relief, p, e.normal, sub, dp, dn, reliefSub)) { pp = dp; nn = dn; if (sub) useSub = reliefSub; }
                } else if (u.shading) {
                    nn = shadingNormalAt(*u.shading, p, e.normal);  // smooth edges (REQUIREMENTS §33 H6): the normal varies across the face
                }
                out.texel[at] = static_cast<uint32_t>((static_cast<size_t>(u.page) * atlas + y) * atlas + x);
                float* P = &out.pos[at * 4];
                float* N = &out.nrm[at * 4];
                P[0] = static_cast<float>(pp.x); P[1] = static_cast<float>(pp.y); P[2] = static_cast<float>(pp.z); P[3] = 1.f;
                N[0] = static_cast<float>(nn.x); N[1] = static_cast<float>(nn.y); N[2] = static_cast<float>(nn.z); N[3] = 0.f;
                float* S = &out.sub[at * 12];
                if (useSub) std::copy(useSub, useSub + 12, S);
                else std::fill(S, S + 12, 0.f);
            };
            for (int pass = 0; pass < 2; ++pass) {
                const std::vector<TexelCandidate>& list = pass == 0 ? e.pass0 : e.pass1;
                size_t at = firstSample[pass][ui];
                for (size_t k = 0; k < list.size(); ++k)
                    if (won[pass][firstOf[pass][ui] + k]) write(at++, list[k].x, list[k].y, list[k].p, list[k].sub);
                if (pass == 0 && tinyWon[ui]) write(at, e.tinyX, e.tinyY, e.centroid, nullptr);
            }
        }
    });
    if (hiddenOut) *hiddenOut = hiddenCount;
    for (auto it = entries_.begin(); it != entries_.end();) it = it->second->used == generation_ ? std::next(it) : entries_.erase(it);
    const auto t2 = Clock::now();
    stats_.generateSeconds = std::chrono::duration<double>(t1 - t0).count();
    stats_.assembleSeconds = std::chrono::duration<double>(t2 - t1).count();
    return out;
}

TexelSamples buildTexelSamples(const LightmapLayout& layout, const std::vector<LightSurface>& surfaces, const SolidSet* solids, size_t* hiddenOut) {
    TexelSampleCache cache;
    return cache.build(layout, surfaces, solids, hiddenOut);
}

// ---- CPU reference evaluator ----------------------------------------------------------------------------------------------
namespace {

constexpr double kMinDistance = 0.05;

double window(double d, double radius) {
    const double q = d / radius;
    const double w = std::clamp(1.0 - q * q * q * q, 0.0, 1.0);
    return w * w;
}

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

// Fraction of the receiver's cosine-weighted hemisphere covered by the rectangle (Arvo's polygon irradiance, after clipping
// the polygon to the receiver's horizon). Irradiance with pi absorbed from a rect of radiance B is B * this.
double rectFormFactor(const Light& L, const Vec3& p, const Vec3& n) {
    const Vec3 hr = L.right * (L.width * 0.5), hu = L.up * (L.height * 0.5);
    const Vec3 corner[4] = {L.position - hr - hu, L.position + hr - hu, L.position + hr + hu, L.position - hr + hu};
    Vec3 poly[8];
    int count = 0;
    for (int i = 0; i < 4; ++i) {  // clip against n . v >= 0 (v relative to the receiver)
        const Vec3 a = corner[i] - p, b = corner[(i + 1) % 4] - p;
        const double da = n.dot(a), db = n.dot(b);
        if (da >= 0) poly[count++] = a;
        if ((da >= 0) != (db >= 0)) poly[count++] = a + (b - a) * (da / (da - db));
    }
    if (count < 3) return 0;
    double sum = 0;
    for (int i = 0; i < count; ++i) {
        const Vec3 a = poly[i], b = poly[(i + 1) % count];
        const double la = a.length(), lb = b.length();
        if (la < 1e-12 || lb < 1e-12) continue;
        const Vec3 c = a.cross(b);
        const double lc = c.length();
        if (lc < 1e-12) continue;
        const double theta = std::acos(std::clamp(a.dot(b) / (la * lb), -1.0, 1.0));
        sum += theta * c.dot(n) / lc;
    }
    return std::fabs(sum) / (2.0 * kPi);
}

// Irradiance (pi absorbed) at point p with unit normal n from one light, shadows excluded.
Vec3 evalLight(const Light& L, const Vec3& p, const Vec3& n) {
    const Vec3 tint = L.color * L.brightness;
    switch (L.type) {
        case LightType::Point:
        case LightType::Spot: {
            const Vec3 d = L.position - p;
            const double dist = d.length();
            if (dist >= L.radius) return {};
            const Vec3 l = dist > 1e-9 ? d * (1.0 / dist) : n;
            const double cosT = std::max(0.0, n.dot(l));
            const double dd = std::max(dist, kMinDistance);
            double k = window(dist, L.radius) / (dd * dd) * cosT;
            if (L.type == LightType::Spot) {
                const double cosInner = std::cos(L.innerAngle * kDeg), cosOuter = std::cos(L.outerAngle * kDeg);
                const double t = std::clamp((L.forward.dot(l * -1.0) - cosOuter) / (cosInner - cosOuter), 0.0, 1.0);
                k *= t * t;
            }
            return tint * k;
        }
        case LightType::Directional:
            return tint * std::max(0.0, n.dot(L.forward * -1.0));
        case LightType::Rect: {
            const Vec3 d = L.position - p;
            const double dist = d.length();
            if (dist >= L.radius || (p - L.position).dot(L.forward) <= 0) return {};  // beyond the radius, or behind the emitter
            return tint * (rectFormFactor(L, p, n) * window(dist, L.radius));
        }
        case LightType::Sky:  // the level's skybox when it has one (sky.h), else a uniform upper hemisphere
            return L.sky ? tint.mul(L.sky->irradianceAt(n)) : tint * ((1.0 + n.y) * 0.5);
    }
    return {};
}

// Bounce source near-field clamp (see kBounceNearDistance in lightbake.h): factor <= 1 applied to the light's contribution to Db only.
double bounceNearFactor(const Light& L, const Vec3& p) {
    if (L.type != LightType::Point && L.type != LightType::Spot) return 1.0;
    const double dist = (L.position - p).length();
    const double dd = std::max(dist, kMinDistance), db = std::max(dist, kBounceNearDistance);
    return (dd * dd) / (db * db);
}

// ---- deterministic sampling (same integer code as the shader) ----------------------------------------------------------------
uint32_t pcg(uint32_t v) {
    const uint32_t state = v * 747796405u + 2891336453u;
    const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
float fract(float x) { return x - std::floor(x); }
// two numbers in [0,1) for (texel, light, sample): per-texel random offset + R2 sequence
void sample2(uint32_t texel, uint32_t light, uint32_t s, float& u, float& v) {
    const uint32_t h = pcg(texel + pcg(light * 7919u + 12345u));
    const float u0 = static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
    const float v0 = static_cast<float>(pcg(h) >> 8) * (1.0f / 16777216.0f);
    u = fract(u0 + static_cast<float>(s) * 0.7548776662f);
    v = fract(v0 + static_cast<float>(s) * 0.5698402909f);
}
// Gather directions: the R2 sequence of sample2 with a per-texel hashed start. (An R2 lattice over the atlas position as the start gave
// neighbouring texels well spread directions, but their errors were strongly correlated along atlas rows: lag-1 correlation 0.8 along x, which shows
// as horizontal streaks that no denoiser can remove. Hashed starts are uncorrelated, ~0.)
// MUST match sampleGi in lightmap_gather.comp.
void sampleGi(uint32_t atlasTexel, int atlas, uint32_t s, float& u, float& v) {
    (void)atlas;
    const uint32_t h = pcg(atlasTexel * 2654435761u + 977u);
    const float u0 = static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
    const float v0 = static_cast<float>(pcg(h) >> 8) * (1.0f / 16777216.0f);
    u = fract(u0 + static_cast<float>(s) * 0.7548776662f);
    v = fract(v0 + static_cast<float>(s) * 0.5698402909f);
}
void frameOf(const Vec3& n, Vec3& t, Vec3& b) {
    t = std::fabs(n.y) < 0.99 ? n.cross(Vec3{0, 1, 0}).normalized() : Vec3{1, 0, 0};
    b = n.cross(t);
}

struct ShadowCtx {
    const TriangleBvh* bvh = nullptr;
    int areaSamples = 32, skySamples = 64;
    uint32_t sampleOffset = 0;
};

// Light that reaches origin `o` (= surface point lifted along the normal), per colour channel: 1 = unshadowed, 0 = fully shadowed, in between for
// soft shadows and for light through translucent surfaces (REQUIREMENTS §23). MUST equal visibility() in lightmap_direct.comp.
Vec3 visibility(const Light& L, uint32_t lightIdx, uint32_t texel, const Vec3& o, const ShadowCtx& sc) {
    auto through = [&](const Vec3& dir, double tmax) { return sc.bvh->transmittance(o, dir, tmax); };
    auto blocked = [&](const Vec3& target) {
        const Vec3 d = target - o;
        const double dist = d.length();
        if (dist <= kShadowBias) return Vec3{1, 1, 1};
        return through(d * (1.0 / dist), dist - kShadowBias);
    };
    const int n = std::max(1, sc.areaSamples);
    Vec3 open{0, 0, 0};
    int total = 1;
    switch (L.type) {
        case LightType::Point:
        case LightType::Spot: {
            if (L.sourceSize <= 0) { open = blocked(L.position); break; }
            total = n;
            const Vec3 l = (L.position - o).normalized();
            Vec3 t, b;
            frameOf(l, t, b);
            for (int i = 0; i < n; ++i) {
                float u, v;
                sample2(texel, lightIdx, static_cast<uint32_t>(i) + sc.sampleOffset, u, v);
                const double phi = 2.0 * kPi * v, r = L.sourceSize * std::sqrt(static_cast<double>(u));
                open = open + blocked(L.position + t * (std::cos(phi) * r) + b * (std::sin(phi) * r));
            }
            break;
        }
        case LightType::Directional: {
            const Vec3 toLight = L.forward * -1.0;
            if (L.sourceSize <= 0) { open = through(toLight, kShadowFar); break; }
            total = n;
            Vec3 t, b;
            frameOf(toLight, t, b);
            const double tanHalf = std::tan(L.sourceSize * 0.5 * kDeg);
            for (int i = 0; i < n; ++i) {
                float u, v;
                sample2(texel, lightIdx, static_cast<uint32_t>(i) + sc.sampleOffset, u, v);
                const double phi = 2.0 * kPi * v, r = tanHalf * std::sqrt(static_cast<double>(u));
                const Vec3 dir = (toLight + t * (std::cos(phi) * r) + b * (std::sin(phi) * r)).normalized();
                open = open + through(dir, kShadowFar);
            }
            break;
        }
        case LightType::Rect: {
            total = n;
            for (int i = 0; i < n; ++i) {
                float u, v;
                sample2(texel, lightIdx, static_cast<uint32_t>(i) + sc.sampleOffset, u, v);
                open = open + blocked(L.position + L.right * ((u - 0.5) * L.width) + L.up * ((v - 0.5) * L.height));
            }
            break;
        }
        case LightType::Sky:
            return {1, 1, 1};  // handled separately
    }
    return open * (1.0 / total);
}

// Sky with occlusion: cosine-weighted rays around the normal; only those pointing up count as sky, each with the skybox's light from its direction
// when the light has one (sky.h), else 1.
Vec3 skyVisibility(const Light& L, uint32_t lightIdx, uint32_t texel, const Vec3& o, const Vec3& n, const ShadowCtx& sc) {
    Vec3 t, b;
    frameOf(n, t, b);
    const int total = std::max(1, sc.skySamples);
    Vec3 open{0, 0, 0};
    for (int i = 0; i < total; ++i) {
        float u, v;
        sample2(texel, lightIdx, static_cast<uint32_t>(i) + sc.sampleOffset, u, v);
        const double r = std::sqrt(static_cast<double>(u)), phi = 2.0 * kPi * v;
        const Vec3 dir = t * (r * std::cos(phi)) + b * (r * std::sin(phi)) + n * std::sqrt(std::max(0.0, 1.0 - u));
        if (dir.y > 0) {
            const Vec3 tr = sc.bvh->transmittance(o, dir, kShadowFar);
            open = open + (L.sky ? L.sky->at(dir).mul(tr) : tr);
        }
    }
    return open * (1.0 / total);
}

}  // namespace

namespace {
// `bounceOut` (optional): 4 floats per sample, the direct light weighted by each light's `bounce` (ambient excluded).
// Super sampling (BakeOptions::superSamples): when shadows are traced and the sample has sub positions, every light is evaluated at each of
// them (each with its own random numbers: seed * 8 + k) and the results are averaged; the ambient term is added once.
std::vector<float> directCpu(const TexelSamples& s, const LightScene& scene, const TriangleBvh* bvh, const BakeOptions& opt, std::vector<float>* bounceOut) {
    std::vector<float> out(s.count() * 4, 0.f);
    if (bounceOut) bounceOut->assign(s.count() * 4, 0.f);
    ShadowCtx sc{bvh && opt.shadows && bvh->nodeCount() ? bvh : nullptr, opt.areaSamples, opt.skySamples, opt.sampleOffset};
    const int ss = std::clamp(opt.superSamples, 1, kMaxSuperSamples);
    const bool superOn = sc.bvh && ss > 1 && s.hasSub();
    const uint32_t ssMask = (1u << ss) - 1u;
    for (size_t i = 0; i < s.count(); ++i) {
        sc.sampleOffset = opt.sampleOffset + s.offsetOf(i);
        const Vec3 p0{s.pos[i * 4], s.pos[i * 4 + 1], s.pos[i * 4 + 2]};
        const Vec3 n{s.nrm[i * 4], s.nrm[i * 4 + 1], s.nrm[i * 4 + 2]};
        // adds every light's contribution at `p` (random id `seedId`) to sum / sumB
        auto shade = [&](const Vec3& p, uint32_t seedId, Vec3& sum, Vec3& sumB) {
            const Vec3 o = p + n * kShadowBias;
            for (size_t k = 0; k < scene.lights.size(); ++k) {
                const Light& L = scene.lights[k];
                const bool traced = sc.bvh && L.castShadows;
                Vec3 v;
                if (L.type == LightType::Sky && traced) {
                    v = (L.color * L.brightness).mul(skyVisibility(L, static_cast<uint32_t>(k), seedId, o, n, sc));
                } else {
                    v = evalLight(L, p, n);
                    if (traced && (v.x != 0 || v.y != 0 || v.z != 0)) v = v.mul(visibility(L, static_cast<uint32_t>(k), seedId, o, sc));
                }
                sum = sum + v;
                sumB = sumB + v * (L.bounce * bounceNearFactor(L, p));
            }
        };
        const uint32_t mask = superOn ? (s.subMask(i) & ssMask) : 0u;
        Vec3 sum, sumB;
        if (mask) {
            int cnt = 0;
            for (int k = 0; k < kMaxSuperSamples; ++k) {
                if (!(mask & (1u << k))) continue;
                shade(s.subPosition(i, k), s.seedOf(i) * 8u + static_cast<uint32_t>(k), sum, sumB);
                ++cnt;
            }
            sum = scene.ambient + sum * (1.0 / cnt);
            sumB = sumB * (1.0 / cnt);
        } else {
            sum = scene.ambient;
            shade(p0, s.seedOf(i), sum, sumB);
        }
        out[i * 4] = static_cast<float>(sum.x);
        out[i * 4 + 1] = static_cast<float>(sum.y);
        out[i * 4 + 2] = static_cast<float>(sum.z);
        if (bounceOut) {
            (*bounceOut)[i * 4] = static_cast<float>(sumB.x);
            (*bounceOut)[i * 4 + 1] = static_cast<float>(sumB.y);
            (*bounceOut)[i * 4 + 2] = static_cast<float>(sumB.z);
        }
    }
    return out;
}
}  // namespace

std::vector<float> evaluateDirectCpu(const TexelSamples& s, const LightScene& scene, const TriangleBvh* bvh, const BakeOptions& opt) {
    return directCpu(s, scene, bvh, opt, nullptr);
}

std::vector<float> evaluateDirectWithBounceCpu(const TexelSamples& s, const LightScene& scene, const TriangleBvh* bvh, const BakeOptions& opt,
                                               std::vector<float>* db) {
    return directCpu(s, scene, bvh, opt, db);
}

// ---- bounces ----------------------------------------------------------------------------------------------------------------
namespace {
// Bilinear lookup in the atlas buffer (rgb + validity per texel, `pages` pages of atlas x atlas). Invalid texels carry weight 0.
bool atlasSample(const std::vector<float>& atlasBuf, int atlas, int page, double u, double v, Vec3& out) {
    const double x = u - 0.5, y = v - 0.5;
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    const double fx = x - x0, fy = y - y0;
    double wsum = 0;
    Vec3 acc;
    for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx) {
            const int tx = std::clamp(x0 + dx, 0, atlas - 1), ty = std::clamp(y0 + dy, 0, atlas - 1);
            const size_t idx = ((static_cast<size_t>(page) * atlas + ty) * atlas + tx) * 4;
            const double w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy) * atlasBuf[idx + 3];
            if (w <= 0) continue;
            acc = acc + Vec3{atlasBuf[idx], atlasBuf[idx + 1], atlasBuf[idx + 2]} * w;
            wsum += w;
        }
    if (wsum <= 1e-9) return false;
    out = acc * (1.0 / wsum);
    return true;
}
}  // namespace

std::vector<float> gatherCpu(const TexelSamples& s, const OccluderMesh& mesh, const TriangleBvh& bvh, const std::vector<float>& atlasS, double albedo,
                             int giSamples, uint32_t sampleOffset, double range, int sAtlas) {
    const int storeAtlas = sAtlas > 0 ? sAtlas : s.atlas;
    std::vector<float> out(s.count() * 4, 0.f);
    const int total = std::max(1, giSamples);
    const bool textured = mesh.hasAlbedo();
    for (size_t i = 0; i < s.count(); ++i) {
        const Vec3 p{s.pos[i * 4], s.pos[i * 4 + 1], s.pos[i * 4 + 2]}, n{s.nrm[i * 4], s.nrm[i * 4 + 1], s.nrm[i * 4 + 2]};
        const Vec3 o = p + n * kShadowBias;
        Vec3 t, b;
        frameOf(n, t, b);
        Vec3 acc;
        for (int k = 0; k < total; ++k) {
            float u, v;
            sampleGi(s.texel[i], s.atlas, static_cast<uint32_t>(k) + sampleOffset, u, v);
            const double r = std::sqrt(static_cast<double>(u)), phi = 2.0 * kPi * v;
            const Vec3 dir = t * (r * std::cos(phi)) + b * (r * std::sin(phi)) + n * std::sqrt(std::max(0.0, 1.0 - u));
            const auto h = bvh.closestHit(o, dir, range);
            if (h.t < 0) continue;
            const float* m = &mesh.lightmap[static_cast<size_t>(h.triangle) * 8];
            if (m[7] < 0.5f) continue;  // shadow caster without a lightmap: absorbs
            const double w0 = 1 - h.b1 - h.b2;
            const double hu = w0 * m[0] + h.b1 * m[2] + h.b2 * m[4], hv = w0 * m[1] + h.b1 * m[3] + h.b2 * m[5];
            Vec3 radiance;
            if (atlasSample(atlasS, storeAtlas, static_cast<int>(m[6]), hu, hv, radiance)) {
                double scale = albedo * window(h.t, range);
                Vec3 colour{1, 1, 1};
                if (textured) {  // the hit surface's own colour: texture at the hit point (same filter as the GPU sampler)
                    const float* am = &mesh.albedoMeta[static_cast<size_t>(h.triangle) * 8];
                    const int tex = static_cast<int>(std::lround(am[6]));
                    if (tex >= 0) {
                        const double au = w0 * am[0] + h.b1 * am[2] + h.b2 * am[4], av = w0 * am[1] + h.b1 * am[3] + h.b2 * am[5];
                        colour = mesh.albedo->images[static_cast<size_t>(tex)]->sample(au, av, am[7]);
                    }
                }
                acc = acc + Vec3{radiance.x * colour.x, radiance.y * colour.y, radiance.z * colour.z} * scale;
            }
        }
        acc = acc * (1.0 / total);
        out[i * 4] = static_cast<float>(acc.x);
        out[i * 4 + 1] = static_cast<float>(acc.y);
        out[i * 4 + 2] = static_cast<float>(acc.z);
    }
    return out;
}

std::vector<float> evaluateGiCpu(const TexelSamples& s, const LightScene& scene, const OccluderMesh& mesh, const TriangleBvh& bvh, const BakeOptions& opt,
                                 std::vector<float>* indirectOut, const GiSamples* gi, std::vector<float>* giOut) {
    std::vector<float> db;
    std::vector<float> direct = directCpu(s, scene, opt.shadows ? &bvh : nullptr, opt, &db);
    if (indirectOut) indirectOut->assign(s.count() * 4, 0.f);
    if (giOut) giOut->clear();
    if (opt.bounces <= 0 || !mesh.hasLightmap() || s.count() == 0) return direct;
    const size_t perPage = static_cast<size_t>(s.atlas) * s.atlas;
    std::vector<float> atlas(perPage * static_cast<size_t>(s.pageCount) * 4, 0.f);
    for (size_t i = 0; i < s.count(); ++i) {
        for (size_t c = 0; c < 3; ++c) atlas[static_cast<size_t>(s.texel[i]) * 4 + c] = db[i * 4 + c];
        atlas[static_cast<size_t>(s.texel[i]) * 4 + 3] = 1.f;
    }
    const bool fine = gi && gi->scale > 1 && gi->count() > 0;
    const int baseBounces = fine ? opt.bounces - 1 : opt.bounces;  // the last one runs on the fine grid
    std::vector<float> indirect(s.count() * 4, 0.f);
    for (int it = 0; it < baseBounces; ++it) {
        indirect = gatherCpu(s, mesh, bvh, atlas, opt.albedo, opt.giSamples, opt.sampleOffset);
        for (size_t i = 0; i < s.count(); ++i)  // S(k+1) = Db + I(k+1)
            for (size_t c = 0; c < 3; ++c) atlas[static_cast<size_t>(s.texel[i]) * 4 + c] = db[i * 4 + c] + indirect[i * 4 + c];
    }
    if (fine) {
        std::vector<float> grid = gatherCpu(gi->s, mesh, bvh, atlas, opt.albedo, opt.giSamples, opt.sampleOffset, 10000.0, s.atlas);
        indirect = resolveGi(*gi, grid);
        if (giOut) *giOut = std::move(grid);
    }
    for (size_t i = 0; i < s.count() * 4; ++i) direct[i] += indirect[i];
    if (indirectOut) *indirectOut = std::move(indirect);
    return direct;
}

GiSamples buildGiSamples(const TexelSamples& base, int scale) {
    GiSamples g;
    if (scale <= 1 || !base.hasSub()) return g;
    scale = scale >= 3 ? 4 : 2;
    g.scale = scale;
    g.s.atlas = base.atlas * scale;
    g.s.pageCount = base.pageCount;
    const size_t atlas = static_cast<size_t>(base.atlas), hi = atlas * static_cast<size_t>(scale), perPage = atlas * atlas;
    // Two passes over all cores: how many entries every sample gets (a prefix sum gives where they go), then every sample writes its own entries.
    const size_t n = base.count();
    g.begin.assign(n + 1, 0);
    std::vector<uint32_t> masks(n);
    parallelFor(n, 4096, [&](size_t b0, size_t e0) {
        for (size_t i = b0; i < e0; ++i) {
            masks[i] = base.gridMask(i, scale);
            g.begin[i + 1] = masks[i] == 0 ? 1u : static_cast<uint32_t>(std::popcount(masks[i]));
        }
    });
    for (size_t i = 0; i < n; ++i) g.begin[i + 1] += g.begin[i];
    const size_t total = g.begin[n];
    g.s.pos.resize(total * 4);
    g.s.nrm.resize(total * 4);
    g.s.texel.resize(total);
    g.owner.resize(total);
    parallelFor(n, 4096, [&](size_t b0, size_t e0) {
        for (size_t i = b0; i < e0; ++i) {
            size_t at = g.begin[i];
            const size_t t = base.texel[i], page = t / perPage, x = t % atlas, y = (t / atlas) % atlas;
            const uint32_t mask = masks[i];
            auto add = [&](const Vec3& p, int cx, int cy) {
                float* po = &g.s.pos[at * 4];
                po[0] = static_cast<float>(p.x); po[1] = static_cast<float>(p.y); po[2] = static_cast<float>(p.z); po[3] = 1.f;
                std::memcpy(&g.s.nrm[at * 4], &base.nrm[i * 4], 4 * sizeof(float));
                g.s.texel[at] = static_cast<uint32_t>((page * hi + y * static_cast<size_t>(scale) + static_cast<size_t>(cy)) * hi + x * static_cast<size_t>(scale) + static_cast<size_t>(cx));
                g.owner[at] = static_cast<uint32_t>(i);
                ++at;
            };
            if (mask == 0) {  // no cell lies inside the fragment: one entry at the sample's own position, in the middle cell
                add({base.pos[i * 4], base.pos[i * 4 + 1], base.pos[i * 4 + 2]}, scale / 2, scale / 2);
                continue;
            }
            for (int c = 0; c < scale * scale; ++c)
                if (mask & (1u << c)) add(base.gridPosition(i, scale, c), c % scale, c / scale);
        }
    });
    return g;
}

std::vector<float> resolveGi(const GiSamples& gi, const std::vector<float>& v) {
    std::vector<float> out(gi.begin.empty() ? 0 : (gi.begin.size() - 1) * 4, 0.f);
    for (size_t i = 0; i + 1 < gi.begin.size(); ++i) {
        const uint32_t a = gi.begin[i], b = gi.begin[i + 1];
        if (b <= a) continue;
        for (uint32_t e = a; e < b; ++e)
            for (size_t c = 0; c < 3; ++c) out[i * 4 + c] += v[static_cast<size_t>(e) * 4 + c];
        for (size_t c = 0; c < 3; ++c) out[i * 4 + c] /= static_cast<float>(b - a);
    }
    return out;
}

// ---- pages ----------------------------------------------------------------------------------------------------------------
LightmapPages assemblePages(const LightmapLayout& layout, const TexelSamples& s, const std::vector<float>& v) {
    LightmapPages P;
    P.atlas = layout.settings.atlasSize;
    P.pageCount = layout.pageCount;
    const size_t per = static_cast<size_t>(P.atlas) * P.atlas;
    P.rgb.assign(static_cast<size_t>(P.pageCount), std::vector<float>(per * 3, 0.f));
    P.valid.assign(static_cast<size_t>(P.pageCount), std::vector<uint8_t>(per, 0));
    for (size_t i = 0; i < s.count(); ++i) {
        const size_t page = s.texel[i] / per, idx = s.texel[i] % per;
        for (int c = 0; c < 3; ++c) P.rgb[page][idx * 3 + c] = v[i * 4 + c];
        P.valid[page][idx] = 1;
    }
    // gutter: grow baked texels outwards by `padding`, averaging the neighbours that are already filled
    for (int page = 0; page < P.pageCount; ++page) {
        for (int it = 0; it < layout.settings.padding; ++it) {
            const std::vector<uint8_t> before = P.valid[static_cast<size_t>(page)];
            for (int y = 0; y < P.atlas; ++y)
                for (int x = 0; x < P.atlas; ++x) {
                    const size_t idx = static_cast<size_t>(y) * P.atlas + x;
                    if (before[idx]) continue;
                    float acc[3] = {0, 0, 0};
                    int n = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int nx = x + dx, ny = y + dy;
                            if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= P.atlas || ny >= P.atlas) continue;
                            const size_t j = static_cast<size_t>(ny) * P.atlas + nx;
                            if (!before[j]) continue;
                            for (int c = 0; c < 3; ++c) acc[c] += P.rgb[static_cast<size_t>(page)][j * 3 + c];
                            ++n;
                        }
                    if (!n) continue;
                    for (int c = 0; c < 3; ++c) P.rgb[static_cast<size_t>(page)][idx * 3 + c] = acc[c] / static_cast<float>(n);
                    P.valid[static_cast<size_t>(page)][idx] = 2;
                }
        }
    }
    return P;
}

LightmapPages bakeLightmaps(const LightmapLayout& layout, const std::vector<LightSurface>& surfaces, const LightScene& lights,
                            const BakeOptions& options, BakeStats* stats) {
    const auto t0 = std::chrono::steady_clock::now();
    BakeStats local;
    BakeStats& st = stats ? *stats : local;
    st = BakeStats{};
    st.warnings = lights.warnings;
    st.lights = lights.lights.size();
    const TexelSamples samples = buildTexelSamples(layout, surfaces, options.solids.get(), &st.hiddenTexels);
    st.texels = samples.count();

    const bool wantRays = options.shadows || options.bounces > 0;
    // textures matter to bounces (colour) and to shadows (transparency, REQUIREMENTS §23): with a loader the mesh carries albedo coordinates either way
    const bool withTextures = static_cast<bool>(options.albedoLoader);
    OccluderMesh mesh = wantRays ? buildOccluders(surfaces, options.classes, options.bounces > 0 || withTextures ? &layout : nullptr) : OccluderMesh{};
    if (withTextures) attachAlbedo(mesh, options.albedoLoader);
    std::vector<float> values, indirect, grid;
    const bool wantDenoise = options.denoiseGi && options.bounces > 0;
    // The bounce light is gathered on a finer grid (BakeOptions::giScale) so the denoiser sees more pixels and the average has more rays.
    const GiSamples gi = options.bounces > 0 && mesh.hasLightmap() ? buildGiSamples(samples, options.giScale) : GiSamples{};
    const GiSamples* giPtr = gi.scale > 1 ? &gi : nullptr;
    st.giScale = gi.scale;
    st.device = "cpu";
    if (!options.forceCpu && samples.count() > 0) {
        const gpu::Result r = gpu::evaluateDirect(samples, lights, &mesh, options, values, nullptr, &indirect, giPtr, &grid);
        st.validationMessages = r.validation;
        if (r.ok) {
            st.usedGpu = true;
            st.device = r.device;
        } else {
            st.warnings.push_back("GPU unavailable, using the CPU evaluator: " + r.error);
            values.clear();
            indirect.clear();
            grid.clear();
        }
    }
    if (!st.usedGpu) {
        const TriangleBvh bvh(mesh);
        values = options.bounces > 0 ? evaluateGiCpu(samples, lights, mesh, bvh, options, &indirect, giPtr, &grid)
                                     : evaluateDirectCpu(samples, lights, options.shadows ? &bvh : nullptr, options);
    }
    if (wantDenoise && indirect.size() == values.size() && !values.empty()) {
        // Only the bounce light is noisy (gather rays), so only it is filtered; the direct light stays as it was. With a finer grid the filter
        // runs on that grid and its result is averaged back to the texels.
        std::vector<float> clean;
        denoise::Result d;
        const auto quality = static_cast<denoise::Quality>(std::clamp(options.denoiseQuality, 0, 2));
        if (giPtr && grid.size() == gi.count() * 4) {
            std::vector<float> g = grid;
            d = denoise::denoiseSamples(gi.s, g, quality);
            if (d.ok) clean = resolveGi(gi, g);
        } else {
            clean = indirect;
            d = denoise::denoiseSamples(samples, clean, quality);
        }
        if (d.ok) {
            for (size_t i = 0; i < values.size(); ++i) values[i] += clean[i] - indirect[i];
            st.denoised = true;
            st.denoiseSeconds = d.seconds;
        } else {
            st.warnings.push_back("GI denoiser not applied (the indirect light stays noisy): " + d.error);
        }
    }
    for (size_t i = 0; i + 2 < values.size(); i += 4)  // dark lights (negative brightness) can leave a texel below 0: a lightmap stores none
        for (size_t c = 0; c < 3; ++c) values[i + c] = std::max(0.f, values[i + c]);
    LightmapPages pages = assemblePages(layout, samples, values);
    st.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return pages;
}

// ---- PNG (stored deflate blocks: no compression, no zlib dependency) -------------------------------------------------------
namespace {
uint32_t crc32(const uint8_t* d, size_t n, uint32_t crc = 0xFFFFFFFFu) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}
void be32(std::vector<uint8_t>& v, uint32_t x) { for (int s = 24; s >= 0; s -= 8) v.push_back(static_cast<uint8_t>(x >> s)); }
void chunk(std::ofstream& f, const char* type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> buf;
    be32(buf, static_cast<uint32_t>(data.size()));
    buf.insert(buf.end(), type, type + 4);
    buf.insert(buf.end(), data.begin(), data.end());
    uint32_t c = crc32(reinterpret_cast<const uint8_t*>(type), 4);
    c = crc32(data.data(), data.size(), c);
    be32(buf, c ^ 0xFFFFFFFFu);
    f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
}
uint8_t srgb8(float v) {
    v = std::clamp(v, 0.f, 1.f);
    const float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::lround(s * 255.f));
}
}  // namespace

bool writePagePng(const LightmapPages& P, int page, const std::string& path) {
    if (page < 0 || page >= P.pageCount) return false;
    std::vector<uint8_t> raw;
    for (int y = 0; y < P.atlas; ++y) {
        raw.push_back(0);  // filter: none
        for (int x = 0; x < P.atlas; ++x) {
            const size_t i = (static_cast<size_t>(y) * P.atlas + x) * 3;
            // untouched texels show as dark magenta so unused atlas space is distinguishable from black light
            if (!P.valid[static_cast<size_t>(page)][static_cast<size_t>(y) * P.atlas + x]) { raw.insert(raw.end(), {40, 0, 40}); continue; }
            for (int c = 0; c < 3; ++c) raw.push_back(srgb8(P.rgb[static_cast<size_t>(page)][i + c]));
        }
    }
    std::vector<uint8_t> z{0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t byte : raw) { a = (a + byte) % 65521; b = (b + a) % 65521; }
    for (size_t pos = 0; pos < raw.size(); pos += 65535) {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n >= raw.size() ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n & 0xFF)); z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n & 0xFF)); z.push_back(static_cast<uint8_t>((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
    }
    be32(z, (b << 16) | a);
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    f.write(reinterpret_cast<const char*>(sig), 8);
    std::vector<uint8_t> ihdr;
    be32(ihdr, static_cast<uint32_t>(P.atlas));
    be32(ihdr, static_cast<uint32_t>(P.atlas));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk(f, "IHDR", ihdr);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", {});
    return static_cast<bool>(f);
}

}  // namespace core
