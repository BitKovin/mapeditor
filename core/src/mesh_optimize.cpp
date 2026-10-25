#include "core/mesh_optimize.h"
#include <meshoptimizer.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace core {

namespace {

using rbsp::DrawVert;
constexpr uint32_t kNone = 0xFFFFFFFFu;

uint64_t mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDull;
    x ^= x >> 33;
    x *= 0xC4CEB9FE1A85EC53ull;
    x ^= x >> 33;
    return x;
}

uint64_t hashBytes(const void* data, size_t size) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    uint64_t h = 0x9E3779B97F4A7C15ull;
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = mix64(h ^ w) + 0x9E3779B97F4A7C15ull;
    }
    for (; i < size; ++i) h = mix64(h ^ p[i]);
    return h;
}

uint64_t hashPosition(const float* p) {
    uint32_t b[3];
    std::memcpy(b, p, 12);
    return mix64(mix64(b[0] + 0x9E3779B97F4A7C15ull) ^ (uint64_t(b[1]) << 21) ^ (uint64_t(b[2]) * 0xD6E8FEB86659FD93ull));
}

// Points in a hash grid. `query` finds the points within r (r <= cell) of a position: at most two cells per axis.
class PointGrid {
public:
    explicit PointGrid(double cell) : inv_(1.0 / cell) {}
    void add(const float* p, uint32_t id) {
        const uint64_t k = key(cellOf(p[0]), cellOf(p[1]), cellOf(p[2]));
        const auto it = heads_.find(k);
        const uint32_t node = static_cast<uint32_t>(nodes_.size());
        nodes_.push_back({{p[0], p[1], p[2]}, id, it == heads_.end() ? kNone : it->second});
        heads_[k] = node;
    }
    /// Calls fn(id) for the points within r of p; stops and returns true when fn returns true.
    template <class F> bool query(const float* p, double r, F&& fn) const {
        int64_t lo[3], hi[3];
        for (int a = 0; a < 3; ++a) { lo[a] = cellOf(static_cast<double>(p[a]) - r); hi[a] = cellOf(static_cast<double>(p[a]) + r); }
        const double r2 = r * r;
        for (int64_t z = lo[2]; z <= hi[2]; ++z)
            for (int64_t y = lo[1]; y <= hi[1]; ++y)
                for (int64_t x = lo[0]; x <= hi[0]; ++x) {
                    const auto it = heads_.find(key(x, y, z));
                    if (it == heads_.end()) continue;
                    for (uint32_t n = it->second; n != kNone; n = nodes_[n].next) {
                        const Node& q = nodes_[n];
                        const double dx = q.p[0] - double(p[0]), dy = q.p[1] - double(p[1]), dz = q.p[2] - double(p[2]);
                        if (dx * dx + dy * dy + dz * dz <= r2 && fn(q.id)) return true;
                    }
                }
        return false;
    }

private:
    struct Node { float p[3]; uint32_t id, next; };
    int64_t cellOf(double v) const { return static_cast<int64_t>(std::floor(v * inv_)); }
    static uint64_t key(int64_t x, int64_t y, int64_t z) {
        return mix64(static_cast<uint64_t>(x) * 0x9E3779B97F4A7C15ull) ^ mix64(static_cast<uint64_t>(y) * 0xC2B2AE3D27D4EB4Full + 1) ^ mix64(static_cast<uint64_t>(z) * 0x165667B19E3779F9ull + 2);
    }
    double inv_;
    std::unordered_map<uint64_t, uint32_t> heads_;
    std::vector<Node> nodes_;
};

bool samePosition(const DrawVert& a, const DrawVert& b) { return std::memcmp(a.xyz, b.xyz, sizeof a.xyz) == 0; }

