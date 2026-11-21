// ui/main_window_mesh -- MainWindow: mesh mode (T11.1f): entering it, the element kind, picking elements in a viewport, the instant tools and the overlay lines.
// The operations are tools/mesh (pure); here they become edits of the document (one undo step each) and the doc::Selection's mesh selection follows.
#include <QStatusBar>

#include <core/texplan.h>

#include <algorithm>
#include <cmath>
#include <set>

#include "doc/selection.h"
#include "render/line_vertex.h"
#include "render/viewport_window.h"
#include "tools/gizmo.h"
#include "tools/mesh/pick.h"
#include "ui/main_window.h"
#include "ui/view_helpers.h"

namespace native::ui {

namespace tm = tools::mesh;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

const char* labelOf(const std::string& id) {
    if (id == "connect") return "Connect vertices";
    if (id == "merge") return "Merge";
    if (id == "fill") return "Fill";
    if (id == "dissolve") return "Dissolve";
    if (id == "delete") return "Delete faces";
    if (id == "flip") return "Flip normals";
    if (id == "recalc") return "Recalculate normals";
    if (id == "bridge") return "Bridge edge loops";
    if (id == "smooth") return "Mark smooth";
    if (id == "sharp") return "Clear smooth";
    if (id == "subdivide") return "Subdivide";
    return "Mesh edit";
}

const uint32_t kEdge = render::packColor(0.55f, 0.75f, 1.0f, 0.9f);
const uint32_t kSmooth = render::packColor(0.35f, 0.9f, 0.45f, 0.95f);
const uint32_t kPicked = render::packColor(1.0f, 0.6f, 0.1f, 1.0f);
const uint32_t kVertex = render::packColor(0.85f, 0.92f, 1.0f, 0.9f);

void addLine(std::vector<render::LineVertex>& out, const core::Vec3& a, const core::Vec3& b, uint32_t rgba) {
    out.push_back({{static_cast<float>(a.x), static_cast<float>(a.y), static_cast<float>(a.z)}, rgba});
    out.push_back({{static_cast<float>(b.x), static_cast<float>(b.y), static_cast<float>(b.z)}, rgba});
}

tm::Project projector(const render::ViewState& view, QSizeF size) {
    const double w = std::max(1.0, size.width()), h = std::max(1.0, size.height());
    return [view, w, h](const core::Vec3& p) -> std::optional<tm::Screen> {
        const render::Mat4& m = view.viewProj;
        if (m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15] <= 1e-9) return std::nullopt;   // behind the camera
        const render::Vec3 n = render::mat4Transform(m, {p.x, p.y, p.z});
        return tm::Screen{(n.x * 0.5 + 0.5) * w, (n.y * 0.5 + 0.5) * h, n.z};
    };
}

} // namespace

// ---- the mode ------------------------------------------------------------------------------------------------------------------------

bool MainWindow::meshMode() const { return selection_->mode() == doc::EditMode::Mesh; }

bool MainWindow::enterMeshMode() {
    if (!document_) return false;
    if (meshMode()) return true;
    const doc::Selection::ModeResult r = selection_->setMode(*document_, doc::EditMode::Mesh);
    if (!r.ok) {
        statusBar()->showMessage(qs(r.message), 5000);
        return false;
    }
    if (!meshTarget()) {   // inside a prefab instance: nothing of the map to edit
        selection_->setMode(*document_, doc::EditMode::Object);
        statusBar()->showMessage(QStringLiteral("Mesh mode edits a brush of this map, not one inside a prefab instance"), 5000);
        return false;
    }
    if (tool_ != Tool::Select) setTool(Tool::Select);
    meshActive_.reset();
    emit toolChanged();
    return true;
}

void MainWindow::leaveMeshMode() {
    if (!document_ || !meshMode()) return;
    selection_->setMode(*document_, doc::EditMode::Object);
    meshActive_.reset();
    emit toolChanged();
}

bool MainWindow::toggleMeshMode() {
    if (meshMode()) {
        leaveMeshMode();
        return true;
    }
    return enterMeshMode();
}

tm::SelMode MainWindow::meshElement() const { return tm::fromDocElement(selection_->meshElement()); }

std::optional<tm::MeshTarget> MainWindow::meshTarget() const {
    if (!document_ || !meshMode() || !selection_->primary()) return std::nullopt;
    return tm::meshTarget(*document_, *selection_->primary());
}

tm::MeshSel MainWindow::meshSelection() const {
    const auto t = meshTarget();
    return t ? tm::fromDocSel(t->brush(), selection_->mesh()) : tm::MeshSel{};
}

void MainWindow::setMeshSelection(const tm::MeshSel& sel) {
    const auto t = meshTarget();
    if (t) selection_->setMesh(tm::toDocSel(t->brush(), sel));
}

