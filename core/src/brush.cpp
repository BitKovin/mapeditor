#include "core/brush.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace core {

Brush Brush::makeBox(const BrushId& id, const Vec3& mins, const Vec3& maxs, const std::string& texture) {
    Brush b;
    b.id = id;
    b.vertices = {
        {mins.x, mins.y, mins.z}, {maxs.x, mins.y, mins.z}, {maxs.x, maxs.y, mins.z},
        {mins.x, maxs.y, mins.z}, {mins.x, mins.y, maxs.z}, {maxs.x, mins.y, maxs.z},
        {maxs.x, maxs.y, maxs.z}, {mins.x, maxs.y, maxs.z},
    };
    // -Z, +Z, -X, +X, -Y, +Y ; each loop is CCW seen from outside.
    const uint32_t loops[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 4, 7, 3},
                                  {1, 2, 6, 5}, {0, 1, 5, 4}, {3, 7, 6, 2}};
    for (uint32_t i = 0; i < 6; ++i) {
        BrushFace f;
        f.id = i;
        f.loop.assign(loops[i], loops[i] + 4);
        f.texture = texture;
        b.faces.push_back(std::move(f));
    }
    return b;
}

bool Brush::bounds(Vec3& outMin, Vec3& outMax) const {
    constexpr double kInf = std::numeric_limits<double>::infinity();
    Vec3 mn{kInf, kInf, kInf}, mx{-kInf, -kInf, -kInf};
    bool any = false;
    for (const auto& f : faces) {
        for (uint32_t idx : f.loop) {
            if (idx >= vertices.size()) continue;
            const Vec3& p = vertices[idx];
            any = true;
            mn = {std::min(mn.x, p.x), std::min(mn.y, p.y), std::min(mn.z, p.z)};
            mx = {std::max(mx.x, p.x), std::max(mx.y, p.y), std::max(mx.z, p.z)};
        }
    }
    if (!any) return false;
    outMin = mn;
    outMax = mx;
    return true;
}

void Brush::translate(const Vec3& delta) {
    for (auto& v : vertices) v = v + delta;
}

Vec3 Brush::faceNormal(const BrushFace& face) const {
    // Newell / vector-area normal, taken relative to the first vertex to keep
    // the cross products small (better precision far from the origin).
    const Vec3& p0 = vertices[face.loop[0]];
    Vec3 sum;
    for (size_t i = 1; i + 1 < face.loop.size(); ++i) {
        sum = sum + (vertices[face.loop[i]] - p0).cross(vertices[face.loop[i + 1]] - p0);
    }
    return sum.normalized();
}

double Brush::signedVolume() const {
    double vol = 0;
    for (const auto& f : faces) {
        const Vec3& a = vertices[f.loop[0]];
        for (size_t i = 1; i + 1 < f.loop.size(); ++i) {
            vol += a.dot(vertices[f.loop[i]].cross(vertices[f.loop[i + 1]])) / 6.0;
        }
    }
    return vol;
}

std::optional<std::string> Brush::checkStructure(double epsilon) const {
    for (size_t i = 0; i < vertices.size(); ++i) {
        const Vec3& v = vertices[i];
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
            return "vertex " + std::to_string(i) + " has a non-finite coordinate";
    }
    if (faces.empty()) return "brush has no faces";

    std::set<uint32_t> faceIds;
    for (const auto& f : faces) {
        const std::string where = "face " + std::to_string(f.id);
        if (!faceIds.insert(f.id).second) return "duplicate face id " + std::to_string(f.id);
        if (f.loop.size() < 3) return where + " has fewer than 3 vertices";
        std::set<uint32_t> seen;
        for (uint32_t idx : f.loop) {
            if (idx >= vertices.size())
                return where + " references vertex " + std::to_string(idx) + " but the brush only has " +
                       std::to_string(vertices.size());
            if (!seen.insert(idx).second) return where + " uses vertex " + std::to_string(idx) + " twice";
        }
        if (!f.uvs.empty() && f.uvs.size() != f.loop.size())
            return where + " has " + std::to_string(f.uvs.size()) + " uvs for " +
                   std::to_string(f.loop.size()) + " corners";
    }

    // Second pass (indices are known good now): non-degenerate and planar.
    for (const auto& f : faces) {
        const Vec3 n = faceNormal(f);
        if (n.length() < 0.5) return "face " + std::to_string(f.id) + " is degenerate (zero area)";
        const Vec3& p0 = vertices[f.loop[0]];
        for (uint32_t idx : f.loop) {
            const double off = std::fabs(n.dot(vertices[idx] - p0));
            if (off > epsilon)
                return "face " + std::to_string(f.id) + " is not planar (vertex " + std::to_string(idx) + " is " +
                       std::to_string(off) + " m off the plane); split it into planar pieces";
        }
    }
    return std::nullopt;
}

