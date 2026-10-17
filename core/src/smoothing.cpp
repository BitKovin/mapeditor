#include "core/smoothing.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>

namespace core {

bool BrushSmoothing::onSmoothEdge(const Vec3& p, const Vec3& q, double eps) const {
    for (const auto& [a, b] : edges) {
        const Vec3 d = b - a;
        const double len = d.length();
        if (len < 1e-12) continue;
        const Vec3 u = d * (1.0 / len);
        auto onLine = [&](const Vec3& x) {
            const Vec3 r = x - a;
            const double t = r.dot(u);
            return t >= -eps && t <= len + eps && (r - u * t).length() <= eps;
        };
        if (onLine(p) && onLine(q)) return true;
    }
    return false;
}

std::shared_ptr<const BrushSmoothing> brushSmoothing(const Brush& b) {
    if (b.smoothEdges.empty()) return nullptr;
    const uint32_t nv = static_cast<uint32_t>(b.vertices.size());
    auto key = [](uint32_t x, uint32_t y) { return x < y ? std::make_pair(x, y) : std::make_pair(y, x); };
    auto edgesOf = [&](const BrushFace& f) {
        std::set<std::pair<uint32_t, uint32_t>> out;
        for (size_t i = 0; i < f.loop.size(); ++i) {
            const uint32_t x = f.loop[i], y = f.loop[(i + 1) % f.loop.size()];
            if (x < nv && y < nv && x != y) out.insert(key(x, y));
        }
        return out;
    };
    // a marked face pair smooths every edge the two faces share (none when they no longer meet)
    std::set<std::pair<uint32_t, uint32_t>> smooth;
    for (const auto& [fa, fb] : b.smoothEdges) {
        if (fa == fb) continue;
        const BrushFace *A = nullptr, *B = nullptr;
        for (const auto& f : b.faces) { if (f.id == fa) A = &f; if (f.id == fb) B = &f; }
        if (!A || !B) continue;
        const auto ea = edgesOf(*A), eb = edgesOf(*B);
        for (const auto& e : ea) if (eb.count(e)) smooth.insert(e);
    }
    if (smooth.empty()) return nullptr;
    if (b.checkStructure()) return nullptr;  // indices / planarity broken: no smoothing rather than wrong normals

    auto out = std::make_shared<BrushSmoothing>();
    for (const auto& [x, y] : smooth) out->edges.push_back({b.vertices[x], b.vertices[y]});

    const size_t nf = b.faces.size();
    std::vector<Vec3> fn(nf);
    for (size_t i = 0; i < nf; ++i) fn[i] = b.faceNormal(b.faces[i]);
    // corners of each vertex: (face, corner index)
    std::vector<std::vector<std::pair<size_t, size_t>>> at(nv);
    for (size_t i = 0; i < nf; ++i)
        for (size_t c = 0; c < b.faces[i].loop.size(); ++c) at[b.faces[i].loop[c]].push_back({i, c});
    auto cornerAngle = [&](size_t face, size_t c) {
        const auto& L = b.faces[face].loop;
        const size_t n = L.size();
        const Vec3 p = b.vertices[L[c]], a = b.vertices[L[(c + n - 1) % n]] - p, q = b.vertices[L[(c + 1) % n]] - p;
        const double la = a.length(), lq = q.length();
        if (la < 1e-12 || lq < 1e-12) return 0.0;
        return std::acos(std::clamp(a.dot(q) / (la * lq), -1.0, 1.0));
    };
    auto hasEdge = [&](size_t face, uint32_t x, uint32_t y) {
        const auto& L = b.faces[face].loop;
        for (size_t i = 0; i < L.size(); ++i) {
            const uint32_t p = L[i], q = L[(i + 1) % L.size()];
            if ((p == x && q == y) || (p == y && q == x)) return true;
        }
        return false;
    };
    std::vector<std::vector<Vec3>> cornerNormal(nf);
    for (size_t i = 0; i < nf; ++i) cornerNormal[i].assign(b.faces[i].loop.size(), fn[i]);
    for (uint32_t v = 0; v < nv; ++v) {
        const auto& list = at[v];
        if (list.size() < 2) continue;
        // faces around v joined through smooth edges at v: union-find over the corner list
        std::vector<size_t> parent(list.size());
        std::iota(parent.begin(), parent.end(), 0);
        auto find = [&](size_t x) { while (parent[x] != x) x = parent[x] = parent[parent[x]]; return x; };
        bool any = false;
        for (const auto& [x, y] : smooth) {
            if (x != v && y != v) continue;
            const uint32_t w = x == v ? y : x;
            size_t first = SIZE_MAX;
            for (size_t k = 0; k < list.size(); ++k) {
                if (!hasEdge(list[k].first, v, w)) continue;
                if (first == SIZE_MAX) first = k;
                else { parent[find(k)] = find(first); any = true; }
            }
        }
        if (!any) continue;
        std::vector<Vec3> sum(list.size());
        for (size_t k = 0; k < list.size(); ++k) sum[find(k)] = sum[find(k)] + fn[list[k].first] * cornerAngle(list[k].first, list[k].second);
        for (size_t k = 0; k < list.size(); ++k) {
            const Vec3 s = sum[find(k)];
            if (s.length() > 1e-12) cornerNormal[list[k].first][list[k].second] = s.normalized();
        }
    }
    for (size_t i = 0; i < nf; ++i) {
        bool differs = false;
        for (const Vec3& n : cornerNormal[i]) if ((n - fn[i]).length() > 1e-6) { differs = true; break; }
        if (!differs) continue;
        FaceShading s;
        for (uint32_t v : b.faces[i].loop) s.corners.push_back(b.vertices[v]);
        s.normals = cornerNormal[i];
        uint64_t h = 1469598103934665603ull;
        for (const Vec3& n : s.normals)
            for (double d : {n.x, n.y, n.z}) { h ^= static_cast<uint64_t>(std::llround(d * 1e6)) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); h *= 0x100000001B3ull; }
        s.hash = h | 1;  // never 0 (0 = not smoothed)
        out->faces.emplace(b.faces[i].id, std::move(s));
    }
    return out;
}

