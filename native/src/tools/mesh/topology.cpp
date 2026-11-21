#include "tools/mesh/topology.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_set>

namespace native::tools::mesh {

using core::Brush;
using core::BrushFace;
using core::Vec3;

namespace {

// A set that remembers the order its members were added in (a JS Set).
template <class T, class Hash = std::hash<T>>
struct OrderedSet {
    std::vector<T> items;
    std::unordered_set<T, Hash> seen;
    bool add(const T& v) {
        if (!seen.insert(v).second) return false;
        items.push_back(v);
        return true;
    }
    bool has(const T& v) const { return seen.count(v) != 0; }
};
using EdgeSet = OrderedSet<Edge, EdgeHash>;

bool contains(const std::vector<uint32_t>& v, uint32_t x) { return std::find(v.begin(), v.end(), x) != v.end(); }

tp::V3 p3(const Brush& b, uint32_t i) { return tp::v3(b.vertices[i]); }

} // namespace

Topology buildTopology(const Brush& b) {
    Topology t;
    for (size_t fi = 0; fi < b.faces.size(); ++fi) {
        const auto& loop = b.faces[fi].loop;
        const size_t n = loop.size();
        for (size_t i = 0; i < n; ++i) {
            const uint32_t a = loop[i], c = loop[(i + 1) % n];
            t.half[Topology::halfKey(a, c)] = fi;
            const Edge k = edgeKey(a, c);
            const auto it = t.edgeIndex.find(k);
            if (it != t.edgeIndex.end()) {
                t.edgeList[it->second].faces.push_back(fi);
            } else {
                t.edgeIndex[k] = t.edgeList.size();
                t.edgeList.push_back({k.a, k.b, {fi}});
            }
            t.vertexFaces[a].push_back(fi);
        }
    }
    return t;
}

bool isClosedManifold(const Brush& b) {
    const Topology t = buildTopology(b);
    for (const EdgeFaces& e : t.edgeList) {
        if (e.faces.size() != 2) return false;
        if (!t.half.count(Topology::halfKey(e.a, e.b)) || !t.half.count(Topology::halfKey(e.b, e.a))) return false;
    }
    return true;
}

const BrushFace* faceById(const Brush& b, uint32_t id) {
    for (const BrushFace& f : b.faces)
        if (f.id == id) return &f;
    return nullptr;
}

uint32_t nextFaceId(const Brush& b) {
    int64_t m = -1;
    for (const BrushFace& f : b.faces) m = std::max<int64_t>(m, f.id);
    return static_cast<uint32_t>(m + 1);
}

BrushFace cloneFace(const BrushFace& from, uint32_t id, const std::vector<uint32_t>& loop) {
    BrushFace f = from;
    f.id = id;
    f.loop = loop;
    f.uvs.clear();
    if (!from.uvs.empty()) f.uvs.assign(loop.size(), core::Vec2{0, 0});
    return f;
}

Vec3 centroid(const Brush& b, const std::vector<uint32_t>& verts) {
    Vec3 c{0, 0, 0};
    for (uint32_t v : verts) c = c + b.vertices[v];
    return verts.empty() ? c : c * (1.0 / static_cast<double>(verts.size()));
}

Vec3 faceCentre(const Brush& b, const BrushFace& f) { return centroid(b, f.loop); }

Vec3 averageNormal(const Brush& b, const std::vector<const BrushFace*>& faces) {
    Vec3 s{0, 0, 0};
    for (const BrushFace* f : faces) {
        const Vec3& p0 = b.vertices[f->loop[0]];
        for (size_t i = 1; i + 1 < f->loop.size(); ++i) s = s + (b.vertices[f->loop[i]] - p0).cross(b.vertices[f->loop[i + 1]] - p0);
    }
    return s.normalized();
}

double edgeLength(const Brush& b, const Edge& e) { return (b.vertices[e.a] - b.vertices[e.b]).length(); }

// ---- selection ----
MeshSel selFromFaces(const Brush& b, const std::vector<uint32_t>& faceIds) {
    const std::unordered_set<uint32_t> ids(faceIds.begin(), faceIds.end());
    OrderedSet<uint32_t> verts;
    EdgeSet edges;
    MeshSel out;
    for (const BrushFace& f : b.faces) {
        if (!ids.count(f.id)) continue;
        out.faces.push_back(f.id);
        for (size_t i = 0; i < f.loop.size(); ++i) {
            verts.add(f.loop[i]);
            edges.add(edgeKey(f.loop[i], f.loop[(i + 1) % f.loop.size()]));
        }
    }
    out.verts = std::move(verts.items);
    out.edges = std::move(edges.items);
    return out;
}

MeshSel selFromVerts(const Brush& b, const std::vector<uint32_t>& vertsIn) {
    OrderedSet<uint32_t> vs;
    for (uint32_t v : vertsIn) vs.add(v);
    EdgeSet edges;
    MeshSel out;
    for (const BrushFace& f : b.faces) {
        const size_t n = f.loop.size();
        const bool all = std::all_of(f.loop.begin(), f.loop.end(), [&](uint32_t v) { return vs.has(v); });
        if (all) out.faces.push_back(f.id);
        for (size_t i = 0; i < n; ++i) {
            const uint32_t v = f.loop[i], w = f.loop[(i + 1) % n];
            if (all || (vs.has(v) && vs.has(w))) edges.add(edgeKey(v, w));
        }
    }
    out.verts = std::move(vs.items);
    out.edges = std::move(edges.items);
    return out;
}

MeshSel selFromEdges(const Brush& b, const std::vector<Edge>& edgeKeys) {
    EdgeSet es;
    for (const Edge& e : edgeKeys) es.add(e);
    OrderedSet<uint32_t> verts;
    for (const Edge& e : es.items) {
        verts.add(e.a);
        verts.add(e.b);
    }
    MeshSel out;
    for (const BrushFace& f : b.faces) {
        bool all = true;
        for (size_t i = 0; i < f.loop.size() && all; ++i) all = es.has(edgeKey(f.loop[i], f.loop[(i + 1) % f.loop.size()]));
        if (all) out.faces.push_back(f.id);
    }
    out.verts = std::move(verts.items);
    out.edges = std::move(es.items);
    return out;
}

MeshSel convertSel(const Brush& b, const MeshSel& sel, SelMode from, SelMode to) {
    if (from == to) return sel;
    if (to == SelMode::Vertex) {
        std::vector<uint32_t> verts;
        if (from == SelMode::Edge) {
            for (const Edge& e : sel.edges) {
                verts.push_back(e.a);
                verts.push_back(e.b);
            }
        } else {
            for (uint32_t id : sel.faces)
                if (const BrushFace* f = faceById(b, id)) verts.insert(verts.end(), f->loop.begin(), f->loop.end());
        }
        OrderedSet<uint32_t> uniq;
        for (uint32_t v : verts) uniq.add(v);
        return selFromVerts(b, uniq.items);
    }
    if (to == SelMode::Edge) {
        if (from == SelMode::Vertex) return selFromEdges(b, selFromVerts(b, sel.verts).edges);
        return selFromEdges(b, sel.faces.empty() ? std::vector<Edge>() : selFromFaces(b, sel.faces).edges);
    }
    // to face: the faces fully inside the selection (vertex mode: every corner selected; edge mode: every edge selected)
    return from == SelMode::Vertex ? selFromVerts(b, sel.verts) : selFromEdges(b, sel.edges);
}

MeshSel allSel(const Brush& b) {
    std::vector<uint32_t> ids;
    for (const BrushFace& f : b.faces) ids.push_back(f.id);
    return selFromFaces(b, ids);
}

MeshSel invertSel(const Brush& b, const MeshSel& sel, SelMode mode) {
    const MeshSel all = allSel(b);
    if (mode == SelMode::Vertex) {
        std::vector<uint32_t> v;
        for (uint32_t x : all.verts)
            if (!contains(sel.verts, x)) v.push_back(x);
        return selFromVerts(b, v);
    }
    if (mode == SelMode::Edge) {
        std::vector<Edge> e;
        for (const Edge& x : all.edges)
            if (std::find(sel.edges.begin(), sel.edges.end(), x) == sel.edges.end()) e.push_back(x);
        return selFromEdges(b, e);
    }
    std::vector<uint32_t> f;
    for (uint32_t x : all.faces)
        if (!contains(sel.faces, x)) f.push_back(x);
    return selFromFaces(b, f);
}

MeshSel growSel(const Brush& b, const MeshSel& sel, SelMode mode) {
    const Topology t = buildTopology(b);
    if (mode == SelMode::Face) {
        OrderedSet<uint32_t> ids;
        for (uint32_t id : sel.faces) ids.add(id);
        const MeshSel base = selFromFaces(b, sel.faces);
        const std::unordered_set<uint32_t> verts(base.verts.begin(), base.verts.end());
        for (const BrushFace& f : b.faces)
            if (std::any_of(f.loop.begin(), f.loop.end(), [&](uint32_t v) { return verts.count(v) != 0; })) ids.add(f.id);
        return selFromFaces(b, ids.items);
    }
    if (mode == SelMode::Edge) {
        std::unordered_set<uint32_t> verts;
        for (const Edge& e : sel.edges) {
            verts.insert(e.a);
            verts.insert(e.b);
        }
        EdgeSet edges;
        for (const Edge& e : sel.edges) edges.add(e);
        for (const EdgeFaces& e : t.edgeList)
            if (verts.count(e.a) || verts.count(e.b)) edges.add(Edge{e.a, e.b});
        return selFromEdges(b, edges.items);
    }
    OrderedSet<uint32_t> verts;
    for (uint32_t v : sel.verts) verts.add(v);
    for (const EdgeFaces& e : t.edgeList) {
        if (contains(sel.verts, e.a)) verts.add(e.b);
        if (contains(sel.verts, e.b)) verts.add(e.a);
    }
    return selFromVerts(b, verts.items);
}

MeshSel shrinkSel(const Brush& b, const MeshSel& sel, SelMode mode) {
    const Topology t = buildTopology(b);
    if (mode == SelMode::Face) {
        const std::unordered_set<uint32_t> ids(sel.faces.begin(), sel.faces.end());
        std::vector<uint32_t> keep;
        for (uint32_t id : sel.faces) {
            const BrushFace* f = faceById(b, id);
            if (!f) continue;
            bool all = true;
            for (uint32_t v : f->loop) {
                const auto it = t.vertexFaces.find(v);
                if (it == t.vertexFaces.end()) continue;
                for (size_t fi : it->second) all = all && ids.count(b.faces[fi].id) != 0;
            }
            if (all) keep.push_back(id);
        }
        return selFromFaces(b, keep);
    }
    if (mode == SelMode::Edge) {
        const std::set<Edge> es(sel.edges.begin(), sel.edges.end());
        std::vector<Edge> keep;
        for (const Edge& k : sel.edges) {
            bool all = true;
            for (uint32_t v : {k.a, k.b})
                for (const EdgeFaces& e : t.edgeList)
                    if (e.a == v || e.b == v) all = all && es.count(Edge{e.a, e.b}) != 0;
            if (all) keep.push_back(k);
        }
        return selFromEdges(b, keep);
    }
    const std::unordered_set<uint32_t> vs(sel.verts.begin(), sel.verts.end());
    std::vector<uint32_t> keep;
    for (uint32_t v : sel.verts) {
        bool all = true;
        for (const EdgeFaces& e : t.edgeList)
            if (e.a == v || e.b == v) all = all && vs.count(e.a) && vs.count(e.b);
        if (all) keep.push_back(v);
    }
    return selFromVerts(b, keep);
}

MeshSel cleanSel(const Brush& b, const MeshSel& sel) {
    const Topology t = buildTopology(b);
    std::unordered_set<uint32_t> ids;
    for (const BrushFace& f : b.faces) ids.insert(f.id);
    MeshSel out;
    OrderedSet<uint32_t> v, f;
    for (uint32_t x : sel.verts)
        if (x < b.vertices.size()) v.add(x);
    EdgeSet e;
    for (const Edge& x : sel.edges)
        if (t.edgeIndex.count(x)) e.add(x);
    for (uint32_t x : sel.faces)
        if (ids.count(x)) f.add(x);
    out.verts = std::move(v.items);
    out.edges = std::move(e.items);
    out.faces = std::move(f.items);
    return out;
}

// ---- the clean-up ----
namespace {

bool nonPlanar(const Brush& b, const BrushFace& f, double eps) {
    if (f.loop.size() <= 3) return false;
    const Vec3 n = b.faceNormal(f);
    const Vec3& p0 = b.vertices[f.loop[0]];
    for (uint32_t i : f.loop)
        if (std::abs(n.dot(b.vertices[i] - p0)) > eps) return true;
    return false;
}

} // namespace

SplitResult splitNonPlanar(const Brush& b, double eps) {
    SplitResult r;
    r.brush = b;
    bool any = false;
    for (const BrushFace& f : b.faces) any = any || nonPlanar(b, f, eps);
    if (!any) return r;
    uint32_t next = nextFaceId(b);
    std::vector<BrushFace> faces;
    for (const BrushFace& f : b.faces) {
        if (!nonPlanar(b, f, eps)) {
            faces.push_back(f);
            continue;
        }
        const auto tris = tp::triangulateFace(b, f);
        std::vector<uint32_t> ids;
        for (size_t k = 0; k < tris.size(); ++k) {
            const uint32_t id = k == 0 ? f.id : next++;
            ids.push_back(id);
            BrushFace nf = cloneFace(f, id, {tris[k][0], tris[k][1], tris[k][2]});
            if (!f.uvs.empty() && f.uvs.size() == f.loop.size()) {
                nf.uvs.clear();
                for (uint32_t v : tris[k]) nf.uvs.push_back(f.uvs[static_cast<size_t>(std::find(f.loop.begin(), f.loop.end(), v) - f.loop.begin())]);
            }
            faces.push_back(std::move(nf));
        }
        r.pieces[f.id] = std::move(ids);
    }
    r.brush.faces = std::move(faces);
    return r;
}

Brush refreshUvs(const Brush& b, const Brush* before, const std::map<uint32_t, uint32_t>& parentOf, const tp::InfoFn& infoIn) {
    const tp::InfoFn info = infoIn ? infoIn : tp::InfoFn([](const std::string&) -> const tp::TexInfo* { return nullptr; });
    bool changed = false;
    Brush out = b;
    for (size_t fi = 0; fi < b.faces.size(); ++fi) {
        const BrushFace& f = b.faces[fi];
        const BrushFace* old = nullptr;
        if (before) {
            const auto p = parentOf.find(f.id);
            old = faceById(*before, p != parentOf.end() ? p->second : f.id);
        }
        if (before && old) {
            const tp::UvAffine m = tp::faceAffine(*before, *old, info);
            bool moved = parentOf.count(f.id) != 0 || old->loop.size() != f.loop.size();
            for (size_t k = 0; !moved && k < f.loop.size(); ++k) {
                const Vec3& o = before->vertices[old->loop[k]];
                const Vec3& n = b.vertices[f.loop[k]];
                moved = o.x != n.x || o.y != n.y || o.z != n.z;
            }
            if (moved || !f.uvs.empty()) {
                changed = true;
                std::vector<core::Vec2> uvs;
                for (uint32_t vi : f.loop) {
                    const tp::V3 q = p3(b, vi);
                    uvs.push_back(core::Vec2{tp::evalAff(m.u, q), tp::evalAff(m.v, q)});
                }
                out.faces[fi].uvs = std::move(uvs);
            }
            continue;
        }
        if (f.uvs.empty()) continue;
        changed = true;
        const tp::Projection proj = tp::projectionOf(tp::faceKey(f, "projection"), info(f.texture));
        const tp::V3 n = tp::faceNormalOf(b, f);
        std::vector<core::Vec2> uvs;
        for (uint32_t vi : f.loop) {
            const tp::V2 uv = tp::projectPoint(n, p3(b, vi), proj);
            uvs.push_back(core::Vec2{uv[0], uv[1]});
        }
        out.faces[fi].uvs = std::move(uvs);
    }
    return changed ? out : b;
}

Cleaned compact(const Brush& b, const MeshSel& sel) {
    std::vector<bool> used(b.vertices.size(), false);
    size_t usedCount = 0;
    for (const BrushFace& f : b.faces)
        for (uint32_t v : f.loop)
            if (v < used.size() && !used[v]) {
                used[v] = true;
                ++usedCount;
            }
    if (usedCount == b.vertices.size()) return {b, cleanSel(b, sel)};
    std::vector<int64_t> map(b.vertices.size(), -1);
    Brush nb = b;
    nb.vertices.clear();
    for (size_t i = 0; i < b.vertices.size(); ++i)
        if (used[i]) {
            map[i] = static_cast<int64_t>(nb.vertices.size());
            nb.vertices.push_back(b.vertices[i]);
        }
    for (BrushFace& f : nb.faces)
        for (uint32_t& v : f.loop) v = static_cast<uint32_t>(map[v]);
    auto remap = [&](uint32_t v) -> int64_t { return v < map.size() ? map[v] : -1; };
    MeshSel s;
    for (uint32_t v : sel.verts)
        if (remap(v) >= 0) s.verts.push_back(static_cast<uint32_t>(remap(v)));
    for (const Edge& e : sel.edges) {
        const int64_t x = remap(e.a), y = remap(e.b);
        if (x >= 0 && y >= 0) s.edges.push_back(edgeKey(static_cast<uint32_t>(x), static_cast<uint32_t>(y)));
    }
    s.faces = sel.faces;
    return {nb, cleanSel(nb, s)};
}

Cleaned finish(const Brush& b, const MeshSel& sel, const tp::InfoFn& info, const Brush* before, const std::map<uint32_t, uint32_t>& splitFrom) {
    const SplitResult split = splitNonPlanar(b);
    MeshSel s = sel;
    for (const auto& [old, ids] : split.pieces)
        if (contains(s.faces, old))
            for (uint32_t id : ids)
                if (id != old) s.faces.push_back(id);
    Cleaned c = compact(split.brush, s);
    std::map<uint32_t, uint32_t> parentOf = splitFrom;
    for (const auto& [old, ids] : split.pieces)
        for (uint32_t id : ids)
            if (id != old) {
                const auto from = splitFrom.find(old);
                parentOf[id] = from != splitFrom.end() ? from->second : old;
            }
    c.brush = refreshUvs(c.brush, before, parentOf, info);
    return c;
}

} // namespace native::tools::mesh
