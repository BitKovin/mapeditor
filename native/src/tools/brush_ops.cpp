#include "tools/brush_ops.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "tools/mesh/ops.h"
#include "tools/mesh/topology.h"

namespace native::tools {

using core::Brush;
using core::BrushFace;
using core::Vec3;

namespace {

constexpr double kEps = 1e-6;

// Orders coplanar points counter-clockwise as seen from outside (looking against `n`).
std::vector<uint32_t> orderAround(const std::vector<Vec3>& pts, const std::vector<uint32_t>& idx, const Vec3& n) {
    Vec3 c{0, 0, 0};
    for (uint32_t i : idx) c = c + pts[i];
    c = c * (1.0 / static_cast<double>(idx.size()));
    const Vec3 ref = std::abs(n.y) < 0.9 ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    const Vec3 u = ref.cross(n).normalized(), v = n.cross(u);
    std::vector<std::pair<double, uint32_t>> keyed;
    for (uint32_t i : idx) keyed.push_back({std::atan2((pts[i] - c).dot(v), (pts[i] - c).dot(u)), i});
    std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<uint32_t> out;
    for (const auto& k : keyed) out.push_back(k.second);
    return out;
}

// Compacts unused vertices away and drops degenerate faces; none with fewer than four faces left. Everything else of `like` (id, op, carve) is kept.
std::optional<Brush> tidy(const Brush& like, const std::vector<Vec3>& vertices, std::vector<BrushFace> faces) {
    faces.erase(std::remove_if(faces.begin(), faces.end(), [](const BrushFace& f) { return f.loop.size() < 3; }), faces.end());
    std::set<uint32_t> usedSet;
    for (const BrushFace& f : faces) usedSet.insert(f.loop.begin(), f.loop.end());
    std::map<uint32_t, uint32_t> remap;
    Brush out = like;
    out.vertices.clear();
    for (uint32_t i : usedSet) {
        remap[i] = static_cast<uint32_t>(out.vertices.size());
        out.vertices.push_back(vertices[i]);
    }
    for (BrushFace& f : faces)
        for (uint32_t& i : f.loop) i = remap[i];
    out.faces = std::move(faces);
    std::set<uint32_t> ids;
    for (const BrushFace& f : out.faces) ids.insert(f.id);
    out.smoothEdges.erase(std::remove_if(out.smoothEdges.begin(), out.smoothEdges.end(), [&](const std::pair<uint32_t, uint32_t>& p) { return !ids.count(p.first) || !ids.count(p.second); }),
                          out.smoothEdges.end());
    if (out.faces.size() < 4) return std::nullopt;
    return out;
}

bool validSolid(const Brush& b) { return b.signedVolume() > 1e-9 && mesh::isProperSolid(b); }

} // namespace

std::optional<Plane> planeFromPoints(const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 n = (b - a).cross(c - a);
    if (n.length() < 1e-9) return std::nullopt;
    const Vec3 u = n.normalized();
    return Plane{u, u.dot(a)};
}

Plane facePlane(const Brush& b, const BrushFace& f) {
    const Vec3 n = b.faceNormal(f);
    return {n, n.dot(b.vertices[f.loop[0]])};
}

ClipResult clipBrush(const Brush& b, const Plane& pl) {
    ClipResult res;
    std::vector<double> s;
    for (const Vec3& v : b.vertices) s.push_back(pl.n.dot(v) - pl.d);
    if (std::all_of(s.begin(), s.end(), [](double x) { return x <= kEps; })) {
        res.kind = ClipKind::Unchanged;
        res.brush = b;
        return res;
    }
    if (std::all_of(s.begin(), s.end(), [](double x) { return x >= -kEps; })) return res;

    std::vector<Vec3> vertices = b.vertices;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> cut;   // edge -> new vertex
    const auto cutVertex = [&](uint32_t i, uint32_t j) {
        const auto key = std::make_pair(std::min(i, j), std::max(i, j));
        const auto it = cut.find(key);
        if (it != cut.end()) return it->second;
        const double t = s[i] / (s[i] - s[j]);
        vertices.push_back(vertices[i] + (vertices[j] - vertices[i]) * t);
        const uint32_t k = static_cast<uint32_t>(vertices.size() - 1);
        cut[key] = k;
        return k;
    };
    std::set<uint32_t> onPlane;
    std::vector<BrushFace> faces;
    for (const BrushFace& f : b.faces) {
        std::vector<uint32_t> out;
        std::vector<core::Vec2> uv;
        const bool hasUv = f.uvs.size() == f.loop.size();
        const size_t n = f.loop.size();
        for (size_t k = 0; k < n; ++k) {
            const uint32_t i = f.loop[k], j = f.loop[(k + 1) % n];
            const double si = s[i], sj = s[j];
            if (si <= kEps) {
                out.push_back(i);
                if (hasUv) uv.push_back(f.uvs[k]);
                if (std::abs(si) <= kEps) onPlane.insert(i);
            }
            if ((si < -kEps && sj > kEps) || (si > kEps && sj < -kEps)) {
                const uint32_t nv = cutVertex(i, j);
                out.push_back(nv);
                onPlane.insert(nv);
                if (hasUv) {
                    const double t = si / (si - sj);
                    const core::Vec2 a = f.uvs[k], c = f.uvs[(k + 1) % n];
                    uv.push_back({a.u + (c.u - a.u) * t, a.v + (c.v - a.v) * t});
                }
            }
        }
        BrushFace nf = f;
        nf.loop.clear();
        nf.uvs.clear();
        for (size_t k = 0; k < out.size(); ++k)   // drops a vertex repeated right after itself
            if (out[k] != out[(k + out.size() - 1) % out.size()] || out.size() == 1) {
                nf.loop.push_back(out[k]);
                if (hasUv) nf.uvs.push_back(uv[k]);
            }
        if (nf.loop.size() >= 3) faces.push_back(std::move(nf));
    }
    const std::vector<uint32_t> capIdx = orderAround(vertices, std::vector<uint32_t>(onPlane.begin(), onPlane.end()), pl.n);
    if (capIdx.size() < 3) return res;
    BrushFace cap;
    cap.id = mesh::nextFaceId(b);
    cap.loop = capIdx;
    for (const BrushFace& f : b.faces)
        if (!f.texture.empty()) {
            cap.texture = f.texture;
            break;
        }
    faces.push_back(std::move(cap));
    std::optional<Brush> r = tidy(b, vertices, std::move(faces));
    if (!r || !validSolid(*r)) return res;
    res.kind = ClipKind::Clipped;
    res.brush = std::move(*r);
    return res;
}

ClipPair clipBy(const Brush& b, const Plane& pl, Keep keep) {
    ClipPair out;
    if (keep != Keep::Front) {
        const ClipResult r = clipBrush(b, pl);
        if (r.has()) out.back = r.brush;
    }
    if (keep != Keep::Back) {
        const ClipResult r = clipBrush(b, {pl.n * -1.0, -pl.d});
        if (r.has()) out.front = r.brush;
    }
    return out;
}

std::optional<std::vector<Brush>> hollowBrush(const Brush& b, double t) {
    if (t <= 0) return std::nullopt;
    std::vector<Plane> planes;
    for (const BrushFace& f : b.faces) planes.push_back(facePlane(b, f));
    std::vector<Plane> inner;
    for (const Plane& p : planes) inner.push_back({p.n, p.d - t});
    ClipResult core{ClipKind::Unchanged, b};
    bool coreCut = false;
    for (const Plane& p : inner) {
        if (!core.has()) break;
        core = clipBrush(core.brush, p);
        if (core.kind == ClipKind::Clipped) coreCut = true;
    }
    if (!core.has() || !coreCut) return std::nullopt;   // nothing is left inside -> the walls would fill the whole brush
    std::vector<Brush> walls;
    for (size_t i = 0; i < planes.size(); ++i) {
        ClipResult w = clipBrush(b, {planes[i].n * -1.0, -inner[i].d});   // keep n_i.p >= d_i - t
        bool cut = w.kind == ClipKind::Clipped;
        for (size_t j = 0; j < i && w.has(); ++j) {   // and inside the earlier inner planes
            w = clipBrush(w.brush, inner[j]);
            if (w.kind == ClipKind::Clipped) cut = true;
        }
        if (w.has() && cut) walls.push_back(std::move(w.brush));
    }
    return walls;
}

std::optional<Brush> hullBrush(const std::vector<Vec3>& points, const std::string& texture) {
    std::vector<Vec3> pts;
    for (const Vec3& p : points) {
        bool dup = false;
        for (const Vec3& q : pts)
            if ((p - q).length() < 1e-7) dup = true;
        if (!dup) pts.push_back(p);
    }
    if (pts.size() < 4 || pts.size() > 120) return std::nullopt;
    struct HullPlane {
        Vec3 n;
        double d;
        std::vector<uint32_t> idx;
    };
    std::vector<HullPlane> planes;
    for (size_t i = 0; i < pts.size(); ++i)
        for (size_t j = i + 1; j < pts.size(); ++j)
            for (size_t k = j + 1; k < pts.size(); ++k) {
                const auto pl = planeFromPoints(pts[i], pts[j], pts[k]);
                if (!pl) continue;
                bool pos = false, neg = false;
                for (const Vec3& p : pts) {
                    const double side = pl->n.dot(p) - pl->d;
                    if (side > kEps) pos = true;
                    if (side < -kEps) neg = true;
                }
                if (pos && neg) continue;
                const Vec3 n = pos ? pl->n * -1.0 : pl->n;   // outward: everything else behind it
                const double d = pos ? -pl->d : pl->d;
                bool known = false;
                for (const HullPlane& q : planes)
                    if (q.n.dot(n) > 1 - 1e-9 && std::abs(q.d - d) < 1e-7) known = true;
                if (known) continue;
                HullPlane hp{n, d, {}};
                for (size_t ix = 0; ix < pts.size(); ++ix)
                    if (std::abs(n.dot(pts[ix]) - d) <= kEps) hp.idx.push_back(static_cast<uint32_t>(ix));
                planes.push_back(std::move(hp));
            }
    if (planes.size() < 4) return std::nullopt;
    std::set<uint32_t> used;
    for (const HullPlane& p : planes) used.insert(p.idx.begin(), p.idx.end());
    std::map<uint32_t, uint32_t> remap;
    Brush out;
    out.op = core::BrushOp::Add;
    for (uint32_t i : used) {
        remap[i] = static_cast<uint32_t>(out.vertices.size());
        out.vertices.push_back(pts[i]);
    }
    for (size_t i = 0; i < planes.size(); ++i) {
        BrushFace f;
        f.id = static_cast<uint32_t>(i);
        for (uint32_t x : orderAround(pts, planes[i].idx, planes[i].n)) f.loop.push_back(remap[x]);
        f.texture = texture;
        out.faces.push_back(std::move(f));
    }
    if (!validSolid(out)) return std::nullopt;
    return out;
}

std::optional<Brush> snapBrush(const Brush& b, double step) {
    if (step <= 0) return b;
    Brush out = b;
    for (Vec3& v : out.vertices) v = {std::round(v.x / step) * step, std::round(v.y / step) * step, std::round(v.z / step) * step};
    if (out.signedVolume() <= 1e-9 || !mesh::isProperSolid(out)) return std::nullopt;
    for (const BrushFace& f : out.faces) {   // every face must stay planar after snapping
        const Vec3 n = out.faceNormal(f), p0 = out.vertices[f.loop[0]];
        for (uint32_t i : f.loop)
            if (std::abs(n.dot(out.vertices[i] - p0)) > 1e-6) return std::nullopt;
    }
    return out;
}

bool isConvex(const Brush& b) {
    for (const BrushFace& f : b.faces) {
        const Plane p = facePlane(b, f);
        for (const Vec3& v : b.vertices)
            if (p.n.dot(v) - p.d > 1e-6) return false;
    }
    return true;
}

std::optional<Brush> intersectBrushes(const Brush& a, const Brush& b) {
    ClipResult cur{ClipKind::Unchanged, a};
    for (const BrushFace& f : b.faces) {
        cur = clipBrush(cur.brush, facePlane(b, f));
        if (!cur.has()) return std::nullopt;
    }
    return cur.brush;
}

namespace {

void unionRec(const std::vector<Brush>& brushes, size_t start, const Brush* cur, int count, double& total) {
    for (size_t i = start; i < brushes.size(); ++i) {
        std::optional<Brush> next;
        if (cur) next = intersectBrushes(*cur, brushes[i]);
        else next = brushes[i];
        if (!next) continue;
        total += (count % 2 == 0 ? 1.0 : -1.0) * next->signedVolume();
        unionRec(brushes, i + 1, &*next, count + 1, total);
    }
}

} // namespace

double unionVolume(const std::vector<Brush>& brushes) {
    double total = 0;
    if (brushes.size() > 12) {
        for (const Brush& b : brushes) total += b.signedVolume();
        return total;
    }
    unionRec(brushes, 0, nullptr, 0, total);
    return total;
}

// ---- select inside / touching ----

bool brushInside(const Brush& volume, const Brush& b) {
    std::vector<Plane> planes;
    for (const BrushFace& f : volume.faces) planes.push_back(facePlane(volume, f));
    for (const Vec3& v : b.vertices)
        for (const Plane& p : planes)
            if (p.n.dot(v) - p.d > kEps) return false;
    return true;
}

namespace {

std::vector<Vec3> edgeVectors(const Brush& b) {
    std::vector<Vec3> out;
    for (const BrushFace& f : b.faces)
        for (size_t i = 0; i < f.loop.size(); ++i) out.push_back(b.vertices[f.loop[(i + 1) % f.loop.size()]] - b.vertices[f.loop[i]]);
    return out;
}

std::pair<double, double> rangeAlong(const Brush& b, const Vec3& axis) {
    double lo = 1e300, hi = -1e300;
    for (const Vec3& v : b.vertices) {
        const double d = axis.dot(v);
        lo = std::min(lo, d);
        hi = std::max(hi, d);
    }
    return {lo, hi};
}

} // namespace

bool brushesTouch(const Brush& a, const Brush& b) {
    std::vector<Vec3> axes;
    for (const BrushFace& f : a.faces) axes.push_back(facePlane(a, f).n);
    for (const BrushFace& f : b.faces) axes.push_back(facePlane(b, f).n);
    const std::vector<Vec3> ea = edgeVectors(a), eb = edgeVectors(b);
    for (const Vec3& x : ea)
        for (const Vec3& y : eb) {
            const Vec3 c = x.cross(y);
            const double l = c.length();
            if (l > 1e-9) axes.push_back(c * (1.0 / l));
        }
    for (const Vec3& ax : axes) {
        const auto [a0, a1] = rangeAlong(a, ax);
        const auto [b0, b1] = rangeAlong(b, ax);
        if (!(a1 >= b0 - kEps && b1 >= a0 - kEps)) return false;
    }
    return true;
}

// ---- clipper points ----

std::optional<Vec3> rayPlaneHit(const render::Ray& ray, const Vec3& planePoint, const Vec3& n) {
    const Vec3 dir{ray.dir.x, ray.dir.y, ray.dir.z}, origin{ray.origin.x, ray.origin.y, ray.origin.z};
    const double den = dir.dot(n);
    if (std::abs(den) < 1e-9) return std::nullopt;
    const double t = (planePoint - origin).dot(n) / den;
    if (!std::isfinite(t)) return std::nullopt;
    return origin + dir * t;
}

Vec3 snapPoint(const Vec3& p, double step, int keepAxis) {
    const auto snap1 = [step](double x) { return step > 0 ? std::round(x / step) * step : x; };
    return {keepAxis == 0 ? p.x : snap1(p.x), keepAxis == 1 ? p.y : snap1(p.y), keepAxis == 2 ? p.z : snap1(p.z)};
}

std::optional<size_t> hitClipPoint(const std::vector<Vec3>& pts, const std::function<std::optional<std::pair<double, double>>(const Vec3&)>& screen, double mx, double my, double radius) {
    std::optional<size_t> best;
    double bd = radius;
    for (size_t i = 0; i < pts.size(); ++i) {
        const auto s = screen(pts[i]);
        if (!s) continue;
        const double d = std::hypot(s->first - mx, s->second - my);
        if (d <= bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

bool clashes(const std::vector<Vec3>& pts, size_t skip, const Vec3& q) {
    for (size_t i = 0; i < pts.size(); ++i)
        if (i != skip && (pts[i] - q).length() < 1e-9) return true;
    return false;
}

int depthAxis(const std::optional<Vec3>& forward) {
    if (!forward) return -1;
    const double c[3] = {forward->x, forward->y, forward->z};
    for (int i = 0; i < 3; ++i)
        if (std::abs(c[i]) > 0.999) return i;
    return -1;
}

} // namespace native::tools
