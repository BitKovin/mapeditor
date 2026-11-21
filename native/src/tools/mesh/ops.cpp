#include "tools/mesh/ops.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "tools/transform.h"

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

std::vector<size_t> faceIdx(const Brush& b, const std::vector<uint32_t>& ids) {
    std::vector<size_t> out;
    for (uint32_t id : ids)
        for (size_t i = 0; i < b.faces.size(); ++i)
            if (b.faces[i].id == id) {
                out.push_back(i);
                break;
            }
    return out;
}

void reverseFace(BrushFace& f) {
    std::reverse(f.loop.begin(), f.loop.end());
    std::reverse(f.uvs.begin(), f.uvs.end());
}

} // namespace

// ---- fans and solids ----
std::vector<FanEntry> fanOf(const Brush& b, const Topology& t, uint32_t v) {
    const auto vf = t.vertexFaces.find(v);
    if (vf == t.vertexFaces.end() || vf->second.empty()) return {};
    const size_t start = vf->second.front();
    std::vector<FanEntry> out;
    size_t fi = start;
    for (int guard = 0; guard < 1000; ++guard) {
        const BrushFace& f = b.faces[fi];
        const size_t n = f.loop.size();
        const size_t i = static_cast<size_t>(std::find(f.loop.begin(), f.loop.end(), v) - f.loop.begin());
        const uint32_t inV = f.loop[(i + n - 1) % n], outV = f.loop[(i + 1) % n];
        out.push_back({fi, inV, outV});
        const auto g = t.half.find(Topology::halfKey(outV, v));
        if (g == t.half.end()) return {};
        if (g->second == start) return out.size() == vf->second.size() ? out : std::vector<FanEntry>();
        fi = g->second;
    }
    return {};
}

bool isProperSolid(const Brush& b) {
    if (!isClosedManifold(b)) return false;
    const Topology t = buildTopology(b);
    for (const auto& [v, faces] : t.vertexFaces) {
        (void)faces;
        if (fanOf(b, t, v).empty()) return false;
    }
    return true;
}

// ---- transform ----
OpResult transformVerts(const Brush& b, const std::vector<uint32_t>& verts, const PointFn& fn, bool lockUv, const tp::InfoFn& info) {
    const std::unordered_set<uint32_t> vs(verts.begin(), verts.end());
    if (vs.empty()) return fail("Nothing selected");
    Brush nb = b;
    for (size_t i = 0; i < nb.vertices.size(); ++i)
        if (vs.count(static_cast<uint32_t>(i))) nb.vertices[i] = fn(b.vertices[i], static_cast<uint32_t>(i));
    return done(finish(nb, selFromVerts(nb, verts), info, lockUv ? &b : nullptr));
}

// ---- extrude ----
OpResult extrudeFaces(const Brush& b, const std::vector<uint32_t>& faceIds, double dist, const std::optional<Vec3>& along, const tp::InfoFn& info) {
    const MeshSel sel = selFromFaces(b, faceIds);
    if (sel.faces.empty()) return fail("Select at least one face to extrude");
    if (along ? along->length() < 1e-9 : std::abs(dist) < 1e-9) return same(b, sel);
    const Topology t = buildTopology(b);
    std::vector<size_t> order = faceIdx(b, sel.faces);
    std::unordered_set<size_t> inSel(order.begin(), order.end());
    std::vector<const BrushFace*> region;
    for (size_t i : order) region.push_back(&b.faces[i]);
    const Vec3 dir = averageNormal(b, region);
    if (dir.length() < 0.5) return fail("The selected faces face opposite ways; extrude them one region at a time");

    Brush nb = b;
    std::unordered_map<uint32_t, uint32_t> dup;
    for (const BrushFace* f : region)
        for (uint32_t v : f->loop)
            if (!dup.count(v)) {
                dup[v] = static_cast<uint32_t>(nb.vertices.size());
                nb.vertices.push_back(b.vertices[v] + (along ? *along : dir * dist));
            }
    for (size_t fi : inSel)
        for (uint32_t& v : nb.faces[fi].loop) v = dup[v];
    uint32_t next = nextFaceId(b);
    for (size_t fi : order) {
        const BrushFace& f = b.faces[fi];
        for (size_t i = 0; i < f.loop.size(); ++i) {
            const uint32_t a = f.loop[i], c = f.loop[(i + 1) % f.loop.size()];
            const auto across = t.half.find(Topology::halfKey(c, a));
            if (across != t.half.end() && inSel.count(across->second)) continue;   // interior edge of the region
            // the side wall takes its look from the face it grows out of (the world aligned projection keeps textures continuous)
            const BrushFace& source = across != t.half.end() ? b.faces[across->second] : f;
            nb.faces.push_back(cloneFace(source, next++, {a, c, dup[c], dup[a]}));
        }
    }
    return done(finish(nb, selFromFaces(nb, sel.faces), info));
}

