#include "core/csg.h"
#include <algorithm>
#include <cmath>
#include <iterator>
#include <memory>
#include <unordered_map>

namespace core {

namespace {

constexpr double kEps = 1e-6;

struct Plane {
    Vec3 n;
    double w = 0;
    static Plane fromPoints(const Vec3& a, const Vec3& b, const Vec3& c) {
        Vec3 n = (b - a).cross(c - a).normalized();
        return {n, n.dot(a)};
    }
    void flip() { n = n * -1.0; w = -w; }
};

struct Tag {
    int source = 0;
    uint32_t faceId = 0;
};

struct Poly {
    std::vector<Vec3> v;
    Plane plane;
    Tag tag;
    void flip() {
        std::reverse(v.begin(), v.end());
        plane.flip();
    }
};

enum Side { kCoplanar = 0, kFront = 1, kBack = 2, kSpanning = 3 };

void splitPolygon(const Plane& plane, const Poly& poly, std::vector<Poly>& coplanarFront, std::vector<Poly>& coplanarBack,
                  std::vector<Poly>& front, std::vector<Poly>& back) {
    int polyType = 0;
    std::vector<int> types;
    types.reserve(poly.v.size());
    for (const auto& p : poly.v) {
        const double t = plane.n.dot(p) - plane.w;
        const int type = t < -kEps ? kBack : (t > kEps ? kFront : kCoplanar);
        polyType |= type;
        types.push_back(type);
    }
    switch (polyType) {
        case kCoplanar: (plane.n.dot(poly.plane.n) > 0 ? coplanarFront : coplanarBack).push_back(poly); break;
        case kFront: front.push_back(poly); break;
        case kBack: back.push_back(poly); break;
        default: {
            Poly f{{}, poly.plane, poly.tag}, b{{}, poly.plane, poly.tag};
            const size_t n = poly.v.size();
            for (size_t i = 0; i < n; ++i) {
                const size_t j = (i + 1) % n;
                const int ti = types[i], tj = types[j];
                const Vec3& vi = poly.v[i];
                const Vec3& vj = poly.v[j];
                if (ti != kBack) f.v.push_back(vi);
                if (ti != kFront) b.v.push_back(vi);
                if ((ti | tj) == kSpanning) {
                    const double t = (plane.w - plane.n.dot(vi)) / plane.n.dot(vj - vi);
                    const Vec3 v = vi + (vj - vi) * t;
                    f.v.push_back(v);
                    b.v.push_back(v);
                }
            }
            if (f.v.size() >= 3) front.push_back(std::move(f));
            if (b.v.size() >= 3) back.push_back(std::move(b));
        }
    }
}

class Node {
public:
    Node() = default;
    explicit Node(std::vector<Poly> polys) { build(std::move(polys)); }

    void invert() {
        for (auto& p : polys_) p.flip();
        if (hasPlane_) plane_.flip();
        if (front_) front_->invert();
        if (back_) back_->invert();
        std::swap(front_, back_);
    }

    std::vector<Poly> clipPolygons(const std::vector<Poly>& in) const {
        if (!hasPlane_) return in;
        std::vector<Poly> front, back;
        for (const auto& p : in) splitPolygon(plane_, p, front, back, front, back);
        if (front_) front = front_->clipPolygons(front);
        back = back_ ? back_->clipPolygons(back) : std::vector<Poly>{};
        front.insert(front.end(), std::make_move_iterator(back.begin()), std::make_move_iterator(back.end()));
        return front;
    }

    void clipTo(const Node& other) {
        polys_ = other.clipPolygons(polys_);
        if (front_) front_->clipTo(other);
        if (back_) back_->clipTo(other);
    }

    std::vector<Poly> all() const {
        std::vector<Poly> out = polys_;
        if (front_) { auto f = front_->all(); out.insert(out.end(), f.begin(), f.end()); }
        if (back_) { auto b = back_->all(); out.insert(out.end(), b.begin(), b.end()); }
        return out;
    }

