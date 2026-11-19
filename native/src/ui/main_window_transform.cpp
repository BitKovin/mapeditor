// ui/main_window_transform -- the transform tools of the main window (T4.1): the modal operators G / R / S, the gizmo (axis arrows, rotation rings, scale handles)
// and the origin handle of brush entities. Every pointer move computes the operation's parameter and asks tools/transform for the document version from the
// version the operation began on; that version is committed into one open history interaction, so the whole operation is one undo step and the scene follows
// every move.
#include <QCursor>
#include <QKeyEvent>
#include <QStatusBar>

#include <algorithm>
#include <cmath>

#include "doc/commands.h"
#include "core/document.h"
#include "doc/selection.h"
#include "render/picking.h"
#include "render/render_thread.h"
#include "render/viewport_window.h"
#include "tools/gizmo_overlay.h"
#include "tools/transform.h"
#include "ui/main_window.h"
#include "ui/transform_session.h"

namespace native::ui {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kOriginHitPx = 8;   // the origin square of a brush entity

render::Vec3 asVec(const tools::V3& v) { return {v[0], v[1], v[2]}; }
tools::V3 asV3(const render::Vec3& v) { return {v.x, v.y, v.z}; }

render::Ray rayAt(const render::ViewState& view, QSizeF size, QPointF p) {
    return render::rayThrough(view.viewProj, p.x() / std::max(1.0, size.width()) * 2.0 - 1.0, p.y() / std::max(1.0, size.height()) * 2.0 - 1.0);
}

tools::Pixel screenOf(const render::ViewState& view, QSizeF size, const render::Vec3& p) {
    const render::Vec3 n = render::mat4Transform(view.viewProj, p);
    return {(n.x * 0.5 + 0.5) * size.width(), (n.y * 0.5 + 0.5) * size.height()};
}

const char* labelOf(tools::ModalKind k) { return k == tools::ModalKind::Grab ? "Move" : (k == tools::ModalKind::Rotate ? "Rotate" : "Scale"); }

} // namespace

// ---- what can be transformed --------------------------------------------------------------------------------------------------------

std::vector<doc::SelRef> MainWindow::transformSelection() const {
    std::vector<doc::SelRef> out;
    if (!selection_) return out;
    for (const doc::SelRef& r : selection_->items())
        if (!r.isFace()) out.push_back(r);   // face mode has its own tools
    return out;
}

bool MainWindow::canStartTransform() const {
    return !transform_ && document_ && !transformSelection().empty() && tools::gizmoPivot(*document_, transformSelection()).has_value();
}

size_t MainWindow::panelIndexOf(const render::ViewportWindow* vp) const {
    for (size_t i = 0; i < panels_.size(); ++i)
        if (panels_[i].window == vp) return i;
    return panels_.size();
}

// ---- starting -------------------------------------------------------------------------------------------------------------------------

bool MainWindow::startTransform(render::ViewportWindow* vp, tools::ModalKind kind) {
    if (transform_ || !document_ || !vp) return false;
    if (kind != tools::ModalKind::Grab && kind != tools::ModalKind::Rotate && kind != tools::ModalKind::Scale) return false;
    const std::vector<doc::SelRef> sels = transformSelection();
    if (sels.empty()) return false;
    const auto pivot = tools::gizmoPivot(*document_, sels);
    if (!pivot) return false;
    const size_t index = panelIndexOf(vp);
    if (index >= panels_.size()) return false;
    auto t = std::make_unique<TransformSession>();
    t->source = TransformSession::Source::Modal;
    t->kind = kind;
    t->viewport = vp;
    t->panelIndex = index;
    t->base = document_;
    t->selection = sels;
    t->pivot = asVec(*pivot);
    t->label = labelOf(kind);
    const render::ViewState view = vp->currentView();
    const QSizeF size = vp->size();
    t->mouse0 = t->mouse = vp->lastMousePos();
    t->forward = render::rayThrough(view.viewProj, 0, 0).dir;
    t->startPoint = tools::planePoint(rayAt(view, size, t->mouse0), t->pivot, t->forward).value_or(t->pivot);
    t->screenPivot = screenOf(view, size, t->pivot);
    history_->beginInteraction(t->label);
    transform_ = std::move(t);
    refreshGizmos();
    showTransformStatus();
    return true;
}