// ---- inset ----
OpResult insetFaces(const Brush& b, const std::vector<uint32_t>& faceIds, double amount, const tp::InfoFn& info) {
    const MeshSel sel = selFromFaces(b, faceIds);
    if (sel.faces.empty()) return fail("Select at least one face to inset");
    if (std::abs(amount) < 1e-9) return same(b, sel);
    if (amount < 0) return fail("Inset amount must be positive");
    Brush nb = b;
    nb.faces.clear();
    uint32_t next = nextFaceId(b);
    std::vector<uint32_t> inner;
    const std::unordered_set<uint32_t> selected(sel.faces.begin(), sel.faces.end());
    for (const BrushFace& f : b.faces) {
        if (!selected.count(f.id)) {
            nb.faces.push_back(f);
            continue;
        }
        const size_t n = f.loop.size();
        const Vec3 N = b.faceNormal(f);
        std::vector<uint32_t> ring;
        for (size_t i = 0; i < n; ++i) {
            const Vec3& p = b.vertices[f.loop[(i + n - 1) % n]];
            const Vec3& v = b.vertices[f.loop[i]];
            const Vec3& q = b.vertices[f.loop[(i + 1) % n]];
            const Vec3 n1 = N.cross((v - p).normalized()), n2 = N.cross((q - v).normalized());
            const double k = 1 + n1.dot(n2);
            if (k < 1e-6) return fail("Inset failed: a corner is too sharp");
            ring.push_back(static_cast<uint32_t>(nb.vertices.size()));
            nb.vertices.push_back(v + (n1 + n2) * (amount / k));
        }
        // the inner polygon must keep the orientation and edge directions of the border, or the inset overshot
        for (size_t i = 0; i < n; ++i) {
            const size_t j = (i + 1) % n;
            const Vec3 e0 = b.vertices[f.loop[j]] - b.vertices[f.loop[i]], e1 = nb.vertices[ring[j]] - nb.vertices[ring[i]];
            if (e0.dot(e1) <= 1e-9) return fail("Inset is larger than the face");
        }
        BrushFace innerFace = f;
        innerFace.loop = ring;
        nb.faces.push_back(innerFace);
        inner.push_back(f.id);
        for (size_t i = 0; i < n; ++i) {
            const size_t j = (i + 1) % n;
            nb.faces.push_back(cloneFace(f, next++, {f.loop[i], f.loop[j], ring[j], ring[i]}));
        }
    }
    return done(finish(nb, selFromFaces(nb, inner), info));
}

// ---- merge ----
namespace {

OpResult weld(const Brush& b, const std::vector<std::vector<uint32_t>>& groups, const std::optional<Vec3>& at, const tp::InfoFn& info) {
    std::unordered_map<uint32_t, uint32_t> target;
    Brush nb = b;
    std::vector<uint32_t> reps;
    for (const auto& g : groups) {
        const uint32_t rep = g.front();
        reps.push_back(rep);
        nb.vertices[rep] = at ? *at : centroid(b, g);
        for (uint32_t v : g) target[v] = rep;
    }
    nb.faces.clear();
    for (const BrushFace& f : b.faces) {
        std::vector<uint32_t> loop;
        std::vector<core::Vec2> uvs;
        const bool hasUvs = !f.uvs.empty();
        for (size_t i = 0; i < f.loop.size(); ++i) {
            const auto it = target.find(f.loop[i]);
            const uint32_t nv = it != target.end() ? it->second : f.loop[i];
            if (!loop.empty() && loop.back() == nv) continue;   // a consecutive duplicate collapses
            loop.push_back(nv);
            if (hasUvs && i < f.uvs.size()) uvs.push_back(f.uvs[i]);
        }
        if (loop.size() > 1 && loop.front() == loop.back()) {
            loop.pop_back();
            if (!uvs.empty()) uvs.pop_back();
        }
        if (loop.size() < 3) continue;   // the face collapsed
        if (std::set<uint32_t>(loop.begin(), loop.end()).size() != loop.size()) return fail("Merge would pinch a face; merge neighbouring vertices only");
        BrushFace nf = f;
        nf.loop = loop;
        if (hasUvs) nf.uvs = uvs;
        nb.faces.push_back(std::move(nf));
    }
    if (isProperSolid(b) && !isProperSolid(nb)) return fail("Merge would leave holes or pinch the surface");
    return done(finish(nb, selFromVerts(nb, reps), info));
}

std::vector<uint32_t> uniq(const std::vector<uint32_t>& v) {
    std::vector<uint32_t> out;
    std::unordered_set<uint32_t> seen;
    for (uint32_t x : v)
        if (seen.insert(x).second) out.push_back(x);
    return out;
}

} // namespace

