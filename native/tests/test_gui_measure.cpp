// M11 item 11, U13: the Measure tool with a real window: two clicks in a viewport place two points, the status bar has the reading, a third click starts over, Esc ends it
// (native_gui_tests).
#include <doctest.h>

#include <QStatusBar>
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

TEST_CASE("measure tool: two clicks give a reading, a third starts over, Esc ends" * doctest::test_suite("gpu")) {
    App app;
    if (!app.ok) return;
    base::JobPool pool(4);
    QTemporaryDir dir;
    ui::Services sv = app.services();
    sv.jobs = &pool;
    sv.assetsRoot = dir.path().toStdString();
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
    const QPoint right(vp->width() / 2 + 80, vp->height() / 2);
    const auto click = [&](QPoint p) {
        sendMouse(vp, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, p, Qt::NoModifier);
        sendMouse(vp, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, p, Qt::NoModifier);
        pumpFor(50);
    };

    CHECK_FALSE(window.measureActive());
    CHECK(window.runAction("view.measure"));
    CHECK(window.measureActive());
    click(mid);
    REQUIRE(window.measurePoints().size() == 1);
    CHECK(window.measurePoints()[0].z == doctest::Approx(1));   // the front face of the box
    CHECK(window.selection().empty());                            // the click did not select
    click(right);
    REQUIRE(window.measurePoints().size() == 2);
    const std::string reading = window.measureReading();
    CHECK(reading.find(" m   dX ") != std::string::npos);
    CHECK(window.statusBar()->currentMessage().toStdString() == reading);
    const auto a = window.measurePoints()[0], b = window.measurePoints()[1];
    CHECK((b - a).length() > 0);
    click(mid);   // the third click starts over
    CHECK(window.measurePoints().size() == 1);
    // Esc ends the tool through its key binding
    sendKey(vp, QEvent::KeyPress, Qt::Key_Escape);
    sendKey(vp, QEvent::KeyRelease, Qt::Key_Escape);
    pumpFor(50);
    CHECK_FALSE(window.measureActive());
    // and a click selects again
    click(mid);
    CHECK_FALSE(window.selection().empty());
}
