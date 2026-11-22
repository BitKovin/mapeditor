// ui/main_window_mesh -- MainWindow: mesh mode (T11.1f): entering it, the element kind, picking elements in a viewport, the instant tools and the overlay lines.
// The operations are tools/mesh (pure); here they become edits of the document (one undo step each) and the doc::Selection's mesh selection follows.
#include <QStatusBar>

#include <core/texplan.h>

#include <algorithm>
#include <cmath>
#include <set>

#include "doc/commands.h"
#include "doc/selection.h"
#include "render/line_vertex.h"
#include "render/viewport_window.h"
#include "tools/box_tool.h"
#include "tools/gizmo.h"
#include "tools/mesh/cut.h"
#include "tools/mesh/ops.h"
#include "tools/mesh/pick.h"
#include "ui/main_window.h"
#include "ui/transform_session.h"
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
    if (knife_) cancelKnife();
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
        meshDrag_ = {vp, p, p, false, (e.mods & actions::kShift) != 0, (e.mods & actions::kCtrl) != 0, (e.mods & actions::kAlt) != 0, e.doubleClick};
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
        } else if (!meshClickAt(vp, p, d.shift, d.ctrl, d.alt || d.loop) && !d.shift) {
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

// ---- modal tools: G / R / S, E, I, Ctrl+B, Ctrl+R on the elements (T11.1g) --------------------------------------------------------------

namespace {

core::Vec3 unit(const core::Vec3& v) {
    const double l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return l < 1e-12 ? core::Vec3{0, 0, 0} : core::Vec3{v.x / l, v.y / l, v.z / l};
}

} // namespace

bool MainWindow::setupMeshSession(TransformSession& t, tools::ModalKind kind, const std::optional<tm::Edge>& edge) {
    const auto target = meshTarget();
    if (!target || !t.viewport) return false;
    const core::Brush& b = target->brush();
    const tm::MeshSel sel = meshSelection();
    const tm::SelMode mode = meshElement();
    const auto hint = [this](const char* text) {
        statusBar()->showMessage(QString::fromUtf8(text), 5000);
        return false;
    };
    MeshSession& m = t.mesh;
    m = MeshSession{};
    m.entityId = target->entityId;
    m.brushId = target->brushId;
    m.detailId = tools::newId("e");
    m.mode = mode;
    m.startSel = sel;
    m.radius = propRadius_;
    if (kind != tools::ModalKind::LoopCut && sel.verts.empty()) return hint("Select something first");
    if (!sel.verts.empty()) {
        const core::Vec3 c = tm::centroid(b, sel.verts);
        t.pivot = {c.x, c.y, c.z};
    }
    const render::ViewState view = t.viewport->currentView();
    const QSizeF size = t.viewport->size();
    const double mpp = tools::metresPerPixelAt(t.viewport->rig().active().distanceTo(t.pivot), t.viewport->rig().active().fovY, std::max(1, t.viewport->height()));
    switch (kind) {
        case tools::ModalKind::Grab:
        case tools::ModalKind::Rotate:
        case tools::ModalKind::Scale:
            m.proportional = proportional_;
            break;
        case tools::ModalKind::Extrude:
        case tools::ModalKind::Inset: {
            if (mode != tm::SelMode::Face || sel.faces.empty())
                return hint(kind == tools::ModalKind::Extrude ? "Extrude works on selected faces: press 3, select a face, then E" : "Inset works on selected faces: press 3, select a face, then I");
            m.faces = sel.faces;
            if (kind == tools::ModalKind::Extrude) {
                std::vector<const core::BrushFace*> faces;
                for (uint32_t id : sel.faces)
                    if (const core::BrushFace* f = tm::faceById(b, id)) faces.push_back(f);
                const core::Vec3 n = unit(tm::averageNormal(b, faces));
                if (n.x == 0 && n.y == 0 && n.z == 0) return hint("These faces have no common direction to extrude along");
                m.dir = {n.x, n.y, n.z};
                const auto along = tools::lineDistance(rayAt(view, size, t.mouse0), t.pivot, m.dir);
                m.t0 = along.value_or(0);
            } else {
                m.metresPerPixel = mpp;
            }
            break;
        }
        case tools::ModalKind::Bevel:
            if (sel.edges.empty()) return hint("Bevel works on edges (or faces): press 2 or 3, select, then Ctrl+B");
            m.edges = sel.edges;
            m.segments = 1;
            m.metresPerPixel = mpp;
            break;
        case tools::ModalKind::LoopCut: {
            if (!edge) return hint("Loop cut needs an edge: hover one, then Ctrl+R");
            const auto ring = tm::edgeRing(b, *edge);
            if (!ring || ring->ring.empty()) return hint("No loop passes through this edge (it needs quad faces around it)");
            const core::Vec3 p = b.vertices[ring->ring[0].first], q = b.vertices[ring->ring[0].second];
            const core::Vec3 d{q.x - p.x, q.y - p.y, q.z - p.z};
            m.edge = *edge;
            m.railFrom = {p.x, p.y, p.z};
            m.railLength = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            const core::Vec3 u = unit(d);
            m.railDir = {u.x, u.y, u.z};
            m.fraction = 0.5;
            t.value = 0.5;
            t.pivot = {(p.x + q.x) / 2, (p.y + q.y) / 2, (p.z + q.z) / 2};
            break;
        }
    }
    m.active = true;
    return true;
}

bool MainWindow::startMeshTransform(render::ViewportWindow* vp, tools::ModalKind kind, const std::optional<tm::Edge>& edge) {
    if (!document_ || transform_ || !vp || !meshTarget()) return false;
    const size_t index = panelIndexOf(vp);
    if (index >= panels_.size()) return false;
    auto t = std::make_unique<TransformSession>();
    t->source = TransformSession::Source::Modal;
    t->kind = kind;
    t->viewport = vp;
    t->panelIndex = index;
    t->base = document_;
    t->mouse0 = t->mouse = vp->lastMousePos();
    switch (kind) {
        case tools::ModalKind::Grab: t->label = "Move"; break;
        case tools::ModalKind::Rotate: t->label = "Rotate"; break;
        case tools::ModalKind::Scale: t->label = "Scale"; break;
        case tools::ModalKind::Extrude: t->label = "Extrude"; break;
        case tools::ModalKind::Inset: t->label = "Inset"; break;
        case tools::ModalKind::Bevel: t->label = "Bevel"; break;
        case tools::ModalKind::LoopCut: t->label = "Loop cut"; break;
    }
    if (!setupMeshSession(*t, kind, edge)) return false;
    const render::ViewState view = vp->currentView();
    const QSizeF size = vp->size();
    t->forward = render::rayThrough(view.viewProj, 0, 0).dir;
    t->startPoint = tools::planePoint(rayAt(view, size, t->mouse0), t->pivot, t->forward).value_or(t->pivot);
    t->screenPivot = screenOf(view, size, t->pivot);
    history_->beginInteraction(t->label);
    transform_ = std::move(t);
    refreshGizmos();
    updateTransform();   // a loop cut shows at the pointer at once
    showTransformStatus();
    emit transformChanged();
    return true;
}

std::shared_ptr<const doc::DocumentVersion> MainWindow::meshAfter(TransformSession& t, const tm::MeshOpFn& op) {
    MeshSession& m = t.mesh;
    const tm::MeshApply r = tm::applyMeshOp(t.base, doc::SelRef::brush(m.entityId, m.brushId), t.label, op, m.startSel, true, m.detailId, true);
    if (!r.ok()) {
        if (!r.error.empty()) statusBar()->showMessage(qs(r.error), 4000);
        return nullptr;
    }
    m.select = r.select;
    m.resultSel = r.sel;
    m.haveResult = true;
    return r.version;
}

void MainWindow::meshApplied(const TransformSession& t) {
    const MeshSession& m = t.mesh;
    if (!m.haveResult) return;
    if (!selection_->primary() || *selection_->primary() != m.select) selection_->select(m.select);
    if (const auto target = tm::meshTarget(*document_, m.select)) selection_->setMesh(tm::toDocSel(target->brush(), m.resultSel));
}

void MainWindow::setMeshProportional(bool on) {
    proportional_ = on;
    char buf[160];
    if (on) std::snprintf(buf, sizeof buf, "Proportional editing on (radius %.2f m; the mouse wheel during G / R / S changes it)", propRadius_);
    else std::snprintf(buf, sizeof buf, "Proportional editing off");
    statusBar()->showMessage(QString::fromUtf8(buf), 4000);
}

bool MainWindow::transformUsesWheel() const {
    if (!transform_ || !transform_->mesh.active) return false;
    if (transform_->kind == tools::ModalKind::Bevel) return true;
    const bool plain = transform_->kind == tools::ModalKind::Grab || transform_->kind == tools::ModalKind::Rotate || transform_->kind == tools::ModalKind::Scale;
    return plain && transform_->mesh.proportional;
}

bool MainWindow::transformWheel(double dy) {
    if (!transformUsesWheel()) return false;
    MeshSession& m = transform_->mesh;
    if (transform_->kind == tools::ModalKind::Bevel) {
        m.segments = tools::stepSegments(m.segments, dy, tm::kMaxBevelSegments);
    } else {
        propRadius_ = std::min(1000.0, std::max(0.01, propRadius_ * (dy < 0 ? 1.2 : 1.0 / 1.2)));
        m.radius = propRadius_;
        char buf[64];
        std::snprintf(buf, sizeof buf, "Proportional radius %.2f m", propRadius_);
        statusBar()->showMessage(QString::fromUtf8(buf), 3000);
    }
    updateTransform();
    return true;
}

bool MainWindow::startLoopCut(render::ViewportWindow* vp) {
    const auto t = meshTarget();
    if (!t || !vp || transform_) return false;
    const QPointF p = vp->lastMousePos();
    const auto edge = tm::pickEdge(t->brush(), projector(vp->currentView(), vp->size()), p.x(), p.y(), 14);
    if (!edge) {
        statusBar()->showMessage(QStringLiteral("Hover an edge, then Ctrl+R: the loop cut goes across the faces around it"), 5000);
        return false;
    }
    return startTransform(vp, tools::ModalKind::LoopCut, edge);
}

bool MainWindow::mirrorMesh(int axis) {
    if (axis < 0 || axis > 2) return false;
    const auto t = meshTarget();
    if (!t) return false;
    const core::texplan::InfoFn info = texEnv_ ? texEnv_->infoFn() : core::texplan::InfoFn{};
    static const char* const names[] = {"Mirror X", "Mirror Y", "Mirror Z"};
    const bool lock = uvLock_;
    return applyMeshOperation(names[axis], [axis, info, lock](const core::Brush& b, const tm::MeshSel& s) {
        std::vector<uint32_t> verts = s.verts;
        if (verts.empty())
            for (uint32_t i = 0; i < b.vertices.size(); ++i) verts.push_back(i);
        return tm::mirrorVerts(b, verts, axis, lock, info);
    });
}

// ---- knife ---------------------------------------------------------------------------------------------------------------------------------

bool MainWindow::startKnife() {
    const auto t = meshTarget();
    if (!t || transform_ || knife_) return false;
    knife_ = KnifeState{t->node, {}, std::nullopt};
    statusBar()->showMessage(QStringLiteral("Knife: click points on edges or vertices (Ctrl = edge middle); Enter or right-click cuts, Esc cancels"), 8000);
    setContextsFor(activeViewport());
    refreshGizmos();
    return true;
}

void MainWindow::addKnifePoint(const tm::KnifePoint& p) {
    if (!knife_) return;
    knife_->points.push_back(p);
    statusBar()->showMessage(QString("Knife: %1 point(s); Enter or right-click cuts, Esc cancels").arg(knife_->points.size()), 8000);
    refreshGizmos();
}

void MainWindow::cancelKnife(const QString& message) {
    if (!knife_) return;
    knife_.reset();
    statusBar()->showMessage(message.isEmpty() ? QStringLiteral("Knife cancelled") : message, 4000);
    setContextsFor(activeViewport());
    refreshGizmos();
}

bool MainWindow::finishKnife() {
    if (!knife_) return false;
    const KnifeState k = *knife_;
    knife_.reset();
    refreshGizmos();
    const auto t = meshTarget();
    if (!t || t->node != k.node) {
        statusBar()->showMessage(QStringLiteral("Knife cancelled: the brush changed while cutting"), 5000);
        return false;
    }
    if (k.points.size() < 2) {
        statusBar()->showMessage(QStringLiteral("Knife cancelled: click at least two points"), 5000);
        return false;
    }
    const core::texplan::InfoFn info = texEnv_ ? texEnv_->infoFn() : core::texplan::InfoFn{};
    const std::vector<tm::KnifePoint> points = k.points;
    return applyMeshOperation("Knife", [points, info](const core::Brush& b, const tm::MeshSel&) { return tm::knifeCut(b, points, info); });
}

std::optional<tm::KnifePoint> MainWindow::knifePointAt(render::ViewportWindow* vp, QPointF p, bool ctrl) const {
    const auto t = meshTarget();
    if (!t || !vp) return std::nullopt;
    const core::Brush& b = t->brush();
    const render::ViewState view = vp->currentView();
    const tm::Project project = projector(view, vp->size());
    if (const auto v = tm::pickVertex(b, project, p.x(), p.y())) return tm::KnifePoint::at(*v);
    const auto edge = tm::pickEdge(b, project, p.x(), p.y(), 10);
    if (!edge) return std::nullopt;
    if (ctrl) return tm::KnifePoint::onEdge(*edge, 0.5);
    const core::Vec3 a = b.vertices[edge->a], c = b.vertices[edge->b];
    const render::Vec3 d{c.x - a.x, c.y - a.y, c.z - a.z};
    const double len = render::length(d);
    double t01 = 0.5;
    if (len > 1e-9) {
        const render::Vec3 dir{d.x / len, d.y / len, d.z / len};
        if (const auto along = tools::lineDistance(rayAt(view, vp->size(), p), {a.x, a.y, a.z}, dir)) t01 = std::min(0.98, std::max(0.02, *along / len));
    }
    return tm::KnifePoint::onEdge(*edge, t01);
}

bool MainWindow::knifeMouse(size_t index, const actions::InputEvent& e) {
    using Type = actions::InputEvent::Type;
    if (!knife_ || index >= panels_.size() || !panels_[index].window) return false;
    render::ViewportWindow* vp = panels_[index].window;
    const bool ctrl = (e.mods & actions::kCtrl) != 0;
    if (e.type == Type::MouseMove) {
        knife_->hover = knifePointAt(vp, QPointF(e.x, e.y), ctrl);
        refreshGizmos();
        return false;   // the camera and the rest still see the move
    }
    if ((e.type == Type::MousePress || e.type == Type::MouseRelease) && (e.button == 1 || e.button == 2)) {
        if (e.type == Type::MousePress) {
            if (e.button == 2) {
                finishKnife();
            } else if (const auto p = knifePointAt(vp, QPointF(e.x, e.y), ctrl)) {
                addKnifePoint(*p);
            }
        }
        return true;   // a knife in progress owns the clicks
    }
    return false;
}

std::vector<render::LineVertex> MainWindow::knifeLines(size_t index) const {
    std::vector<render::LineVertex> out;
    const auto t = meshTarget();
    if (!knife_ || !t || t->node != knife_->node || index >= panels_.size() || !panels_[index].window) return out;
    render::ViewportWindow* vp = panels_[index].window;
    const core::Brush& b = t->brush();
    std::vector<core::Vec3> pts;
    for (const tm::KnifePoint& p : knife_->points) pts.push_back(tm::knifePos(b, p));
    if (knife_->hover) pts.push_back(tm::knifePos(b, *knife_->hover));
    const uint32_t color = render::packColor(1.0f, 0.85f, 0.2f, 1.0f);
    for (size_t i = 1; i < pts.size(); ++i) addLine(out, pts[i - 1], pts[i], color);
    const render::CameraBasis basis = vp->rig().active().basis();
    for (const core::Vec3& p : pts) {   // a small cross per point
        const double half = 5.0 * tools::metresPerPixelAt(vp->rig().active().distanceTo({p.x, p.y, p.z}), vp->rig().active().fovY, std::max(1, vp->height()));
        addLine(out, {p.x - basis.right.x * half, p.y - basis.right.y * half, p.z - basis.right.z * half}, {p.x + basis.right.x * half, p.y + basis.right.y * half, p.z + basis.right.z * half}, color);
        addLine(out, {p.x - basis.up.x * half, p.y - basis.up.y * half, p.z - basis.up.z * half}, {p.x + basis.up.x * half, p.y + basis.up.y * half, p.z + basis.up.z * half}, color);
    }
    return out;
}

} // namespace native::ui
