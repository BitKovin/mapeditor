// ui/main_window_angle -- MainWindow: the angle handle of a selected point entity (REQUIREMENTS 33 H2, port of the handle code in viewport.ts). A small diamond sits at the
// tip of the entity's direction arrow; dragging it turns the entity to look at the pointer: in a perspective view on the horizontal plane through the arrow's base (only the
// yaw follows, the pitch stays), with Shift on the plane facing the camera (yaw and pitch); in an orthographic view in the view plane. Roll is kept, the angles are snapped
// with the rotation snap, the whole drag is one undo step.
#include <QStatusBar>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "doc/commands.h"
#include "doc/edit.h"
#include "doc/selection.h"
#include "render/line_vertex.h"
#include "render/viewport_window.h"
#include "scene/arrows.h"
#include "tools/box_tool.h"
#include "tools/entity_edit.h"
#include "tools/gizmo.h"
#include "tools/gizmo_overlay.h"
#include "ui/main_window.h"
#include "ui/transform_session.h"
#include "ui/view_helpers.h"

namespace native::ui {

namespace {

constexpr double kAngleHitPx = 9;   // the grab radius of the diamond
constexpr double kAngleHalfPx = 5;  // its drawn size

bool isOrtho(const render::ViewportWindow& vp) {
    const render::CameraMode m = vp.rig().mode();
    return m == render::CameraMode::Top || m == render::CameraMode::Front || m == render::CameraMode::Side;
}

} // namespace

std::optional<MainWindow::AngleHandle> MainWindow::angleHandle() const {
    if (!document_ || transform_ || tool_ != Tool::Select || box_ || knife_ || clip_ || measure_ || meshMode()) return std::nullopt;
    const std::vector<doc::SelRef> sels = transformSelection();
    if (sels.size() != 1 || !sels[0].instancePath.empty() || !sels[0].isEntity()) return std::nullopt;
    const int ei = document_->findEntity(sels[0].entityId);
    if (ei < 0) return std::nullopt;
    const auto& node = *document_->entities[static_cast<size_t>(ei)];
    const core::Entity& e = node.typed;
    if (!node.brushes.empty() || e.classname == "worldspawn" || e.classname == "decal") return std::nullopt;
    const scene::DirectionSpan span = scene::directionSpan(e, scene::classBoxes(entityDefs_.get()));
    AngleHandle h;
    h.entityId = e.id;
    h.base = {span.from[0], span.from[1], span.from[2]};
    h.tip = {span.from[0] + span.dir[0] * span.len, span.from[1] + span.dir[1] * span.len, span.from[2] + span.dir[2] * span.len};
    return h;
}

std::optional<render::Vec3> MainWindow::angleHandleTip() const {
    const auto h = angleHandle();
    if (!h) return std::nullopt;
    return h->tip;
}

bool MainWindow::beginAngleDrag(render::ViewportWindow* vp, QPointF p, bool shift) {
    if (!vp || transform_) return false;
    const auto h = angleHandle();
    if (!h) return false;
    const size_t index = panelIndexOf(vp);
    if (index >= panels_.size()) return false;
    const tools::Pixel s = screenOf(vp->currentView(), vp->size(), h->tip);
    if (std::hypot(s.x - p.x(), s.y - p.y()) > kAngleHitPx) return false;
    auto t = std::make_unique<TransformSession>();
    t->source = TransformSession::Source::Gizmo;
    t->kind = tools::ModalKind::Rotate;
    t->viewport = vp;
    t->panelIndex = index;
    t->base = document_;
    t->selection = transformSelection();
    t->mouse0 = t->mouse = p;
    t->label = "Turn entity";
    t->angle.active = true;
    t->angle.entityId = h->entityId;
    t->angle.base = h->base;
    const bool viewPlane = isOrtho(*vp) || shift;
    t->angle.normal = viewPlane ? vp->rig().active().basis().forward : render::Vec3{0, 1, 0};
    t->angle.keepPitch = std::fabs(t->angle.normal.y) > 0.9;
    history_->beginInteraction(t->label);
    transform_ = std::move(t);
    refreshGizmos();
    return true;
}

void MainWindow::updateAngleDrag(TransformSession& t) {
    render::ViewportWindow* vp = t.viewport;
    const render::Ray ray = rayAt(vp->currentView(), vp->size(), t.mouse);
    const tools::DrawPlane plane = tools::planeThrough(t.angle.base, t.angle.normal);
    const auto at = tools::rayPlane(ray, plane);
    if (!at) return;
    const int ei = t.base->findEntity(t.angle.entityId);
    if (ei < 0) return;
    const core::Vec3 cur = t.base->entities[static_cast<size_t>(ei)]->typed.angles;
    const double snapDeg = render::rotSnapRadians(vp->rig().settings()) * 180.0 / 3.14159265358979323846;
    const auto a = tools::anglesToward(cur, {at->x - t.angle.base.x, at->y - t.angle.base.y, at->z - t.angle.base.z}, t.angle.keepPitch, snapDeg);
    if (!a) return;
    const tools::EditResult r = tools::setAngles(t.base, t.angle.entityId, *a);
    const std::shared_ptr<const doc::DocumentVersion> after = r.version ? r.version : t.base;   // back at the start angles: the base itself
    if (after != document_) history_->commit({t.label, document_, after});
    char buf[96];
    std::snprintf(buf, sizeof buf, "Angle: yaw %.2f\xC2\xB0, pitch %.2f\xC2\xB0", a->y, a->x);
    statusBar()->showMessage(QString::fromUtf8(buf));
    refreshGizmos();
}

std::vector<render::LineVertex> MainWindow::angleHandleLinesFor(size_t index) const {
    if (index >= panels_.size() || !panels_[index].window) return {};
    render::ViewportWindow* vp = panels_[index].window;
    std::optional<AngleHandle> h;
    const bool dragging = transform_ && transform_->angle.active;
    if (dragging) {
        if (transform_->viewport != vp) return {};
        const int ei = document_ ? document_->findEntity(transform_->angle.entityId) : -1;
        if (ei < 0) return {};
        const scene::DirectionSpan span = scene::directionSpan(document_->entities[static_cast<size_t>(ei)]->typed, scene::classBoxes(entityDefs_.get()));
        AngleHandle a;
        a.entityId = transform_->angle.entityId;
        a.base = {span.from[0], span.from[1], span.from[2]};
        a.tip = {span.from[0] + span.dir[0] * span.len, span.from[1] + span.dir[1] * span.len, span.from[2] + span.dir[2] * span.len};
        h = a;
    } else {
        h = angleHandle();
    }
    if (!h) return {};
    const render::CameraBasis b = vp->rig().active().basis();
    const double half = kAngleHalfPx * tools::metresPerPixelAt(vp->rig().active().distanceTo(h->tip), vp->rig().active().fovY, std::max(1, vp->height()));
    return tools::angleHandleLines(h->base, h->tip, b.right, b.up, half, dragging);
}

} // namespace native::ui