std::optional<std::string> Brush::checkConvexSolid(double epsilon) const {
    if (auto err = checkStructure(epsilon)) return err;
    if (faces.size() < 4) return "a solid needs at least 4 faces";

    // Closed 2-manifold: every directed edge appears exactly once and its
    // reverse appears exactly once.
    std::map<std::pair<uint32_t, uint32_t>, int> edges;
    for (const auto& f : faces) {
        for (size_t i = 0; i < f.loop.size(); ++i) {
            ++edges[{f.loop[i], f.loop[(i + 1) % f.loop.size()]}];
        }
    }
    for (const auto& [edge, count] : edges) {
        if (count != 1)
            return "edge " + std::to_string(edge.first) + "->" + std::to_string(edge.second) +
                   " is used twice in the same direction (inconsistent winding or non-manifold)";
        if (!edges.count({edge.second, edge.first}))
            return "mesh is not closed (open edge between vertices " + std::to_string(edge.first) +
                   " and " + std::to_string(edge.second) + ")";
    }

    const double vol = signedVolume();
    if (vol < 0) return "faces are wound inward (negative volume)";
    if (vol < 1e-12) return "brush is degenerate (zero volume)";

    // Every referenced vertex must lie on or behind every face's plane
    // (faces are already known planar and non-degenerate). That is "convex".
    std::set<uint32_t> used;
    for (const auto& f : faces) used.insert(f.loop.begin(), f.loop.end());

    for (const auto& f : faces) {
        const Vec3 n = faceNormal(f);
        const double d = n.dot(vertices[f.loop[0]]);
        for (uint32_t idx : used) {
            if (n.dot(vertices[idx]) - d > epsilon)
                return "brush is not convex (vertex " + std::to_string(idx) + " lies in front of face " +
                       std::to_string(f.id) + ")";
        }
    }
    return std::nullopt;
}

bool faceUvMap(const Brush& b, const BrushFace& f, UvMap& out) {
    const size_t k = f.loop.size();
    if (k < 3 || f.uvs.size() != k) return false;
    for (uint32_t i : f.loop) if (i >= b.vertices.size()) return false;
    const Vec3 n = b.faceNormal(f);
    if (n.length() < 0.5) return false;
    const Vec3 helper = std::fabs(n.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    const Vec3 t = n.cross(helper).normalized(), bt = n.cross(t);
    // least squares  w = c0 + c1 x + c2 y  over the corners (x, y = coordinates in the face plane), for w = u and w = v
    const Vec3 o = b.vertices[f.loop[0]];
    double m[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}, ru[3] = {0, 0, 0}, rv[3] = {0, 0, 0};
    for (size_t i = 0; i < k; ++i) {
        const Vec3 d = b.vertices[f.loop[i]] - o;
        const double row[3] = {1, d.dot(t), d.dot(bt)};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) m[r][c] += row[r] * row[c];
            ru[r] += row[r] * f.uvs[i].u;
            rv[r] += row[r] * f.uvs[i].v;
        }
    }
    const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (std::fabs(det) < 1e-12) return false;
    auto solve = [&](const double r[3], double x[3]) {  // Cramer
        for (int c = 0; c < 3; ++c) {
            double a[3][3];
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) a[i][j] = j == c ? r[i] : m[i][j];
            x[c] = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0])) / det;
        }
    };
    double cu[3], cv[3];
    solve(ru, cu);
    solve(rv, cv);
    // back to world space: w(p) = c0 + c1 (p - o).t + c2 (p - o).bt
    const Vec3 gu = t * cu[1] + bt * cu[2], gv = t * cv[1] + bt * cv[2];
    out.u[0] = gu.x; out.u[1] = gu.y; out.u[2] = gu.z; out.u[3] = cu[0] - gu.dot(o);
    out.v[0] = gv.x; out.v[1] = gv.y; out.v[2] = gv.z; out.v[3] = cv[0] - gv.dot(o);
    return true;
}

Vec2 projectAlbedoUv(const Vec3& n, const Vec3& p, const FaceProjection& proj) {
    const double ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
    auto sgn = [](double v) { const double w = v != 0 ? v : 1.0; return w < 0 ? -1.0 : 1.0; };
    double a, b;
    if (ay >= ax && ay >= az) { a = sgn(n.y) * p.x; b = p.z; }           // floor / ceiling
    else if (ax >= az) { a = -sgn(n.x) * p.z; b = -p.y; }                 // X facing walls
    else { a = sgn(n.z) * p.x; b = -p.y; }                                // Z facing walls
    const double r = proj.rotate * 3.14159265358979323846 / 180.0, c = std::cos(r), s = std::sin(r);
    const double ra = a * c - b * s, rb = a * s + b * c;
    return {ra / proj.scaleU + proj.shiftU, rb / proj.scaleV + proj.shiftV};
}

}  // namespace core
