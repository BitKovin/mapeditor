// T11.11: the right button in a real viewport: Shift + right-click lifts the material of the face under the pointer, a plain right-click opens the object menu
// and selects what is under it (a real window and GPU: native_gui_tests).
#include <doctest.h>

#include <QMenu>
#include <QTemporaryDir>

#include "base/jobs.h"
#include "doc/edit.h"
#include "doc/io.h"
#include "gui_support.h"
#include "render/picking.h"

using namespace native;
using namespace guitest;

namespace {

std::shared_ptr<const doc::DocumentVersion> oneBox() {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    core::Entity e;
    e.id = "w";
    e.classname = "worldspawn";
    e.brushes.push_back(core::Brush::makeBox("b", {-1, -1, -1}, {1, 1, 1}, "dev/wall"));
    REQUIRE(ed.addEntity(e));
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

} // namespace

TEST_CASE("right button in a viewport: Shift + click lifts the material, a plain click opens the menu and selects" * doctest::test_suite("gpu")) {
    App app;
    if (!app.ok) return;
    base::JobPool pool(4);
    QTemporaryDir dir;
    ui::Services sv = app.services();
    sv.jobs = &pool;
    sv.assetsRoot = dir.path().toStdString();
    {
        ui::MainWindow window(sv);
        window.resize(900, 650);
        window.show();
        REQUIRE(waitUntil([&] { return allReady(window); }));
        window.setDocument(oneBox(), true);
        REQUIRE(waitUntil([&] { return window.sceneReady() && window.picker() && window.picker()->objectCount() == 1; }, 20000));
        pumpFor(200);
        render::ViewportWindow* vp = window.viewports()[0].window;
        vp->rig().fly().position = {0, 0, 6};
        vp->rig().fly().yaw = 3.14159265358979;
        vp->rig().fly().pitch = 0;
        vp->viewChanged();
        pumpFor(100);
        const QPoint mid(vp->width() / 2, vp->height() / 2);
        const QPoint corner(8, 8);
        const auto rightClick = [&](QPoint p, Qt::KeyboardModifiers mods = Qt::NoModifier) {
            sendMouse(vp, QEvent::MouseButtonPress, Qt::RightButton, Qt::RightButton, p, mods);
            sendMouse(vp, QEvent::MouseButtonRelease, Qt::RightButton, Qt::NoButton, p, mods);
        };

        // Shift + right-click on the box: its texture becomes the active one; on nothing: it stays
        window.setActiveTexture("dev/other");
        rightClick(corner, Qt::ShiftModifier);
        pumpFor(100);
        CHECK(window.activeTexture() == "dev/other");
        rightClick(mid, Qt::ShiftModifier);
        pumpFor(100);
        CHECK(window.activeTexture() == "dev/wall");
        CHECK(window.selection().empty());   // lifting does not select

        // a plain right-click selects the box and pops the menu up (after the event)
        rightClick(mid);
        REQUIRE(waitUntil([&] { return !window.selection().empty(); }, 3000));
        CHECK(window.selection().items()[0].brushId == "b");
        QMenu* menu = nullptr;
        REQUIRE(waitUntil([&] {
            for (QWidget* w : QApplication::topLevelWidgets())
                if (auto* m = qobject_cast<QMenu*>(w); m && m->isVisible()) menu = m;
            return menu != nullptr;
        }, 3000));
        CHECK(menu->actions().size() > 10);
        menu->close();
        pumpFor(100);
    }
}
