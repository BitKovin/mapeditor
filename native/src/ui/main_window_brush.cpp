// ui/main_window_brush -- MainWindow: the Radiant brush operations (T11.2): Hollow, Merge, Snap, Select inside / touching and the clipper tool.
// The operations are tools/brush_edit (pure); here they become one undo step each and the selection follows.
#include <QStatusBar>

#include <algorithm>
#include <cmath>

#include "doc/commands.h"
#include "doc/selection.h"
#include "render/line_vertex.h"
#include "render/picking.h"
#include "render/viewport_window.h"
#include "tools/gizmo.h"
#include "ui/main_window.h"
#include "ui/view_helpers.h"

namespace native::ui {

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

void addLine(std::vector<render::LineVertex>& out, const core::Vec3& a, const core::Vec3& b, uint32_t rgba) {
    out.push_back({{static_cast<float>(a.x), static_cast<float>(a.y), static_cast<float>(a.z)}, rgba});
    out.push_back({{static_cast<float>(b.x), static_cast<float>(b.y), static_cast<float>(b.z)}, rgba});
}

core::Vec3 toCore(const render::Vec3& v) { return {v.x, v.y, v.z}; }
render::Vec3 toRender(const core::Vec3& v) { return {v.x, v.y, v.z}; }

} // namespace

// ---- the operations ------------------------------------------------------------------------------------------------------------------

bool MainWindow::applyBrushEdit(const tools::BrushEdit& edit) {
    if (!edit.ok()) {
        if (!edit.message.empty()) statusBar()->showMessage(qs(edit.message), 6000);
        return false;
    }
    if (!commitEdit(edit.label, edit.version)) return false;
    if (!edit.select.empty()) selection_->selectMany(*document_, edit.select);
    statusBar()->showMessage(qs(edit.message.empty() ? edit.label : edit.message), 4000);
    refreshOutline();
    return true;
}

bool MainWindow::hollowSelected(double thickness) {
    if (!document_) return false;
    return applyBrushEdit(tools::hollowSelection(document_, selection_->items(), thickness > 0 ? thickness : 0.25));
}

bool MainWindow::mergeSelected() {
    if (!document_) return false;
    return applyBrushEdit(tools::mergeSelection(document_, selection_->items()));
}

bool MainWindow::snapSelected() {
    if (!document_) return false;
    const render::ViewportWindow* vp = activeViewport();
    const double grid = vp ? vp->rig().settings().gridSize : 0.25;
    return applyBrushEdit(tools::snapSelection(document_, selection_->items(), grid > 0 ? grid : 0.25));
}

bool MainWindow::selectByVolume(tools::VolumeMode mode) {
    if (!document_) return false;
    const tools::VolumeSelect r = tools::selectByVolume(*document_, selection_->items(), mode);
    statusBar()->showMessage(qs(r.message), 5000);
    if (r.found.empty()) return false;
    selection_->selectMany(*document_, r.found);
    refreshOutline();
    return true;
}

// ---- the clipper ---------------------------------------------------------------------------------------------------------------------

bool MainWindow::startClipper() {
    if (!document_ || clip_ || transform_ || knife_ || box_ || meshMode()) return false;
    if (tools::selectedBrushes(*document_, selection_->items()).empty()) {
        statusBar()->showMessage(QStringLiteral("Clipper: select a brush first"), 5000);
        return false;
    }
    clip_ = ClipState{};
    statusBar()->showMessage(QStringLiteral("Clipper: click 2-3 points (2 = plane along the view). Enter keeps the back side, Shift+Enter splits, Ctrl+Enter keeps the front, Esc cancels"), 10000);
    setContextsFor(activeViewport());
    refreshGizmos();
    return true;
}

void MainWindow::stopClipper(const QString& message) {
    if (!clip_) return;
    clip_.reset();
    if (!message.isEmpty()) statusBar()->showMessage(message, 3000);
    setContextsFor(activeViewport());
    refreshGizmos();
}

