#include "core/lightmap_uv.h"
#include "core/smoothing.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <tuple>
#include <unordered_map>

using nlohmann::json;

namespace core {

std::string LightmapFace::key() const {
    return instance + "|" + entityId + "|" + brushInstance + "|" + brushId + "|" + std::to_string(faceId) + (side == FaceSide::Front ? "|F" : "|B");
}

namespace {

constexpr double kPi = 3.14159265358979323846;

// ---- small linear algebra --------------------------------------------------------------------------------------------
struct M3 {  // columns
    Vec3 c0{1, 0, 0}, c1{0, 1, 0}, c2{0, 0, 1};
    Vec3 mul(const Vec3& v) const { return c0 * v.x + c1 * v.y + c2 * v.z; }
};

struct P2 {
    double s = 0, t = 0;
};

// ---- internal face model ---------------------------------------------------------------------------------------------
struct Face {
    LightmapFace out;
    std::vector<std::vector<Vec3>> pieces;
    std::string group;  // instance + entity: faces only merge inside one group
    std::vector<int> styles;  // non-base styles touching the face (LightmapSettings::faceStyles)
    double scale = 1;   // density multiplier
    const BrushSmoothing* smooth = nullptr;  // smooth edges of the owning brush (a face joined by one is unfolded whatever the angle)
    Vec3 mn, mx;       // world bounds
    Vec3 u, v;         // root frame when this face is an island root
};

struct Adj {
    int other;
    Vec3 p, q;  // shared edge in THIS face's traversal direction
};

Vec3 newell(const std::vector<Vec3>& v) {
    Vec3 n;
    for (size_t i = 0; i < v.size(); ++i) {
        const Vec3 &a = v[i], &b = v[(i + 1) % v.size()];
        n = n + Vec3{(a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y)};
    }
    return n;  // length = 2 * area
}

void frameFor(const Vec3& n, Vec3& u, Vec3& v) {
    if (std::fabs(n.y) > 0.70710678118654752) {
        u = (Vec3{1, 0, 0} - n * n.x).normalized();  // world +X projected into the plane
    } else {
        u = Vec3{0, 1, 0}.cross(n).normalized();  // horizontal
    }
    v = n.cross(u);
}

// SAT on two convex 2D polygons. True when they overlap by more than `eps` (touching along an edge is not overlap).
bool overlaps(const std::vector<P2>& A, const std::vector<P2>& B, double eps) {
    auto test = [&](const std::vector<P2>& poly) {
        for (size_t i = 0; i < poly.size(); ++i) {
            const P2 &a = poly[i], &b = poly[(i + 1) % poly.size()];
            double ax = -(b.t - a.t), ay = b.s - a.s;
            const double len = std::hypot(ax, ay);
            if (len < 1e-12) continue;
            ax /= len;
            ay /= len;
            double minA = 1e300, maxA = -1e300, minB = 1e300, maxB = -1e300;
            for (const auto& p : A) { const double d = p.s * ax + p.t * ay; minA = std::min(minA, d); maxA = std::max(maxA, d); }
            for (const auto& p : B) { const double d = p.s * ax + p.t * ay; minB = std::min(minB, d); maxB = std::max(maxB, d); }
            if (maxA <= minB + eps || maxB <= minA + eps) return true;  // separated
        }
        return false;
    };
    return !(test(A) || test(B));
}

uint32_t part1by2(uint32_t x) {
    x &= 0x3ff;
    x = (x | (x << 16)) & 0x030000FF;
    x = (x | (x << 8)) & 0x0300F00F;
    x = (x | (x << 4)) & 0x030C30C3;
    x = (x | (x << 2)) & 0x09249249;
    return x;
}

// ---- MaxRects (best short side fit, no rotation) ---------------------------------------------------------------------
struct Rect {
    int x, y, w, h;
};

struct Page {
    std::vector<Rect> freeRects;
    long long used = 0;
    explicit Page(int size) { freeRects.push_back({0, 0, size, size}); }