OpResult mergeVerts(const Brush& b, const std::vector<uint32_t>& verts, const std::optional<Vec3>& at, const tp::InfoFn& info) {
    const std::vector<uint32_t> vs = uniq(verts);
    if (vs.size() < 2) return fail("Select at least two vertices to merge");
    return weld(b, {vs}, at, info);
}

OpResult mergeByDistance(const Brush& b, const std::vector<uint32_t>& verts, double distance, const tp::InfoFn& info) {
    const std::vector<uint32_t> vs = uniq(verts);
    std::unordered_map<uint32_t, uint32_t> parent;
    for (uint32_t v : vs) parent[v] = v;
    auto find = [&](uint32_t v) {
        while (parent[v] != v) v = parent[v];
        return v;
    };
    for (size_t i = 0; i < vs.size(); ++i)
        for (size_t j = i + 1; j < vs.size(); ++j)
            if ((b.vertices[vs[i]] - b.vertices[vs[j]]).length() <= distance) parent[find(vs[j])] = find(vs[i]);
    std::vector<uint32_t> roots;
    std::unordered_map<uint32_t, std::vector<uint32_t>> groups;
    for (uint32_t v : vs) {
        const uint32_t r = find(v);
        if (!groups.count(r)) roots.push_back(r);
        groups[r].push_back(v);
    }
    std::vector<std::vector<uint32_t>> clusters;
    for (uint32_t r : roots)
        if (groups[r].size() > 1) clusters.push_back(groups[r]);
    if (clusters.empty()) return fail("No vertices closer than that distance");
    return weld(b, clusters, std::nullopt, info);
}

// ---- dissolve ----
OpResult dissolveEdges(const Brush& b, const std::vector<Edge>& edgeKeys, const tp::InfoFn& info) {
    std::vector<Edge> es;
    {
        std::set<Edge> seen;
        for (const Edge& e : edgeKeys)
            if (seen.insert(e).second) es.push_back(e);
    }
    if (es.empty()) return fail("Select at least one edge to dissolve");
    const Topology t = buildTopology(b);
    std::vector<size_t> parent(b.faces.size());
    for (size_t i = 0; i < parent.size(); ++i) parent[i] = i;
    auto find = [&](size_t i) {
        while (parent[i] != i) i = parent[i] = parent[parent[i]];
        return i;
    };
    for (const Edge& k : es) {
        const EdgeFaces* e = t.find(k);
        if (!e || e->faces.size() != 2) return fail("Dissolve: an edge must lie between two faces");
        parent[find(e->faces[0])] = find(e->faces[1]);
    }
    std::vector<size_t> roots;
    std::unordered_map<size_t, std::vector<size_t>> groups;
    for (size_t i = 0; i < b.faces.size(); ++i) {
        const size_t r = find(i);
        if (!groups.count(r)) roots.push_back(r);
        groups[r].push_back(i);
    }
    Brush nb = b;
    nb.faces.clear();
    std::vector<uint32_t> survivors;
    for (size_t root : roots) {
        std::vector<size_t>& g = groups[root];
        if (g.size() == 1) {
            nb.faces.push_back(b.faces[g[0]]);
            continue;
        }
        // the boundary of the group: directed edges whose reverse is not in the group
        const std::unordered_set<size_t> inGroup(g.begin(), g.end());
        std::unordered_map<uint32_t, uint32_t> next;
        bool haveStart = false;
        uint32_t start = 0;
        for (size_t fi : g) {
            const BrushFace& f = b.faces[fi];
            for (size_t i = 0; i < f.loop.size(); ++i) {
                const uint32_t a = f.loop[i], c = f.loop[(i + 1) % f.loop.size()];
                const auto other = t.half.find(Topology::halfKey(c, a));
                if (other != t.half.end() && inGroup.count(other->second)) continue;   // an interior edge of the merged region
                if (!haveStart) {
                    haveStart = true;
                    start = a;
                }
                next[a] = c;
            }
        }
        std::vector<uint32_t> loop;
        uint32_t cur = start;
        for (int guard = 0; guard < 100000; ++guard) {
            loop.push_back(cur);
            const auto nx = next.find(cur);
            if (nx == next.end()) return fail("Dissolve would leave a hole in the face");
            cur = nx->second;
            if (cur == start) break;
        }
        if (loop.size() != next.size()) return fail("Dissolve would create a face with a hole or two separate pieces");
        if (loop.size() < 3) return fail("Dissolve would remove the whole surface");
        std::sort(g.begin(), g.end(), [&](size_t x, size_t y) { return b.faces[x].id < b.faces[y].id; });
        const BrushFace& keep = b.faces[g.front()];
        nb.faces.push_back(cloneFace(keep, keep.id, loop));
        survivors.push_back(keep.id);
    }
    return done(finish(nb, selFromFaces(nb, survivors), info));
}