void MainWindow::addClipperPoint(const core::Vec3& point, const render::Vec3& viewDir) {
    if (!clip_) return;
    if (clip_->pts.size() >= 3) clip_->pts.clear();   // a fourth click starts over
    for (const ClipPoint& c : clip_->pts)
        if ((c.p - point).length() < 1e-9) {
            statusBar()->showMessage(QStringLiteral("Clipper: that point is already used"), 4000);
            return;
        }
    clip_->pts.push_back({point, viewDir});
    refreshGizmos();
}

std::vector<core::Vec3> MainWindow::clipperPoints() const {
    std::vector<core::Vec3> out;
    if (clip_)
        for (const ClipPoint& c : clip_->pts) out.push_back(c.p);
    return out;
}

std::optional<tools::Plane> MainWindow::clipperPlane() const {
    if (!clip_ || clip_->pts.size() < 2) return std::nullopt;
    const core::Vec3 a = clip_->pts[0].p, b = clip_->pts[1].p;
    const core::Vec3 c = clip_->pts.size() > 2 ? clip_->pts[2].p : a + toCore(clip_->pts[1].dir);   // two points: the plane contains the view direction
    return tools::planeFromPoints(a, b, c);
}

bool MainWindow::clipperApply(tools::Keep keep) {
    if (!clip_) return false;
    const auto pl = clipperPlane();
    if (!pl) {
        statusBar()->showMessage(QStringLiteral("Clipper: place at least two points first"), 4000);
        return false;
    }
    const tools::BrushEdit r = tools::clipSelection(document_, selection_->items(), *pl, keep);
    if (!applyBrushEdit(r)) return false;
    stopClipper();
    return true;
}

bool MainWindow::clipperMouse(size_t index, const actions::InputEvent& e) {
    using Type = actions::InputEvent::Type;
    if (!clip_ || index >= panels_.size() || !panels_[index].window) return false;
    if (tools::selectedBrushes(*document_, selection_->items()).empty()) {   // the brushes went away (undo, delete): the clipper has nothing to cut
        stopClipper(QStringLiteral("Clipper stopped: no brush is selected"));
        return false;
    }
    render::ViewportWindow* vp = panels_[index].window;
    const render::ViewState view = vp->currentView();
    const QSizeF size = vp->size();
    const double step = render::snapStep(vp->rig().settings());
    const bool ortho = render::isOrthoMode(vp->rig().mode());
    const render::Vec3 forward = vp->rig().active().basis().forward;
    const render::Ray ray = rayAt(view, size, QPointF(e.x, e.y));
    if (e.type == Type::MouseMove) {
        if (!clip_->drag || *clip_->drag >= clip_->pts.size()) return false;
        // a dragged point stays on the plane through its old position facing the camera, snapped to the grid (not along the depth axis of an ortho view)
        const auto hit = tools::rayPlaneHit(ray, clip_->pts[*clip_->drag].p, toCore(forward));
        if (!hit) return true;
        std::vector<core::Vec3> pts;
        for (const ClipPoint& c : clip_->pts) pts.push_back(c.p);
        const core::Vec3 q = tools::snapPoint(*hit, step, ortho ? tools::depthAxis(toCore(forward)) : -1);
        if (tools::clashes(pts, *clip_->drag, q)) return true;
        clip_->pts[*clip_->drag].p = q;
        refreshGizmos();
        return true;
    }
    if (e.button != 1) return false;
    if (e.type == Type::MouseRelease) {
        clip_->drag.reset();
        return true;   // the press was ours
    }
    if (e.type != Type::MousePress) return false;
    // grabbing an existing point drags it
    std::vector<core::Vec3> pts;
    for (const ClipPoint& c : clip_->pts) pts.push_back(c.p);
    const auto screen = [&](const core::Vec3& p) -> std::optional<std::pair<double, double>> {
        const tools::Pixel s = screenOf(view, size, toRender(p));
        return std::make_pair(s.x, s.y);
    };
    if (const auto grab = tools::hitClipPoint(pts, screen, e.x, e.y)) {
        clip_->drag = *grab;
        return true;
    }
    std::optional<core::Vec3> p;
    if (ortho) {   // the point lies in the plane through the selection's centre
        core::Vec3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
        for (const tools::SelectedBrush& sb : tools::selectedBrushes(*document_, selection_->items()))
            for (const core::Vec3& v : sb.brush().vertices) {
                lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
                hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
            }
        p = tools::rayPlaneHit(ray, (lo + hi) * 0.5, toCore(forward));
    } else {
        std::optional<render::PickHit> hit;
        if (picker_) hit = picker_->pick(ray, pickOptions());
        if (hit) p = toCore(hit->point);
        else if (std::abs(ray.dir.y) > 1e-6 && -ray.origin.y / ray.dir.y > 0) p = toCore(ray.origin) + toCore(ray.dir) * (-ray.origin.y / ray.dir.y);   // the ground
    }
    if (!p) {
        statusBar()->showMessage(QStringLiteral("Clipper: click on a surface or the ground"), 4000);
        return true;
    }
    addClipperPoint(tools::snapPoint(*p, step), ray.dir);
    return true;
}