bool MainWindow::beginGizmoDrag(render::ViewportWindow* vp, QPointF p) {
    if (transform_ || !document_ || !vp) return false;
    const size_t index = panelIndexOf(vp);
    if (index >= panels_.size()) return false;
    const GizmoTarget g = gizmoTarget(vp);
    if (!g.valid) return false;
    const render::ViewState view = vp->currentView();
    const QSizeF size = vp->size();
    const render::Ray ray = rayAt(view, size, p);
    const tools::Axis axis = gizmoAxisAt(vp, g, p);
    if (axis < 0) return false;
    auto t = std::make_unique<TransformSession>();
    t->source = TransformSession::Source::Gizmo;
    t->kind = g.mode == render::GizmoMode::Rotate ? tools::ModalKind::Rotate : (g.mode == render::GizmoMode::Scale ? tools::ModalKind::Scale : tools::ModalKind::Grab);
    t->viewport = vp;
    t->panelIndex = index;
    t->base = document_;
    t->selection = transformSelection();
    t->pivot = g.pivot;
    t->axis = axis;
    t->mouse0 = t->mouse = p;
    t->originEntity = g.originEntity;
    t->label = g.originEntity.empty() ? labelOf(t->kind) : "Move origin";
    if (t->kind == tools::ModalKind::Rotate) {
        const auto a = tools::ringAngle(ray, t->pivot, axis);
        if (!a) return false;
        t->startT = *a;
    } else {
        const auto d = tools::axisDistance(ray, t->pivot, axis);
        if (!d) return false;
        t->startT = *d;
    }
    history_->beginInteraction(t->label);
    transform_ = std::move(t);
    refreshGizmos();
    return true;
}

// ---- running ----------------------------------------------------------------------------------------------------------------------------

void MainWindow::updateTransform() {
    if (!transform_ || !document_) return;
    TransformSession& t = *transform_;
    render::ViewportWindow* vp = t.viewport;
    const render::ViewState view = vp->currentView();
    const QSizeF size = vp->size();
    const render::ViewportSettings& settings = vp->rig().settings();
    const render::Ray ray = rayAt(view, size, t.mouse);
    const tools::V3 pivot = asV3(t.pivot);
    tools::Affine map;
    bool flip = false;
    double yawDeg = 0;
    std::shared_ptr<const doc::DocumentVersion> after;
    const std::optional<double> typed = tools::typedValue(t.typed);

    if (t.source == TransformSession::Source::Modal) {
        const render::Vec3 toward = t.forward * -1.0;
        switch (t.kind) {
            case tools::ModalKind::Grab: {
                render::Vec3 delta{0, 0, 0};
                if (typed) delta = tools::axisDir(t.axis < 0 ? 0 : t.axis) * *typed;
                else delta = tools::grabDelta(ray, t.startPoint, t.pivot, t.forward, t.axis, render::snapStep(settings));
                t.value = render::length(delta);
                map = tools::translateBy(asV3(delta));
                break;
            }
            case tools::ModalKind::Rotate: {
                const render::Vec3 axisVec = t.axis < 0 ? toward : tools::axisDir(t.axis);
                const double angle = typed ? *typed * kPi / 180.0
                                           : tools::rotateAngle(t.screenPivot, {t.mouse0.x(), t.mouse0.y()}, {t.mouse.x(), t.mouse.y()}, render::dot(axisVec, toward) >= 0,
                                                                render::rotSnapRadians(settings));
                t.value = angle;
                map = tools::rotateAbout(pivot, asV3(axisVec), angle);
                yawDeg = t.axis == 1 ? angle * 180.0 / kPi : 0;
                break;
            }
            default: {
                const double f = typed ? *typed : tools::scaleFactor(t.screenPivot, {t.mouse0.x(), t.mouse0.y()}, {t.mouse.x(), t.mouse.y()}, render::scaleSnapStep(settings));
                t.value = f;
                const render::Vec3 vec = tools::scaleVector(f == 0 ? 1e-6 : f, t.axis);
                map = tools::scaleAbout(pivot, asV3(vec));
                flip = tools::mirrors(asV3(vec));
                break;
            }
        }
        after = tools::transformSelections(t.base, t.selection, map, flip, yawDeg, true);
    } else if (t.kind == tools::ModalKind::Rotate) {
        const auto a = tools::ringAngle(ray, t.pivot, t.axis);
        if (!a) return;
        double delta = tools::angleDelta(t.startT, *a);
        const double snap = render::rotSnapRadians(settings);
        if (snap > 0) delta = std::round(delta / snap) * snap;
        t.value = delta;
        map = tools::rotateAbout(pivot, asV3(tools::axisDir(t.axis)), delta);
        yawDeg = t.axis == 1 ? delta * 180.0 / kPi : 0;
        after = tools::transformSelections(t.base, t.selection, map, false, yawDeg, true);
    } else if (t.kind == tools::ModalKind::Scale) {
        const auto d = tools::axisDistance(ray, t.pivot, t.axis);
        if (!d || std::fabs(t.startT) < 1e-6) return;
        double f = *d / t.startT;
        const double snap = render::scaleSnapStep(settings);
        if (snap > 0) f = std::round(f / snap) * snap;
        if (std::fabs(f) < 0.01) return;
        t.value = f;
        tools::V3 vec{1, 1, 1};
        vec[static_cast<size_t>(t.axis)] = f;
        after = tools::transformSelections(t.base, t.selection, tools::scaleAbout(pivot, vec), f < 0, 0, true);
    } else {
        const render::Vec3 delta = tools::dragDelta(ray, t.pivot, t.axis, t.startT, render::snapStep(settings));
        t.value = render::length(delta);
        if (!t.originEntity.empty()) after = tools::moveEntityOrigin(t.base, t.originEntity, asV3(delta));
        else after = tools::transformSelections(t.base, t.selection, tools::translateBy(asV3(delta)), false, 0, true);
    }
    if (!after) return;   // an invalid result (a brush that would not be valid): the last good version stays
    if (after != document_) history_->commit({t.label, document_, after});
    refreshGizmos();
    showTransformStatus();
}

