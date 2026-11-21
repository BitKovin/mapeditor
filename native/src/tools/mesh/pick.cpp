#include "tools/mesh/pick.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace native::tools::mesh {

using core::Brush;
using core::BrushFace;
using core::Vec3;

namespace {

// Moeller-Trumbore, as render/picking.ts rayTriangle: the distance along the ray, or a negative number for a miss.
double rayTriangle(const Ray& r, const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 e1 = b - a, e2 = c - a;
    const Vec3 p = r.dir.cross(e2);
    const double det = e1.dot(p);
    if (std::abs(det) < 1e-12) return -1;
    const double inv = 1 / det;
    const Vec3 tv = r.origin - a;
    const double u = tv.dot(p) * inv;
    if (u < 0 || u > 1) return -1;
    const Vec3 q = tv.cross(e1);
    const double v = r.dir.dot(q) * inv;
    if (v < 0 || u + v > 1) return -1;
    const double t = e2.dot(q) * inv;
    return t > 0 ? t : -1;
}

} // namespace

std::optional<uint32_t> pickVertex(const Brush& b, const Project& project, double mx, double my, double radius) {
    std::optional<uint32_t> best;
    double bestD = radius * radius;
    for (size_t i = 0; i < b.vertices.size(); ++i) {
        const auto s = project(b.vertices[i]);
        if (!s) continue;
        const double d = (s->x - mx) * (s->x - mx) + (s->y - my) * (s->y - my);
        if (d < bestD) {
            bestD = d;
            best = static_cast<uint32_t>(i);
        }
    }
    return best;
}

std::optional<Edge> pickEdge(const Brush& b, const Project& project, double mx, double my, double radius, const std::set<Edge>* only) {
    std::optional<Edge> best;
    double bestD = std::isinf(radius) ? std::numeric_limits<double>::infinity() : radius * radius;
    for (const EdgeFaces& e : buildTopology(b).edgeList) {
        const Edge k{e.a, e.b};
        if (only && !only->count(k)) continue;
        const auto a = project(b.vertices[e.a]), c = project(b.vertices[e.b]);
        if (!a || !c) continue;
        const double dx = c->x - a->x, dy = c->y - a->y, l2 = dx * dx + dy * dy;
        const double t = l2 < 1e-9 ? 0 : std::max(0.0, std::min(1.0, ((mx - a->x) * dx + (my - a->y) * dy) / l2));
        const double d = (a->x + dx * t - mx) * (a->x + dx * t - mx) + (a->y + dy * t - my) * (a->y + dy * t - my);
        if (d < bestD) {
            bestD = d;
            best = k;
        }
    }
    return best;
}

std::optional<Edge> nearestFaceEdge(const Brush& b, uint32_t faceId, const Project& project, double mx, double my) {
    const BrushFace* f = nullptr;
    for (const BrushFace& x : b.faces)
        if (x.id == faceId) f = &x;
    if (!f) return std::nullopt;
    std::set<Edge> only;
    for (size_t i = 0; i < f->loop.size(); ++i) only.insert(edgeKey(f->loop[i], f->loop[(i + 1) % f->loop.size()]));
    return pickEdge(b, project, mx, my, std::numeric_limits<double>::infinity(), &only);
}

std::optional<uint32_t> pickFaceId(const Brush& b, const Ray& ray) {
    std::optional<uint32_t> best;
    double bestT = std::numeric_limits<double>::infinity();
    for (const BrushFace& f : b.faces)
        for (const auto& tri : core::texplan::triangulateFace(b, f)) {
            const double t = rayTriangle(ray, b.vertices[tri[0]], b.vertices[tri[1]], b.vertices[tri[2]]);
            if (t > 0 && t < bestT) {
                bestT = t;
                best = f.id;
            }
        }
    return best;
}

MeshSel boxSelect(const Brush& b, const Project& project, SelMode mode, double x0, double y0, double x1, double y1) {
    const double lx = std::min(x0, x1), hx = std::max(x0, x1), ly = std::min(y0, y1), hy = std::max(y0, y1);
    auto inside = [&](const Vec3& p) {
        const auto s = project(p);
        return s && s->x >= lx && s->x <= hx && s->y >= ly && s->y <= hy;
    };
    if (mode == SelMode::Vertex) {
        std::vector<uint32_t> v;
        for (size_t i = 0; i < b.vertices.size(); ++i)
            if (inside(b.vertices[i])) v.push_back(static_cast<uint32_t>(i));
        return selFromVerts(b, v);
    }
    if (mode == SelMode::Edge) {
        std::vector<Edge> keys;
        for (const EdgeFaces& e : buildTopology(b).edgeList)
            if (inside(b.vertices[e.a]) && inside(b.vertices[e.b])) keys.push_back(Edge{e.a, e.b});
        return selFromEdges(b, keys);
    }
    std::vector<uint32_t> ids;
    for (const BrushFace& f : b.faces)
        if (std::all_of(f.loop.begin(), f.loop.end(), [&](uint32_t v) { return inside(b.vertices[v]); })) ids.push_back(f.id);
    return selFromFaces(b, ids);
}

MeshSel combineSel(const Brush& b, const MeshSel& base, const MeshSel& add, SelMode mode, bool toggle) {
    if (mode == SelMode::Edge) {
        std::vector<Edge> out = base.edges;
        for (const Edge& x : add.edges) {
            const bool had = std::find(base.edges.begin(), base.edges.end(), x) != base.edges.end();
            const auto at = std::find(out.begin(), out.end(), x);
            if (toggle && had) {
                if (at != out.end()) out.erase(at);
            } else if (at == out.end()) {
                out.push_back(x);
            }
        }
        return selFromEdges(b, out);
    }
    const std::vector<uint32_t>& cur = mode == SelMode::Vertex ? base.verts : base.faces;
    const std::vector<uint32_t>& next = mode == SelMode::Vertex ? add.verts : add.faces;
    std::vector<uint32_t> out = cur;
    for (uint32_t x : next) {
        const bool had = std::find(cur.begin(), cur.end(), x) != cur.end();
        const auto at = std::find(out.begin(), out.end(), x);
        if (toggle && had) {
            if (at != out.end()) out.erase(at);
        } else if (at == out.end()) {
            out.push_back(x);
        }
    }
    return mode == SelMode::Vertex ? selFromVerts(b, out) : selFromFaces(b, out);
}

} // namespace native::tools::mesh
