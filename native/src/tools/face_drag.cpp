#include "tools/face_drag.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <optional>

#include "tools/brush_ops.h"
#include "tools/mesh/ops.h"

namespace native::tools {

using core::Brush;
using core::BrushFace;
using core::Vec2;
using core::Vec3;

namespace {

constexpr double kEps = 1e-6;

DragResult failure(const std::string& message) {
    DragResult r;
    r.error = message;
    return r;
}
DragResult success(Brush b) {
    DragResult r;
    r.brush = std::move(b);
    return r;
}

bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// Groups the faces touching a vertex into distinct planes (coplanar faces count once).
std::vector<Vec3> distinctNormals(const Brush& b, const std::vector<size_t>& faceIdx) {
    std::vector<Vec3> out;
    for (size_t i : faceIdx) {
        const Vec3 n = b.faceNormal(b.faces[i]);
        bool known = false;
        for (const Vec3& m : out)
            if (m.dot(n) > 1 - 1e-7) known = true;
        if (!known) out.push_back(n);
    }
    return out;
}

using UvFit = std::function<Vec2(const Vec3&)>;

// UV of a point on a plane from a fitted affine map (3 non-collinear corners of the old face -> their uvs).
std::optional<UvFit> uvFit(const Brush& b, const BrushFace& f) {
    if (f.uvs.size() != f.loop.size()) return std::nullopt;
    std::vector<Vec3> P;
    for (uint32_t i : f.loop) P.push_back(b.vertices[i]);
    for (size_t i = 1; i + 1 < P.size(); ++i) {
        const Vec3 e1 = P[i] - P[0], e2 = P[i + 1] - P[0];
        const Vec3 n = e1.cross(e2);
        const double nn = n.dot(n);
        if (nn < 1e-12) continue;
        const double u0 = f.uvs[0].u, v0 = f.uvs[0].v;
        const double d1u = f.uvs[i].u - u0, d1v = f.uvs[i].v - v0, d2u = f.uvs[i + 1].u - u0, d2v = f.uvs[i + 1].v - v0;
        const Vec3 P0 = P[0];
        return UvFit([=](const Vec3& p) {
            const Vec3 q = p - P0;
            // barycentric-style coefficients a,c with q = a*e1 + c*e2 (q projected on the plane)
            const double a = q.cross(e2).dot(n) / nn, c = e1.cross(q).dot(n) / nn;
            return Vec2{u0 + a * d1u + c * d2u, v0 + a * d1v + c * d2v};
        });
    }
    return std::nullopt;
}

DragResult slideDrag(const Brush& b, uint32_t faceId, double dist) {
    size_t fi = b.faces.size();
    for (size_t i = 0; i < b.faces.size(); ++i)
        if (b.faces[i].id == faceId) fi = i;
    if (fi == b.faces.size()) return failure("face not found");
    if (std::abs(dist) < kEps) return success(b);
    const BrushFace& F = b.faces[fi];
    const Vec3 n = b.faceNormal(F);
    const double d1 = n.dot(b.vertices[F.loop[0]]) + dist;
    std::map<uint32_t, Vec3> moved;
    for (uint32_t vi : F.loop) {
        const Vec3 v = b.vertices[vi];
        std::vector<size_t> touching;
        for (size_t i = 0; i < b.faces.size(); ++i)
            if (i != fi && std::find(b.faces[i].loop.begin(), b.faces[i].loop.end(), vi) != b.faces[i].loop.end()) touching.push_back(i);
        std::vector<Vec3> normals;
        for (const Vec3& m : distinctNormals(b, touching))
            if (std::abs(m.dot(n)) < 1 - 1e-7) normals.push_back(m);
        if (normals.size() != 2)
            return failure(normals.size() > 2 ? "This face has a corner where 3+ faces meet - drag its vertices instead" : "This face cannot be dragged (degenerate corner)");
        const Vec3 dir = normals[0].cross(normals[1]);
        if (dir.length() < kEps) return failure("This face cannot be dragged (parallel neighbours)");
        const Vec3 u = dir.normalized();
        const double k = n.dot(u);
        if (std::abs(k) < 1e-6) return failure("This face cannot be dragged along its normal (edge parallel to the face)");
        moved[vi] = v + u * ((d1 - n.dot(v)) / k);
    }
    Brush out = b;
    for (const auto& [vi, p] : moved) out.vertices[vi] = p;
    for (size_t i = 0; i < b.faces.size(); ++i) {
        const auto fit = uvFit(b, b.faces[i]);
        if (!fit) continue;
        for (size_t k = 0; k < b.faces[i].loop.size(); ++k) {
            Vec3 p = out.vertices[b.faces[i].loop[k]];
            if (i == fi) p = p - n * (n.dot(p) - n.dot(b.vertices[F.loop[0]]));   // keep the texture where it was on the plane
            out.faces[i].uvs[k] = (*fit)(p);
        }
    }
    for (const Vec3& v : out.vertices)
        if (!finite(v)) return failure("drag failed");
    // the face must still face outward and the solid must stay valid (dragging past the opposite side turns it inside out)
    if (out.faceNormal(out.faces[fi]).dot(n) < 0.99) return failure("The face cannot move further (brush would collapse)");
    if (out.signedVolume() <= 1e-9 * std::max(1.0, b.signedVolume())) return failure("The face cannot move further (brush would collapse)");
    if (!mesh::isProperSolid(out)) return failure("The face cannot move further (brush would become invalid)");
    return success(std::move(out));
}

// Plane-based drag for convex brushes: shifts the face's plane and re-intersects all planes, so corners where 3+ faces meet split or merge like in Radiant.
DragResult planeDrag(const Brush& b, uint32_t faceId, double dist) {
    size_t fi = b.faces.size();
    for (size_t i = 0; i < b.faces.size(); ++i)
        if (b.faces[i].id == faceId) fi = i;
    if (fi == b.faces.size()) return failure("face not found");
    std::vector<Plane> planes;
    for (const BrushFace& f : b.faces) planes.push_back(facePlane(b, f));
    const std::vector<Plane> orig = planes;
    planes[fi].d += dist;
    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    for (const Vec3& v : b.vertices) {
        const double c[3] = {v.x, v.y, v.z};
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], c[k]);
            hi[k] = std::max(hi[k], c[k]);
        }
    }
    double span = 1;
    for (int k = 0; k < 3; ++k) span = std::max(span, hi[k] - lo[k]);
    const double R = (span + std::abs(dist)) * 1000;
    const Vec3 c{(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
    ClipResult cur;
    cur.kind = ClipKind::Unchanged;
    cur.brush = Brush::makeBox("t", {c.x - R, c.y - R, c.z - R}, {c.x + R, c.y + R, c.z + R});
    for (const Plane& pl : planes) {
        cur = clipBrush(cur.brush, pl);
        if (!cur.has()) return failure("The face cannot move further (brush would collapse)");
    }
    for (const Vec3& v : cur.brush.vertices)
        if (std::abs(v.x) > R * 0.5 || std::abs(v.y) > R * 0.5 || std::abs(v.z) > R * 0.5) return failure("The face cannot move further (brush would become unbounded)");
    std::vector<std::optional<UvFit>> fits;
    for (const BrushFace& f : b.faces) fits.push_back(uvFit(b, f));
    Brush out = b;
    out.faces.clear();
    for (const BrushFace& rf : cur.brush.faces) {
        const Plane pl = facePlane(cur.brush, rf);
        size_t i = planes.size();
        for (size_t q = 0; q < planes.size(); ++q)
            if (planes[q].n.dot(pl.n) > 1 - 1e-7 && std::abs(planes[q].d - pl.d) < 1e-5 * std::max(1.0, R / 1000)) {
                i = q;
                break;
            }
        if (i == planes.size()) return failure("drag failed");
        BrushFace f = b.faces[i];
        f.loop = rf.loop;
        f.uvs.clear();
        if (fits[i])
            for (uint32_t vi : rf.loop) {
                const Vec3 p = cur.brush.vertices[vi];
                const Vec3 q = i == fi ? p - planes[i].n * (planes[i].n.dot(p) - orig[i].d) : p;
                f.uvs.push_back((*fits[i])(q));
            }
        out.faces.push_back(std::move(f));
    }
    bool kept = false;
    for (const BrushFace& f : out.faces)
        if (f.id == faceId) kept = true;
    if (!kept) return failure("The face cannot move further (brush would collapse)");
    out.vertices = cur.brush.vertices;
    if (out.signedVolume() <= 1e-9 || !mesh::isProperSolid(out)) return failure("The face cannot move further (brush would become invalid)");
    return success(std::move(out));
}

} // namespace