void MainWindow::showTransformStatus() {
    if (!transform_) return;
    const TransformSession& t = *transform_;
    if (t.source == TransformSession::Source::Modal) {
        statusBar()->showMessage(QString::fromUtf8(tools::describeModal(t.kind, t.value, t.axis, t.typed).c_str()));
        return;
    }
    char buf[96];
    if (t.kind == tools::ModalKind::Rotate) std::snprintf(buf, sizeof buf, "Rotate %c: %.1f\xC2\xB0", "XYZ"[t.axis], t.value * 180.0 / kPi);
    else if (t.kind == tools::ModalKind::Scale) std::snprintf(buf, sizeof buf, "Scale %c: \xC3\x97%.2f", "XYZ"[t.axis], t.value);
    else std::snprintf(buf, sizeof buf, "%s %c: %.3f m", t.label.c_str(), "XYZ"[t.axis], t.value);
    statusBar()->showMessage(QString::fromUtf8(buf));
}

void MainWindow::endTransform(bool confirm) {
    if (!transform_) return;
    const std::shared_ptr<const doc::DocumentVersion> base = transform_->base;
    if (!confirm && document_ != base) history_->commit({transform_->label, document_, base});   // back to where it began: the interaction then pushes nothing
    history_->endInteraction();
    const std::string label = transform_->label;
    transform_.reset();
    statusBar()->showMessage(confirm ? QString::fromStdString(label) : QStringLiteral("Cancelled"), 3000);
    refreshGizmos();
}

void MainWindow::toggleTransformAxis(int axis) {
    if (!transform_ || transform_->source != TransformSession::Source::Modal) return;
    transform_->axis = transform_->axis == axis ? -1 : axis;
    updateTransform();
}

// ---- input ------------------------------------------------------------------------------------------------------------------------------

bool MainWindow::transformKey(const QKeyEvent& e, bool press) {
    if (!transform_ || transform_->source != TransformSession::Source::Modal || !press) return false;
    if (e.modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) return false;
    std::string key;
    switch (e.key()) {
        case Qt::Key_0: case Qt::Key_1: case Qt::Key_2: case Qt::Key_3: case Qt::Key_4:
        case Qt::Key_5: case Qt::Key_6: case Qt::Key_7: case Qt::Key_8: case Qt::Key_9: key = std::string(1, static_cast<char>('0' + (e.key() - Qt::Key_0))); break;
        case Qt::Key_Period: key = "."; break;
        case Qt::Key_Comma: key = ","; break;
        case Qt::Key_Minus: key = "-"; break;
        case Qt::Key_Backspace: key = "Backspace"; break;
        default: return false;
    }
    const auto next = tools::typeInto(transform_->typed, key);
    if (!next) return false;
    transform_->typed = *next;
    updateTransform();
    return true;
}