// Alike in everything but the position: within what float noise (and a hair more) explains.
bool attributesAlike(const DrawVert& a, const DrawVert& b, const MeshItem& it) {
    const double st = it.stTolerance * 0.1, lm = it.lightmapTolerance * 0.1;
    for (int k = 0; k < 2; ++k)
        if (std::fabs(double(a.st[k]) - b.st[k]) > st) return false;
    for (int s = 0; s < rbsp::kStyles; ++s)
        for (int k = 0; k < 2; ++k)
            if (std::fabs(double(a.lightmap[s][k]) - b.lightmap[s][k]) > lm) return false;
    for (int k = 0; k < 3; ++k)
        if (std::fabs(double(a.normal[k]) - b.normal[k]) > 1e-3) return false;
    for (int s = 0; s < rbsp::kStyles; ++s)
        for (int k = 0; k < 4; ++k)
            if (std::abs(int(a.color[s][k]) - int(b.color[s][k])) > 1) return false;
    return true;
}

// Drops the vertices no triangle uses, keeping the order of the others. `flags` (per vertex, may be null) follows them.
void compact(MeshItem& it, std::vector<uint8_t>* flags) {
    const size_t n = it.verts.size();
    std::vector<uint32_t> map(n, kNone);
    for (int32_t i : it.indexes) map[static_cast<size_t>(i)] = 0;
    size_t kept = 0;
    for (size_t i = 0; i < n; ++i)
        if (map[i] != kNone) {
            map[i] = static_cast<uint32_t>(kept);
            it.verts[kept] = it.verts[i];
            if (flags) (*flags)[kept] = (*flags)[i];
            ++kept;
        }
    it.verts.resize(kept);
    if (flags) flags->resize(kept);
    for (int32_t& i : it.indexes) i = static_cast<int32_t>(map[static_cast<size_t>(i)]);
}

// 1. The vertices that are one point: equal bytes; then positions within weldDistance made the same bits; then equal positions alike in every attribute.
void weldItem(MeshItem& it, const MeshOptimizeParams& P, MeshOptimizeStats& S) {
    std::vector<DrawVert>& V = it.verts;
    const size_t n = V.size();
    if (n == 0 || it.indexes.empty()) return;
    std::vector<uint32_t> rep(n);
    {
        std::unordered_map<uint64_t, uint32_t> head;
        head.reserve(n / 2 + 1);
        std::vector<uint32_t> next(n, kNone);
        for (uint32_t i = 0; i < n; ++i) {
            rep[i] = i;
            const auto ins = head.emplace(hashBytes(&V[i], sizeof(DrawVert)), i);
            if (ins.second) continue;
            bool found = false;
            for (uint32_t j = ins.first->second; j != kNone; j = next[j])
                if (std::memcmp(&V[j], &V[i], sizeof(DrawVert)) == 0) { rep[i] = j; found = true; break; }
            if (!found) { next[i] = ins.first->second; ins.first->second = i; }
        }
    }
    {
        PointGrid grid(P.weldDistance);
        for (uint32_t i = 0; i < n; ++i) {
            if (rep[i] != i) continue;
            const bool found = grid.query(V[i].xyz, P.weldDistance, [&](uint32_t j) {
                std::memcpy(V[i].xyz, V[j].xyz, sizeof V[i].xyz);
                return true;
            });
            if (!found) grid.add(V[i].xyz, i);
        }
    }
    std::vector<uint32_t> rep2(n);
    size_t distinct = 0;
    {
        std::unordered_map<uint64_t, uint32_t> head;
        head.reserve(n / 2 + 1);
        std::vector<uint32_t> next(n, kNone);
        for (uint32_t i = 0; i < n; ++i) {
            rep2[i] = i;
            if (rep[i] != i) continue;
            const auto ins = head.emplace(hashPosition(V[i].xyz), i);
            if (ins.second) { ++distinct; continue; }
            bool found = false;
            for (uint32_t j = ins.first->second; j != kNone; j = next[j])
                if (samePosition(V[j], V[i]) && attributesAlike(V[j], V[i], it)) { rep2[i] = j; found = true; break; }
            if (!found) { next[i] = ins.first->second; ins.first->second = i; ++distinct; }
        }
    }
    std::vector<int32_t> out;
    out.reserve(it.indexes.size());
    for (size_t t = 0; t + 2 < it.indexes.size(); t += 3) {
        const uint32_t a = rep2[rep[static_cast<size_t>(it.indexes[t])]], b = rep2[rep[static_cast<size_t>(it.indexes[t + 1])]], c = rep2[rep[static_cast<size_t>(it.indexes[t + 2])]];
        if (a == b || b == c || a == c) { ++S.removedTriangles; continue; }  // the welding closed it
        out.push_back(static_cast<int32_t>(a)); out.push_back(static_cast<int32_t>(b)); out.push_back(static_cast<int32_t>(c));
    }
    it.indexes = std::move(out);
    compact(it, nullptr);
    S.weldedVertices += n - distinct;
    S.removedVertices += distinct - V.size();
}

