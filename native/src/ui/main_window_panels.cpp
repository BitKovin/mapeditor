// ui/main_window_panels -- the Place, Outliner and Details docks of the main window and what they ask of it (T4.4): the docks themselves, hide toggles and
// selection from the Outliner, Details edits (tools/details), and placing entities and prefabs where the camera looks.
#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>

#include <QStatusBar>

#include <cmath>

#include "core/entdef.h"
#include "doc/selection.h"
#include "render/picking.h"
#include "render/viewport_window.h"
#include "scene/point_box.h"
#include "tools/entity_edit.h"
#include "ui/details_panel.h"
#include "ui/main_window.h"
#include "ui/outliner_panel.h"
#include "ui/place_panel.h"
#include "ui/surface_panel.h"

namespace native::ui {

namespace {
constexpr int kLeftWidth = 250;
constexpr int kRightWidth = 380;
} // namespace

void MainWindow::createToolDocks() {
    const auto make = [this](const char* title, const char* objectName, QWidget* w) {
        auto* dock = new ads::CDockWidget(dockManager_, QString::fromLatin1(title));
        dock->setObjectName(QString::fromLatin1(objectName));
        dock->setWidget(w, ads::CDockWidget::ForceNoScrollArea);
        return dock;
    };
    outliner_ = new OutlinerPanel(*this);
    outlinerDock_ = make("Outliner", "outlinerDock", outliner_);
    details_ = new DetailsPanel(*this);
    detailsDock_ = make("Details", "detailsDock", details_);
    surface_ = new SurfacePanel(*this);
    surfaceDock_ = make("Surface", "surfaceDock", surface_);
    place_ = new PlacePanel(*this);
    placeDock_ = make("Place", "placeDock", place_);
}

// Place on the left; on the right the Outliner above the Details, which share their area with Stats as tabs.
void MainWindow::layoutToolDocks(ads::CDockAreaWidget* statsArea) {
    detailsDock_->setAsCurrentTab();
    ads::CDockAreaWidget* detailsArea = dockManager_->addDockWidget(ads::CenterDockWidgetArea, detailsDock_, statsArea);
    dockManager_->addDockWidget(ads::CenterDockWidgetArea, surfaceDock_, detailsArea);
    detailsDock_->setAsCurrentTab();
    ads::CDockAreaWidget* outlinerArea = dockManager_->addDockWidget(ads::TopDockWidgetArea, outlinerDock_, detailsArea);
    dockManager_->setSplitterSizes(outlinerArea, {360, 440});
    (void)kRightWidth;
}

// ---- what the panels ask for -------------------------------------------------------------------------------------------------------

void MainWindow::toggleHiddenKey(const std::string& key) {
    std::set<std::string> keys = hide_.keys;
    if (!keys.erase(key)) keys.insert(key);
    setHiddenKeys(keys);
}

bool MainWindow::selectFromOutliner(const doc::SelRef& ref, bool hidden, bool toggle) {
    if (!document_) return false;
    if (hidden) {   // a hidden thing cannot be selected (it could be moved or deleted unseen): the click only says how to get it back
        statusBar()->showMessage(QString::fromLatin1(tools::hiddenRowMessage()), 5000);
        return false;
    }
    if (toggle) selection_->toggle(*document_, ref);
    else selection_->select(ref);
    return true;
}

bool MainWindow::editDetails(const tools::DetailsEdit& edit, const tools::EditValue& value) {
    if (!document_) return false;
    return applyEdit(tools::applyDetailsEdit(document_, entityDefs_.get(), edit, value), false);
}

render::Vec3 MainWindow::placementPoint(const std::string& classname) const {
    render::Vec3 point{0, 0, 0}, normal{0, 1, 0};
    if (render::ViewportWindow* vp = activeViewport()) {
        const render::Camera& cam = vp->rig().active();
        const render::Vec3 eye = cam.eye(), target = cam.target();
        const render::Vec3 delta = target - eye;
        const double len = render::length(delta);
        point = target;
        if (len > 1e-9) {
            const render::Ray ray{eye, delta * (1.0 / len)};
            std::optional<render::PickHit> hit;
            if (picker_) hit = picker_->pick(ray, pickOptions());
            if (hit) {
                point = hit->point;
                normal = hit->normal;
            } else if (ray.dir.y < -1e-3 && eye.y > 0) {   // the ground at height 0, when it is in front of the camera and not too far
                const double s = -eye.y / ray.dir.y;
                if (s < 200) {
                    point = eye + ray.dir * s;
                    normal = {0, 1, 0};
                }
            }
        }
    }
    // the entity rests on the surface it lands on: its box is pushed out along the normal
    if (classname.empty()) return point;   // a prefab: the point itself
    const scene::Box box = scene::classBoxes(entityDefs_.get())(classname);
    render::Vec3 o = point;
    for (int i = 0; i < 3; ++i) {
        if (normal[i] > 0.5) o[i] = point[i] - box.min[static_cast<size_t>(i)];
        else if (normal[i] < -0.5) o[i] = point[i] - box.max[static_cast<size_t>(i)];
    }
    return o;
}

bool MainWindow::placeEntity(const std::string& classname, const std::vector<std::pair<std::string, std::string>>& keyvalues) {
    if (!document_ || classname.empty()) return false;
    const render::Vec3 o = placementPoint(classname);
    return applyEdit(tools::addPointEntity(document_, classname, {o.x, o.y, o.z}, keyvalues));
}

bool MainWindow::placePrefab(const std::string& file) {
    if (!document_ || file.empty()) return false;
    const render::Vec3 o = placementPoint("");
    return applyEdit(tools::spawnInstance(document_, file, {o.x, o.y, o.z}));
}

} // namespace native::ui
