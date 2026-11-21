#include "tools/mesh/cut.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_set>

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

bool has(const std::vector<uint32_t>& v, uint32_t x) { return std::find(v.begin(), v.end(), x) != v.end(); }

// Rebuilds the brush with every face replaced by its pieces and finishes with the UV maps kept per piece.
OpResult rebuild(const Brush& b, std::vector<Vec3> vertices, const std::function<std::vector<BrushFace>(const BrushFace&, const std::function<uint32_t()>&)>& perFace,
                 const std::function<std::vector<Edge>(const Brush&)>& selEdges, const tp::InfoFn& info) {
    uint32_t next = nextFaceId(b);
    const std::function<uint32_t()> newId = [&next] { return next++; };
    std::map<uint32_t, uint32_t> splitFrom;
    Brush nb = b;
    nb.vertices = std::move(vertices);
    nb.faces.clear();
    for (const BrushFace& f : b.faces) {
        std::vector<BrushFace> pieces = perFace(f, newId);
        for (const BrushFace& p : pieces)
            if (p.id != f.id) splitFrom[p.id] = f.id;
        for (BrushFace& p : pieces) nb.faces.push_back(std::move(p));
    }
    Cleaned c = finish(nb, selFromEdges(nb, selEdges(nb)), info, &b, splitFrom);
    OpResult r;
    r.brush = std::move(c.brush);
    r.sel = std::move(c.sel);
    return r;
}

void addUnique(std::vector<Edge>& list, const Edge& e) {
    if (std::find(list.begin(), list.end(), e) == list.end()) list.push_back(e);
}

} // namespace

SplitPieces splitFace(const BrushFace& f, const std::vector<std::pair<uint32_t, uint32_t>>& chords, const std::function<uint32_t()>& newId) {
    SplitPieces out;
    out.pieces.push_back(f);
    for (const auto& [a, c] : chords) {
        size_t k = out.pieces.size();
        for (size_t x = 0; x < out.pieces.size(); ++x)
            if (has(out.pieces[x].loop, a) && has(out.pieces[x].loop, c)) {
                k = x;
                break;
            }
        if (k >= out.pieces.size()) continue;
        const BrushFace p = out.pieces[k];
        const size_t n = p.loop.size();
        const size_t i = static_cast<size_t>(std::find(p.loop.begin(), p.loop.end(), a) - p.loop.begin());
        const size_t j = static_cast<size_t>(std::find(p.loop.begin(), p.loop.end(), c) - p.loop.begin());
        if ((i + 1) % n == j || (j + 1) % n == i || i == j) continue;
        auto arc = [&](size_t from, size_t to) {
            std::vector<uint32_t> loop;
            for (size_t x = from;; x = (x + 1) % n) {
                loop.push_back(p.loop[x]);
                if (x == to) break;
            }
            return loop;
        };
        BrushFace first = p;
        first.loop = arc(i, j);
        BrushFace second = cloneFace(p, newId(), arc(j, i));
        out.pieces[k] = std::move(first);
        out.pieces.insert(out.pieces.begin() + static_cast<std::ptrdiff_t>(k) + 1, std::move(second));
        out.made.push_back({a, c});
    }
    return out;
}

OpResult connectVerts(const Brush& b, const std::vector<uint32_t>& verts, const tp::InfoFn& info) {
    const std::unordered_set<uint32_t> vs(verts.begin(), verts.end());
    if (vs.size() < 2) return fail("Connect: select two or more vertices that share a face");
    std::vector<Edge> chords;
    OpResult r = rebuild(b, b.vertices, [&](const BrushFace& f, const std::function<uint32_t()>& newId) {
        std::vector<uint32_t> at;
        for (uint32_t v : f.loop)
            if (vs.count(v)) at.push_back(v);
        if (at.size() < 2) return std::vector<BrushFace>{f};
        std::vector<std::pair<uint32_t, uint32_t>> pairs;
        if (at.size() == 2) pairs.push_back({at[0], at[1]});
        else
            for (size_t i = 0; i < at.size(); ++i) pairs.push_back({at[i], at[(i + 1) % at.size()]});
        SplitPieces s = splitFace(f, pairs, newId);
        for (const auto& [a, c] : s.made) chords.push_back(edgeKey(a, c));
        return s.pieces;
    }, [&](const Brush&) { return chords; }, info);
    if (chords.empty()) return fail("Connect: no face holds two selected vertices that are not already joined by an edge");
    return r;
}

