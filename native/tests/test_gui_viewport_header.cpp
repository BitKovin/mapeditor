// T14.4: the header strip above each viewport (camera / render / show menus, maximise, float) with a real window and viewports (native_gui_tests).
#include <doctest.h>

#include <QMenu>
#include <QToolButton>

#include <DockAreaTitleBar.h>
#include <DockAreaWidget.h>

#include "base/jobs.h"
#include "gui_support.h"
#include "ui/registry_menus.h"
#include "ui/viewport_header.h"

using namespace native;
using namespace guitest;

TEST_CASE("viewport header: menus, maximise and float act on their own viewport" * doctest::test_suite("gpu")) {
    App app;
    if (!app.ok) return;
    base::JobPool pool(4);
    ui::Services sv = app.services();
    sv.jobs = &pool;
    ui::MainWindow window(sv);
    window.resize(1200, 800);
    window.show();
    REQUIRE(waitUntil([&] { return allReady(window); }));
    window.runAction("view.layout", nlohmann::json{{"count", 2}});
    REQUIRE(waitUntil([&] { return window.viewports().size() == 2 && allReady(window); }));
    pumpFor(200);
    const auto& panels = window.viewports();
    REQUIRE(panels[0].header);
    REQUIRE(panels[1].header);

    // the strip replaces the dock's title bar, and shows the name of the camera and the render mode
    CHECK_FALSE(panels[0].dock->dockAreaWidget()->titleBar()->isVisible());
    CHECK(panels[0].header->isVisible());
    CHECK(panels[0].header->cameraButton()->text().contains("Persp"));
    CHECK(panels[1].header->cameraButton()->text().contains("Top"));
    CHECK(panels[0].header->renderButton()->text().startsWith("Unlit"));

    // a click on a menu button makes its viewport the active one and opens the menu of the window's menu bar
    window.setActiveViewport(0);
    CHECK(window.activeViewportIndex() == 0);
    panels[1].header->cameraButton()->click();
    CHECK(window.activeViewportIndex() == 1);
    QMenu* cameraMenu = window.menus()->menu("&View/&Camera (active viewport)");
    REQUIRE(cameraMenu);
    CHECK(cameraMenu->isVisible());
    cameraMenu->close();
    CHECK(panels[1].header->property("active").toBool());
    CHECK_FALSE(panels[0].header->property("active").toBool());

    // changing the render mode of the active viewport shows in its strip
    panels[1].window->setRenderMode(render::RenderMode::Wireframe);
    pumpFor(100);
    CHECK(panels[1].header->renderButton()->text().startsWith("Wireframe"));
    CHECK(panels[0].header->renderButton()->text().startsWith("Unlit"));

    // maximise: only that viewport stays, the button is checked; again: all are back
    panels[1].header->maximiseButton()->click();
    CHECK(window.maximisedViewport() == 1);
    CHECK(panels[0].dock->isClosed());
    CHECK_FALSE(panels[1].dock->isClosed());
    CHECK(panels[1].header->maximiseButton()->isChecked());
    CHECK_FALSE(panels[1].dock->dockAreaWidget()->titleBar()->isVisible());
    panels[1].header->maximiseButton()->click();
    CHECK(window.maximisedViewport() == -1);
    CHECK_FALSE(panels[0].dock->isClosed());
    CHECK_FALSE(panels[1].header->maximiseButton()->isChecked());

    // float: the dock leaves the main window
    panels[0].header->floatButton()->click();
    CHECK(panels[0].dock->isFloating());
}
