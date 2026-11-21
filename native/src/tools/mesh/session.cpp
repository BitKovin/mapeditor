// tools/mesh/session -- see session.h.
#include "tools/mesh/session.h"

#include <core/entity_class.h>

#include <algorithm>
#include <map>
#include <set>

#include "doc/edit.h"
#include "tools/box_tool.h"
#include "tools/mesh/cut.h"

namespace native::tools::mesh {

using core::Brush;
using core::BrushFace;
using core::Vec3;

std::optional<MeshTarget> meshTarget(const doc::DocumentVersion& d, const doc::SelRef& primary) {
    const doc::SelRef sel = doc::soleBrushOf(d, primary);   // an entity of one brush is edited through that brush
    if (!sel.instancePath.empty() || sel.entityId.empty() || sel.brushId.empty()) return std::nullopt;
    const int ei = d.findEntity(sel.entityId);
    const int bi = d.findBrush(sel.entityId, sel.brushId);
    if (ei < 0 || bi < 0) return std::nullopt;
    MeshTarget t;
    t.entityId = sel.entityId;
    t.brushId = sel.brushId;
    t.classname = d.entities[static_cast<size_t>(ei)]->typed.classname;
    t.node = d.entities[static_cast<size_t>(ei)]->brushes[static_cast<size_t>(bi)];
    return t;
}

doc::MeshSel toDocSel(const Brush& b, const MeshSel& s) {
    doc::MeshSel out;
    out.vertices = s.verts;
    out.faces = s.faces;
    if (!s.edges.empty()) {
        const Topology t = buildTopology(b);
        for (const Edge& e : s.edges) {
            const auto it = t.edgeIndex.find(e);
            if (it != t.edgeIndex.end()) out.edges.push_back(static_cast<uint32_t>(it->second));
        }
    }
    return out;
}

MeshSel fromDocSel(const Brush& b, const doc::MeshSel& s) {
    MeshSel out;
    out.verts = s.vertices;
    out.faces = s.faces;
    if (!s.edges.empty()) {
        const Topology t = buildTopology(b);
        for (uint32_t i : s.edges)
            if (i < t.edgeList.size()) out.edges.push_back(Edge{t.edgeList[i].a, t.edgeList[i].b});
    }
    return out;
}

doc::MeshElement toDocElement(SelMode m) {
    return m == SelMode::Vertex ? doc::MeshElement::Vertex : m == SelMode::Edge ? doc::MeshElement::Edge : doc::MeshElement::Face;
}
SelMode fromDocElement(doc::MeshElement e) {
    return e == doc::MeshElement::Vertex ? SelMode::Vertex : e == doc::MeshElement::Edge ? SelMode::Edge : SelMode::Face;
}

std::optional<std::string> brushProblem(const std::string& classname, const Brush& b) {
    const bool convex = core::EntityClassRegistry::requiresConvexBrushes(classname) && b.op != core::BrushOp::Subtract;
    return convex ? b.checkConvexSolid() : b.checkStructure();
}

namespace {

bool sameGeometry(const Brush& a, const Brush& b) {
    if (a.vertices.size() != b.vertices.size() || a.faces.size() != b.faces.size() || a.smoothEdges != b.smoothEdges) return false;
    for (size_t i = 0; i < a.vertices.size(); ++i)
        if (a.vertices[i].x != b.vertices[i].x || a.vertices[i].y != b.vertices[i].y || a.vertices[i].z != b.vertices[i].z) return false;
    for (size_t i = 0; i < a.faces.size(); ++i)
        if (a.faces[i].id != b.faces[i].id || a.faces[i].loop != b.faces[i].loop) return false;
    return true;
}

MeshApply refuse(const std::string& label, const std::string& error) {
    MeshApply r;
    r.label = label;
    r.error = error;
    return r;
}

} // namespace

MeshApply applyMeshOp(const std::shared_ptr<const doc::DocumentVersion>& base, const doc::SelRef& target, const std::string& label, const MeshOpFn& op, const MeshSel& sel,
                      bool autoDetail, const std::string& detailId) {
    if (!base) return refuse(label, "no document");
    const std::optional<MeshTarget> t = meshTarget(*base, target);
    if (!t) return refuse(label, "Select a brush first (click it), then switch to mesh mode to edit its mesh");
    OpResult r = op(t->brush(), sel);
    if (!r.ok()) return refuse(label, r.error);
    const std::optional<std::string> problem = brushProblem(t->classname, r.brush);
    MeshApply out;
    out.label = label;
    out.sel = cleanSel(r.brush, r.sel);
    if (problem) {
        // concave in a class that needs convex solids: a valid solid goes into func_detail instead (when the setting allows)
        if (!autoDetail || r.brush.checkStructure()) return refuse(label, label + ": " + *problem);
        const std::string detail = detailId.empty() ? newId("e") : detailId;
        doc::VersionEditor ed(base);
        ed.removeBrush(t->entityId, t->brushId);
        if (base->findEntity(detail) >= 0) {
            ed.addBrush(detail, r.brush, t->node->raw);
        } else {
            core::Entity e;
            e.id = detail;
            e.classname = "func_detail";
            e.brushes.push_back(r.brush);
            Vec3 lo = r.brush.vertices.front(), hi = lo;
            for (const Vec3& v : r.brush.vertices) {
                lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
                hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
            }
            e.origin = {(lo.x + hi.x) / 2, (lo.y + hi.y) / 2, (lo.z + hi.z) / 2};
            ed.addEntity(e, doc::Json(), {t->node->raw});
        }
        out.version = ed.finish();
        if (!out.version) return refuse(label, ed.error());
        out.select = doc::SelRef::brush(detail, t->brushId);
        out.movedToDetail = true;
        out.message = "The brush is concave, so it moved into a new func_detail entity";
        return out;
    }
    if (sameGeometry(r.brush, t->brush())) return refuse(label, "");   // nothing changed
    doc::VersionEditor ed(base);
    ed.replaceBrush(t->entityId, r.brush);
    out.version = ed.finish();
    if (!out.version) return refuse(label, ed.error());
    out.select = doc::SelRef::brush(t->entityId, t->brushId);
    return out;
}

// ---- smooth edges ----

namespace {

std::pair<uint32_t, uint32_t> orderedPair(uint32_t a, uint32_t b) { return a < b ? std::make_pair(a, b) : std::make_pair(b, a); }

} // namespace

Brush markSmooth(const Brush& b, const std::vector<Edge>& edges, bool on) {
    const Topology t = buildTopology(b);
    std::set<std::pair<uint32_t, uint32_t>> meets;
    for (const EdgeFaces& e : t.edgeList)
        if (e.faces.size() == 2) meets.insert(orderedPair(b.faces[e.faces[0]].id, b.faces[e.faces[1]].id));
    std::set<std::pair<uint32_t, uint32_t>> cur;
    for (const auto& p : b.smoothEdges) {
        const auto k = orderedPair(p.first, p.second);
        if (meets.count(k)) cur.insert(k);
    }
    for (const Edge& e : edges) {
        const EdgeFaces* f = t.find(e);
        if (!f || f->faces.size() != 2) continue;
        const auto k = orderedPair(b.faces[f->faces[0]].id, b.faces[f->faces[1]].id);
        if (on) cur.insert(k);
        else cur.erase(k);
    }
    const std::vector<std::pair<uint32_t, uint32_t>> next(cur.begin(), cur.end());   // a set of ordered pairs is sorted already
    if (next == b.smoothEdges) return b;
    Brush out = b;
    out.smoothEdges = next;
    return out;
}

// ---- the instant tools ----

namespace {

OpResult failed(const std::string& message) {
    OpResult r;
    r.error = message;
    return r;
}

std::vector<uint32_t> facesTouching(const Brush& b, const MeshSel& s, SelMode mode) {
    std::vector<uint32_t> ids;
    if (mode == SelMode::Face) return s.faces;
    for (const BrushFace& f : b.faces) {
        bool hit = false;
        for (size_t i = 0; i < f.loop.size() && !hit; ++i) {
            if (mode == SelMode::Vertex) hit = std::find(s.verts.begin(), s.verts.end(), f.loop[i]) != s.verts.end();
            else hit = std::find(s.edges.begin(), s.edges.end(), edgeKey(f.loop[i], f.loop[(i + 1) % f.loop.size()])) != s.edges.end();
        }
        if (hit) ids.push_back(f.id);
    }
    return ids;
}

} // namespace

OpResult runMeshTool(const std::string& id, const Brush& b, const MeshSel& sel, SelMode mode, double param, const tp::InfoFn& info) {
    if (id == "connect") return connectVerts(b, sel.verts, info);
    if (id == "merge") return mergeVerts(b, sel.verts, std::nullopt, info);
    if (id == "fill") return fillHole(b, sel.verts, info);
    if (id == "dissolve")
        return mode == SelMode::Vertex ? dissolveVerts(b, sel.verts, info) : mode == SelMode::Edge ? dissolveEdges(b, sel.edges, info) : dissolveFaces(b, sel.faces, info);
    if (id == "delete") return deleteFaces(b, facesTouching(b, sel, mode), info);
    if (id == "flip") return flipFaces(b, sel.faces);
    if (id == "recalc") return recalcNormals(b, true);
    if (id == "bridge") return bridgeLoops(b, sel, mode, info);
    if (id == "subdivide") {
        if (mode != SelMode::Edge) return failed("Subdivide works on edges: press 2, then select edges");
        return subdivideEdges(b, sel.edges, std::max(1, static_cast<int>(param)), info);
    }
    if (id == "smooth" || id == "sharp") {
        // edge / vertex mode: the selected edges; face mode: the edges between two selected faces (Blender's Shade Smooth), all edges of a lone face
        std::vector<Edge> keys = sel.edges;
        if (mode == SelMode::Face) {
            std::vector<Edge> faceEdges, inner;
            for (const BrushFace& f : b.faces)
                if (std::find(sel.faces.begin(), sel.faces.end(), f.id) != sel.faces.end())
                    for (size_t i = 0; i < f.loop.size(); ++i) faceEdges.push_back(edgeKey(f.loop[i], f.loop[(i + 1) % f.loop.size()]));
            for (size_t i = 0; i < faceEdges.size(); ++i)
                if (std::find(faceEdges.begin(), faceEdges.begin() + static_cast<std::ptrdiff_t>(i), faceEdges[i]) != faceEdges.begin() + static_cast<std::ptrdiff_t>(i)) inner.push_back(faceEdges[i]);
            keys = inner.empty() ? faceEdges : inner;
        }
        if (keys.empty()) return failed("Select edges (2) or faces (3) first");
        const bool on = id == "smooth";
        OpResult r;
        r.brush = markSmooth(b, keys, on);
        r.sel = sel;
        if (r.brush.smoothEdges == b.smoothEdges) r.error = on ? "Those edges are already smooth" : "Those edges are not smooth";
        return r;
    }
    return failed("Unknown mesh tool " + id);
}

std::optional<MeshSel> runSelectTool(const std::string& id, const Brush& b, const MeshSel& sel, SelMode mode) {
    if (id == "selectall") return allSel(b);
    if (id == "selectnone") return MeshSel{};
    if (id == "invert") return invertSel(b, sel, mode);
    if (id == "grow") return growSel(b, sel, mode);
    if (id == "shrink") return shrinkSel(b, sel, mode);
    return std::nullopt;
}

} // namespace native::tools::mesh