    void build(std::vector<Poly> polys) {
        if (polys.empty()) return;
        if (!hasPlane_) { plane_ = polys[0].plane; hasPlane_ = true; }
        std::vector<Poly> front, back;
        // A coplanar polygon facing the other way bounds solid that lies in front of this plane (concave brushes have them):
        // it belongs to the front subtree. Keeping it here, as csg.js does, makes the tree call part of that solid air.
        for (auto& p : polys) splitPolygon(plane_, p, polys_, front, front, back);
        if (!front.empty()) { if (!front_) front_ = std::make_unique<Node>(); front_->build(std::move(front)); }
        if (!back.empty()) { if (!back_) back_ = std::make_unique<Node>(); back_->build(std::move(back)); }
    }

private:
    Plane plane_;
    bool hasPlane_ = false;
    std::unique_ptr<Node> front_, back_;
    std::vector<Poly> polys_;
};

// a - b (both given as polygon soups of closed solids)
std::vector<Poly> subtractPolys(const std::vector<Poly>& aPolys, const std::vector<Poly>& bPolys) {
    Node a(aPolys), b(bPolys);
    a.invert();
    a.clipTo(b);
    b.clipTo(a);
    b.invert();
    b.clipTo(a);
    b.invert();
    a.build(b.all());
    a.invert();
    return a.all();
}

bool isConvexLoop(const std::vector<Vec3>& v, const Vec3& n) {
    const size_t c = v.size();
    for (size_t i = 0; i < c; ++i) {
        const Vec3 e1 = v[(i + 1) % c] - v[i], e2 = v[(i + 2) % c] - v[(i + 1) % c];
        if (e1.cross(e2).dot(n) < -1e-9) return false;
    }
    return true;
}

// Ear clipping for a concave planar polygon whose Newell normal is `n`.
std::vector<std::vector<Vec3>> triangulate(const std::vector<Vec3>& v, const Vec3& n) {
    std::vector<size_t> idx(v.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::vector<std::vector<Vec3>> tris;
    auto inside = [&](const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
        return (b - a).cross(p - a).dot(n) > -1e-12 && (c - b).cross(p - b).dot(n) > -1e-12 && (a - c).cross(p - c).dot(n) > -1e-12;
    };
    size_t guard = 0;
    while (idx.size() > 3 && guard++ < 10000) {
        bool clipped = false;
        for (size_t k = 0; k < idx.size(); ++k) {
            const size_t ia = idx[(k + idx.size() - 1) % idx.size()], ib = idx[k], ic = idx[(k + 1) % idx.size()];
            const Vec3 &a = v[ia], &b = v[ib], &c = v[ic];
            if ((b - a).cross(c - b).dot(n) <= 1e-12) continue;  // reflex or degenerate corner
            bool ear = true;
            for (size_t o : idx) {
                if (o == ia || o == ib || o == ic) continue;
                if (inside(v[o], a, b, c)) { ear = false; break; }
            }
            if (!ear) continue;
            tris.push_back({a, b, c});
            idx.erase(idx.begin() + static_cast<std::ptrdiff_t>(k));
            clipped = true;
            break;
        }
        if (!clipped) break;  // degenerate input: give up on the rest
    }
    if (idx.size() == 3) tris.push_back({v[idx[0]], v[idx[1]], v[idx[2]]});
    return tris;
}

std::vector<Poly> polysOf(const Brush& b, int source) {
    std::vector<Poly> out;
    for (const auto& f : b.faces) {
        std::vector<Vec3> v;
        v.reserve(f.loop.size());
        for (uint32_t i : f.loop) v.push_back(b.vertices[i]);
        Vec3 n;  // Newell
        for (size_t i = 0; i < v.size(); ++i) {
            const Vec3 &p = v[i], &q = v[(i + 1) % v.size()];
            n = n + Vec3{(p.y - q.y) * (p.z + q.z), (p.z - q.z) * (p.x + q.x), (p.x - q.x) * (p.y + q.y)};
        }
        n = n.normalized();
        if (n.length() < 0.5) continue;
        auto emit = [&](const std::vector<Vec3>& loop) {
            Poly p;
            p.v = loop;
            p.plane = {n, n.dot(loop[0])};
            p.tag = {source, f.id};
            out.push_back(std::move(p));
        };
        if (v.size() == 3 || isConvexLoop(v, n)) emit(v);
        else for (const auto& t : triangulate(v, n)) emit(t);
    }
    return out;
}

void bounds(const Brush& b, Vec3& lo, Vec3& hi) {
    lo = {1e300, 1e300, 1e300};
    hi = {-1e300, -1e300, -1e300};
    for (const auto& v : b.vertices) {
        lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
        hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
    }
}

double polygonArea(const std::vector<Vec3>& v) {
    Vec3 s;
    for (size_t i = 1; i + 1 < v.size(); ++i) s = s + (v[i] - v[0]).cross(v[i + 1] - v[0]);
    return 0.5 * s.length();
}

}  // namespace

bool brushesMayOverlap(const Brush& a, const Brush& b, double margin) {
    if (a.vertices.empty() || b.vertices.empty()) return false;
    Vec3 alo, ahi, blo, bhi;
    bounds(a, alo, ahi);
    bounds(b, blo, bhi);
    return alo.x <= bhi.x + margin && ahi.x >= blo.x - margin && alo.y <= bhi.y + margin && ahi.y >= blo.y - margin &&
           alo.z <= bhi.z + margin && ahi.z >= blo.z - margin;
}

bool csgSubtract(const Brush& target, const std::vector<const Brush*>& subtractors, std::vector<CsgFragment>& out,
                 const CsgLimits& limits) {
    out.clear();
    std::vector<Poly> acc = polysOf(target, 0);
    for (size_t k = 0; k < subtractors.size(); ++k) {
        const Brush& s = *subtractors[k];
        if (!brushesMayOverlap(target, s)) continue;
        acc = subtractPolys(acc, polysOf(s, static_cast<int>(k) + 1));
        if (acc.size() > limits.maxFragments) return false;
    }
    for (auto& p : acc) {
        if (p.v.size() < 3 || polygonArea(p.v) < 1e-9) continue;  // slivers left by the epsilon split
        out.push_back({p.tag.source, p.tag.faceId, std::move(p.v)});
    }
    return true;
}

bool carveScopeApplies(const CarveScope& scope, bool sameEntity, const Entity& target) {
    if (sameEntity) return scope.self;
    return EntityClassRegistry::isStructural(target.classname) ? scope.structural : scope.nonStructural;
}

std::vector<SurfacePiece> brushSurfaces(const Brush& b) {
    std::vector<SurfacePiece> out;
    for (auto& p : polysOf(b, 0)) out.push_back({p.tag.faceId, std::move(p.v)});
    return out;
}

}  // namespace core
