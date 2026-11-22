// T4.2 (Qt side): the box and shape tools in a viewport: drag a rectangle, move for the height, click to finish; Esc and the right button cancel; shapes; one undo
// step each (a real window and GPU: native_gui_tests).
#include <doctest.h>

#include <QTemporaryDir>

#include "base/jobs.h"
#include "doc/commands.h"
#include "doc/edit.h"
#include "doc/io.h"
#include "doc/selection.h"
#include "gui_support.h"
#include "render/picking.h"
#include "tools/shapes.h"

using namespace native;
using namespace guitest;

namespace {

std::shared_ptr<const doc::DocumentVersion> oneBox() {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    core::Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    w.brushes.push_back(core::Brush::makeBox("b", {-1, -1, -1}, {1, 1, 1}, "dev/grey"));
    REQUIRE(ed.addEntity(w));
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

size_t brushCount(ui::MainWindow& w) {
    size_t n = 0;
    for (const auto& e : w.document()->entities) n += e->brushes.size();
    return n;
}

QPoint screenOf(render::ViewportWindow* vp, const render::Vec3& p) {
    const render::ViewState view = vp->currentView();
    const render::Vec3 n = render::mat4Transform(view.viewProj, p);
    return QPoint(static_cast<int>((n.x * 0.5 + 0.5) * vp->width()), static_cast<int>((n.y * 0.5 + 0.5) * vp->height()));
}

} // namespace

TEST_CASE("the box and shape tools draw in a viewport as one undo step" * doctest::test_suite("gpu")) {
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
        REQUIRE(window.frameScene());
        pumpFor(300);
        render::ViewportWindow* vp = window.viewports()[0].window;
        const auto move = [&](QPoint p, Qt::MouseButtons buttons = Qt::NoButton) { sendMouse(vp, QEvent::MouseMove, Qt::NoButton, buttons, p); };
        const auto press = [&](QPoint p) { sendMouse(vp, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, p); };
        const auto release = [&](QPoint p) { sendMouse(vp, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, p); };
        const auto key = [&](int k) { sendKey(vp, QEvent::KeyPress, k); sendKey(vp, QEvent::KeyRelease, k); };
        const QPoint start = screenOf(vp, {0, 1, 0});   // on the top face of the box

        REQUIRE(window.tool() == ui::MainWindow::Tool::Select);
        window.setTool(ui::MainWindow::Tool::Box);
        CHECK(window.tool() == ui::MainWindow::Tool::Box);

        SUBCASE("drag a rectangle, move for the height, click: a box, one undo step, selected") {
            move(start);
            press(start);
            CHECK(window.drawingBox());
            move(start + QPoint(160, 90), Qt::LeftButton);
            CHECK(window.boxPreviewLines(0).size() == 4 * 2);   // the flat rectangle
            release(start + QPoint(160, 90));
            CHECK(window.drawingBox());
            move(start + QPoint(90, -60));
            CHECK(window.boxPreviewLines(0).size() == 12 * 2);   // the box
            CHECK(window.gizmoLinesFor(0).size() == 12 * 2);
            CHECK(brushCount(window) == 1);                     // nothing yet
            press(start + QPoint(90, -60));
            release(start + QPoint(90, -60));
            CHECK_FALSE(window.drawingBox());
            REQUIRE(brushCount(window) == 2);
            CHECK(window.history().undoCount() == 1);
            CHECK(window.history().undoLabel() == "Draw box");
            REQUIRE(window.selection().items().size() == 1);
            CHECK(window.selection().items()[0].isBrush());
            CHECK(window.selection().items()[0].brushId != "b");
            const auto& world = window.document()->entities[0];
            core::Vec3 lo, hi;
            REQUIRE(world->brushes[1]->brush.bounds(lo, hi));
            CHECK(hi.x - lo.x > 0.1);
            CHECK(hi.y - lo.y > 0.1);
            CHECK(hi.z - lo.z > 0.1);
            CHECK_FALSE(world->brushes[1]->brush.checkConvexSolid());
            window.undo();
            CHECK(brushCount(window) == 1);
            window.redo();
            CHECK(brushCount(window) == 2);
        }

        SUBCASE("Escape and the right button cancel; a click without a drag draws nothing") {
            move(start);
            press(start);
            move(start + QPoint(160, 90), Qt::LeftButton);
            key(Qt::Key_Escape);
            CHECK_FALSE(window.drawingBox());
            release(start + QPoint(160, 90));
            CHECK_FALSE(window.drawingBox());
            CHECK(window.gizmoLinesFor(0).empty());
            CHECK(brushCount(window) == 1);
            press(start);
            release(start);
            CHECK_FALSE(window.drawingBox());
            press(start);
            move(start + QPoint(160, 90), Qt::LeftButton);
            release(start + QPoint(160, 90));
            REQUIRE(window.drawingBox());
            sendMouse(vp, QEvent::MouseButtonPress, Qt::RightButton, Qt::RightButton, start);
            sendMouse(vp, QEvent::MouseButtonRelease, Qt::RightButton, Qt::NoButton, start);
            CHECK_FALSE(window.drawingBox());
            CHECK(brushCount(window) == 1);
            CHECK(window.history().undoCount() == 0);
        }

        SUBCASE("a shape: stairs make several convex brushes in one step") {
            window.setShape(tools::ShapeId::Stairs);
            CHECK(window.tool() == ui::MainWindow::Tool::Box);
            move(start);
            press(start);
            move(start + QPoint(160, 90), Qt::LeftButton);
            release(start + QPoint(160, 90));
            move(start + QPoint(100, -70));
            CHECK(window.boxPreviewLines(0).size() > 12 * 2);   // the stairs have more edges than their box
            press(start + QPoint(100, -70));
            release(start + QPoint(100, -70));
            CHECK(brushCount(window) == 1 + static_cast<size_t>(window.shapeParams().steps));
            CHECK(window.history().undoCount() == 1);
            CHECK(window.history().undoLabel() == "Draw stairs");
            CHECK(window.selection().items().size() == static_cast<size_t>(window.shapeParams().steps));
            window.undo();
            CHECK(brushCount(window) == 1);
        }

        SUBCASE("the B and V keys and the select tool") {
            window.setTool(ui::MainWindow::Tool::Select);
            QCursor::setPos(vp->mapToGlobal(QPoint(vp->width() / 2, vp->height() / 2)));
            move(QPoint(vp->width() / 2, vp->height() / 2));
            pumpFor(50);
            key(Qt::Key_B);
            CHECK(window.tool() == ui::MainWindow::Tool::Box);
            key(Qt::Key_V);
            CHECK(window.tool() == ui::MainWindow::Tool::Select);
            // in the select tool a click selects, it does not draw
            press(start);
            release(start);
            CHECK_FALSE(window.drawingBox());
            CHECK(window.selection().items().size() == 1);
        }

        SUBCASE("an empty document gets its worldspawn on the first box") {
            window.setDocument(doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})").version, true);
            pumpFor(200);
            const QPoint a(vp->width() / 2, vp->height() / 2);
            move(a);
            press(a);
            move(a + QPoint(300, 150), Qt::LeftButton);
            release(a + QPoint(300, 150));
            REQUIRE(window.drawingBox());
            move(a + QPoint(300, -100));
            press(a + QPoint(300, -100));
            release(a + QPoint(300, -100));
            REQUIRE(window.document()->entities.size() == 1);
            CHECK(window.document()->entities[0]->typed.classname == "worldspawn");
            CHECK(brushCount(window) == 1);
        }
    }
    CHECK(app.device->allocationStats().allocationCount <= app.baseAllocations + 1);
}