std::vector<render::LineVertex> MainWindow::clipperLines(size_t index) const {
    std::vector<render::LineVertex> out;
    if (!clip_ || index >= panels_.size() || !panels_[index].window) return out;
    render::ViewportWindow* vp = panels_[index].window;
    const uint32_t color = render::packColor(1.0f, 0.55f, 0.1f, 1.0f);
    const auto seg = [&](const core::Vec3& a, const core::Vec3& b) { addLine(out, a, b, color); };
    std::vector<core::Vec3> pts;
    for (const ClipPoint& c : clip_->pts) pts.push_back(c.p);
    for (const core::Vec3& p : pts) {
        const double r = vp->rig().active().distanceTo(toRender(p)) * 0.012;
        seg(p + core::Vec3{r, 0, 0}, p + core::Vec3{-r, 0, 0});
        seg(p + core::Vec3{0, r, 0}, p + core::Vec3{0, -r, 0});
        seg(p + core::Vec3{0, 0, r}, p + core::Vec3{0, 0, -r});
    }
    for (size_t i = 0; i + 1 < pts.size(); ++i) seg(pts[i], pts[i + 1]);
    if (pts.size() == 3) seg(pts[2], pts[0]);
    if (const auto pl = clipperPlane()) {   // a quad on the plane around the selection, and an arrow to the side that is CUT AWAY with Enter
        core::Vec3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
        for (const tools::SelectedBrush& sb : tools::selectedBrushes(*document_, selection_->items()))
            for (const core::Vec3& v : sb.brush().vertices) {
                lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
                hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
            }
        if (lo.x < 1e299) {
            const core::Vec3 c0 = (lo + hi) * 0.5;
            const double half = std::max(0.5, (hi - lo).length() * 0.6);
            const core::Vec3 c = c0 - pl->n * (pl->n.dot(c0) - pl->d);
            const core::Vec3 ref = std::abs(pl->n.y) < 0.9 ? core::Vec3{0, 1, 0} : core::Vec3{1, 0, 0};
            const core::Vec3 u = pl->n.cross(ref).normalized();
            const core::Vec3 v = pl->n.cross(u);
            const auto corner = [&](double a, double b) { return c + u * (a * half) + v * (b * half); };
            const core::Vec3 q[4] = {corner(-1, -1), corner(1, -1), corner(1, 1), corner(-1, 1)};
            for (int i = 0; i < 4; ++i) seg(q[i], q[(i + 1) % 4]);
            seg(c, c + pl->n * (half * 0.35));
        }
    }
    return out;
}

} // namespace native::ui