    std::optional<Rect> place(int w, int h) {
        int best = -1;
        long long bestShort = std::numeric_limits<long long>::max(), bestLong = bestShort;
        for (size_t i = 0; i < freeRects.size(); ++i) {
            const Rect& f = freeRects[i];
            if (f.w < w || f.h < h) continue;
            const long long dw = f.w - w, dh = f.h - h;
            const long long sh = std::min(dw, dh), lg = std::max(dw, dh);
            // ties: lower y, then lower x, for a deterministic and compact result
            if (sh < bestShort || (sh == bestShort && lg < bestLong) ||
                (sh == bestShort && lg == bestLong && best >= 0 && std::tie(f.y, f.x) < std::tie(freeRects[best].y, freeRects[best].x))) {
                best = static_cast<int>(i);
                bestShort = sh;
                bestLong = lg;
            }
        }
        if (best < 0) return std::nullopt;
        const Rect r{freeRects[best].x, freeRects[best].y, w, h};
        std::vector<Rect> next;
        for (const Rect& f : freeRects) {
            if (r.x >= f.x + f.w || r.x + r.w <= f.x || r.y >= f.y + f.h || r.y + r.h <= f.y) { next.push_back(f); continue; }
            if (r.x > f.x) next.push_back({f.x, f.y, r.x - f.x, f.h});
            if (r.x + r.w < f.x + f.w) next.push_back({r.x + r.w, f.y, f.x + f.w - (r.x + r.w), f.h});
            if (r.y > f.y) next.push_back({f.x, f.y, f.w, r.y - f.y});
            if (r.y + r.h < f.y + f.h) next.push_back({f.x, r.y + r.h, f.w, f.y + f.h - (r.y + r.h)});
        }
        // prune rectangles contained in another one
        std::vector<Rect> kept;
        for (size_t i = 0; i < next.size(); ++i) {
            bool contained = false;
            for (size_t j = 0; j < next.size() && !contained; ++j) {
                if (i == j) continue;
                const Rect &a = next[i], &b = next[j];
                if (a.x >= b.x && a.y >= b.y && a.x + a.w <= b.x + b.w && a.y + a.h <= b.y + b.h && (i > j || !(a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h))) contained = true;
            }
            if (!contained) kept.push_back(next[i]);
        }
        freeRects = std::move(kept);
        used += static_cast<long long>(w) * h;
        return r;
    }
};

struct Member {
    int face;
    M3 M;
    Vec3 b;
};

struct Island {
    int root = -1;
    Vec3 u, v;
    double k = 1;  // texels per metre (root density)
    std::vector<Member> members;
    double minS = 1e300, maxS = -1e300, minT = 1e300, maxT = -1e300;
    std::vector<std::vector<P2>> polys;  // all pieces of all members, in island texel space
    std::vector<int> polyFace;           // owning face of each entry of `polys`
    std::vector<std::array<double, 4>> polyBox;
    // result of layout
    int w = 0, h = 0, originS = 0, originT = 0;
    std::string bucket;
    uint32_t morton = 0;
    Vec3 center;
    std::string texture, rootKey;
    double shrink = 1;
};

}  // namespace

// ---- lookup ----------------------------------------------------------------------------------------------------------
const LightmapFace* LightmapLayout::find(const std::string& instance, const std::string& entityId, const std::string& brushInstance,
                                         const std::string& brushId, uint32_t faceId, FaceSide side) const {
    for (const auto& f : faces)
        if (f.faceId == faceId && f.side == side && f.brushId == brushId && f.entityId == entityId && f.instance == instance && f.brushInstance == brushInstance) return &f;
    return nullptr;
}

const LightmapFace* LightmapLayout::findForFragment(const std::string& instance, const std::string& entityId,
                                                    const std::string& brushInstance, const std::string& brushId, uint32_t faceId,
                                                    const std::vector<Vec3>& verts) const {
    const Vec3 n = newell(verts).normalized();
    const LightmapFace* front = find(instance, entityId, brushInstance, brushId, faceId, FaceSide::Front);
    const LightmapFace* back = find(instance, entityId, brushInstance, brushId, faceId, FaceSide::Back);
    if (front && back) return n.dot(front->normal) >= n.dot(back->normal) ? front : back;
    return front ? front : back;
}

json LightmapLayout::toJson() const {
    json fj = json::array();
    for (const auto& f : faces) {
        fj.push_back(json{{"instance", f.instance}, {"entityId", f.entityId}, {"brushInstance", f.brushInstance}, {"brush", f.brushId},
                          {"face", f.faceId}, {"side", f.side == FaceSide::Front ? "front" : "back"}, {"texture", f.texture},
                          {"chart", f.chart}, {"su", {f.su.x, f.su.y, f.su.z}}, {"sv", {f.sv.x, f.sv.y, f.sv.z}},
                          {"so", f.so}, {"to", f.to}, {"texelsPerMeter", f.texelsPerMeter}});
    }
    json cj = json::array();
    for (const auto& c : charts)
        cj.push_back(json{{"page", c.page}, {"x", c.x}, {"y", c.y}, {"w", c.w}, {"h", c.h}, {"texture", c.texture}, {"faces", c.faces}, {"styles", c.styles}});
    return json{{"type", "lightmap_layout"}, {"atlasSize", settings.atlasSize}, {"pageCount", pageCount}, {"pageFill", pageFill},
                {"faces", fj}, {"charts", cj}, {"warnings", warnings}};
}

// ---- the generator ---------------------------------------------------------------------------------------------------
LightmapLayout buildLightmapLayout(const std::vector<LightSurface>& surfaces, const LightingClasses& classes, const LightmapSettings& cfg) {
    LightmapLayout out;
    out.settings = cfg;
    const int pad = std::max(0, cfg.padding);
    const int atlas = std::max(8, cfg.atlasSize);
    const int sideLimit = std::max(1, std::min(cfg.maxChartTexels, atlas - 2 * pad));

    // ---- A. faces (one per authored face side) ----------------------------------------------------------------------
    std::map<std::string, size_t> byKey;
    std::vector<Face> faces;
    for (const auto& s : surfaces) {
        if (!classes.isLightmapped(s.classname) || s.verts.size() < 3) continue;
        LightmapFace probe;
        probe.instance = s.instance; probe.entityId = s.entityId; probe.brushInstance = s.brushInstance; probe.brushId = s.brushId;
        probe.faceId = s.faceId; probe.side = s.side;
        const std::string key = probe.key();
        auto it = byKey.find(key);
        if (it == byKey.end()) {
            it = byKey.emplace(key, faces.size()).first;
            Face f;
            f.out = probe;
            f.out.texture = s.texture;
            f.group = s.instance + "|" + s.entityId;
            f.smooth = s.smoothing.get();
            const auto sc = cfg.densityScale.find(key);
            // the face's own scale (authored in the document) times the settings' override for this face key, if any
            f.scale = std::clamp(s.lightmapScale, kMinLightmapScale, kMaxLightmapScale) * ((sc != cfg.densityScale.end() && sc->second > 1e-6) ? sc->second : 1.0);
            const auto st = cfg.faceStyles.find(key);
            if (st != cfg.faceStyles.end()) f.styles = st->second;
            faces.push_back(std::move(f));
        }
        Face& f = faces[it->second];
        const Vec3 n2 = newell(s.verts);
        const double area = 0.5 * n2.length();
        f.out.area += area;
        f.out.normal = f.out.normal + n2 * 0.5;  // area-weighted accumulation
        f.pieces.push_back(s.verts);
    }
    // deterministic order, drop degenerate faces
    std::vector<Face> sorted;
    for (auto& f : faces) {
        if (f.out.area < 1e-9 || f.out.normal.length() < 1e-12) continue;
        f.out.normal = f.out.normal.normalized();
        sorted.push_back(std::move(f));
    }
    std::sort(sorted.begin(), sorted.end(), [](const Face& a, const Face& b) { return a.out.key() < b.out.key(); });
    faces = std::move(sorted);
    const int N = static_cast<int>(faces.size());
    for (auto& f : faces) {
        frameFor(f.out.normal, f.u, f.v);
        f.mn = Vec3{1e300, 1e300, 1e300};
        f.mx = Vec3{-1e300, -1e300, -1e300};
        for (const auto& piece : f.pieces)
            for (const auto& p : piece) {
                f.mn = {std::min(f.mn.x, p.x), std::min(f.mn.y, p.y), std::min(f.mn.z, p.z)};
                f.mx = {std::max(f.mx.x, p.x), std::max(f.mx.y, p.y), std::max(f.mx.z, p.z)};
            }
    }

    // ---- B. adjacency (shared edges) --------------------------------------------------------------------------------
    struct PieceRef {
        int face;
        int piece;
        Vec3 mn, mx;
    };
    std::vector<PieceRef> prefs;
    for (int fi = 0; fi < N; ++fi)
        for (size_t pi = 0; pi < faces[fi].pieces.size(); ++pi) {
            PieceRef r{fi, static_cast<int>(pi), Vec3{1e300, 1e300, 1e300}, Vec3{-1e300, -1e300, -1e300}};
            for (const auto& p : faces[fi].pieces[pi]) {
                r.mn = {std::min(r.mn.x, p.x), std::min(r.mn.y, p.y), std::min(r.mn.z, p.z)};
                r.mx = {std::max(r.mx.x, p.x), std::max(r.mx.y, p.y), std::max(r.mx.z, p.z)};
            }
            const double e = cfg.edgeEps * 2;
            r.mn = r.mn - Vec3{e, e, e};
            r.mx = r.mx + Vec3{e, e, e};
            prefs.push_back(r);
        }
    constexpr double kCell = 4.0;
    constexpr long long kMaxCells = 4096;
    std::unordered_map<uint64_t, std::vector<int>> grid;
    std::vector<int> big;
    auto cellKey = [](long long x, long long y, long long z) {
        return (static_cast<uint64_t>(x + 1000000) << 42) ^ (static_cast<uint64_t>(y + 1000000) << 21) ^ static_cast<uint64_t>(z + 1000000);
    };
    for (int i = 0; i < static_cast<int>(prefs.size()); ++i) {
        const auto& r = prefs[i];
        const long long x0 = static_cast<long long>(std::floor(r.mn.x / kCell)), x1 = static_cast<long long>(std::floor(r.mx.x / kCell));
        const long long y0 = static_cast<long long>(std::floor(r.mn.y / kCell)), y1 = static_cast<long long>(std::floor(r.mx.y / kCell));
        const long long z0 = static_cast<long long>(std::floor(r.mn.z / kCell)), z1 = static_cast<long long>(std::floor(r.mx.z / kCell));
        if ((x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > kMaxCells) { big.push_back(i); continue; }
        for (long long x = x0; x <= x1; ++x)
            for (long long y = y0; y <= y1; ++y)
                for (long long z = z0; z <= z1; ++z) grid[cellKey(x, y, z)].push_back(i);
    }
    std::set<uint64_t> seenPairs;
    std::vector<std::pair<int, int>> pairs;
    auto consider = [&](int a, int b) {
        if (a == b) return;
        if (a > b) std::swap(a, b);
        const auto &ra = prefs[a], &rb = prefs[b];
        if (ra.face == rb.face || faces[ra.face].group != faces[rb.face].group) return;
        if (ra.mn.x > rb.mx.x || rb.mn.x > ra.mx.x || ra.mn.y > rb.mx.y || rb.mn.y > ra.mx.y || ra.mn.z > rb.mx.z || rb.mn.z > ra.mx.z) return;
        if (seenPairs.insert((static_cast<uint64_t>(a) << 32) | static_cast<uint32_t>(b)).second) pairs.push_back({a, b});
    };
    for (const auto& kv : grid)
        for (size_t i = 0; i < kv.second.size(); ++i)
            for (size_t j = i + 1; j < kv.second.size(); ++j) consider(kv.second[i], kv.second[j]);
    for (int b : big)
        for (int i = 0; i < static_cast<int>(prefs.size()); ++i) consider(b, i);

    struct Shared {
        Vec3 p, q;  // in the traversal direction of the lower face index
        double len = 0;
    };
    std::map<std::pair<int, int>, Shared> shared;
    for (const auto& pr : pairs) {
        const PieceRef &ra = prefs[pr.first], &rb = prefs[pr.second];
        const auto& A = faces[ra.face].pieces[ra.piece];
        const auto& B = faces[rb.face].pieces[rb.piece];
        for (size_t i = 0; i < A.size(); ++i) {
            const Vec3 a0 = A[i], a1 = A[(i + 1) % A.size()];
            const Vec3 d = a1 - a0;
            const double len = d.length();
            if (len < cfg.edgeEps) continue;
            const Vec3 du = d * (1.0 / len);
            for (size_t j = 0; j < B.size(); ++j) {
                const Vec3 b0 = B[j], b1 = B[(j + 1) % B.size()];
                const Vec3 e = b1 - b0;
                const double elen = e.length();
                if (elen < cfg.edgeEps) continue;
                if (du.cross(e * (1.0 / elen)).length() > 1e-5) continue;  // not parallel
                if ((b0 - a0).cross(du).length() > cfg.edgeEps || (b1 - a0).cross(du).length() > cfg.edgeEps) continue;  // not collinear
                const double t0 = (b0 - a0).dot(du), t1 = (b1 - a0).dot(du);
                const double lo = std::max(0.0, std::min(t0, t1)), hi = std::min(len, std::max(t0, t1));
                if (hi - lo <= cfg.edgeEps) continue;
                // direction of the piece of face ra.face; normalise to "traversal of the lower face index"
                const std::pair<int, int> key{std::min(ra.face, rb.face), std::max(ra.face, rb.face)};
                Shared s{a0 + du * lo, a0 + du * hi, hi - lo};
                if (ra.face > rb.face) std::swap(s.p, s.q);
                auto it = shared.find(key);
                if (it == shared.end() || it->second.len < s.len) shared[key] = s;
            }
        }
    }
    std::vector<std::vector<Adj>> adj(N);
    for (const auto& kv : shared) {
        const int lo = kv.first.first, hi = kv.first.second;
        adj[lo].push_back({hi, kv.second.p, kv.second.q});
        adj[hi].push_back({lo, kv.second.q, kv.second.p});
    }
    for (auto& list : adj)
        std::sort(list.begin(), list.end(), [&](const Adj& a, const Adj& b) { return faces[a.other].out.key() < faces[b.other].out.key(); });

    std::set<std::pair<int, int>> adjacentFaces;  // faces that share an edge in 3D (or, on a smoothed brush, a corner)
    for (int i = 0; i < N; ++i)
        for (const Adj& a : adj[i]) adjacentFaces.insert({i, a.other});
    // Faces of one smoothed brush that share a corner (REQUIREMENTS §34 L1): their light is the same at that corner (smooth normals), so in a chart they
    // may touch there like edge neighbours instead of keeping the isolation gap (a sphere's diagonal quads and the triangles round a pole).
    {
        std::map<std::tuple<const BrushSmoothing*, long long, long long, long long>, std::vector<int>> corners;
        for (int i = 0; i < N; ++i) {
            if (!faces[i].smooth) continue;
            std::set<std::tuple<long long, long long, long long>> mine;
            for (const auto& piece : faces[i].pieces)
                for (const auto& p : piece) mine.insert({std::llround(p.x / cfg.edgeEps), std::llround(p.y / cfg.edgeEps), std::llround(p.z / cfg.edgeEps)});
            for (const auto& [x, y, z] : mine) corners[{faces[i].smooth, x, y, z}].push_back(i);
        }
        for (const auto& kv : corners)
            for (int a : kv.second)
                for (int b : kv.second) if (a != b) adjacentFaces.insert({a, b});
    }
    // Faces of one island that are NOT joined by a shared edge must stay this many texels apart (and keep their own gutter), otherwise
    // bilinear filtering at the edge of one reads texels of the other: light leaks, dark / bright edges at corners.
    const double isolationGap = 2.0 * pad + 1.0;

    // ---- C. islands (unfold connected faces) ------------------------------------------------------------------------
    const double cosLimit = std::cos(std::min(180.0, std::max(0.0, cfg.mergeAngleDeg)) * kPi / 180.0) - 1e-6;  // tolerance ~0.06 deg
    std::vector<int> assigned(N, -1);
    std::vector<Island> islands;

    std::vector<int> order(N);
    for (int i = 0; i < N; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return faces[a].out.area > faces[b].out.area; });

    auto polysOf = [&](const Island& isl, int face, const M3& M, const Vec3& b) {
        std::vector<std::vector<P2>> res;
        for (const auto& piece : faces[face].pieces) {
            std::vector<P2> poly;
            for (const auto& p : piece) {
                const Vec3 q = M.mul(p) + b;
                poly.push_back({q.dot(isl.u) * isl.k, q.dot(isl.v) * isl.k});
            }
            res.push_back(std::move(poly));
        }
        return res;
    };
    auto boxOf = [](const std::vector<P2>& poly) {
        std::array<double, 4> bx{1e300, 1e300, -1e300, -1e300};
        for (const auto& p : poly) { bx[0] = std::min(bx[0], p.s); bx[1] = std::min(bx[1], p.t); bx[2] = std::max(bx[2], p.s); bx[3] = std::max(bx[3], p.t); }
        return bx;
    };

    for (int rootIdx : order) {
        if (assigned[rootIdx] >= 0) continue;
        Island isl;
        isl.root = rootIdx;
        isl.u = faces[rootIdx].u;
        isl.v = faces[rootIdx].v;
        isl.k = cfg.texelsPerMeter * faces[rootIdx].scale;
        const int islandIndex = static_cast<int>(islands.size());
        auto addMember = [&](int face, const M3& M, const Vec3& b) {
            isl.members.push_back({face, M, b});
            assigned[face] = islandIndex;
            for (auto& poly : polysOf(isl, face, M, b)) {
                const auto bx = boxOf(poly);
                isl.minS = std::min(isl.minS, bx[0]); isl.minT = std::min(isl.minT, bx[1]);
                isl.maxS = std::max(isl.maxS, bx[2]); isl.maxT = std::max(isl.maxT, bx[3]);
                isl.polyBox.push_back(bx);
                isl.polyFace.push_back(face);
                isl.polys.push_back(std::move(poly));
            }
        };
        addMember(rootIdx, M3{}, Vec3{});

        // Best-first growth (REQUIREMENTS §34 L1): the next face to unfold is the one whose normal is closest to the root's. Coplanar neighbours come
        // first exactly as before; a smooth round surface (faces joined by smooth edges at any angle) grows as a compact cap around the root instead of
        // breadth-first rings, so unfolding runs into overlaps later and the surface needs fewer, larger charts (fewer seams in its light).
        struct Cand { double dot; int face; size_t member; size_t adj; };
        auto worse = [](const Cand& x, const Cand& y) { return x.dot != y.dot ? x.dot < y.dot : x.face != y.face ? x.face > y.face : x.member > y.member; };
        std::priority_queue<Cand, std::vector<Cand>, decltype(worse)> queue(worse);
        const Vec3 rootNormal = faces[rootIdx].out.normal;
        auto offer = [&](size_t member) {
            const auto& list = adj[isl.members[member].face];
            for (size_t k = 0; k < list.size(); ++k)
                if (assigned[list[k].other] < 0) queue.push({faces[list[k].other].out.normal.dot(rootNormal), list[k].other, member, k});
        };
        offer(0);
        while (!queue.empty()) {
            const Cand cand = queue.top();
            queue.pop();
            const Member parent = isl.members[cand.member];
            const Face& pf = faces[parent.face];
            {
                const Adj& a = adj[parent.face][cand.adj];
                if (assigned[a.other] >= 0) continue;
                const Face& cf = faces[a.other];
                if (std::fabs(cf.scale - pf.scale) > 1e-9) continue;
                if (cf.styles != pf.styles) continue;  // as few style layers per chart as possible: never merge faces that different styled lights touch
                // faces joined by a smooth edge share one chart at any angle: their light is continuous across it (REQUIREMENTS §33 H6)
                const bool smoothJoin = pf.smooth && pf.smooth == cf.smooth && pf.smooth->onSmoothEdge(a.p, a.q, cfg.edgeEps * 2);
                if (!smoothJoin && cf.out.normal.dot(pf.out.normal) < cosLimit) continue;
                // unfold: rotate the child about the shared edge into the parent's (already transformed) plane
                const Vec3 np = parent.M.mul(pf.out.normal);
                Vec3 ep = parent.M.mul((a.q - a.p).normalized());
                ep = (ep - np * ep.dot(np)).normalized();
                const Vec3 nc = cf.out.normal;
                Vec3 ec = (a.q - a.p).normalized();
                ec = (ec - nc * ec.dot(nc)).normalized();
                if (ep.length() < 0.5 || ec.length() < 0.5) continue;
                const Vec3 wc = nc.cross(ec * -1.0), wp = np.cross(ep * -1.0);
                auto rot = [&](const Vec3& x) { return ep * x.dot(ec) + np * x.dot(nc) + wp * x.dot(wc); };
                M3 R{rot({1, 0, 0}), rot({0, 1, 0}), rot({0, 0, 1})};
                const Vec3 aT = parent.M.mul(a.p) + parent.b;
                const Vec3 bT = aT - R.mul(a.p);

                // overlap + size checks on a scratch copy
                const auto newPolys = polysOf(isl, a.other, R, bT);
                bool ok = true;
                double minS = isl.minS, maxS = isl.maxS, minT = isl.minT, maxT = isl.maxT;
                std::vector<std::array<double, 4>> newBoxes;
                for (const auto& poly : newPolys) {
                    const auto bx = boxOf(poly);
                    newBoxes.push_back(bx);
                    minS = std::min(minS, bx[0]); minT = std::min(minT, bx[1]); maxS = std::max(maxS, bx[2]); maxT = std::max(maxT, bx[3]);
                }
                if (std::ceil(maxS) - std::floor(minS) > sideLimit || std::ceil(maxT) - std::floor(minT) > sideLimit) ok = false;
                constexpr double kEps = 1e-3;  // texels
                for (size_t n = 0; ok && n < newPolys.size(); ++n)
                    for (size_t o = 0; ok && o < isl.polys.size(); ++o) {
                        const auto &x = newBoxes[n], &y = isl.polyBox[o];
                        const bool joined = isl.polyFace[o] == a.other || adjacentFaces.count({a.other, isl.polyFace[o]}) > 0;
                        const double gap = joined ? 0.0 : isolationGap;  // joined faces may touch along their shared edge
                        if (x[0] >= y[2] + gap - kEps || y[0] >= x[2] + gap - kEps || x[1] >= y[3] + gap - kEps || y[1] >= x[3] + gap - kEps) continue;
                        if (overlaps(newPolys[n], isl.polys[o], joined ? kEps : -gap)) ok = false;
                    }
                if (!ok) continue;
                addMember(a.other, R, bT);
                offer(isl.members.size() - 1);
            }
        }
        islands.push_back(std::move(isl));
    }

    // ---- D. chart rectangles ----------------------------------------------------------------------------------------
    for (auto& isl : islands) {
        auto extent = [&] {
            isl.originS = static_cast<int>(std::floor(isl.minS));
            isl.originT = static_cast<int>(std::floor(isl.minT));
            isl.w = std::max(1, static_cast<int>(std::ceil(isl.maxS)) - isl.originS);
            isl.h = std::max(1, static_cast<int>(std::ceil(isl.maxT)) - isl.originT);
        };
        extent();
        if (isl.w > sideLimit || isl.h > sideLimit) {
            // a single face that is too large (merging never produces this): shrink density until it fits
            double f = std::min(static_cast<double>(sideLimit) / isl.w, static_cast<double>(sideLimit) / isl.h);
            for (int guard = 0; guard < 64; ++guard) {
                const double minS = isl.minS * f, maxS = isl.maxS * f, minT = isl.minT * f, maxT = isl.maxT * f;
                if (std::ceil(maxS) - std::floor(minS) <= sideLimit && std::ceil(maxT) - std::floor(minT) <= sideLimit) break;
                f *= 0.99;
            }
            isl.shrink = f;
            isl.k *= f;
            isl.minS *= f; isl.maxS *= f; isl.minT *= f; isl.maxT *= f;
            extent();
            out.warnings.push_back("face " + faces[isl.root].out.key() + " exceeds the chart size limit: density reduced to " +
                                   std::to_string(isl.k) + " texels/m");
        }
        isl.w += 2 * pad;
        isl.h += 2 * pad;
        // texture, centre, rootKey
        double bestArea = -1;
        Vec3 mn{1e300, 1e300, 1e300}, mx{-1e300, -1e300, -1e300};
        for (const auto& m : isl.members) {
            const Face& f = faces[m.face];
            if (f.out.area > bestArea) { bestArea = f.out.area; isl.texture = f.out.texture; }
            mn = {std::min(mn.x, f.mn.x), std::min(mn.y, f.mn.y), std::min(mn.z, f.mn.z)};
            mx = {std::max(mx.x, f.mx.x), std::max(mx.y, f.mx.y), std::max(mx.z, f.mx.z)};
        }
        isl.center = (mn + mx) * 0.5;
        isl.rootKey = faces[isl.root].out.key();
        isl.bucket = faces[isl.root].group + "|" + isl.texture;
    }

    // ---- E. packing: buckets (instance, entity, texture) in order, Morton order inside a bucket -----------------------
    Vec3 gmn{1e300, 1e300, 1e300}, gmx{-1e300, -1e300, -1e300};
    for (const auto& f : faces) {
        gmn = {std::min(gmn.x, f.mn.x), std::min(gmn.y, f.mn.y), std::min(gmn.z, f.mn.z)};
        gmx = {std::max(gmx.x, f.mx.x), std::max(gmx.y, f.mx.y), std::max(gmx.z, f.mx.z)};
    }
    auto q = [&](double v, double lo, double hi) {
        return hi - lo < 1e-9 ? 0u : static_cast<uint32_t>(std::min(1023.0, std::max(0.0, (v - lo) / (hi - lo) * 1023.0)));
    };
    for (auto& isl : islands)
        isl.morton = part1by2(q(isl.center.x, gmn.x, gmx.x)) | (part1by2(q(isl.center.y, gmn.y, gmx.y)) << 1) |
                     (part1by2(q(isl.center.z, gmn.z, gmx.z)) << 2);

    std::map<std::string, std::vector<int>> buckets;
    for (int i = 0; i < static_cast<int>(islands.size()); ++i) buckets[islands[i].bucket].push_back(i);
    std::vector<Page> pages;
    std::vector<std::array<int, 3>> placement(islands.size(), {-1, 0, 0});  // page, x, y
    for (auto& kv : buckets) {
        auto& list = kv.second;
        std::sort(list.begin(), list.end(), [&](int a, int b) {
            if (islands[a].morton != islands[b].morton) return islands[a].morton < islands[b].morton;
            return islands[a].rootKey < islands[b].rootKey;
        });
        auto tryAll = [&](Page& page, std::vector<std::array<int, 3>>& res) {
            for (int idx : list) {
                const auto r = page.place(islands[idx].w, islands[idx].h);
                if (!r) return false;
                res.push_back({idx, r->x, r->y});
            }
            return true;
        };
        std::vector<std::array<int, 3>> res;
        // 1. the whole bucket on one existing page (newest first), so holes left by earlier buckets get used
        bool done = false;
        for (int pi = static_cast<int>(pages.size()) - 1; pi >= 0 && !done; --pi) {
            Page trial = pages[static_cast<size_t>(pi)];
            res.clear();
            if (tryAll(trial, res)) {
                pages[static_cast<size_t>(pi)] = std::move(trial);
                for (auto& r : res) placement[static_cast<size_t>(r[0])] = {pi, r[1], r[2]};
                done = true;
            }
        }
        if (done) continue;
        // 2. the whole bucket on a new page
        Page fresh(atlas);
        res.clear();
        if (tryAll(fresh, res)) {
            pages.push_back(std::move(fresh));
            for (auto& r : res) placement[static_cast<size_t>(r[0])] = {static_cast<int>(pages.size()) - 1, r[1], r[2]};
            continue;
        }
        // 3. a bucket larger than a page: chart by chart, first page with room, else a new page
        for (int idx : list) {
            std::optional<Rect> r;
            int where = -1;
            for (int pi = static_cast<int>(pages.size()) - 1; pi >= 0 && !r; --pi) {
                r = pages[static_cast<size_t>(pi)].place(islands[static_cast<size_t>(idx)].w, islands[static_cast<size_t>(idx)].h);
                if (r) where = pi;
            }
            if (!r) {
                pages.emplace_back(atlas);
                where = static_cast<int>(pages.size()) - 1;
                r = pages.back().place(islands[static_cast<size_t>(idx)].w, islands[static_cast<size_t>(idx)].h);
            }
            placement[static_cast<size_t>(idx)] = {where, r->x, r->y};
        }
    }
    if (islands.empty()) pages.clear();

    // ---- F. write the result ----------------------------------------------------------------------------------------
    out.pageCount = static_cast<int>(pages.size());
    for (const auto& p : pages) out.pageFill.push_back(static_cast<double>(p.used) / (static_cast<double>(atlas) * atlas));
    std::map<std::string, int> faceIndexByKey;
    for (int i = 0; i < N; ++i) faceIndexByKey[faces[i].out.key()] = i;
    out.faces.resize(N);
    for (int i = 0; i < N; ++i) out.faces[i] = faces[i].out;
    for (int ci = 0; ci < static_cast<int>(islands.size()); ++ci) {
        const Island& isl = islands[ci];
        LightmapChart c;
        c.page = placement[ci][0];
        c.x = placement[ci][1];
        c.y = placement[ci][2];
        c.w = isl.w;
        c.h = isl.h;
        c.instance = faces[isl.root].out.instance;
        c.entityId = faces[isl.root].out.entityId;
        c.texture = isl.texture;
        c.center = isl.center;
        c.texelsPerMeter = isl.k;
        c.styles = {0};
        c.styles.insert(c.styles.end(), faces[isl.root].styles.begin(), faces[isl.root].styles.end());
        for (const auto& m : isl.members) {
            LightmapFace& f = out.faces[m.face];
            f.chart = ci;
            c.faces.push_back(m.face);
            // s = (M p + b).u * k - originS + pad + chart.x
            f.su = Vec3{m.M.c0.dot(isl.u), m.M.c1.dot(isl.u), m.M.c2.dot(isl.u)} * isl.k;
            f.sv = Vec3{m.M.c0.dot(isl.v), m.M.c1.dot(isl.v), m.M.c2.dot(isl.v)} * isl.k;
            f.so = m.b.dot(isl.u) * isl.k - isl.originS + pad + c.x;
            f.to = m.b.dot(isl.v) * isl.k - isl.originT + pad + c.y;
            f.texelsPerMeter = isl.k;
        }
        std::sort(c.faces.begin(), c.faces.end());
        out.charts.push_back(std::move(c));
    }
    // charts remember their page-local rect; the page index is stored on the chart, faces address the page through it
    return out;
}


LightmapLayout layoutForStyle(const LightmapLayout& layout, int style) {
    LightmapLayout out;
    out.settings = layout.settings;
    out.pageCount = layout.pageCount;
    out.pageFill = layout.pageFill;
    out.warnings = layout.warnings;
    std::vector<int> chartMap(layout.charts.size(), -1);
    for (size_t c = 0; c < layout.charts.size(); ++c) {
        const auto& st = layout.charts[c].styles;
        if (std::find(st.begin(), st.end(), style) == st.end()) continue;
        chartMap[c] = static_cast<int>(out.charts.size());
        out.charts.push_back(layout.charts[c]);
    }
    std::vector<int> faceMap(layout.faces.size(), -1);
    for (size_t f = 0; f < layout.faces.size(); ++f) {
        const int c = layout.faces[f].chart;
        if (c < 0 || chartMap[static_cast<size_t>(c)] < 0) continue;
        faceMap[f] = static_cast<int>(out.faces.size());
        out.faces.push_back(layout.faces[f]);
        out.faces.back().chart = chartMap[static_cast<size_t>(c)];
    }
    for (auto& c : out.charts) {
        std::vector<int> fs;
        for (int f : c.faces) if (faceMap[static_cast<size_t>(f)] >= 0) fs.push_back(faceMap[static_cast<size_t>(f)]);
        c.faces = std::move(fs);
    }
    return out;
}

}  // namespace core
