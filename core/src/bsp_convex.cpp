// Convex pieces of brushes for the rBSP collision brushes (DESIGN §12): plane sets, their windings, concave brushes split by their own face
// planes, and subtraction of a convex cutter.
#include "core/bsp_compile.h"
#include "core/csg.h"
#include <cmath>

namespace core {

namespace {

constexpr double kEps = 1e-6;

// Keeps the part of `poly` where n . p <= d (+ eps).
std::vector<Vec3> clipBack(const std::vector<Vec3>& poly, const Vec3& n, double d) {
    std::vector<Vec3> out;
    const size_t k = poly.size();
    for (size_t i = 0; i < k; ++i) {
        const Vec3& a = poly[i];
        const Vec3& b = poly[(i + 1) % k];
        const double da = a.dot(n) - d, db = b.dot(n) - d;
        if (da <= kEps) out.push_back(a);
        if ((da < -kEps && db > kEps) || (da > kEps && db < -kEps)) out.push_back(a + (b - a) * (da / (da - db)));
    }
    return out;
}

double polyArea(const std::vector<Vec3>& p, const Vec3& n) {
    Vec3 s;
    for (size_t i = 0; i + 2 < p.size(); ++i) s = s + (p[i + 1] - p[0]).cross(p[i + 2] - p[0]);
    return 0.5 * s.dot(n);
}

struct Poly {
    Vec3 n;
    double d = 0;
    std::string texture;
    std::vector<Vec3> v;
};

void splitPoly(const Poly& p, const Vec3& n, double d, std::vector<Poly>& front, std::vector<Poly>& back) {
    bool f = false, b = false;
    for (const Vec3& v : p.v) {
        const double s = v.dot(n) - d;
        if (s > kEps) f = true;
        if (s < -kEps) b = true;
    }
    if (!f && !b) {
        if (p.n.dot(n) > 0) return;  // coplanar, same orientation: consumed by the splitter
        return;                      // coplanar, opposite: a degenerate sliver inside one brush; dropped
    }
    if (!b) { front.push_back(p); return; }
    if (!f) { back.push_back(p); return; }
    Poly pf = p, pb = p;
    pb.v = clipBack(p.v, n, d);
    pf.v = clipBack(p.v, n * -1.0, -d);
    if (pf.v.size() >= 3) front.push_back(std::move(pf));
    if (pb.v.size() >= 3) back.push_back(std::move(pb));
}

void faceBsp(const std::vector<Poly>& polys, std::vector<HalfSpace>& region, std::vector<ConvexPiece>& out) {
    // splitter: the plane that cuts the fewest other polygons
    size_t best = 0;
    int bestCuts = INT32_MAX;
    for (size_t i = 0; i < polys.size() && bestCuts > 0; ++i) {
        int cuts = 0;
        for (size_t j = 0; j < polys.size(); ++j) {
            bool f = false, b = false;
            for (const Vec3& v : polys[j].v) {
                const double s = v.dot(polys[i].n) - polys[i].d;
                f = f || s > kEps;
                b = b || s < -kEps;
            }
            cuts += f && b;
        }
        if (cuts < bestCuts) { bestCuts = cuts; best = i; }
    }
    const Poly& s = polys[best];
    std::vector<Poly> front, back;
    for (const Poly& p : polys) splitPoly(p, s.n, s.d, front, back);
    if (!front.empty()) {  // in front of a face: outside, unless more faces lie there
        region.push_back({s.n * -1.0, -s.d, s.texture});
        faceBsp(front, region, out);
        region.pop_back();
    }
    region.push_back({s.n, s.d, s.texture});
    if (back.empty()) {
        ConvexPiece piece{region};
        if (tidyConvex(piece)) out.push_back(std::move(piece));
    } else {
        faceBsp(back, region, out);
    }
    region.pop_back();
}

}  // namespace

std::vector<std::vector<Vec3>> convexWindings(const std::vector<HalfSpace>& sides, double size) {
    std::vector<std::vector<Vec3>> out(sides.size());
    for (size_t i = 0; i < sides.size(); ++i) {
        const Vec3 n = sides[i].normal;
        const Vec3 helper = std::fabs(n.x) < 0.6 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        const Vec3 u = n.cross(helper).normalized() * size;
        const Vec3 v = n.cross(u.normalized()) * size;  // u x v = n
        const Vec3 c = n * sides[i].dist;
        std::vector<Vec3> w = {c - u - v, c + u - v, c + u + v, c - u + v};
        for (size_t j = 0; j < sides.size() && w.size() >= 3; ++j)
            if (j != i) {
                // identical planes: keep only the first (else both clip each other away)
                if (sides[j].normal.dot(n) > 1 - 1e-9 && std::fabs(sides[j].dist - sides[i].dist) < kEps) {
                    if (j < i) { w.clear(); break; }
                    continue;
                }
                w = clipBack(w, sides[j].normal, sides[j].dist);
            }
        if (w.size() >= 3 && polyArea(w, n) > 1e-12) out[i] = std::move(w);
    }
    return out;
}

bool tidyConvex(ConvexPiece& piece, double minVolume) {
    const auto w = convexWindings(piece.sides);
    std::vector<HalfSpace> keep;
    double volume = 0;
    for (size_t i = 0; i < w.size(); ++i) {
        if (w[i].empty()) continue;
        volume += polyArea(w[i], piece.sides[i].normal) * w[i][0].dot(piece.sides[i].normal) / 3.0;
        keep.push_back(piece.sides[i]);
    }
    piece.sides = std::move(keep);
    return piece.sides.size() >= 4 && volume > minVolume;
}

std::vector<ConvexPiece> convexPieces(const Brush& brush) {
    std::vector<Poly> polys;
    for (auto& sp : brushSurfaces(brush)) {
        const BrushFace* face = nullptr;
        for (const auto& f : brush.faces) if (f.id == sp.faceId) face = &f;
        if (!face || sp.verts.size() < 3) continue;
        Vec3 nn;
        for (size_t i = 0; i + 2 < sp.verts.size(); ++i) nn = nn + (sp.verts[i + 1] - sp.verts[0]).cross(sp.verts[i + 2] - sp.verts[0]);
        const Vec3 n = nn.normalized();
        if (n.length() < 0.5) continue;
        polys.push_back({n, n.dot(sp.verts[0]), face->texture, std::move(sp.verts)});
    }
    std::vector<ConvexPiece> out;
    Vec3 mn, mx;
    if (polys.empty() || !brush.bounds(mn, mx)) return out;
    const double m = 1.0;
    std::vector<HalfSpace> region = {
        {{1, 0, 0}, mx.x + m, ""}, {{-1, 0, 0}, -(mn.x - m), ""}, {{0, 1, 0}, mx.y + m, ""},
        {{0, -1, 0}, -(mn.y - m), ""}, {{0, 0, 1}, mx.z + m, ""}, {{0, 0, -1}, -(mn.z - m), ""}};
    faceBsp(polys, region, out);
    return out;
}

std::vector<ConvexPiece> subtractConvex(const ConvexPiece& piece, const ConvexPiece& cutter) {
    std::vector<ConvexPiece> out;
    ConvexPiece rest = piece;
    for (const HalfSpace& c : cutter.sides) {
        ConvexPiece outside = rest;
        outside.sides.push_back({c.normal * -1.0, -c.dist, c.texture});
        if (tidyConvex(outside)) out.push_back(std::move(outside));
        rest.sides.push_back(c);
        if (!tidyConvex(rest)) return out;  // nothing of the piece is left inside the cutter's remaining planes: the rest is untouched
    }
    return out;  // `rest` lies inside the cutter: removed
}

}  // namespace core
