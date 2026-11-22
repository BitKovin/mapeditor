// ui/main_window_measure -- MainWindow: the Measure tool (10_UI_AND_INPUT.md U13). Left clicks place two points on a surface (or the ground, or the plane through the view target in
// an orthographic view), snapped to the grid; while the second one is not placed the point under the cursor stands in for it; a third click starts over. The status bar shows the
// distance and the per-axis deltas, the viewport draws the line with its X / Y / Z legs. Esc or the action again ends the tool.
#include <QStatusBar>

#include <algorithm>
#include <cmath>

#include "render/line_vertex.h"
#include "render/picking.h"
#include "render/viewport_window.h"
#include "tools/brush_ops.h"
#include "tools/gizmo.h"
#include "tools/measure.h"
#include "ui/main_window.h"
#include "ui/view_helpers.h"

namespace native::ui {

namespace {

void addLine(std::vector<render::LineVertex>& out, const core::Vec3& a, const core::Vec3& b, uint32_t rgba) {
    out.push_back({{static_cast<float>(a.x), static_cast<float>(a.y), static_cast<float>(a.z)}, rgba});
    out.push_back({{static_cast<float>(b.x), static_cast<float>(b.y), static_cast<float>(b.z)}, rgba});
}

core::Vec3 toCore(const render::Vec3& v) { return {v.x, v.y, v.z}; }
render::Vec3 toRender(const core::Vec3& v) { return {v.x, v.y, v.z}; }

} // namespace

bool MainWindow::startMeasure() {
    if (!canSelectItems() || measure_) return false;
    measure_ = MeasureState{};
    statusBar()->showMessage(QStringLiteral("Measure: click two points (Esc ends)"), 8000);
    setContextsFor(activeViewport());
    refreshGizmos();
    return true;
}

void MainWindow::stopMeasure() {
    if (!measure_) return;
    measure_.reset();
    statusBar()->showMessage(QStringLiteral("Measure ended"), 3000);
    setContextsFor(activeViewport());
    refreshGizmos();
}

std::vector<core::Vec3> MainWindow::measurePoints() const {
    std::vector<core::Vec3> out;
    if (measure_) out = measure_->pts;
    return out;
}

void MainWindow::addMeasurePoint(const core::Vec3& p) {
    if (!measure_) return;
    if (measure_->pts.size() >= 2) measure_->pts.clear();   // a third click starts over
    measure_->pts.push_back(p);
    measure_->hover.reset();
    if (measure_->pts.size() == 2) statusBar()->showMessage(QString::fromStdString(measureReading()), 0);
    else statusBar()->showMessage(QStringLiteral("Measure: click the second point"), 0);
    refreshGizmos();
}

std::string MainWindow::measureReading() const {
    if (!measure_ || measure_->pts.empty()) return {};
    const core::Vec3 a = measure_->pts[0];
    const std::optional<core::Vec3> b = measure_->pts.size() > 1 ? std::optional<core::Vec3>(measure_->pts[1]) : measure_->hover;
    if (!b) return {};
    return tools::measureText(tools::measure(a, *b));
}

bool MainWindow::measureMouse(size_t index, const actions::InputEvent& e) {
    using Type = actions::InputEvent::Type;
    if (!measure_ || index >= panels_.size() || !panels_[index].window || !document_) return false;
    if (e.type != Type::MousePress && e.type != Type::MouseRelease && e.type != Type::MouseMove) return false;
    render::ViewportWindow* vp = panels_[index].window;
    const render::ViewState view = vp->currentView();
    const QSizeF size = vp->size();
    const double step = render::snapStep(vp->rig().settings());
    const bool ortho = render::isOrthoMode(vp->rig().mode());
    const render::Vec3 forward = vp->rig().active().basis().forward;
    const render::Ray ray = rayAt(view, size, QPointF(e.x, e.y));
    // where a click here would land
    const auto pointAt = [&]() -> std::optional<core::Vec3> {
        std::optional<core::Vec3> p;
        if (ortho) {   // the plane through the view target facing the camera; the depth axis is not snapped
            if (const auto hit = tools::rayPlaneHit(ray, toCore(vp->rig().active().target()), toCore(forward))) return tools::snapPoint(*hit, step, tools::depthAxis(toCore(forward)));
            return std::nullopt;
        }
        std::optional<render::PickHit> hit;
        if (picker_) hit = picker_->pick(ray, pickOptions());
        if (hit) p = toCore(hit->point);
        else if (std::abs(ray.dir.y) > 1e-6 && -ray.origin.y / ray.dir.y > 0) p = toCore(ray.origin) + toCore(ray.dir) * (-ray.origin.y / ray.dir.y);   // the ground
        if (!p) return std::nullopt;
        return tools::snapPoint(*p, step);
    };
    if (e.type == Type::MouseMove) {
        if (measure_->pts.size() != 1) return false;
        measure_->hover = pointAt();
        if (measure_->hover) statusBar()->showMessage(QString::fromStdString(measureReading()), 0);
        refreshGizmos();
        return false;   // the camera still sees the move
    }
    if (e.button != 1) return false;
    if (e.type == Type::MouseRelease) return true;   // the press was ours
    const auto p = pointAt();
    if (!p) {
        statusBar()->showMessage(QStringLiteral("Measure: click on a surface or the ground"), 4000);
        return true;
    }
    addMeasurePoint(*p);
    return true;
}

std::vector<render::LineVertex> MainWindow::measureLines(size_t index) const {
    std::vector<render::LineVertex> out;
    if (!measure_ || index >= panels_.size() || !panels_[index].window || measure_->pts.empty()) return out;
    render::ViewportWindow* vp = panels_[index].window;
    const uint32_t main = render::packColor(1.0f, 0.9f, 0.2f, 1.0f);
    const uint32_t xc = render::packColor(1.0f, 0.35f, 0.35f, 1.0f), yc = render::packColor(0.4f, 1.0f, 0.4f, 1.0f), zc = render::packColor(0.4f, 0.6f, 1.0f, 1.0f);
    std::vector<core::Vec3> pts = measure_->pts;
    if (pts.size() == 1 && measure_->hover) pts.push_back(*measure_->hover);
    for (const core::Vec3& p : measure_->pts) {
        const double r = vp->rig().active().distanceTo(toRender(p)) * 0.012;
        addLine(out, p + core::Vec3{r, 0, 0}, p + core::Vec3{-r, 0, 0}, main);
        addLine(out, p + core::Vec3{0, r, 0}, p + core::Vec3{0, -r, 0}, main);
        addLine(out, p + core::Vec3{0, 0, r}, p + core::Vec3{0, 0, -r}, main);
    }
    if (pts.size() == 2) {
        const core::Vec3 a = pts[0], b = pts[1];
        addLine(out, a, b, main);
        // the legs: X, then Z, then Y (up) - the staircase from a to b
        const core::Vec3 p1{b.x, a.y, a.z}, p2{b.x, a.y, b.z};
        addLine(out, a, p1, xc);
        addLine(out, p1, p2, zc);
        addLine(out, p2, b, yc);
    }
    return out;
}

} // namespace native::ui