// Vertices on an edge that one triangle only (or more than two) uses: the outline of the surface, and its non-manifold spots.
std::vector<uint8_t> outlineVertices(size_t vertexCount, const std::vector<int32_t>& idx) {
    std::vector<uint64_t> edges;
    edges.reserve(idx.size());
    for (size_t t = 0; t + 2 < idx.size(); t += 3)
        for (int k = 0; k < 3; ++k) {
            const uint32_t a = static_cast<uint32_t>(idx[t + static_cast<size_t>(k)]), b = static_cast<uint32_t>(idx[t + static_cast<size_t>((k + 1) % 3)]);
            edges.push_back((uint64_t(std::min(a, b)) << 32) | std::max(a, b));
        }
    std::sort(edges.begin(), edges.end());
    std::vector<uint8_t> flags(vertexCount, 0);
    for (size_t i = 0; i < edges.size();) {
        size_t j = i;
        while (j < edges.size() && edges[j] == edges[i]) ++j;
        if (j - i != 2) { flags[edges[i] >> 32] = 1; flags[edges[i] & 0xFFFFFFFFu] = 1; }
        i = j;
    }
    return flags;
}

// Alike within what is visible (the tolerances of the surface): two vertices that may become one without a change the eye can see.
bool attributesClose(const DrawVert& a, const DrawVert& b, const MeshItem& it, const MeshOptimizeParams& P) {
    for (int k = 0; k < 2; ++k)
        if (std::fabs(double(a.st[k]) - b.st[k]) > it.stTolerance) return false;
    for (int s = 0; s < it.lightmapSlots && s < rbsp::kStyles; ++s)
        for (int k = 0; k < 2; ++k)
            if (std::fabs(double(a.lightmap[s][k]) - b.lightmap[s][k]) > it.lightmapTolerance) return false;
    for (int k = 0; k < 3; ++k)
        if (std::fabs(double(a.normal[k]) - b.normal[k]) > P.normalTolerance) return false;
    if (it.vertexLit)
        for (int k = 0; k < 3; ++k)
            if (std::fabs(double(a.color[0][k]) - b.color[0][k]) / 255.0 > P.colorTolerance) return false;
    return true;
}

// 1b. Collapses the edges shorter than P.minEdge. Clusters are grown around a representative (a vertex another surface shares first: it never moves; then
// the others in order) from the vertices an edge joins to it and that lie within minEdge of the representative itself, so a chain of close vertices
// does not drift. The members take the representative's whole vertex. Two shared vertices never merge: their surfaces would part.
void collapseShortEdges(MeshItem& it, const std::vector<uint8_t>& shared, const MeshOptimizeParams& P, MeshOptimizeStats& S) {
    const size_t n = it.verts.size();
    if (n == 0 || it.indexes.empty() || !(P.minEdge > 0)) return;
    std::vector<std::vector<uint32_t>> adj(n);
    const double e2 = P.minEdge * P.minEdge;
    auto dist2 = [&](uint32_t a, uint32_t b) {
        double d = 0;
        for (int k = 0; k < 3; ++k) { const double x = double(it.verts[a].xyz[k]) - it.verts[b].xyz[k]; d += x * x; }
        return d;
    };
    bool any = false;
    for (size_t t = 0; t + 2 < it.indexes.size(); t += 3)
        for (size_t k = 0; k < 3; ++k) {
            const uint32_t a = static_cast<uint32_t>(it.indexes[t + k]), b = static_cast<uint32_t>(it.indexes[t + (k + 1) % 3]);
            if (a != b && dist2(a, b) < e2) { adj[a].push_back(b); adj[b].push_back(a); any = true; }
        }
    if (!any) return;
    std::vector<uint32_t> rep(n, kNone);
    std::vector<uint32_t> order;
    order.reserve(n);
    for (uint32_t i = 0; i < n; ++i) if (shared[i]) order.push_back(i);
    for (uint32_t i = 0; i < n; ++i) if (!shared[i]) order.push_back(i);
    for (uint32_t r : order) {
        if (rep[r] != kNone) continue;
        rep[r] = r;
        for (uint32_t m : adj[r]) {
            if (rep[m] != kNone || shared[m] || dist2(r, m) >= e2 || !attributesClose(it.verts[r], it.verts[m], it, P)) continue;
            rep[m] = r;
            ++S.collapsedEdges;
        }
    }
    std::vector<int32_t> out;
    out.reserve(it.indexes.size());
    for (size_t t = 0; t + 2 < it.indexes.size(); t += 3) {
        const int32_t a = static_cast<int32_t>(rep[static_cast<size_t>(it.indexes[t])]), b = static_cast<int32_t>(rep[static_cast<size_t>(it.indexes[t + 1])]), c = static_cast<int32_t>(rep[static_cast<size_t>(it.indexes[t + 2])]);
        if (a == b || b == c || a == c) { ++S.removedTriangles; continue; }
        out.push_back(a); out.push_back(b); out.push_back(c);
    }
    it.indexes = std::move(out);
}

