#include "tools/mesh/select.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "tools/mesh/ops.h"

namespace native::tools::mesh {

using core::Brush;
using core::BrushFace;

namespace {

using VertexEdges = std::unordered_map<uint32_t, std::vector<Edge>>;

VertexEdges vertexEdges(const Topology& t) {
    VertexEdges out;
    for (const EdgeFaces& e : t.edgeList)
        for (uint32_t v : {e.a, e.b}) out[v].push_back(Edge{e.a, e.b});
    return out;
}

} // namespace

std::vector<Edge> edgeLoop(const Brush& b, const Edge& edge) {
    const Topology t = buildTopology(b);
    if (!t.find(edge)) return {};
    const VertexEdges ve = vertexEdges(t);
    auto next = [&](const Edge& cur, uint32_t at) -> std::optional<Edge> {
        const std::vector<size_t>& curFaces = t.find(cur)->faces;
        std::vector<Edge> around;
        if (const auto it = ve.find(at); it != ve.end())
            for (const Edge& k : it->second)
                if (!(k == cur)) around.push_back(k);
        if (curFaces.size() == 1) {   // an open border: the other border edge at this vertex, if there is exactly one
            std::vector<Edge> border;
            for (const Edge& k : around)
                if (t.find(k)->faces.size() == 1) border.push_back(k);
            return border.size() == 1 ? std::optional<Edge>(border[0]) : std::nullopt;
        }
        if (around.size() != 3) return std::nullopt;
        std::vector<Edge> opposite;
        for (const Edge& k : around) {
            const auto& fs = t.find(k)->faces;
            const bool shares = std::any_of(fs.begin(), fs.end(), [&](size_t f) { return std::find(curFaces.begin(), curFaces.end(), f) != curFaces.end(); });
            if (!shares) opposite.push_back(k);
        }
        return opposite.size() == 1 ? std::optional<Edge>(opposite[0]) : std::nullopt;
    };
    std::vector<Edge> out{edge};
    std::set<Edge> seen{edge};
    for (uint32_t start : {edge.b, edge.a}) {
        Edge cur = edge;
        uint32_t at = start;
        for (int guard = 0; guard < 100000; ++guard) {
            const auto n = next(cur, at);
            if (!n || seen.count(*n)) break;
            seen.insert(*n);
            if (start == edge.b) out.push_back(*n);
            else out.insert(out.begin(), *n);
            at = n->a == at ? n->b : n->a;
            cur = *n;
        }
    }
    return out;
}

std::vector<Edge> edgeRingEdges(const Brush& b, const Edge& edge) {
    const auto r = edgeRing(b, edge);
    std::vector<Edge> out;
    if (r)
        for (const auto& [p, q] : r->ring) out.push_back(edgeKey(p, q));
    return out;
}

std::vector<uint32_t> faceLoop(const Brush& b, uint32_t faceId, const Edge& edge) {
    if (!faceById(b, faceId)) return {};
    const std::vector<Edge> ringList = edgeRingEdges(b, edge);
    const std::set<Edge> ring(ringList.begin(), ringList.end());
    std::vector<uint32_t> out{faceId};
    for (const BrushFace& g : b.faces) {
        if (g.id == faceId || g.loop.size() != 4) continue;
        int hits = 0;
        for (size_t i = 0; i < 4; ++i)
            if (ring.count(edgeKey(g.loop[i], g.loop[(i + 1) % 4]))) ++hits;
        if (hits >= 2) out.push_back(g.id);
    }
    return out;
}

std::vector<uint32_t> shortestVertexPath(const Brush& b, uint32_t from, uint32_t to) {
    if (from == to) return {from};
    const Topology t = buildTopology(b);
    const VertexEdges ve = vertexEdges(t);
    std::map<uint32_t, double> dist{{from, 0}};
    std::unordered_map<uint32_t, uint32_t> prev;
    std::unordered_set<uint32_t> done;
    // a brush has few vertices: a plain O(n^2) Dijkstra is enough
    for (;;) {
        bool found = false;
        uint32_t u = 0;
        double best = std::numeric_limits<double>::infinity();
        for (const auto& [v, d] : dist)
            if (!done.count(v) && d < best) {
                best = d;
                u = v;
                found = true;
            }
        if (!found) return {};
        if (u == to) break;
        done.insert(u);
        const auto it = ve.find(u);
        if (it == ve.end()) continue;
        for (const Edge& k : it->second) {
            const uint32_t w = k.a == u ? k.b : k.a;
            const double d = best + edgeLength(b, k);
            const auto dw = dist.find(w);
            if (d < (dw == dist.end() ? std::numeric_limits<double>::infinity() : dw->second) - 1e-12) {
                dist[w] = d;
                prev[w] = u;
            }
        }
    }
    std::vector<uint32_t> path{to};
    while (path.front() != from) path.insert(path.begin(), prev[path.front()]);
    return path;
}

std::vector<Edge> shortestEdgePath(const Brush& b, const Edge& from, const Edge& to) {
    if (from == to) return {from};
    std::vector<uint32_t> best;
    double bestLen = std::numeric_limits<double>::infinity();
    for (uint32_t a : {from.a, from.b})
        for (uint32_t c : {to.a, to.b}) {
            const std::vector<uint32_t> p = shortestVertexPath(b, a, c);
            if (p.empty()) continue;
            double len = 0;
            for (size_t i = 1; i < p.size(); ++i) len += edgeLength(b, edgeKey(p[i - 1], p[i]));
            if (len < bestLen - 1e-12) {
                bestLen = len;
                best = p;
            }
        }
    if (best.empty()) return {};
    std::vector<Edge> out{from};
    for (size_t i = 1; i < best.size(); ++i) {
        const Edge k = edgeKey(best[i - 1], best[i]);
        if (std::find(out.begin(), out.end(), k) == out.end()) out.push_back(k);
    }
    if (std::find(out.begin(), out.end(), to) == out.end()) out.push_back(to);
    return out;
}

std::vector<uint32_t> shortestFacePath(const Brush& b, uint32_t from, uint32_t to) {
    if (from == to) return {from};
    const Topology t = buildTopology(b);
    std::unordered_map<uint32_t, size_t> idx;
    for (size_t i = 0; i < b.faces.size(); ++i) idx[b.faces[i].id] = i;
    const auto s = idx.find(from), g = idx.find(to);
    if (s == idx.end() || g == idx.end()) return {};
    std::unordered_map<size_t, long long> prev{{s->second, -1}};
    std::deque<size_t> queue{s->second};
    while (!queue.empty()) {
        const size_t fi = queue.front();
        queue.pop_front();
        if (fi == g->second) break;
        const BrushFace& f = b.faces[fi];
        for (size_t i = 0; i < f.loop.size(); ++i) {
            const EdgeFaces* e = t.find(edgeKey(f.loop[i], f.loop[(i + 1) % f.loop.size()]));
            if (!e) continue;
            for (size_t n : e->faces)
                if (!prev.count(n)) {
                    prev[n] = static_cast<long long>(fi);
                    queue.push_back(n);
                }
        }
    }
    if (!prev.count(g->second)) return {};
    std::vector<size_t> path{g->second};
    while (prev[path.front()] >= 0) path.insert(path.begin(), static_cast<size_t>(prev[path.front()]));
    std::vector<uint32_t> ids;
    for (size_t i : path) ids.push_back(b.faces[i].id);
    return ids;
}

// ---- what a click selects ----
namespace {

std::vector<ElemId> elems(const MeshSel& s, SelMode mode) {
    std::vector<ElemId> out;
    if (mode == SelMode::Vertex) {
        for (uint32_t v : s.verts) out.push_back({v, {}});
    } else if (mode == SelMode::Edge) {
        for (const Edge& e : s.edges) out.push_back({0, e});
    } else {
        for (uint32_t f : s.faces) out.push_back({f, {}});
    }
    return out;
}

MeshSel selOf(const Brush& b, SelMode mode, const std::vector<ElemId>& ids) {
    if (mode == SelMode::Edge) {
        std::vector<Edge> e;
        for (const ElemId& i : ids) e.push_back(i.e);
        return selFromEdges(b, e);
    }
    std::vector<uint32_t> n;
    for (const ElemId& i : ids) n.push_back(i.n);
    return mode == SelMode::Vertex ? selFromVerts(b, n) : selFromFaces(b, n);
}

std::vector<ElemId> uniqueIds(const std::vector<ElemId>& in) {
    std::vector<ElemId> out;
    for (const ElemId& i : in)
        if (std::find(out.begin(), out.end(), i) == out.end()) out.push_back(i);
    return out;
}

} // namespace

std::optional<ClickResult> clickSelect(const Brush& b, SelMode mode, const MeshPickAt& at, const ClickMods& mods, const MeshSel& cur, const std::optional<ActiveElem>& active, bool loop) {
    std::optional<ElemId> id;
    if (mode == SelMode::Vertex && at.vertex) id = ElemId{*at.vertex, {}};
    else if (mode == SelMode::Edge && at.edge) id = ElemId{0, *at.edge};
    else if (mode == SelMode::Face && at.face) id = ElemId{*at.face, {}};

    if (loop) {
        std::vector<ElemId> ids;
        if (mode == SelMode::Face) {
            if (!at.face || !at.faceEdge) return std::nullopt;
            for (uint32_t f : faceLoop(b, *at.face, *at.faceEdge)) ids.push_back({f, {}});
        } else {
            if (!at.edge) return std::nullopt;
            const std::vector<Edge> edges = mods.ctrl ? edgeRingEdges(b, *at.edge) : edgeLoop(b, *at.edge);
            if (mode == SelMode::Edge) {
                for (const Edge& e : edges) ids.push_back({0, e});
            } else {
                std::vector<ElemId> vs;
                for (const Edge& e : edges) {
                    vs.push_back({e.a, {}});
                    vs.push_back({e.b, {}});
                }
                ids = uniqueIds(vs);
            }
        }
        std::vector<ElemId> merged = ids;
        if (mods.shift) {
            merged = elems(cur, mode);
            merged.insert(merged.end(), ids.begin(), ids.end());
            merged = uniqueIds(merged);
        }
        ClickResult r;
        r.sel = selOf(b, mode, merged);
        r.active = id ? std::optional<ActiveElem>(ActiveElem{mode, *id}) : active;
        return r;
    }
    if (!id) return std::nullopt;
    if (mods.ctrl) {
        std::vector<ElemId> path;
        if (!active || active->mode != mode) {
            path = {*id};
        } else if (mode == SelMode::Vertex) {
            for (uint32_t v : shortestVertexPath(b, active->id.n, id->n)) path.push_back({v, {}});
        } else if (mode == SelMode::Edge) {
            for (const Edge& e : shortestEdgePath(b, active->id.e, id->e)) path.push_back({0, e});
        } else {
            for (uint32_t f : shortestFacePath(b, active->id.n, id->n)) path.push_back({f, {}});
        }
        if (path.empty()) path = {*id};
        std::vector<ElemId> all = elems(cur, mode);
        all.insert(all.end(), path.begin(), path.end());
        ClickResult r;
        r.sel = selOf(b, mode, uniqueIds(all));
        r.active = ActiveElem{mode, *id};
        return r;
    }
    if (mods.shift) {
        std::vector<ElemId> have = elems(cur, mode);
        const bool on = std::find(have.begin(), have.end(), *id) != have.end();
        if (on) have.erase(std::remove(have.begin(), have.end(), *id), have.end());
        else have.push_back(*id);
        ClickResult r;
        r.sel = selOf(b, mode, have);
        if (!on) r.active = ActiveElem{mode, *id};
        return r;
    }
    ClickResult r;
    r.sel = selOf(b, mode, {*id});
    r.active = ActiveElem{mode, *id};
    return r;
}

} // namespace native::tools::mesh