bool MainWindow::setMeshElement(tm::SelMode element) {
    const auto t = meshTarget();
    if (!t) return false;
    const tm::SelMode from = meshElement();
    if (from == element) return true;
    const tm::MeshSel converted = tm::convertSel(t->brush(), meshSelection(), from, element);
    selection_->setMeshElement(tm::toDocElement(element));   // drops the picked elements
    setMeshSelection(converted);
    meshActive_.reset();
    return true;
}

// ---- operations ----------------------------------------------------------------------------------------------------------------------

bool MainWindow::applyMeshOperation(const std::string& label, const tm::MeshOpFn& op) {
    if (!document_ || !selection_->primary()) return false;
    const tm::MeshApply r = tm::applyMeshOp(document_, *selection_->primary(), label, op, meshSelection(), true);
    if (!r.ok()) {
        if (!r.error.empty()) statusBar()->showMessage(qs(r.error), 6000);
        return false;
    }
    if (!commitEdit(label, r.version)) return false;
    if (!selection_->primary() || *selection_->primary() != r.select) selection_->select(r.select);   // normally the selection followed the brush already
    if (const auto t = tm::meshTarget(*document_, r.select)) selection_->setMesh(tm::toDocSel(t->brush(), r.sel));
    statusBar()->showMessage(qs(r.message.empty() ? label : r.message), r.message.empty() ? 3000 : 6000);
    refreshOutline();
    return true;
}

bool MainWindow::runMeshTool(const std::string& id, double param) {
    const auto t = meshTarget();
    if (!t) {
        statusBar()->showMessage(QStringLiteral("Press Tab to enter mesh mode first"), 4000);
        return false;
    }
    const tm::SelMode mode = meshElement();
    if (const auto sel = tm::runSelectTool(id, t->brush(), meshSelection(), mode)) {
        setMeshSelection(*sel);
        return true;
    }
    const core::texplan::InfoFn info = texEnv_ ? texEnv_->infoFn() : core::texplan::InfoFn{};
    return applyMeshOperation(labelOf(id), [id, mode, param, info](const core::Brush& b, const tm::MeshSel& s) { return tm::runMeshTool(id, b, s, mode, param, info); });
}

// ---- picking -------------------------------------------------------------------------------------------------------------------------

bool MainWindow::meshClickAt(render::ViewportWindow* vp, QPointF p, bool shift, bool ctrl, bool alt) {
    const auto t = meshTarget();
    if (!t || !vp) return false;
    const core::Brush& b = t->brush();
    const render::ViewState view = vp->currentView();
    const tm::Project project = projector(view, vp->size());
    const tm::SelMode mode = meshElement();
    const bool loop = alt;
    tm::MeshPickAt at;
    if (mode == tm::SelMode::Face) {
        const render::Ray ray = rayAt(view, vp->size(), p);
        at.face = tm::pickFaceId(b, tm::Ray{{ray.origin.x, ray.origin.y, ray.origin.z}, {ray.dir.x, ray.dir.y, ray.dir.z}});
        if (at.face && loop) at.faceEdge = tm::nearestFaceEdge(b, *at.face, project, p.x(), p.y());
    } else {
        if (mode == tm::SelMode::Vertex) at.vertex = tm::pickVertex(b, project, p.x(), p.y());
        at.edge = tm::pickEdge(b, project, p.x(), p.y(), loop && mode == tm::SelMode::Vertex ? 24 : 8);
    }
    // the active element only counts while it is still selected (another tool may have replaced the selection)
    const tm::MeshSel cur = meshSelection();
    std::optional<tm::ActiveElem> active = meshActive_;
    if (active) {
        const tm::ElemId& id = active->id;
        const bool still = active->mode != mode ? false
                           : mode == tm::SelMode::Vertex ? std::find(cur.verts.begin(), cur.verts.end(), id.n) != cur.verts.end()
                           : mode == tm::SelMode::Edge   ? std::find(cur.edges.begin(), cur.edges.end(), id.e) != cur.edges.end()
                                                         : std::find(cur.faces.begin(), cur.faces.end(), id.n) != cur.faces.end();
        if (!still) active.reset();
    }
    tm::ClickMods mods;
    mods.shift = shift;
    mods.ctrl = ctrl;
    mods.alt = alt;
    const std::optional<tm::ClickResult> r = tm::clickSelect(b, mode, at, mods, cur, active, loop);
    if (!r) return false;
    meshActive_ = r->active;
    setMeshSelection(r->sel);
    return true;
}