DragResult dragFace(const Brush& b, uint32_t faceId, double dist) {
    DragResult r = slideDrag(b, faceId, dist);
    if (!r.ok() && r.error.find("3+ faces") != std::string::npos && isConvex(b)) return planeDrag(b, faceId, dist);
    return r;
}

DragResult extendFace(const Brush& b, uint32_t faceId, double dist, const std::string& newId) {
    const BrushFace* F = nullptr;
    for (const BrushFace& f : b.faces)
        if (f.id == faceId) F = &f;
    if (!F) return failure("face not found");
    if (!(dist > 1e-6)) return failure("Drag outward to extend the face");
    const Vec3 n = b.faceNormal(*F);
    const std::vector<uint32_t>& L = F->loop;
    const size_t k = L.size();
    std::vector<Vec3> base;
    for (uint32_t i : L) base.push_back(b.vertices[i]);
    const Vec3 offset = n * dist;
    std::vector<Vec3> vertices = base;
    for (const Vec3& p : base) vertices.push_back(p + offset);
    const auto strip = [](const BrushFace& f) {
        BrushFace r = f;
        r.loop.clear();
        r.uvs.clear();
        return r;
    };
    BrushFace cap = strip(*F);
    cap.id = 0;
    for (size_t i = 0; i < k; ++i) cap.loop.push_back(static_cast<uint32_t>(k + i));
    BrushFace bottom = strip(*F);
    bottom.id = 1;
    for (size_t i = 0; i < k; ++i) bottom.loop.push_back(static_cast<uint32_t>(k - 1 - i));
    if (F->uvs.size() == k) {
        cap.uvs = F->uvs;
        bottom.uvs = F->uvs;
        std::reverse(bottom.uvs.begin(), bottom.uvs.end());
    }
    Brush out;
    out.id = newId;
    out.op = b.op;
    out.carve = b.carve;
    out.vertices = vertices;
    out.faces = {cap, bottom};
    for (size_t i = 0; i < k; ++i) {
        const size_t j = (i + 1) % k;
        // the face of b across the edge L[i] -> L[j] contains that edge the other way round
        const BrushFace* g = nullptr;
        for (const BrushFace& f : b.faces) {
            if (&f == F) continue;
            for (size_t x = 0; x < f.loop.size(); ++x)
                if (f.loop[x] == L[j] && f.loop[(x + 1) % f.loop.size()] == L[i]) g = &f;
            if (g) break;
        }
        BrushFace wall = strip(g ? *g : *F);
        wall.id = static_cast<uint32_t>(2 + i);
        wall.loop = {static_cast<uint32_t>(i), static_cast<uint32_t>(j), static_cast<uint32_t>(k + j), static_cast<uint32_t>(k + i)};
        if (g)
            if (const auto fit = uvFit(b, *g))
                for (uint32_t vi : wall.loop) wall.uvs.push_back((*fit)(vertices[vi]));
        out.faces.push_back(std::move(wall));
    }
    if (!mesh::isProperSolid(out)) return failure("This face cannot be extended (the result would not be a valid solid)");
    return success(std::move(out));
}

} // namespace native::tools