OpResult dissolveVerts(const Brush& b, const std::vector<uint32_t>& verts, const tp::InfoFn& info) {
    const std::vector<uint32_t> vs = uniq(verts);
    if (vs.empty()) return fail("Select at least one vertex to dissolve");
    Brush cur = b;
    std::vector<Edge> merge;
    for (uint32_t v : vs) {
        const Topology t = buildTopology(b);
        std::vector<Edge> edgesAt;
        for (const EdgeFaces& e : t.edgeList)
            if (e.a == v || e.b == v) edgesAt.push_back(Edge{e.a, e.b});
        if (edgesAt.size() == 2) {
            for (BrushFace& f : cur.faces) {
                if (std::find(f.loop.begin(), f.loop.end(), v) == f.loop.end()) continue;
                std::vector<uint32_t> loop;
                std::vector<core::Vec2> uvs;
                for (size_t i = 0; i < f.loop.size(); ++i)
                    if (f.loop[i] != v) {
                        loop.push_back(f.loop[i]);
                        if (!f.uvs.empty()) uvs.push_back(f.uvs[i]);
                    }
                f.loop = loop;
                if (!f.uvs.empty()) f.uvs = uvs;
            }
        } else {
            merge.insert(merge.end(), edgesAt.begin(), edgesAt.end());
        }
    }
    if (!merge.empty()) {
        OpResult r = dissolveEdges(cur, merge, info);
        if (!r.ok()) return r;
        r.sel.verts.clear();
        r.sel.edges.clear();
        return r;
    }
    return done(finish(cur, MeshSel{}, info));
}

OpResult dissolveFaces(const Brush& b, const std::vector<uint32_t>& faceIds, const tp::InfoFn& info) {
    const std::unordered_set<uint32_t> ids(faceIds.begin(), faceIds.end());
    if (ids.size() < 2) return fail("Select at least two faces to dissolve into one");
    const Topology t = buildTopology(b);
    std::vector<Edge> inner;
    for (const EdgeFaces& e : t.edgeList)
        if (e.faces.size() == 2 && ids.count(b.faces[e.faces[0]].id) && ids.count(b.faces[e.faces[1]].id)) inner.push_back(Edge{e.a, e.b});
    if (inner.empty()) return fail("The selected faces do not touch");
    return dissolveEdges(b, inner, info);
}

// ---- delete / fill ----
OpResult deleteFaces(const Brush& b, const std::vector<uint32_t>& faceIds, const tp::InfoFn& info) {
    const std::unordered_set<uint32_t> ids(faceIds.begin(), faceIds.end());
    if (ids.empty()) return fail("Nothing selected");
    Brush nb = b;
    nb.faces.clear();
    for (const BrushFace& f : b.faces)
        if (!ids.count(f.id)) nb.faces.push_back(f);
    if (nb.faces.empty()) return fail("That would delete every face of the brush; delete the brush instead");
    return done(finish(nb, MeshSel{}, info));
}