// 2. One quadric edge collapse pass over the surface. Error is a distance (Quake units): the attributes are weighed so that a change of the size that
// counts as visible costs as much as moving the surface by `tolerance`.
void simplifyPass(MeshItem& it, std::vector<uint8_t>& shared, double tolerance, bool lockOutline, const MeshOptimizeParams& P, MeshOptimizeStats& S) {
    const size_t n = it.verts.size(), triangles = it.indexes.size() / 3;
    if (n == 0 || triangles < P.minTriangles) return;
    const float scale = meshopt_simplifyScale(it.verts[0].xyz, n, sizeof(DrawVert));
    if (!(scale > 0)) return;
    const int slots = std::clamp(it.lightmapSlots, 0, rbsp::kStyles);
    const size_t na = 2 + 2 * static_cast<size_t>(slots) + 3 + (it.vertexLit ? 3 : 0);
    const float rel = static_cast<float>(tolerance / scale);  // the simplifier works in units of the mesh extent
    std::vector<float> weights;
    weights.insert(weights.end(), 2, rel / static_cast<float>(it.stTolerance));
    weights.insert(weights.end(), 2 * static_cast<size_t>(slots), rel / static_cast<float>(it.lightmapTolerance));
    weights.insert(weights.end(), 3, rel / static_cast<float>(P.normalTolerance));
    if (it.vertexLit) weights.insert(weights.end(), 3, rel / static_cast<float>(P.colorTolerance));
    std::vector<float> attr(n * na);
    for (size_t i = 0; i < n; ++i) {
        const DrawVert& v = it.verts[i];
        float* a = &attr[i * na];
        *a++ = v.st[0]; *a++ = v.st[1];
        for (int s = 0; s < slots; ++s) { *a++ = v.lightmap[s][0]; *a++ = v.lightmap[s][1]; }
        *a++ = v.normal[0]; *a++ = v.normal[1]; *a++ = v.normal[2];
        if (it.vertexLit) { *a++ = v.color[0][0] / 255.f; *a++ = v.color[0][1] / 255.f; *a++ = v.color[0][2] / 255.f; }
    }
    std::vector<unsigned int> out(it.indexes.size());
    const unsigned int options = meshopt_SimplifyErrorAbsolute | (lockOutline ? meshopt_SimplifyLockBorder : 0u);
    const size_t kept = meshopt_simplifyWithAttributes(out.data(), reinterpret_cast<const unsigned int*>(it.indexes.data()), it.indexes.size(), it.verts[0].xyz, n, sizeof(DrawVert),
                                                      attr.data(), na * sizeof(float), weights.data(), na, lockOutline ? nullptr : shared.data(), 0, static_cast<float>(tolerance), options, nullptr);
    if (kept >= it.indexes.size() || kept == 0) return;
    S.removedTriangles += (it.indexes.size() - kept) / 3;
    const size_t before = n;
    it.indexes.assign(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(kept));
    compact(it, &shared);
    S.removedVertices += before - it.verts.size();
}