OpResult subdivideEdges(const Brush& b, const std::vector<Edge>& edgeKeys, int cuts, const tp::InfoFn& info) {
    std::vector<Edge> es;
    for (const Edge& e : edgeKeys) addUnique(es, e);
    if (es.empty()) return fail("Subdivide: select one or more edges");
    cuts = std::max(1, std::min(64, cuts));
    std::vector<Vec3> vertices = b.vertices;
    std::map<Edge, std::vector<uint32_t>> along;   // edge -> the new vertices from its lower index to its higher
    std::vector<uint32_t> fresh;
    for (const Edge& k : es) {
        if (k.a >= b.vertices.size() || k.b >= b.vertices.size()) continue;
        std::vector<uint32_t> ids;
        for (int i = 1; i <= cuts; ++i) {
            ids.push_back(static_cast<uint32_t>(vertices.size()));
            fresh.push_back(ids.back());
            vertices.push_back(b.vertices[k.a] + (b.vertices[k.b] - b.vertices[k.a]) * (static_cast<double>(i) / (cuts + 1)));
        }
        along[k] = ids;
    }
    std::vector<Edge> chords;
    return [&] {
        OpResult r = rebuild(b, vertices, [&](const BrushFace& f, const std::function<uint32_t()>& newId) {
            std::vector<uint32_t> loop;
            std::vector<std::vector<uint32_t>> runs;
            for (size_t i = 0; i < f.loop.size(); ++i) {
                const uint32_t v = f.loop[i], w = f.loop[(i + 1) % f.loop.size()];
                loop.push_back(v);
                const auto it = along.find(edgeKey(v, w));
                if (it == along.end()) continue;
                std::vector<uint32_t> run = it->second;
                if (!(v < w)) std::reverse(run.begin(), run.end());
                loop.insert(loop.end(), run.begin(), run.end());
                runs.push_back(run);
            }
            if (runs.empty()) return std::vector<BrushFace>{f};
            BrushFace g = f;
            g.loop = loop;
            g.uvs.clear();   // re-evaluated from the face's map by finish
            std::vector<std::pair<uint32_t, uint32_t>> pairs;
            if (runs.size() == 2) {
                for (size_t i = 0; i < runs[0].size(); ++i) pairs.push_back({runs[0][i], runs[1][runs[1].size() - 1 - i]});
            } else if (runs.size() > 2 && cuts == 1) {
                for (size_t i = 0; i < runs.size(); ++i) pairs.push_back({runs[i][0], runs[(i + 1) % runs.size()][0]});
            }
            SplitPieces s = splitFace(g, pairs, newId);
            for (const auto& [a, c] : s.made) chords.push_back(edgeKey(a, c));
            return s.pieces;
        }, [&](const Brush& nb) {
            if (!chords.empty()) return chords;
            const std::unordered_set<uint32_t> freshSet(fresh.begin(), fresh.end());
            std::vector<uint32_t> all = fresh;
            for (const Edge& e : es) {
                all.push_back(e.a);
                all.push_back(e.b);
            }
            std::vector<Edge> out;
            for (const Edge& e : selFromVerts(nb, all).edges)
                if (freshSet.count(e.a) || freshSet.count(e.b)) out.push_back(e);
            return out;
        }, info);
        return r;
    }();
}

Vec3 knifePos(const Brush& b, const KnifePoint& p) {
    if (p.onVertex) return b.vertices[p.vertex];
    return b.vertices[p.edge.a] + (b.vertices[p.edge.b] - b.vertices[p.edge.a]) * p.t;
}

OpResult knifeCut(const Brush& b, const std::vector<KnifePoint>& points, const tp::InfoFn& info) {
    if (points.size() < 2) return fail("Knife: click at least two points on edges or vertices");
    std::vector<Vec3> vertices = b.vertices;
    struct Cut {
        double t;
        uint32_t v;
    };
    std::map<Edge, std::vector<Cut>> onEdge;   // edge -> the new vertices sorted by t
    std::vector<uint32_t> ids;
    for (const KnifePoint& p : points) {
        if (p.onVertex) {
            ids.push_back(p.vertex);
            continue;
        }
        const double t = std::min(1.0, std::max(0.0, p.t));
        if (t < 1e-6) {
            ids.push_back(p.edge.a);
            continue;
        }
        if (t > 1 - 1e-6) {
            ids.push_back(p.edge.b);
            continue;
        }
        std::vector<Cut>& list = onEdge[p.edge];
        auto hit = std::find_if(list.begin(), list.end(), [&](const Cut& x) { return std::abs(x.t - t) < 1e-6; });
        if (hit == list.end()) {
            list.push_back({t, static_cast<uint32_t>(vertices.size())});
            vertices.push_back(knifePos(b, KnifePoint::onEdge(p.edge, t)));
            std::sort(list.begin(), list.end(), [](const Cut& x, const Cut& y) { return x.t < y.t; });
            hit = std::find_if(list.begin(), list.end(), [&](const Cut& x) { return std::abs(x.t - t) < 1e-6; });
        }
        ids.push_back(hit->v);
    }
    std::vector<Edge> chords;
    OpResult r = rebuild(b, vertices, [&](const BrushFace& f, const std::function<uint32_t()>& newId) {
        std::vector<uint32_t> loop;
        for (size_t i = 0; i < f.loop.size(); ++i) {
            const uint32_t v = f.loop[i], w = f.loop[(i + 1) % f.loop.size()];
            loop.push_back(v);
            const auto it = onEdge.find(edgeKey(v, w));
            if (it == onEdge.end()) continue;
            std::vector<Cut> list = it->second;
            if (!(v < w)) std::reverse(list.begin(), list.end());
            for (const Cut& c : list) loop.push_back(c.v);
        }
        BrushFace g = f;
        g.loop = loop;
        if (loop.size() != f.loop.size()) g.uvs.clear();   // re-evaluated from the face's map by finish
        std::vector<std::pair<uint32_t, uint32_t>> pairs;
        for (size_t i = 1; i < ids.size(); ++i)
            if (has(loop, ids[i - 1]) && has(loop, ids[i]) && ids[i - 1] != ids[i]) pairs.push_back({ids[i - 1], ids[i]});
        if (pairs.empty()) return std::vector<BrushFace>{loop.size() == f.loop.size() ? f : g};
        SplitPieces s = splitFace(g, pairs, newId);
        for (const auto& [a, c] : s.made) chords.push_back(edgeKey(a, c));
        return s.pieces;
    }, [&](const Brush&) { return chords; }, info);
    if (chords.empty()) return fail("Knife: no two consecutive points lie on the same face across it");
    return r;
}

} // namespace native::tools::mesh