LoopsClosed closeOpenLoops(const Brush& b, const std::function<bool(uint32_t)>& include, uint32_t firstId, const BrushFace* tmpl) {
    LoopsClosed out;
    const Topology t = buildTopology(b);
    // the reverse-walk graph: vertex -> next vertices along the missing face (the half edges in the order the faces list them)
    std::vector<uint32_t> order;
    std::unordered_map<uint32_t, std::vector<uint32_t>> open;
    for (const BrushFace& f : b.faces) {
        for (size_t i = 0; i < f.loop.size(); ++i) {
            const uint32_t a = f.loop[i], c = f.loop[(i + 1) % f.loop.size()];
            if (t.half.count(Topology::halfKey(c, a)) || !include(a) || !include(c)) continue;
            // the missing face walks c -> a
            if (!open.count(c)) order.push_back(c);
            open[c].push_back(a);
        }
    }
    out.brush = b;
    uint32_t id = firstId;
    size_t head = 0;
    while (true) {
        while (head < order.size() && !open.count(order[head])) ++head;
        if (head >= order.size()) break;
        const uint32_t start = order[head];
        std::vector<uint32_t> loop{start};
        uint32_t cur = start;
        for (int guard = 0; guard < 10000; ++guard) {
            const auto it = open.find(cur);
            if (it == open.end() || it->second.empty()) {
                out.error = "Could not close a hole: the open edges do not form a loop";
                return out;
            }
            const uint32_t nx = it->second.back();
            it->second.pop_back();
            if (it->second.empty()) open.erase(it);
            if (nx == start) break;
            loop.push_back(nx);
            cur = nx;
        }
        if (loop.size() < 3) {
            out.error = "Could not close a hole: it has fewer than 3 corners";
            return out;
        }
        // the texture comes from a neighbour of the hole
        const BrushFace* neighbour = tmpl;
        if (!neighbour) {
            auto h = t.half.find(Topology::halfKey(loop[1], loop[0]));
            if (h == t.half.end()) h = t.half.find(Topology::halfKey(loop[0], loop.back()));
            neighbour = &b.faces[h != t.half.end() ? h->second : 0];
        }
        out.brush.faces.push_back(cloneFace(*neighbour, id, loop));
        out.created.push_back(id++);
    }
    return out;
}

OpResult fillHole(const Brush& b, const std::vector<uint32_t>& verts, const tp::InfoFn& info) {
    const std::unordered_set<uint32_t> vs(verts.begin(), verts.end());
    if (vs.size() < 3) return fail("Select the vertices around a hole");
    LoopsClosed r = closeOpenLoops(b, [&](uint32_t v) { return vs.count(v) != 0; }, nextFaceId(b));
    if (!r.error.empty()) return fail(r.error);
    if (r.created.empty()) return fail("No open edges among the selected vertices");
    return done(finish(r.brush, selFromFaces(r.brush, r.created), info));
}

// ---- normals ----
OpResult flipFaces(const Brush& b, const std::vector<uint32_t>& faceIds) {
    const std::unordered_set<uint32_t> ids(faceIds.begin(), faceIds.end());
    if (ids.empty()) return fail("Select faces to flip");
    Brush nb = b;
    for (BrushFace& f : nb.faces)
        if (ids.count(f.id)) reverseFace(f);
    OpResult r;
    r.brush = nb;
    r.sel = selFromFaces(nb, faceIds);
    return r;
}

