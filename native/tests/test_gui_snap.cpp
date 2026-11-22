// M11 item 11, U5: the Snapping menu (View > Snapping and the click on the grid field of the status bar) with a real window and viewports (native_gui_tests).
#include <doctest.h>

#include <QMenu>
#include <QMenuBar>

#include "base/jobs.h"
#include "doc/io.h"
#include "gui_support.h"
#include "ui/status_bar.h"

using namespace native;
using namespace guitest;

namespace {

QAction* find(QMenu* m, const QString& text) {
    for (QAction* a : m->actions())
        if (a->text().remove('&') == text) return a;
    return nullptr;
}

} // namespace

TEST_CASE("snapping: snap, grid size, rotation and scale of the viewport through the actions and the status bar menu" * doctest::test_suite("gpu")) {
    App app;
    if (!app.ok) return;
    base::JobPool pool(4);
    ui::Services sv = app.services();
    sv.jobs = &pool;
    ui::MainWindow window(sv);
    window.resize(900, 650);
    window.show();
    REQUIRE(waitUntil([&] { return allReady(window); }));
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    window.setDocument(r.version, true);
    render::ViewportWindow* vp = window.activeViewport();
    REQUIRE(vp);
    const auto settings = [&] { return vp->rig().settings(); };

    CHECK(settings().snapEnabled);
    window.runAction("snap.toggle");
    CHECK_FALSE(settings().snapEnabled);
    CHECK(render::snapStep(settings()) == 0);
    window.runAction("snap.toggle");
    CHECK(settings().snapEnabled);

    window.runAction("snap.grid_size", nlohmann::json{{"value", 0.25}});
    CHECK(settings().gridSize == doctest::Approx(0.25));
    window.runAction("snap.rotation", nlohmann::json{{"value", 45}});
    CHECK(settings().rotSnap == doctest::Approx(45));
    window.runAction("snap.scale", nlohmann::json{{"value", 0.5}});
    CHECK(settings().scaleSnap == doctest::Approx(0.5));
    window.runAction("snap.grid_size", nlohmann::json{{"value", 0}});   // not a size: nothing changes
    CHECK(settings().gridSize == doctest::Approx(0.25));
    pumpFor(50);
    window.statusFields()->refreshNow();
    CHECK(window.statusFields()->gridText().contains(QStringLiteral("0.25")));

    // the menu of the status bar shows the current values as checked and runs the actions
    const std::unique_ptr<QMenu> menu = window.statusFields()->buildSnapMenu();
    REQUIRE(menu);
    QAction* snap = find(menu.get(), QStringLiteral("Snap to grid"));
    REQUIRE(snap);
    CHECK(snap->isChecked());
    QAction* grid = find(menu.get(), QStringLiteral("Grid size"));
    REQUIRE(grid);
    QMenu* gridMenu = grid->menu();
    REQUIRE(gridMenu);
    QAction* quarter = find(gridMenu, QStringLiteral("0.25 m"));
    REQUIRE(quarter);
    CHECK(quarter->isChecked());
    QAction* one = find(gridMenu, QStringLiteral("1 m"));
    REQUIRE(one);
    CHECK_FALSE(one->isChecked());
    one->trigger();
    CHECK(settings().gridSize == doctest::Approx(1));
    snap->trigger();
    CHECK_FALSE(settings().snapEnabled);

    // View > Snapping lists the same things, with the state of the active viewport
    QMenu* view = nullptr;
    for (QAction* a : window.menuBar()->actions())
        if (a->text().remove('&') == QStringLiteral("View")) view = a->menu();
    REQUIRE(view);
    view->aboutToShow();
    QAction* sub = find(view, QStringLiteral("Snapping"));
    REQUIRE(sub);
    REQUIRE(sub->menu());
    sub->menu()->aboutToShow();
    CHECK(find(sub->menu(), QStringLiteral("Snap to grid")) != nullptr);
    CHECK(find(sub->menu(), QStringLiteral("Grid size")) != nullptr);
    CHECK(find(sub->menu(), QStringLiteral("Rotation snap")) != nullptr);
    CHECK(find(sub->menu(), QStringLiteral("Scale snap")) != nullptr);
}