// 3. The triangles without area that the passes above leave: the weld snaps vertices together and the lossless collapse removes the middle vertex of a
// straight outline edge, which leaves a flat triangle of three collinear vertices behind. They draw nothing (they have no area) but the physics mesh
// the engine builds from the same triangles rejects them. The vertices only they used go with them.
void dropDegenerate(MeshItem& it, const MeshOptimizeParams& P, MeshOptimizeStats& S) {
    std::vector<int32_t> out;
    out.reserve(it.indexes.size());
    for (size_t t = 0; t + 2 < it.indexes.size(); t += 3) {
        const float* a = it.verts[static_cast<size_t>(it.indexes[t])].xyz;
        const float* b = it.verts[static_cast<size_t>(it.indexes[t + 1])].xyz;
        const float* c = it.verts[static_cast<size_t>(it.indexes[t + 2])].xyz;
        const double ux = double(b[0]) - a[0], uy = double(b[1]) - a[1], uz = double(b[2]) - a[2];
        const double vx = double(c[0]) - a[0], vy = double(c[1]) - a[1], vz = double(c[2]) - a[2];
        const double cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
        if (!(std::sqrt(cx * cx + cy * cy + cz * cz) >= P.minTriangleCross)) { ++S.removedTriangles; continue; }  // also catches NaN
        const double wx = double(c[0]) - b[0], wy = double(c[1]) - b[1], wz = double(c[2]) - b[2];
        const double e2 = P.minEdge * P.minEdge;  // two corners the physics mesh would weld into one
        if (ux * ux + uy * uy + uz * uz < e2 || vx * vx + vy * vy + vz * vz < e2 || wx * wx + wy * wy + wz * wz < e2) { ++S.removedTriangles; continue; }
        out.push_back(it.indexes[t]); out.push_back(it.indexes[t + 1]); out.push_back(it.indexes[t + 2]);
    }
    if (out.size() == it.indexes.size()) return;
    const size_t before = it.verts.size();
    it.indexes = std::move(out);
    compact(it, nullptr);
    S.removedVertices += before - it.verts.size();
}

}  // namespace

void optimizeSurfaces(std::vector<MeshItem>& items, const MeshOptimizeParams& P, MeshOptimizeStats* stats) {
    MeshOptimizeStats local;
    MeshOptimizeStats& S = stats ? *stats : local;
    for (MeshItem& it : items)
        if (it.optimize) weldItem(it, P, S);
    // the outline vertices of every surface: where two surfaces meet, both keep their vertices
    PointGrid grid(P.shareDistance);
    std::vector<std::vector<uint8_t>> outline(items.size());
    for (size_t s = 0; s < items.size(); ++s) {
        std::vector<uint8_t> o = outlineVertices(items[s].verts.size(), items[s].indexes);
        for (size_t v = 0; v < o.size(); ++v)
            if (o[v]) grid.add(items[s].verts[v].xyz, static_cast<uint32_t>(s));
        if (items[s].optimize) outline[s] = std::move(o);
    }
    for (size_t s = 0; s < items.size(); ++s) {
        MeshItem& it = items[s];
        if (!it.optimize || it.verts.empty()) continue;
        std::vector<uint8_t> shared(it.verts.size(), 0);
        for (size_t v = 0; v < shared.size(); ++v) {
            if (!outline[s][v]) continue;
            shared[v] = grid.query(it.verts[v].xyz, P.shareDistance, [&](uint32_t other) { return other != s; }) ? 1 : 0;
            S.sharedVertices += shared[v];
        }
        const size_t trisBefore = it.indexes.size() / 3;
        collapseShortEdges(it, shared, P, S);
        {  // the surface is smaller now: the vertices only the removed triangles used go (and `shared` follows)
            const size_t before = it.verts.size();
            compact(it, &shared);
            S.removedVertices += before - it.verts.size();
        }
        if (P.tolerance > P.losslessTolerance) simplifyPass(it, shared, P.tolerance, true, P, S);
        simplifyPass(it, shared, P.losslessTolerance, false, P, S);
        dropDegenerate(it, P, S);
        S.surfaces += trisBefore != it.indexes.size() / 3;
    }
}

}  // namespace core