Vec3 shadingNormalAt(const FaceShading& s, const Vec3& p, const Vec3& fallback) {
    const size_t n = s.corners.size();
    if (n < 3 || s.normals.size() != n) return fallback;
    std::vector<Vec3> d(n);
    std::vector<double> r(n), t(n);
    for (size_t i = 0; i < n; ++i) {
        d[i] = s.corners[i] - p;
        r[i] = d[i].length();
        if (r[i] < 1e-9) return s.normals[i];
    }
    for (size_t i = 0; i < n; ++i) {
        const size_t j = (i + 1) % n;
        const double cr = d[i].cross(d[j]).length(), dt = d[i].dot(d[j]);
        if (cr <= 1e-12 * r[i] * r[j] && dt < 0) {  // on the edge i-j: linear along it
            const double k = r[i] / (r[i] + r[j]);
            const Vec3 m = s.normals[i] * (1 - k) + s.normals[j] * k;
            return m.length() > 1e-12 ? m.normalized() : fallback;
        }
        const double den = r[i] * r[j] + dt;
        t[i] = den > 1e-300 ? cr / den : 0;  // tan(angle / 2)
    }
    Vec3 sum;
    double wsum = 0;
    for (size_t i = 0; i < n; ++i) {
        const double w = (t[(i + n - 1) % n] + t[i]) / r[i];
        sum = sum + s.normals[i] * w;
        wsum += w;
    }
    if (!(wsum > 0) || sum.length() < 1e-12) return fallback;
    const Vec3 m = sum.normalized();
    return m.dot(fallback) > 0 ? m : fallback;  // never turn a sample away from its own face
}

}  // namespace core
