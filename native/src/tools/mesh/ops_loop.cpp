// tools/mesh/ops_loop -- bevel (with round profiles), loop cut and bridge: the second half of shell/src/model/meshOps.ts.
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "tools/mesh/ops.h"

namespace native::tools::mesh {

using core::Brush;
using core::BrushFace;
using core::Vec3;

namespace {

OpResult fail(const std::string& message) {
    OpResult r;
    r.error = message;
    return r;
}
OpResult same(const Brush& b, const MeshSel& sel) {
    OpResult r;
    r.brush = b;
    r.sel = sel;
    return r;
}
OpResult done(Cleaned c) {
    OpResult r;
    r.brush = std::move(c.brush);
    r.sel = std::move(c.sel);
    return r;
}

Vec3 dirBetween(const Brush& b, uint32_t from, uint32_t to) { return (b.vertices[to] - b.vertices[from]).normalized(); }

struct StripInfo {
    uint32_t id;
    uint32_t x, y;   // the bevelled edge the strip replaced
};

struct Rail {
    uint32_t from = 0;
    std::vector<uint32_t> pts;   // inserted vertices, ordered from `from`
    Vec3 corner;
    double w = 0;
};
using RailKey = std::pair<uint32_t, uint32_t>;
RailKey railKey(uint32_t a, uint32_t b) { return a < b ? RailKey{a, b} : RailKey{b, a}; }

// Turns each chamfer strip into `segs` strips following a circular-arc profile (rational quadratic Bezier from one end of the strip's rail to the other with the original corner
// as the control point; weight sin(angle / 2) = the exact circle). The new rail vertices are inserted into the one other face that shares the rail edge (a corner cap, or the
// face beside a bevel that ends), so the mesh stays closed; such faces may stop being planar and are split by `finish`.
struct Rounded {
    Brush brush;
    std::vector<uint32_t> strips;
};
Rounded roundStrips(const Brush& b, const Brush& orig, const std::vector<StripInfo>& info, int segs) {
    Brush nb = b;
    std::vector<Vec3>& vertices = nb.vertices;
    std::map<RailKey, Rail> rails;
    auto arc = [&](uint32_t from, uint32_t to, const Vec3& vertex, const Vec3& e0, const Vec3& e1) -> std::vector<uint32_t> {
        const RailKey key = railKey(from, to);
        const auto have = rails.find(key);
        auto dirOf = [&](const Rail& r) {
            std::vector<uint32_t> pts = r.pts;
            if (r.from != from) std::reverse(pts.begin(), pts.end());
            return pts;
        };
        if (have != rails.end()) return dirOf(have->second);
        const Vec3 A = vertices[from], C = vertices[to];
        // the control point: where the two tangents meet, i.e. the original edge at the height of the rail (the rail's ends projected onto the edge line)
        const Vec3 ed = (e1 - e0).normalized();
        auto proj = [&](const Vec3& p) { return e0 + ed * (p - e0).dot(ed); };
        const Vec3 pa = proj(A), pc = proj(C);
        // where the rail's two ends sit at different heights along the edge (a chain corner), the tangents do not meet on the edge: fall back to the original corner
        const bool aligned = (pa - pc).length() < 1e-6 * std::max(1.0, (A - C).length());
        const Vec3 corner = aligned ? (pa + pc) * 0.5 : vertex;
        const Vec3 l1 = (A - corner).normalized(), l2 = (C - corner).normalized();
        const double w = std::sqrt(std::max(0.0, (1 - l1.dot(l2)) / 2));   // sin(phi / 2), phi = the angle between the legs at the corner
        std::vector<uint32_t> pts;
        for (int i = 1; i < segs; ++i) {
            const double t = static_cast<double>(i) / segs, k0 = (1 - t) * (1 - t), k1 = 2 * w * t * (1 - t), k2 = t * t, den = k0 + k1 + k2;
            pts.push_back(static_cast<uint32_t>(vertices.size()));
            vertices.push_back((A * k0 + corner * k1 + C * k2) * (1.0 / den));
        }
        Rail r;
        r.from = from;
        r.pts = pts;
        r.corner = vertex;
        r.w = w;
        rails[key] = r;
        return pts;
    };
    std::unordered_set<uint32_t> strips;
    for (const StripInfo& s : info) strips.insert(s.id);
    uint32_t next = nextFaceId(b);
    std::vector<BrushFace> out;
    std::vector<uint32_t> ids;
    for (const BrushFace& f : b.faces) {
        const auto inf = std::find_if(info.begin(), info.end(), [&](const StripInfo& x) { return x.id == f.id; });
        if (inf == info.end()) {
            out.push_back(f);
            continue;
        }
        const uint32_t p0 = f.loop[0], p1 = f.loop[1], p2 = f.loop[2], p3 = f.loop[3];
        const Vec3 ex = orig.vertices[inf->x], ey = orig.vertices[inf->y];
        std::vector<uint32_t> A{p0}, B{p1};
        for (uint32_t v : arc(p0, p3, ex, ex, ey)) A.push_back(v);
        A.push_back(p3);
        for (uint32_t v : arc(p1, p2, ey, ex, ey)) B.push_back(v);
        B.push_back(p2);
        for (int i = 0; i < segs; ++i) {
            const uint32_t id = i == 0 ? f.id : next++;
            ids.push_back(id);
            const std::vector<uint32_t> loop{A[i], B[i], B[i + 1], A[i + 1]};
            if (i == 0) {
                BrushFace nf = f;
                nf.loop = loop;
                out.push_back(std::move(nf));
            } else {
                out.push_back(cloneFace(f, id, loop));
            }
        }
    }
    // the face on the other side of every rail gets the new vertices along that edge; a corner cap where three or more arcs meet becomes a dome (a fan around a centre pushed out
    // toward the original corner) so it stays convex
    std::vector<BrushFace> faces;
    for (const BrushFace& f : out) {
        if (strips.count(f.id) || std::find(ids.begin(), ids.end(), f.id) != ids.end()) {
            faces.push_back(f);
            continue;
        }
        std::vector<uint32_t> loop;
        std::vector<const Rail*> touched;
        for (size_t i = 0; i < f.loop.size(); ++i) {
            const uint32_t u = f.loop[i], w = f.loop[(i + 1) % f.loop.size()];
            loop.push_back(u);
            const auto r = rails.find(railKey(u, w));
            if (r != rails.end() && !r->second.pts.empty()) {
                std::vector<uint32_t> pts = r->second.pts;
                if (r->second.from != u) std::reverse(pts.begin(), pts.end());
                loop.insert(loop.end(), pts.begin(), pts.end());
                touched.push_back(&r->second);
            }
        }
        if (touched.size() < 3) {
            BrushFace nf = f;
            if (loop.size() != f.loop.size()) nf.loop = loop;
            faces.push_back(std::move(nf));
            continue;
        }
        Vec3 mid{0, 0, 0};
        for (uint32_t v : loop) mid = mid + vertices[v] * (1.0 / static_cast<double>(loop.size()));
        const Vec3 corner = touched[0]->corner;
        double wsum = 0;
        for (const Rail* r : touched) wsum += r->w;
        const double w = wsum / static_cast<double>(touched.size()), k = w / (1 + w);
        const uint32_t centre = static_cast<uint32_t>(vertices.size());
        vertices.push_back(mid + (corner - mid) * k);
        for (size_t i = 0; i < loop.size(); ++i) {
            const std::vector<uint32_t> tri{loop[i], loop[(i + 1) % loop.size()], centre};
            if (i == 0) {
                BrushFace nf = f;
                nf.loop = tri;
                faces.push_back(std::move(nf));
            } else {
                faces.push_back(cloneFace(f, next++, tri));
            }
        }
    }
    nb.faces = std::move(faces);
    return {std::move(nb), ids};
}

struct CornerKey {
    size_t face;
    uint32_t vertex;
    bool operator<(const CornerKey& o) const { return face != o.face ? face < o.face : vertex < o.vertex; }
};

} // namespace

// ---- bevel ----
OpResult bevelEdges(const Brush& b, const std::vector<Edge>& edgeKeys, double width, int segments, const tp::InfoFn& info) {
    const int segs = std::max(1, std::min(kMaxBevelSegments, segments == 0 ? 1 : segments));
    const MeshSel sel = selFromEdges(b, edgeKeys);
    if (sel.edges.empty()) return fail("Select at least one edge to bevel");
    if (std::abs(width) < 1e-9) return same(b, sel);
    if (width < 0) return fail("Bevel width must be positive");
    if (!isClosedManifold(b)) return fail("Bevel needs a closed brush");
    const Topology t = buildTopology(b);
    const std::set<Edge> beveled(sel.edges.begin(), sel.edges.end());
    std::vector<uint32_t> bevVertList;
    std::unordered_set<uint32_t> bevVerts;
    for (const Edge& k : sel.edges)
        for (uint32_t v : {k.a, k.b})
            if (bevVerts.insert(v).second) bevVertList.push_back(v);

    Brush nb = b;
    std::vector<Vec3>& vertices = nb.vertices;
    // (face index, original vertex) -> the vertices replacing that corner, in loop order
    std::map<CornerKey, std::vector<uint32_t>> corner;
    auto setCorner = [&](size_t fi, uint32_t v, std::vector<uint32_t> list) { corner[{fi, v}] = std::move(list); };
    auto edgeLen = [&](uint32_t x, uint32_t y) { return (b.vertices[x] - b.vertices[y]).length(); };
    std::unordered_set<uint32_t> newVerts;
    auto addVert = [&](const Vec3& p) {
        newVerts.insert(static_cast<uint32_t>(vertices.size()));
        vertices.push_back(p);
        return static_cast<uint32_t>(vertices.size() - 1);
    };
    // the distance along the unbevelled edge `alongTo` (leaving v) at which the offset line of edge `bevTo` (also leaving v) is `width` away; < 0 when degenerate
    auto tAlong = [&](uint32_t v, uint32_t bevTo, uint32_t alongTo) {
        const double s = dirBetween(b, v, bevTo).cross(dirBetween(b, v, alongTo)).length();
        return s < 1e-6 ? -1.0 : width / s;
    };

    for (uint32_t v : bevVertList) {
        const std::vector<FanEntry> fan = fanOf(b, t, v);
        if (fan.empty()) return fail("Bevel: the mesh is not a clean closed surface around a vertex");
        const size_t n = fan.size();
        std::vector<size_t> marks;   // k such that edge (v, fan[k].outV) is bevelled; it lies between fan[k] and fan[k+1]
        for (size_t k = 0; k < n; ++k)
            if (beveled.count(edgeKey(v, fan[k].outV))) marks.push_back(k);
        const size_t m = marks.size();
        if (m == 1) {
            const size_t a = marks[0];
            const FanEntry &Fa = fan[a], &Fb = fan[(a + 1) % n];
            const double ta = tAlong(v, Fa.outV, Fa.inV), tb = tAlong(v, Fb.inV, Fb.outV);
            if (ta < 0 || tb < 0) return fail("Bevel failed at a degenerate corner");
            if (ta >= edgeLen(v, Fa.inV) || tb >= edgeLen(v, Fb.outV)) return fail("Bevel is wider than the geometry");
            const uint32_t pa = addVert(b.vertices[v] + dirBetween(b, v, Fa.inV) * ta);
            const uint32_t pb = addVert(b.vertices[v] + dirBetween(b, v, Fb.outV) * tb);
            setCorner(Fa.face, v, {pa});
            setCorner(Fb.face, v, {pb});
            if (n == 3) {
                setCorner(fan[(a + 2) % n].face, v, {pb, pa});
            } else {
                setCorner(fan[(a + 2) % n].face, v, {pb, v});
                setCorner(fan[(a + n - 1) % n].face, v, {v, pa});
            }
            continue;
        }
        // m >= 2: one new vertex per sector between consecutive bevelled edges
        for (size_t j = 0; j < m; ++j) {
            const size_t a = marks[j], z = marks[(j + 1) % m];
            size_t count = (z + n - a) % n;
            if (count == 0) count = n;
            const FanEntry &first = fan[(a + 1) % n], &last = fan[z];
            Vec3 p;
            if (count == 1) {
                const Vec3 N = b.faceNormal(b.faces[first.face]);
                const Vec3 n1 = N.cross(dirBetween(b, first.inV, v)), n2 = N.cross(dirBetween(b, v, first.outV));
                const double k = 1 + n1.dot(n2);
                if (k < 1e-6) return fail("Bevel failed at a sharp corner");
                p = b.vertices[v] + (n1 + n2) * (width / k);
            } else if (count == 2) {
                const uint32_t shared = first.outV;   // the edge between the two faces of the sector
                const double t1 = tAlong(v, first.inV, shared), t2 = tAlong(v, last.outV, shared);
                if (t1 < 0 || t2 < 0) return fail("Bevel failed at a degenerate corner");
                if ((t1 + t2) / 2 >= edgeLen(v, shared)) return fail("Bevel is wider than the geometry");
                p = b.vertices[v] + dirBetween(b, v, shared) * ((t1 + t2) / 2);
            } else {
                return fail("Bevel: select the edges around this corner so that at most two faces lie between bevelled edges");
            }
            const uint32_t nv = addVert(p);
            for (size_t c = 1; c <= count; ++c) setCorner(fan[(a + c) % n].face, v, {nv});
        }
    }

    auto cornerOf = [&](size_t fi, uint32_t v) -> const std::vector<uint32_t>* {
        const auto it = corner.find({fi, v});
        return it == corner.end() ? nullptr : &it->second;
    };
    // an offset that overshoots turns an edge around; catch that before building anything
    for (size_t fi = 0; fi < b.faces.size(); ++fi) {
        const auto& loop = b.faces[fi].loop;
        for (size_t i = 0; i < loop.size(); ++i) {
            const uint32_t v = loop[i], w = loop[(i + 1) % loop.size()];
            const auto* cv = cornerOf(fi, v);
            const auto* cw = cornerOf(fi, w);
            const uint32_t A = cv ? cv->back() : v, C = cw ? cw->front() : w;
            if ((vertices[C] - vertices[A]).dot(b.vertices[w] - b.vertices[v]) < 1e-12) return fail("Bevel is wider than the geometry");
        }
    }

    // rebuild the existing faces with their corners replaced
    for (size_t fi = 0; fi < nb.faces.size(); ++fi) {
        std::vector<uint32_t> loop;
        for (uint32_t v : b.faces[fi].loop) {
            const auto* c = cornerOf(fi, v);
            if (c) loop.insert(loop.end(), c->begin(), c->end());
            else loop.push_back(v);
        }
        nb.faces[fi].loop = loop;
    }

    // the strip that replaces each bevelled edge
    uint32_t next = nextFaceId(b);
    std::vector<uint32_t> strips;
    std::vector<StripInfo> stripInfo;
    for (const Edge& k : sel.edges) {
        const uint32_t x = k.a, y = k.b;
        const auto f1 = t.half.find(Topology::halfKey(x, y)), f2 = t.half.find(Topology::halfKey(y, x));
        if (f1 == t.half.end() || f2 == t.half.end()) return fail("Bevel needs an edge between two faces");
        auto pick = [&](size_t fi, uint32_t v) {   // the faces beside a bevelled edge always have a single replacement
            const auto* c = cornerOf(fi, v);
            return c ? c->front() : v;
        };
        // oriented so the strip contains (a > b as walked by f2): [a_f2, b_f2, b_f1, a_f1] for the edge a > b in f1
        const std::vector<uint32_t> loop{pick(f2->second, x), pick(f2->second, y), pick(f1->second, y), pick(f1->second, x)};
        const uint32_t id = next++;
        strips.push_back(id);
        stripInfo.push_back({id, x, y});
        nb.faces.push_back(cloneFace(b.faces[f1->second], id, loop));
    }

    // close the little holes left at corners where three or more bevelled edges meet
    LoopsClosed filled = closeOpenLoops(nb, [&](uint32_t v) { return newVerts.count(v) != 0 || bevVerts.count(v) != 0; }, next);
    if (!filled.error.empty()) return fail(filled.error);
    // no surviving face may have flipped or collapsed (the bevel is wider than the geometry allows)
    for (const BrushFace& f : b.faces) {
        const BrushFace* nf = faceById(filled.brush, f.id);
        if (nf && b.faceNormal(f).dot(filled.brush.faceNormal(*nf)) <= 0.05) return fail("Bevel is wider than the geometry");
    }
    Brush result = filled.brush;
    std::vector<uint32_t> stripIds = strips;
    if (segs > 1) {
        Rounded round = roundStrips(result, b, stripInfo, segs);
        result = std::move(round.brush);
        stripIds = std::move(round.strips);
    }
    return done(finish(result, selFromFaces(result, stripIds), info));
}

// ---- loop cut ----
std::optional<LoopCutInfo> edgeRing(const Brush& b, const Edge& edge) {
    const Topology t = buildTopology(b);
    const EdgeFaces* start = t.find(edge);
    if (!start) return std::nullopt;
    // oriented pairs (p, q): the rail vertices, kept consistent along the ring
    std::vector<std::pair<uint32_t, uint32_t>> oriented{{edge.a, edge.b}};
    std::set<Edge> seen{edge};
    auto step = [&](uint32_t p, uint32_t q, size_t fi) -> std::optional<std::pair<uint32_t, uint32_t>> {
        const BrushFace& f = b.faces[fi];
        if (f.loop.size() != 4) return std::nullopt;
        const size_t i = static_cast<size_t>(std::find(f.loop.begin(), f.loop.end(), p) - f.loop.begin());
        const size_t j = static_cast<size_t>(std::find(f.loop.begin(), f.loop.end(), q) - f.loop.begin());
        const size_t n = 4;
        if (i >= n || j >= n) return std::nullopt;
        // p, q adjacent in the loop; the opposite edge joins the other two vertices, matched p <-> p', q <-> q'
        if ((i + 1) % n == j) return std::make_pair(f.loop[(i + 3) % n], f.loop[(j + 1) % n]);
        if ((j + 1) % n == i) return std::make_pair(f.loop[(i + 1) % n], f.loop[(j + 3) % n]);
        return std::nullopt;
    };
    bool closed = false;
    for (size_t dir = 0; dir < 2 && !closed; ++dir) {
        uint32_t p = edge.a, q = edge.b;
        if (dir >= start->faces.size()) continue;
        size_t fi = start->faces[dir];
        bool haveFace = true;
        for (int guard = 0; guard < 1000 && haveFace; ++guard) {
            const auto nx = step(p, q, fi);
            if (!nx) break;
            const Edge k = edgeKey(nx->first, nx->second);
            if (k == edge) {
                closed = true;
                break;
            }
            if (seen.count(k)) break;
            seen.insert(k);
            if (dir == 0) oriented.push_back(*nx);
            else oriented.insert(oriented.begin(), *nx);
            p = nx->first;
            q = nx->second;
            // continue on the far side of the new edge
            const EdgeFaces* e = t.find(k);
            haveFace = false;
            if (e)
                for (size_t x : e->faces)
                    if (x != fi) {
                        fi = x;
                        haveFace = true;
                        break;
                    }
        }
    }
    LoopCutInfo info;
    info.ring = std::move(oriented);
    info.closed = closed;
    return info;
}

OpResult loopCut(const Brush& b, const Edge& edge, double param, const tp::InfoFn& info) {
    const auto ring = edgeRing(b, edge);
    if (!ring) return fail("Loop cut: pick an edge");
    if (!(param > 0.001 && param < 0.999)) return fail("Loop cut position must be strictly between the rails");
    Brush nb = b;
    std::map<Edge, uint32_t> mid;   // undirected ring edge -> the new vertex
    for (const auto& [p, q] : ring->ring) {
        mid[edgeKey(p, q)] = static_cast<uint32_t>(nb.vertices.size());
        nb.vertices.push_back(b.vertices[p] + (b.vertices[q] - b.vertices[p]) * param);
    }
    nb.faces.clear();
    uint32_t next = nextFaceId(b);
    std::vector<Edge> cutEdges;
    for (const BrushFace& f : b.faces) {
        struct In {
            size_t i;
            Edge k;
        };
        std::vector<In> in;
        for (size_t i = 0; i < f.loop.size(); ++i) {
            const Edge k = edgeKey(f.loop[i], f.loop[(i + 1) % f.loop.size()]);
            if (mid.count(k)) in.push_back({i, k});
        }
        if (in.empty()) {
            nb.faces.push_back(f);
            continue;
        }
        if (f.loop.size() == 4 && in.size() == 2 && (in[0].i + 2) % 4 == in[1].i) {
            // a quad crossed by the cut: split it into two quads along the new vertices
            const In &e0 = in[0], &e1 = in[1];
            const size_t n = 4;
            const uint32_t m0 = mid[e0.k], m1 = mid[e1.k];
            const auto& l = f.loop;
            const std::vector<uint32_t> A{l[e0.i], m0, m1, l[(e1.i + 1) % n]};   // from the first corner of e0 across to the end of e1
            const std::vector<uint32_t> B{m0, l[(e0.i + 1) % n], l[e1.i], m1};
            BrushFace fa = f;
            fa.loop = A;
            nb.faces.push_back(std::move(fa));
            nb.faces.push_back(cloneFace(f, next++, B));
            const Edge cut = edgeKey(m0, m1);
            if (std::find(cutEdges.begin(), cutEdges.end(), cut) == cutEdges.end()) cutEdges.push_back(cut);
            continue;
        }
        // any other face that touches the ring just receives the new vertices on its edges
        std::vector<uint32_t> loop;
        for (size_t i = 0; i < f.loop.size(); ++i) {
            loop.push_back(f.loop[i]);
            const auto m = mid.find(edgeKey(f.loop[i], f.loop[(i + 1) % f.loop.size()]));
            if (m != mid.end()) loop.push_back(m->second);
        }
        BrushFace nf = f;
        nf.loop = loop;
        nb.faces.push_back(std::move(nf));
    }
    return done(finish(nb, selFromEdges(nb, cutEdges), info));
}

// ---- bridge ----
OpResult bridgeLoops(const Brush& b, const MeshSel& sel, SelMode mode, const tp::InfoFn& info) {
    Brush cur = b;
    std::set<Edge> allowed;   // undirected edges that may be bridged
    const BrushFace* tmpl = nullptr;
    BrushFace tmplCopy;
    if (mode == SelMode::Face) {
        const std::unordered_set<uint32_t> ids(sel.faces.begin(), sel.faces.end());
        if (ids.size() < 2) return fail("Bridge: select two faces (or two groups of faces) to connect");
        cur.faces.clear();
        bool first = true;
        for (const BrushFace& f : b.faces) {
            if (!ids.count(f.id)) {
                cur.faces.push_back(f);
                continue;
            }
            if (first) {
                tmplCopy = f;
                tmpl = &tmplCopy;
                first = false;
            }
            for (size_t i = 0; i < f.loop.size(); ++i) allowed.insert(edgeKey(f.loop[i], f.loop[(i + 1) % f.loop.size()]));
        }
        if (cur.faces.empty()) return fail("Bridge: that would remove every face");
    } else {
        allowed.insert(sel.edges.begin(), sel.edges.end());
        if (allowed.size() < 6) return fail("Bridge: select two closed loops of open edges (delete faces first to open holes), or two faces in face mode");
    }
    const Topology t = buildTopology(cur);
    // the new faces walk every open hole edge opposite to its existing face: u -> v for an existing half edge v -> u without a twin
    std::unordered_map<uint32_t, uint32_t> next;
    std::vector<uint32_t> nextOrder;
    for (const BrushFace& f : cur.faces) {
        for (size_t i = 0; i < f.loop.size(); ++i) {
            const uint32_t a = f.loop[i], c = f.loop[(i + 1) % f.loop.size()];
            if (t.half.count(Topology::halfKey(c, a)) || !allowed.count(edgeKey(a, c))) continue;
            if (next.count(c)) return fail("Bridge: the loops touch each other (a vertex is on both)");
            next[c] = a;
            nextOrder.push_back(c);
        }
    }
    std::vector<std::vector<uint32_t>> loops;
    std::unordered_set<uint32_t> seen;
    for (uint32_t start : nextOrder) {
        if (seen.count(start)) continue;
        std::vector<uint32_t> loop;
        uint32_t v = start;
        for (size_t guard = 0; guard <= next.size(); ++guard) {
            if (seen.count(v)) break;
            seen.insert(v);
            loop.push_back(v);
            const auto n = next.find(v);
            if (n == next.end()) return fail("Bridge: the selected edges do not form closed loops");
            v = n->second;
        }
        if (v != start) return fail("Bridge: the selected edges do not form closed loops");
        loops.push_back(std::move(loop));
    }
    if (loops.size() != 2) return fail("Bridge: needs exactly two loops (found " + std::to_string(loops.size()) + ")");
    const std::vector<uint32_t>&A = loops[0], &B = loops[1];
    if (A.size() != B.size()) return fail("Bridge: the loops have " + std::to_string(A.size()) + " and " + std::to_string(B.size()) + " corners; they need the same number");
    const int64_t n = static_cast<int64_t>(A.size());
    const auto& P = cur.vertices;
    auto d = [&](uint32_t x, uint32_t y) { return (P[x] - P[y]).length(); };
    auto bAt = [&](int64_t k) { return B[static_cast<size_t>(((k % n) + n) % n)]; };
    // a_i pairs with b_(s - i): the quad over a_i -> a_(i+1) walks b_(s-i-1) -> b_(s-i) as B's own new edge
    int64_t best = 0;
    double bestLen = std::numeric_limits<double>::infinity();
    for (int64_t s = 0; s < n; ++s) {
        double sum = 0;
        for (int64_t i = 0; i < n; ++i) sum += d(A[static_cast<size_t>(i)], bAt(s - i));
        if (sum < bestLen - 1e-9) {
            bestLen = sum;
            best = s;
        }
    }
    uint32_t id = nextFaceId(b);
    std::vector<uint32_t> created;
    if (!tmpl) {
        auto h = t.half.find(Topology::halfKey(A[1], A[0]));
        tmpl = &cur.faces[h != t.half.end() ? h->second : 0];
    }
    Brush nb = cur;
    for (int64_t i = 0; i < n; ++i) {
        const std::vector<uint32_t> loop{A[static_cast<size_t>(i)], A[static_cast<size_t>((i + 1) % n)], bAt(best - i - 1), bAt(best - i)};
        nb.faces.push_back(cloneFace(*tmpl, id, loop));
        created.push_back(id++);
    }
    return done(finish(nb, selFromFaces(nb, created), info, &b));
}

} // namespace native::tools::mesh