void MainWindow::meshBoxSelectIn(render::ViewportWindow* vp, QPointF a, QPointF c, bool shift) {
    const auto t = meshTarget();
    if (!t || !vp) return;
    const tm::Project project = projector(vp->currentView(), vp->size());
    const tm::SelMode mode = meshElement();
    const tm::MeshSel inBox = tm::boxSelect(t->brush(), project, mode, a.x(), a.y(), c.x(), c.y());
    setMeshSelection(shift ? tm::combineSel(t->brush(), meshSelection(), inBox, mode, true) : inBox);
}

void MainWindow::meshPickInput(render::ViewportWindow* vp, const actions::InputEvent& e) {
    using Type = actions::InputEvent::Type;
    const QPointF p(e.x, e.y);
    if (e.type == Type::MousePress) {
        meshDrag_ = {vp, p, p, false, (e.mods & actions::kShift) != 0, (e.mods & actions::kCtrl) != 0, (e.mods & actions::kAlt) != 0};
        return;
    }
    if (meshDrag_.viewport != vp) return;
    if (e.type == Type::MouseMove) {
        meshDrag_.current = p;
        if (!meshDrag_.dragging && (p - meshDrag_.start).manhattanLength() >= 4) meshDrag_.dragging = true;
        if (meshDrag_.dragging) vp->setRubberBand(true, meshDrag_.start, p);
        return;
    }
    if (e.type == Type::MouseRelease) {
        const MeshDrag d = meshDrag_;
        meshDrag_ = {};
        if (d.dragging) {
            vp->setRubberBand(false);
            meshBoxSelectIn(vp, d.start, p, d.shift);
        } else if (!meshClickAt(vp, p, d.shift, d.ctrl, d.alt) && !d.shift) {
            setMeshSelection({});   // a click on nothing clears the selection
        }
    }
}

// ---- overlays ------------------------------------------------------------------------------------------------------------------------

std::vector<render::LineVertex> MainWindow::meshEdgeLines() const {
    std::vector<render::LineVertex> out;
    const auto t = meshTarget();
    if (!t) return out;
    const core::Brush& b = t->brush();
    const tm::Topology topo = tm::buildTopology(b);
    const tm::MeshSel sel = meshSelection();
    const std::set<tm::Edge> picked(sel.edges.begin(), sel.edges.end());
    const std::set<uint32_t> faces(sel.faces.begin(), sel.faces.end());
    std::set<std::pair<uint32_t, uint32_t>> smooth;
    for (const auto& pr : b.smoothEdges) smooth.insert({std::min(pr.first, pr.second), std::max(pr.first, pr.second)});
    for (const tm::EdgeFaces& e : topo.edgeList) {
        uint32_t color = kEdge;
        if (e.faces.size() == 2 && smooth.count({std::min(b.faces[e.faces[0]].id, b.faces[e.faces[1]].id), std::max(b.faces[e.faces[0]].id, b.faces[e.faces[1]].id)})) color = kSmooth;
        if (picked.count(tm::Edge{e.a, e.b})) color = kPicked;
        for (size_t f : e.faces)
            if (faces.count(b.faces[f].id)) color = kPicked;
        addLine(out, b.vertices[e.a], b.vertices[e.b], color);
    }
    return out;
}

std::vector<render::LineVertex> MainWindow::meshVertexLines(size_t index) const {
    std::vector<render::LineVertex> out;
    const auto t = meshTarget();
    if (!t || index >= panels_.size() || !panels_[index].window) return out;
    render::ViewportWindow* vp = panels_[index].window;
    const core::Brush& b = t->brush();
    const std::set<uint32_t> picked(selection_->mesh().vertices.begin(), selection_->mesh().vertices.end());
    const render::CameraBasis basis = vp->rig().active().basis();
    const double fov = vp->rig().active().fovY;
    const int height = std::max(1, vp->height());
    for (uint32_t i = 0; i < b.vertices.size(); ++i) {
        const core::Vec3& v = b.vertices[i];
        const bool on = picked.count(i) != 0;
        const double half = (on ? 5.0 : 3.0) * tools::metresPerPixelAt(vp->rig().active().distanceTo({v.x, v.y, v.z}), fov, height);
        const core::Vec3 r{basis.right.x * half, basis.right.y * half, basis.right.z * half}, u{basis.up.x * half, basis.up.y * half, basis.up.z * half};
        const uint32_t color = on ? kPicked : kVertex;
        addLine(out, {v.x - r.x, v.y - r.y, v.z - r.z}, {v.x + r.x, v.y + r.y, v.z + r.z}, color);
        addLine(out, {v.x - u.x, v.y - u.y, v.z - u.z}, {v.x + u.x, v.y + u.y, v.z + u.z}, color);
    }
    return out;
}

} // namespace native::ui