OpResult recalcNormals(const Brush& b, bool outward) {
    const Topology t = buildTopology(b);
    std::vector<bool> flip(b.faces.size(), false), seen(b.faces.size(), false);
    for (size_t s = 0; s < b.faces.size(); ++s) {
        if (seen[s]) continue;
        seen[s] = true;
        std::vector<size_t> queue{s};
        while (!queue.empty()) {
            const size_t fi = queue.back();
            queue.pop_back();
            std::vector<uint32_t> loop = b.faces[fi].loop;
            if (flip[fi]) std::reverse(loop.begin(), loop.end());
            for (size_t i = 0; i < loop.size(); ++i) {
                const uint32_t a = loop[i], c = loop[(i + 1) % loop.size()];
                const EdgeFaces* e = t.find(edgeKey(a, c));
                if (!e) continue;
                for (size_t gi : e->faces) {
                    if (gi == fi || seen[gi]) continue;
                    // consistent means the neighbour walks c -> a; if it walks a -> c it must be flipped
                    const auto& walked = b.faces[gi].loop;
                    const size_t j = static_cast<size_t>(std::find(walked.begin(), walked.end(), a) - walked.begin());
                    const bool forward = walked[(j + 1) % walked.size()] == c;
                    flip[gi] = forward;
                    seen[gi] = true;
                    queue.push_back(gi);
                }
            }
        }
    }
    Brush nb = b;
    for (size_t i = 0; i < nb.faces.size(); ++i)
        if (flip[i]) reverseFace(nb.faces[i]);
    double vol = 0;
    for (const BrushFace& f : nb.faces) {
        const Vec3& a = nb.vertices[f.loop[0]];
        for (size_t i = 1; i + 1 < f.loop.size(); ++i) vol += a.dot(nb.vertices[f.loop[i]].cross(nb.vertices[f.loop[i + 1]])) / 6;
    }
    if ((vol < 0) == outward)
        for (BrushFace& f : nb.faces) reverseFace(f);
    std::vector<uint32_t> ids;
    for (const BrushFace& f : nb.faces) ids.push_back(f.id);
    OpResult r;
    r.sel = selFromFaces(nb, ids);
    r.brush = std::move(nb);
    return r;
}

// ---- mirror ----
OpResult mirrorVerts(const Brush& b, const std::vector<uint32_t>& verts, int axis, bool lockUv, const tp::InfoFn& info) {
    const std::vector<uint32_t> vs = uniq(verts);
    if (vs.empty()) return fail("Mirror: select something first");
    const Vec3 c = centroid(b, vs);
    const double cc = axis == 0 ? c.x : axis == 1 ? c.y : c.z;
    auto map = [&](const Vec3& p, uint32_t) {
        Vec3 q = p;
        double& comp = axis == 0 ? q.x : axis == 1 ? q.y : q.z;
        const double pv = axis == 0 ? p.x : axis == 1 ? p.y : p.z;
        comp = 2 * cc - pv;
        return q;
    };
    if (vs.size() == b.vertices.size()) {
        V3 f{1, 1, 1};
        f[static_cast<size_t>(axis)] = -1;
        const Brush nb = mapBrushPoints(b, scaleAbout(V3{c.x, c.y, c.z}, f), true, lockUv);
        OpResult r;
        r.brush = nb;
        r.sel = selFromVerts(nb, vs);
        return r;
    }
    OpResult r = transformVerts(b, vs, map, lockUv, info);
    if (!r.ok()) return r;
    if (isProperSolid(b) && (!isProperSolid(r.brush) || r.brush.signedVolume() <= 0)) return fail("Mirror would turn the brush inside out; mirror the whole brush or fewer vertices");
    return r;
}

// ---- proportional editing ----
double falloff(double d, double radius) {
    if (!(radius > 0) || d >= radius) return 0;
    const double t = 1 - d / radius;
    return t * t * (3 - 2 * t);
}

OpResult proportionalVerts(const Brush& b, const std::vector<uint32_t>& verts, const PointFn& fn, double radius, bool lockUv, const tp::InfoFn& info) {
    const std::unordered_set<uint32_t> sel(verts.begin(), verts.end());
    if (sel.empty()) return fail("Nothing selected");
    std::map<uint32_t, double> w;
    for (size_t i = 0; i < b.vertices.size(); ++i) {
        const uint32_t vi = static_cast<uint32_t>(i);
        if (sel.count(vi)) {
            w[vi] = 1;
            continue;
        }
        double d = std::numeric_limits<double>::infinity();
        for (uint32_t j : sel) d = std::min(d, (b.vertices[i] - b.vertices[j]).length());
        const double k = falloff(d, radius);
        if (k > 0) w[vi] = k;
    }
    std::vector<uint32_t> keys;
    for (const auto& [v, k] : w) {
        (void)k;
        keys.push_back(v);
    }
    OpResult r = transformVerts(b, keys, [&](const Vec3& p, uint32_t i) {
        const double k = w[i];
        const Vec3 q = fn(p, i);
        return k == 1 ? q : p + (q - p) * k;
    }, lockUv, info);
    if (!r.ok()) return r;
    r.sel = selFromVerts(r.brush, verts);
    return r;
}

} // namespace native::tools::mesh