TEST_CASE("the decal tool lays a projector on the surface under the click, one undo step" * doctest::test_suite("gpu")) {
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
        REQUIRE(window.frameScene());
        pumpFor(300);
        render::ViewportWindow* vp = window.viewports()[0].window;
        const QPoint top = screenOf(vp, {0, 1, 0});   // on the top face of the box
        window.setActiveTexture("dev/grate");
        window.setDecalParams({2, 1.5, 0.4});
        window.setTool(ui::MainWindow::Tool::Decal);
        CHECK(window.modeContext() == "tool:decal");
        sendMouse(vp, QEvent::MouseMove, Qt::NoButton, Qt::NoButton, top);
        sendMouse(vp, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, top);
        sendMouse(vp, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, top);
        pumpFor(100);
        REQUIRE(window.document()->entities.size() == 2);
        const core::Entity& e = window.document()->entities[1]->typed;
        CHECK(e.classname == "decal");
        CHECK(e.keyvalues.at("texture") == "dev/grate");
        CHECK(e.keyvalues.at("width") == "2");
        CHECK(e.keyvalues.at("height") == "1.5");
        CHECK(e.keyvalues.at("depth") == "0.4");
        CHECK(e.origin.y == doctest::Approx(1).epsilon(1e-6));   // on the top face
        CHECK(e.angles.x == doctest::Approx(0).epsilon(1e-6).scale(1));   // looking straight down: the zero angles
        CHECK(window.history().undoLabel() == "Add decal");
        CHECK(window.tool() == ui::MainWindow::Tool::Decal);   // the tool stays for the next one

        // a click on nothing places nothing
        sendMouse(vp, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, QPoint(3, 3));
        sendMouse(vp, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, QPoint(3, 3));
        pumpFor(100);
        CHECK(window.document()->entities.size() == 2);
        window.history().undo();
        CHECK(window.document()->entities.size() == 1);
    }
    CHECK(app.device->allocationStats().allocationCount <= app.baseAllocations + 1);
}

