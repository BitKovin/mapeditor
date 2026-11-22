// ui/main_window_facedrag -- MainWindow: dragging one face along its normal and the 2D resize handles of an orthographic view (T11.2c).
// The geometry is tools/face_drag and tools/resize2d (pure); here the pointer becomes a distance / a pair of pixel offsets, every move is a document version computed from the
// version the drag began on, and the whole drag is one undo step (the same session machinery as the gizmo, ui/main_window_transform.cpp).
#include <QStatusBar>

#include <algorithm>
#include <cmath>

#include "doc/commands.h"
#include "doc/edit.h"
#include "doc/selection.h"
#include "render/line_vertex.h"
#include "render/picking.h"
#include "render/viewport_window.h"
#include "tools/face_drag.h"
#include "tools/gizmo.h"
#include "ui/main_window.h"
#include "ui/transform_session.h"
#include "ui/view_helpers.h"

namespace native::ui {

namespace {

constexpr double kResizeHitPx = 10;   // the grab radius of a resize handle
constexpr double kHandleHalfPx = 4;   // the drawn size of a resize handle

void addLine(std::vector<render::LineVertex>& out, const render::Vec3& a, const render::Vec3& b, uint32_t rgba) {
    out.push_back({{static_cast<float>(a.x), static_cast<float>(a.y), static_cast<float>(a.z)}, rgba});
    out.push_back({{static_cast<float>(b.x), static_cast<float>(b.y), static_cast<float>(b.z)}, rgba});
}

render::Vec3 asVec(const tools::V3& v) { return {v[0], v[1], v[2]}; }
tools::V3 asV3(const render::Vec3& v) { return {v.x, v.y, v.z}; }

const core::Brush* findBrush(const doc::DocumentVersion& d, const std::string& entityId, const std::string& brushId) {
    const int ei = d.findEntity(entityId);
    if (ei < 0) return nullptr;
    const int bi = d.findBrush(entityId, brushId);
    if (bi < 0) return nullptr;
    return &d.entities[static_cast<size_t>(ei)]->brushes[static_cast<size_t>(bi)]->brush;
}

// The outward normal and the centre of one face; false when the face is gone.
bool faceFrame(const core::Brush& b, uint32_t faceId, render::Vec3& pivot, render::Vec3& dir) {
    for (const core::BrushFace& f : b.faces) {
        if (f.id != faceId || f.loop.size() < 3) continue;
        core::Vec3 c{0, 0, 0};
        for (uint32_t i : f.loop) c = c + b.vertices[i];
        c = c * (1.0 / static_cast<double>(f.loop.size()));
        const core::Vec3 n = b.faceNormal(f);
        pivot = {c.x, c.y, c.z};
        dir = {n.x, n.y, n.z};
        return true;
    }
    return false;
}

render::Vec3 axisVec(const tools::ScreenAxis& a) {
    render::Vec3 v{0, 0, 0};
    (a.axis == 0 ? v.x : a.axis == 1 ? v.y : v.z) = a.sign;
    return v;
}

} // namespace

// ---- the handles ------------------------------------------------------------------------------------------------------------------------

std::optional<MainWindow::FaceHandle> MainWindow::faceHandle() const {
    if (!document_ || !selection_ || transform_ || tool_ != Tool::Select || box_ || knife_ || clip_ || meshMode()) return std::nullopt;
    const std::vector<doc::SelRef>& items = selection_->items();
    if (items.size() != 1 || !items[0].isFace() || !items[0].instancePath.empty()) return std::nullopt;
    const core::Brush* b = findBrush(*document_, items[0].entityId, items[0].brushId);
    if (!b) return std::nullopt;
    FaceHandle h;
    h.entityId = items[0].entityId;
    h.brushId = items[0].brushId;
    h.faceId = *items[0].faceId;
    if (!faceFrame(*b, h.faceId, h.pivot, h.dir)) return std::nullopt;
    return h;
}

std::optional<MainWindow::ResizeInfo> MainWindow::resizeInfo(const render::ViewportWindow* vp) const {
    if (!document_ || !selection_ || !vp || transform_ || tool_ != Tool::Select || box_ || knife_ || clip_ || meshMode()) return std::nullopt;
    if (!render::isOrthoMode(vp->rig().mode()) || faceHandle()) return std::nullopt;
    for (const doc::SelRef& s : selection_->items())
        if (s.isFace()) return std::nullopt;
    const auto* cam = dynamic_cast<const render::OrthoCamera*>(&vp->rig().active());
    if (!cam) return std::nullopt;
    ResizeInfo r;
    if (!tools::selectionBounds(*document_, selection_->items(), r.min, r.max)) return std::nullopt;
    const render::CameraBasis b = vp->rig().active().basis();
    r.right = tools::screenAxis(asV3(b.right));
    r.up = tools::screenAxis(asV3(b.up));
    if (r.right.axis == r.up.axis) return std::nullopt;
    r.metresPerPixel = cam->metresPerPixel(std::max(1, vp->height()));
    return r;
}

// ---- starting ---------------------------------------------------------------------------------------------------------------------------

bool MainWindow::beginFaceDrag(render::ViewportWindow* vp, QPointF p, bool alt) {
    if (transform_ || !document_ || !vp || tool_ != Tool::Select || box_ || knife_ || clip_ || meshMode()) return false;
    const size_t index = panelIndexOf(vp);
    if (index >= panels_.size()) return false;
    const render::ViewState view = vp->currentView();
    const render::Ray ray = rayAt(view, vp->size(), p);
    std::optional<FaceHandle> h;
    if (alt) {   // Alt + left button on any face: that face is selected and dragged
        if (!picker_) return false;
        const auto hit = picker_->pick(ray, pickOptions());
        if (!hit || !hit->instance.empty() || hit->pointEntity || hit->brushId.empty() || !hit->faceId) return false;
        selection_->select(doc::SelRef::face(hit->entityId, hit->brushId, *hit->faceId));
        refreshOutline();
        h = faceHandle();
    } else {
        h = faceHandle();
        if (h) {
            const double len = tools::gizmoLength(vp->rig().active().distanceTo(h->pivot));
            if (!tools::hitDir(ray, h->pivot, h->dir, len * 1.15, len * 0.07)) return false;
        }
    }
    if (!h) return false;
    const auto start = tools::lineDistance(ray, h->pivot, h->dir);
    if (!start) return false;
    auto t = std::make_unique<TransformSession>();
    t->source = TransformSession::Source::Gizmo;
    t->kind = tools::ModalKind::Grab;
    t->viewport = vp;
    t->panelIndex = index;
    t->base = document_;
    t->selection = selection_->items();
    t->pivot = h->pivot;
    t->mouse0 = t->mouse = p;
    t->startT = *start;
    t->label = alt ? "Resize brush" : "Drag face";
    t->face.active = true;
    t->face.entityId = h->entityId;
    t->face.brushId = h->brushId;
    t->face.faceId = h->faceId;
    t->face.dir = h->dir;
    history_->beginInteraction(t->label);
    transform_ = std::move(t);
    refreshGizmos();
    statusBar()->showMessage(QStringLiteral("Drag the face along its normal"), 4000);
    return true;
}

bool MainWindow::beginResizeDrag(render::ViewportWindow* vp, QPointF p) {
    if (transform_ || !vp) return false;
    const auto info = resizeInfo(vp);
    if (!info) return false;
    const size_t index = panelIndexOf(vp);
    if (index >= panels_.size()) return false;
    const render::ViewState view = vp->currentView();
    const QSizeF size = vp->size();
    const tools::Handle2D* best = nullptr;
    double bestD = kResizeHitPx;
    const std::vector<tools::Handle2D> handles = tools::handles2D(info->min, info->max, info->right, info->up);
    for (const tools::Handle2D& h : handles) {
        const tools::Pixel s = screenOf(view, size, asVec(h.pos));
        const double d = std::hypot(s.x - p.x(), s.y - p.y());
        if (d <= bestD) {
            bestD = d;
            best = &h;
        }
    }
    if (!best) return false;
    auto t = std::make_unique<TransformSession>();
    t->source = TransformSession::Source::Gizmo;
    t->kind = tools::ModalKind::Grab;
    t->viewport = vp;
    t->panelIndex = index;
    t->base = document_;
    t->selection = transformSelection();
    t->mouse0 = t->mouse = p;
    t->label = "Resize";
    t->resize.active = true;
    t->resize.min = info->min;
    t->resize.max = info->max;
    t->resize.right = info->right;
    t->resize.up = info->up;
    t->resize.h = best->h;
    t->resize.v = best->v;
    t->resize.metresPerPixel = info->metresPerPixel;
    t->resize.width = info->max[static_cast<size_t>(info->right.axis)] - info->min[static_cast<size_t>(info->right.axis)];
    t->resize.height = info->max[static_cast<size_t>(info->up.axis)] - info->min[static_cast<size_t>(info->up.axis)];
    history_->beginInteraction(t->label);
    transform_ = std::move(t);
    refreshGizmos();
    return true;
}

// ---- running ----------------------------------------------------------------------------------------------------------------------------

void MainWindow::updateFaceDrag(TransformSession& t) {
    render::ViewportWindow* vp = t.viewport;
    const render::Ray ray = rayAt(vp->currentView(), vp->size(), t.mouse);
    const auto along = tools::lineDistance(ray, t.pivot, t.face.dir);
    if (!along) return;
    const double step = render::snapStep(vp->rig().settings());
    double dist = *along - t.startT;
    if (step > 0) dist = std::round(dist / step) * step;
    t.value = dist;
    const tools::FaceDragEdit r = tools::dragFaceEdit(t.base, t.face.entityId, t.face.brushId, t.face.faceId, dist);
    if (!r.version) {   // the last good version stays
        statusBar()->showMessage(QString::fromStdString("Drag face: " + r.error));
        return;
    }
    if (r.version != document_) history_->commit({t.label, document_, r.version});
    refreshGizmos();
    showTransformStatus();
}

void MainWindow::updateResize(TransformSession& t) {
    render::ViewportWindow* vp = t.viewport;
    const ResizeSession& r = t.resize;
    const auto metres = tools::pixelsToMetres(t.mouse.x() - t.mouse0.x(), t.mouse.y() - t.mouse0.y(), r.metresPerPixel);
    const double step = render::snapStep(vp->rig().settings());
    const tools::Affine map = tools::resizeMap(r.min, r.max, r.right, r.up, r.h, r.v, metres[0], metres[1], step);
    const tools::V3 lo = map(r.min), hi = map(r.max);
    t.resize.width = std::fabs(hi[static_cast<size_t>(r.right.axis)] - lo[static_cast<size_t>(r.right.axis)]);
    t.resize.height = std::fabs(hi[static_cast<size_t>(r.up.axis)] - lo[static_cast<size_t>(r.up.axis)]);
    const auto after = tools::transformSelections(t.base, t.selection, map, false, 0, uvLock_);
    if (!after) return;   // a brush that would not be valid: the last good version stays
    if (after != document_) history_->commit({t.label, document_, after});
    refreshGizmos();
    showTransformStatus();
}

// ---- drawing ----------------------------------------------------------------------------------------------------------------------------

std::vector<render::LineVertex> MainWindow::faceDragLines(size_t index) const {
    std::vector<render::LineVertex> out;
    if (index >= panels_.size() || !panels_[index].window || !document_) return out;
    render::ViewportWindow* vp = panels_[index].window;
    const uint32_t normal = render::packColor(0.25f, 0.8f, 1.0f, 1.0f);
    const uint32_t active = render::packColor(1.0f, 0.9f, 0.2f, 1.0f);
    const bool dragFace = transform_ && transform_->face.active;
    const bool dragResize = transform_ && transform_->resize.active;
    if (transform_ && transform_->viewport != vp) return out;
    if (transform_ && !dragFace && !dragResize) return out;

    std::optional<FaceHandle> h;
    if (dragFace) {   // the arrow follows the face as it moves
        const core::Brush* b = findBrush(*document_, transform_->face.entityId, transform_->face.brushId);
        FaceHandle f;
        if (b && faceFrame(*b, transform_->face.faceId, f.pivot, f.dir)) h = f;
    } else if (!transform_) {
        h = faceHandle();
    }
    if (h) {
        const double len = tools::gizmoLength(vp->rig().active().distanceTo(h->pivot));
        const render::Vec3 tip = h->pivot + h->dir * len;
        const render::Vec3 ref = std::abs(h->dir.y) < 0.9 ? render::Vec3{0, 1, 0} : render::Vec3{1, 0, 0};
        const render::Vec3 u = render::normalize(render::cross(h->dir, ref));
        const render::Vec3 v = render::cross(h->dir, u);
        const render::Vec3 base = tip - h->dir * (len * 0.18);
        const uint32_t color = dragFace ? active : normal;
        addLine(out, h->pivot, tip, color);
        for (const render::Vec3& s : {u * (len * 0.07), u * (-len * 0.07), v * (len * 0.07), v * (-len * 0.07)}) addLine(out, tip, base + s, color);
        return out;
    }

    std::optional<ResizeInfo> info;
    if (dragResize) {
        ResizeInfo r;
        r.right = transform_->resize.right;
        r.up = transform_->resize.up;
        r.metresPerPixel = transform_->resize.metresPerPixel;
        if (tools::selectionBounds(*document_, transform_->selection, r.min, r.max)) info = r;
    } else if (!transform_) {
        info = resizeInfo(vp);
    }
    if (!info) return out;
    const std::vector<tools::Handle2D> handles = tools::handles2D(info->min, info->max, info->right, info->up);
    const render::Vec3 right = axisVec(info->right), up = axisVec(info->up);
    const double half = kHandleHalfPx * info->metresPerPixel;
    for (const tools::Handle2D& handle : handles) {
        const bool on = dragResize && handle.h == transform_->resize.h && handle.v == transform_->resize.v;
        const uint32_t color = on ? active : normal;
        const render::Vec3 c = asVec(handle.pos);
        const render::Vec3 q[4] = {c + right * (-half) + up * (-half), c + right * half + up * (-half), c + right * half + up * half, c + right * (-half) + up * half};
        for (int i = 0; i < 4; ++i) addLine(out, q[i], q[(i + 1) % 4], color);
    }
    // the rectangle the corner handles sit on
    const auto at = [&](int h, int v) {
        for (const tools::Handle2D& handle : handles)
            if (handle.h == h && handle.v == v) return asVec(handle.pos);
        return render::Vec3{0, 0, 0};
    };
    const render::Vec3 corner[4] = {at(-1, 1), at(1, 1), at(1, -1), at(-1, -1)};
    for (int i = 0; i < 4; ++i) addLine(out, corner[i], corner[(i + 1) % 4], normal);
    return out;
}

} // namespace native::ui