bool MainWindow::transformMouse(size_t index, const actions::InputEvent& e) {
    using Type = actions::InputEvent::Type;
    if (index >= panels_.size()) return false;
    render::ViewportWindow* vp = panels_[index].window;
    if (transform_) {
        if (transform_->viewport != vp) return false;
        if (e.type == Type::MouseMove) {
            transform_->mouse = QPointF(e.x, e.y);
            updateTransform();
            return true;
        }
        if (transform_->source == TransformSession::Source::Gizmo && e.type == Type::MouseRelease && e.button == 1) {
            endTransform(true);
            return true;
        }
        return false;   // modal: a click confirms, a right click cancels (keymap, context modal:transform)
    }
    if (e.type == Type::MouseMove) {
        if (router_.holding()) return false;   // a camera drag or a box select owns the move
        const GizmoTarget g = gizmoTarget(vp);
        const tools::Axis axis = g.valid ? gizmoAxisAt(vp, g, QPointF(e.x, e.y)) : -1;
        if (axis != gizmoHover_.axis || index != gizmoHover_.panel) {
            gizmoHover_ = {index, axis};
            refreshGizmo(index);
        }
        return false;
    }
    if (e.type == Type::MousePress && e.button == 1 && e.mods == 0) {
        if (originHit(vp, QPointF(e.x, e.y))) return true;   // the square sits on the gizmo's pivot: it wins
        if (beginGizmoDrag(vp, QPointF(e.x, e.y))) return true;
        if (!originSel_.empty()) {   // a click that grabs neither the origin nor its gizmo ends the origin editing
            originSel_.clear();
            refreshGizmos();
        }
    }
    return false;
}

// ---- the gizmo ----------------------------------------------------------------------------------------------------------------------------

MainWindow::GizmoTarget MainWindow::gizmoTarget(render::ViewportWindow* vp) {
    GizmoTarget g;
    if (!document_ || transform_ || !vp) return g;
    const std::vector<doc::SelRef> sels = transformSelection();
    if (sels.empty()) return g;
    if (!originSel_.empty()) {
        const int ei = document_->findEntity(originSel_);
        if (ei < 0) return g;
        const core::Vec3 o = document_->entities[static_cast<size_t>(ei)]->typed.origin;
        g.pivot = {o.x, o.y, o.z};
        g.mode = render::GizmoMode::Move;   // with the origin selected it is always the move gizmo
        g.originEntity = originSel_;
    } else {
        const auto pivot = tools::gizmoPivot(*document_, sels);
        if (!pivot) return g;
        g.pivot = asVec(*pivot);
        g.mode = vp->rig().settings().gizmo;
    }
    g.length = tools::gizmoLength(vp->rig().active().distanceTo(g.pivot));
    g.valid = true;
    return g;
}

tools::Axis MainWindow::gizmoAxisAt(render::ViewportWindow* vp, const GizmoTarget& g, QPointF p) {
    const render::ViewState view = vp->currentView();
    const render::Ray ray = rayAt(view, vp->size(), p);
    return g.mode == render::GizmoMode::Rotate ? tools::hitRing(ray, g.pivot, g.length, g.length * 0.06) : tools::hitAxis(ray, g.pivot, g.length * 1.15, g.length * 0.07);
}

// A click on the origin square of the one selected brush entity with a custom origin selects the origin: the usual move gizmo then moves it alone.
bool MainWindow::originHit(render::ViewportWindow* vp, QPointF p) {
    if (!document_ || !vp || !originSel_.empty()) return false;
    const auto origin = originHandle();
    if (!origin) return false;
    const render::ViewState view = vp->currentView();
    const tools::Pixel s = screenOf(view, vp->size(), *origin);
    if (std::hypot(s.x - p.x(), s.y - p.y()) > kOriginHitPx) return false;
    originSel_ = transformSelection()[0].entityId;
    gizmoHover_ = {panelIndexOf(vp), -1};
    refreshGizmos();
    statusBar()->showMessage(QStringLiteral("Origin selected: drag an arrow to move it (Esc or a click elsewhere to finish)"), 6000);
    return true;
}