TEST_CASE("the angle handle of a selected point entity turns it as one undo step" * doctest::test_suite("gpu")) {
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
        {
            const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
            doc::VersionEditor ed(r.version);
            core::Entity w;
            w.id = "w";
            w.classname = "worldspawn";
            w.brushes.push_back(core::Brush::makeBox("b", {-1, -1, -1}, {1, 1, 1}, "dev/grey"));
            REQUIRE(ed.addEntity(w));
            core::Entity p;
            p.id = "p";
            p.classname = "nav_point";
            p.origin = {1.5, 0, 0};
            REQUIRE(ed.addEntity(p));
            window.setDocument(ed.finish(), true);
        }
        REQUIRE(waitUntil([&] { return window.sceneReady() && window.picker(); }, 20000));
        REQUIRE(window.frameScene());
        pumpFor(300);
        render::ViewportWindow* vp = window.viewports()[0].window;
        window.selection().select(doc::SelRef::entity("p"));
        pumpFor(100);
        const auto handle = window.angleHandleTip();
        REQUIRE(handle);
        const QPoint tip = screenOf(vp, *handle);
        const render::Vec3 target{handle->x - 0.7, handle->y, 0};   // to the -X side (towards the box, on screen) on the entity's horizontal plane: about yaw -90
        const QPoint to = screenOf(vp, target);
        REQUIRE(to.x() > 0);
        REQUIRE(to.x() < vp->width());
        REQUIRE(to.y() > 0);
        REQUIRE(to.y() < vp->height());
        const size_t undoBefore = window.history().undoCount();
        sendMouse(vp, QEvent::MouseMove, Qt::NoButton, Qt::NoButton, tip);
        sendMouse(vp, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, tip);
        CHECK(window.transforming());
        sendMouse(vp, QEvent::MouseMove, Qt::NoButton, Qt::LeftButton, (tip + to) / 2);
        sendMouse(vp, QEvent::MouseMove, Qt::NoButton, Qt::LeftButton, to);
        sendMouse(vp, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, to);
        pumpFor(100);
        CHECK_FALSE(window.transforming());
        const core::Entity& e = window.document()->entities[1]->typed;
        CHECK(e.angles.y <= -75);   // snapped to the rotation step, near the -X direction
        CHECK(e.angles.y >= -105);
        CHECK(e.angles.x == doctest::Approx(0).scale(1).epsilon(1e-6));   // a drag on the horizontal plane keeps the pitch
        CHECK(window.history().undoCount() == undoBefore + 1);
        CHECK(window.history().undoLabel() == "Turn entity");
        CHECK(window.selection().primary()->entityId == "p");   // still selected
        window.history().undo();
        CHECK(window.document()->entities[1]->typed.angles.y == doctest::Approx(0).scale(1));
    }
    CHECK(app.device->allocationStats().allocationCount <= app.baseAllocations + 1);
}
