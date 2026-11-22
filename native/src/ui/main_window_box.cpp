// ui/main_window_box -- the box / shape tool of the main window (T4.2) and the tool state (select or box, the shape and its parameters).
// Press on a surface (or on the ground when the ray hits nothing) and drag: a rectangle on that plane. Release, move the pointer to set the height, click to
// finish (Shift = square, then cube; Esc or the right button cancel). The result is one undo step ("Draw box", "Draw stairs" ...) and gets selected.
#include <QGuiApplication>
#include <QStatusBar>

#include <algorithm>
#include <cmath>

#include "core/document.h"
#include "doc/edit.h"
#include "doc/selection.h"
#include "render/picking.h"
#include "render/render_thread.h"
#include "render/viewport_window.h"
#include "tools/entity_edit.h"
#include "tools/gizmo.h"
#include "tools/shapes.h"
#include "ui/box_drawing.h"
#include "ui/main_window.h"
#include "tools/parametric.h"
#include "ui/view_helpers.h"

namespace native::ui {

namespace {

bool shiftHeld() { return QGuiApplication::queryKeyboardModifiers().testFlag(Qt::ShiftModifier); }

} // namespace

// ---- the tool state ---------------------------------------------------------------------------------------------------------------------

void MainWindow::setTool(Tool tool) {
    if (tool_ == tool) return;
    if (tool != Tool::Box) cancelBox();
    if ((tool == Tool::Box || tool == Tool::Decal) && meshMode()) leaveMeshMode();   // drawing a new solid ends the mesh editing of the brush
    tool_ = tool;
    refreshGizmos();
    emit toolChanged();
    statusBar()->showMessage(tool == Tool::Box     ? QStringLiteral("Box tool: drag a rectangle, move for the height, click to finish (Shift: square / cube, Esc cancels)")
                             : tool == Tool::Decal ? QStringLiteral("Decal tool: click a surface. The texture is the active one (use one with an alpha channel)")
                                                   : QStringLiteral("Select tool"),
                             5000);
}

void MainWindow::setShape(tools::ShapeId shape) {
    shape_ = shape;
    setTool(Tool::Box);
    emit toolChanged();
    updateBoxPreview();
}

void MainWindow::setShapeParams(const tools::ShapeParams& params) {
    shapeParams_ = params;
    shapeParams_.turn &= 3;
    updateBoxPreview();
    emit toolChanged();
}

void MainWindow::turnShape(int quarterTurns) {
    tools::ShapeParams p = shapeParams_;
    p.turn = ((p.turn + quarterTurns) % 4 + 4) % 4;
    setShapeParams(p);
}

// ---- the decal tool ----------------------------------------------------------------------------------------------------------------------

void MainWindow::setDecalParams(const DecalParams& params) {
    DecalParams p = params;
    const auto clampSize = [](double v) { return std::clamp(v, 0.05, 50.0); };
    p.width = clampSize(p.width);
    p.height = clampSize(p.height);
    p.depth = std::clamp(p.depth, 0.05, 10.0);
    if (p == decalParams_) return;
    decalParams_ = p;
    emit toolChanged();
}

bool MainWindow::placeDecalAt(render::ViewportWindow* vp, QPointF p) {
    if (!vp || !document_ || !picker_) return false;
    const QSizeF size = vp->size();
    if (size.width() < 1 || size.height() < 1) return false;
    const render::Ray ray = rayAt(vp->currentView(), vp->size(), p);
    const std::optional<render::PickHit> hit = picker_->pick(ray, pickOptions());
    if (!hit || hit->brushId.empty()) {
        statusBar()->showMessage(QStringLiteral("Click on a surface to place a decal"), 4000);
        return false;
    }
    return applyEdit(tools::addDecal(document_, activeTexture_, {hit->point.x, hit->point.y, hit->point.z}, {hit->normal.x, hit->normal.y, hit->normal.z}, decalParams_.width,
                                     decalParams_.height, decalParams_.depth));
}

// ---- drawing ----------------------------------------------------------------------------------------------------------------------------

bool MainWindow::drawingBox() const { return box_ != nullptr; }

void MainWindow::boxInput(render::ViewportWindow* vp, const actions::InputEvent& e) {
    using Type = actions::InputEvent::Type;
    if (!vp || !document_) return;
    const QPointF p(e.x, e.y);
    if (e.type == Type::MousePress) {
        if (box_) return;
        const render::ViewState view = vp->currentView();
        const render::Ray ray = rayAt(view, vp->size(), p);
        std::optional<render::PickHit> hit;
        if (picker_) hit = picker_->pick(ray, pickOptions());
        const tools::DrawPlane plane = hit ? tools::planeThrough(hit->point, hit->normal) : tools::fallbackPlane(ray);
        // the start point lies on the world grid (measured along the surface), never where the click landed
        const render::Vec3 p0 = tools::snapToPlane(hit ? hit->point : tools::rayPlane(ray, plane).value_or(plane.origin), plane, render::snapStep(vp->rig().settings()));
        box_ = std::make_unique<BoxDrawing>();
        box_->viewport = vp;
        box_->plane = plane;
        box_->start = box_->current = p0;
        updateBoxPreview();
        return;
    }
    if (!box_ || box_->viewport != vp || box_->phase != BoxDrawing::Phase::Rect) return;
    if (e.type == Type::MouseMove) {
        boxMove(p);
    } else if (e.type == Type::MouseRelease) {
        const tools::Rect rect = tools::rectFromDrag(box_->plane, box_->start, box_->current, shiftHeld());
        if (std::fabs(rect.du) < tools::kMinSize || std::fabs(rect.dv) < tools::kMinSize) {   // a click without a drag (or a sliver) cancels
            cancelBox();
            return;
        }
        box_->phase = BoxDrawing::Phase::Height;
        box_->rect = rect;
        box_->height = 0;
        boxMove(p);
    }
}

// The pointer moved with the drawing in progress: phase 1 follows it on the plane, phase 2 sets the height.
void MainWindow::boxMove(QPointF p) {
    if (!box_) return;
    render::ViewportWindow* vp = box_->viewport;
    const render::ViewState view = vp->currentView();
    const render::Ray ray = rayAt(view, vp->size(), p);
    const double step = render::snapStep(vp->rig().settings());
    if (box_->phase == BoxDrawing::Phase::Rect) {
        if (const auto q = tools::rayPlane(ray, box_->plane)) box_->current = tools::snapToPlane(*q, box_->plane, step);
    } else {
        const tools::Rect& r = box_->rect;
        const render::Vec3 centre = r.origin + box_->plane.u * (r.du / 2) + box_->plane.v * (r.dv / 2);
        if (const auto raw = tools::lineDistance(ray, centre, box_->plane.normal)) box_->height = tools::heightFor(*raw, r, shiftHeld(), step);
    }
    updateBoxPreview();
}

bool MainWindow::boxMouse(size_t index, const actions::InputEvent& e) {
    using Type = actions::InputEvent::Type;
    if (!box_ || index >= panels_.size() || panels_[index].window != box_->viewport) return false;
    if (box_->phase != BoxDrawing::Phase::Height) return false;
    if (e.type == Type::MouseMove) {
        boxMove(QPointF(e.x, e.y));
        return true;
    }
    if (e.type == Type::MousePress && e.button == 1) {   // the second click finishes the shape
        commitBox();
        return true;
    }
    if (e.type == Type::MousePress && e.button == 2) {
        cancelBox();
        return true;
    }
    return false;
}

void MainWindow::cancelBox() {
    if (!box_) return;
    box_.reset();
    updateBoxPreview();
    statusBox();
}

void MainWindow::statusBox() { statusBar()->showMessage(QStringLiteral("Box tool"), 2000); }

bool MainWindow::commitBox() {
    if (!box_ || box_->phase != BoxDrawing::Phase::Height || !document_) return false;
    const BoxDrawing d = *box_;
    box_.reset();
    std::vector<core::Brush> brushes;
    std::string label = "Draw box";
    if (shape_ == tools::ShapeId::Box) {
        if (auto b = tools::buildBrush(tools::newId("b"), d.plane, d.rect, d.height, activeTexture_)) brushes.push_back(std::move(*b));
    } else {
        const tools::ShapeOutcome r = tools::buildShape(shape_, d.plane, d.rect, d.height, activeTexture_, shapeParams_, [] { return tools::newId("b"); }, shapeParams_.turn);
        if (!r.ok()) {
            statusBar()->showMessage(QString::fromStdString(r.error), 6000);
            updateBoxPreview();
            return false;
        }
        brushes = r.brushes;
        label = std::string("Draw ") + tools::shapeName(shape_);
    }
    updateBoxPreview();
    if (brushes.empty()) return false;
    doc::VersionEditor ed(document_);
    std::string worldId;
    for (const auto& e : document_->entities)
        if (e->typed.classname == "worldspawn") {
            worldId = e->typed.id;
            break;
        }
    if (worldId.empty()) {
        core::Entity w;
        w.id = worldId = "e_worldspawn";
        w.classname = "worldspawn";
        if (!ed.addEntity(w)) return false;
    }
    if (shape_ != tools::ShapeId::Box) {   // every part carries the record that lets the Details panel rebuild the shape
        if (!tools::parametric::addShape(ed, worldId, brushes, shape_, tools::frameOf(d.plane, d.rect, d.height), shapeParams_, shapeParams_.turn, tools::newId("s"))) {
            statusBar()->showMessage(QString::fromStdString(ed.error()), 6000);
            return false;
        }
    } else {
        for (const core::Brush& b : brushes)
            if (!ed.addBrush(worldId, b)) {
                statusBar()->showMessage(QString::fromStdString(ed.error()), 6000);
                return false;
            }
    }
    const auto after = ed.finish();
    if (!after || !commitEdit(label, after)) return false;
    std::vector<doc::SelRef> refs;
    for (const core::Brush& b : brushes) refs.push_back(doc::SelRef::brush(worldId, b.id));
    selection_->selectMany(*document_, refs);
    statusBar()->showMessage(QString::fromStdString(label), 3000);
    return true;
}

// The lines of the drawing in progress (the rectangle, then the shape or the box), for the viewport it is drawn in.
std::vector<render::LineVertex> MainWindow::boxPreviewLines(size_t panelIndex) const {
    std::vector<render::LineVertex> out;
    if (!box_ || panelIndex >= panels_.size() || panels_[panelIndex].window != box_->viewport) return out;
    std::vector<float> flat;
    if (box_->phase == BoxDrawing::Phase::Rect) {
        flat = tools::previewLines(box_->plane, tools::rectFromDrag(box_->plane, box_->start, box_->current, shiftHeld()), std::nullopt);
    } else if (std::fabs(box_->height) < tools::kMinSize) {
        flat = tools::previewLines(box_->plane, box_->rect, std::nullopt);
    } else {
        if (shape_ != tools::ShapeId::Box) flat = tools::shapePreviewLines(shape_, box_->plane, box_->rect, box_->height, shapeParams_, shapeParams_.turn);
        if (flat.empty()) flat = tools::previewLines(box_->plane, box_->rect, box_->height);
    }
    const uint32_t c = render::packColor(0.45f, 0.85f, 1.0f, 1.0f);
    for (size_t i = 0; i + 2 < flat.size(); i += 3) out.push_back({{flat[i], flat[i + 1], flat[i + 2]}, c});
    return out;
}

void MainWindow::updateBoxPreview() {
    for (size_t i = 0; i < panels_.size(); ++i) refreshGizmo(i);
}

} // namespace native::ui