void MainWindow::leaveOrigin() {
    if (originSel_.empty()) return;
    originSel_.clear();
    refreshGizmos();
}

std::vector<render::LineVertex> MainWindow::gizmoLinesFor(size_t index) {
    if (index >= panels_.size()) return {};
    render::ViewportWindow* vp = panels_[index].window;
    if (!vp) return {};
    if (transform_ && transform_->source == TransformSession::Source::Modal) return {};   // the modal operators show no gizmo
    GizmoTarget g;
    tools::Axis active = -1;
    if (transform_) {   // a gizmo drag: the gizmo follows the pivot, the dragged handle is highlighted
        if (transform_->viewport != vp) return {};
        const TransformSession& t = *transform_;
        g.valid = true;
        g.pivot = t.pivot;
        g.mode = t.kind == tools::ModalKind::Rotate ? render::GizmoMode::Rotate : (t.kind == tools::ModalKind::Scale ? render::GizmoMode::Scale : render::GizmoMode::Move);
        g.length = tools::gizmoLength(vp->rig().active().distanceTo(g.pivot));
        active = t.axis;
        if (g.mode == render::GizmoMode::Move) {
            // moving: the pivot is where the selection now is
            const auto now = !t.originEntity.empty() ? std::optional<tools::V3>{} : tools::gizmoPivot(*document_, t.selection);
            if (now) g.pivot = asVec(*now);
            else if (!t.originEntity.empty()) {
                const int ei = document_->findEntity(t.originEntity);
                if (ei >= 0) {
                    const core::Vec3 o = document_->entities[static_cast<size_t>(ei)]->typed.origin;
                    g.pivot = {o.x, o.y, o.z};
                }
            }
            g.length = tools::gizmoLength(vp->rig().active().distanceTo(g.pivot));
        }
    } else {
        std::vector<render::LineVertex> handle;
        if (const auto origin = originHandle()) {   // the square of a brush entity with a custom origin
            const render::CameraBasis b = vp->rig().active().basis();
            const double d = vp->rig().active().distanceTo(*origin);
            const double half = 5.0 * tools::metresPerPixelAt(d, vp->rig().active().fovY, std::max(1, vp->height()));
            handle = tools::originHandleLines(*origin, b.right, b.up, half, !originSel_.empty());
        }
        g = gizmoTarget(vp);
        if (!g.valid) return handle;
        if (gizmoHover_.panel == index) active = gizmoHover_.axis;
        std::vector<render::LineVertex> lines = tools::gizmoLines(g.pivot, g.length, g.mode, active);
        lines.insert(lines.end(), handle.begin(), handle.end());
        return lines;
    }
    return tools::gizmoLines(g.pivot, g.length, g.mode, active);
}

// Where the origin square of the one selected brush entity with a custom origin is; none otherwise.
std::optional<render::Vec3> MainWindow::originHandle() const {
    if (!document_ || transform_) return std::nullopt;
    const std::vector<doc::SelRef> sels = transformSelection();
    if (sels.size() != 1 || !sels[0].instancePath.empty() || sels[0].entityId.empty()) return std::nullopt;
    const int ei = document_->findEntity(sels[0].entityId);
    if (ei < 0) return std::nullopt;
    const core::Entity& e = document_->entities[static_cast<size_t>(ei)]->typed;
    const auto& brushes = document_->entities[static_cast<size_t>(ei)]->brushes;
    if (e.classname == "worldspawn" || brushes.empty() || !core::EntityClassRegistry::showsCustomOriginGizmo(e.classname)) return std::nullopt;
    return render::Vec3{e.origin.x, e.origin.y, e.origin.z};
}

void MainWindow::refreshGizmo(size_t index) {
    if (index >= panels_.size() || !services_.renderThread) return;
    render::ViewportWindow* vp = panels_[index].window;
    if (!vp || vp->viewportId() == 0) return;
    std::vector<render::LineVertex> lines = gizmoLinesFor(index);
    if (lines.empty() && !panels_[index].hasGizmo) return;
    panels_[index].hasGizmo = !lines.empty();
    services_.renderThread->setGizmoLines(vp->viewportId(), lines.empty() ? nullptr : std::make_shared<const std::vector<render::LineVertex>>(std::move(lines)));
}

void MainWindow::refreshGizmos() {
    for (size_t i = 0; i < panels_.size(); ++i) refreshGizmo(i);
}

} // namespace native::ui
