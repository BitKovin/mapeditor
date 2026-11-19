// T4.1 (Qt side): the modal operators G / R / S and the gizmo in a viewport go through the action system and the history: every operation is one undo step,
// follows the pointer, can be typed, constrained and cancelled (a real window and GPU: native_gui_tests).
#include <doctest.h>

#include <QCursor>
#include <QTemporaryDir>

#include <chrono>
#include <filesystem>

#include "base/jobs.h"
#include "doc/commands.h"
#include "doc/edit.h"
#include "doc/io.h"
#include "doc/selection.h"
#include "gui_support.h"
#include "render/picking.h"
#include "tools/gizmo_overlay.h"

using namespace native;
using namespace guitest;

namespace {

std::shared_ptr<const doc::DocumentVersion> twoBoxes() {
    const doc::LoadResult r = doc::loadDocumentText(R"({"schemaVersion": "0.2", "entities": []})");
    REQUIRE(r.version);
    doc::VersionEditor ed(r.version);
    core::Entity w;
    w.id = "w";
    w.classname = "worldspawn";
    w.brushes.push_back(core::Brush::makeBox("b", {-1, -1, -1}, {1, 1, 1}, "dev/grey"));
    REQUIRE(ed.addEntity(w));
    core::Entity door;
    door.id = "door";
    door.classname = "func_door";
    door.origin = {4, 0, 0};
    door.brushes.push_back(core::Brush::makeBox("d", {3, -1, -1}, {5, 1, 1}, "dev/grey"));
    REQUIRE(ed.addEntity(door));
    const auto v = ed.finish();
    REQUIRE(v);
    return v;
}

core::Vec3 minOf(ui::MainWindow& w, const char* entity, size_t brush = 0) {
    const auto doc = w.document();
    const int ei = doc->findEntity(entity);
    REQUIRE(ei >= 0);
    core::Vec3 lo{}, hi{};
    REQUIRE(doc->entities[static_cast<size_t>(ei)]->brushes[brush]->brush.bounds(lo, hi));
    return lo;
}
core::Vec3 maxOf(ui::MainWindow& w, const char* entity, size_t brush = 0) {
    const auto doc = w.document();
    const int ei = doc->findEntity(entity);
    REQUIRE(ei >= 0);
    core::Vec3 lo{}, hi{};
    REQUIRE(doc->entities[static_cast<size_t>(ei)]->brushes[brush]->brush.bounds(lo, hi));
    return hi;
}

QPoint screenOf(render::ViewportWindow* vp, const render::Vec3& p) {
    const render::ViewState view = vp->currentView();
    const render::Vec3 n = render::mat4Transform(view.viewProj, p);
    return QPoint(static_cast<int>((n.x * 0.5 + 0.5) * vp->width()), static_cast<int>((n.y * 0.5 + 0.5) * vp->height()));
}

} // namespace

TEST_CASE("modal G / R / S and the gizmo transform the selection as one undo step" * doctest::test_suite("gpu")) {
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
        window.setDocument(twoBoxes(), true);
        REQUIRE(waitUntil([&] { return window.sceneReady() && window.picker() && window.picker()->objectCount() == 2; }, 20000));
        pumpFor(200);
        render::ViewportWindow* vp = window.viewports()[0].window;
        vp->rig().fly().position = {1.5, 0, 9};
        vp->rig().fly().yaw = 3.14159265358979;   // towards -Z
        vp->rig().fly().pitch = 0;
        vp->viewChanged();
        pumpFor(100);
        const QPoint mid(vp->width() / 2, vp->height() / 2);
        const doc::SelRef brush = doc::SelRef::brush("w", "b");
        const auto move = [&](QPoint p, Qt::MouseButtons buttons = Qt::NoButton) { sendMouse(vp, QEvent::MouseMove, Qt::NoButton, buttons, p); };
        const auto key = [&](int k) { sendKey(vp, QEvent::KeyPress, k); sendKey(vp, QEvent::KeyRelease, k); };
        window.selection().select(brush);

        SUBCASE("G follows the pointer and ends with a click: one undo step") {
            move(mid);
            REQUIRE(window.startTransform(vp, tools::ModalKind::Grab));
            CHECK(window.transforming());
            move(mid + QPoint(120, 0));
            const double x1 = minOf(window, "w").x;
            CHECK(x1 > -1.0 + 0.2);   // it moved to the right ...
            move(mid + QPoint(240, 0));
            CHECK(minOf(window, "w").x > x1 + 0.2);   // ... and goes on following
            CHECK(window.history().undoCount() == 0);   // still one open interaction
            sendMouse(vp, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, mid + QPoint(240, 0));
            sendMouse(vp, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, mid + QPoint(240, 0));
            CHECK_FALSE(window.transforming());
            CHECK(window.history().undoCount() == 1);
            CHECK(window.history().undoLabel() == "Move");
            CHECK(window.selection().items().size() == 1);   // the click confirmed, it did not select
            window.undo();
            CHECK(minOf(window, "w").x == doctest::Approx(-1.0));
            window.redo();
            CHECK(minOf(window, "w").x > -0.5);
        }

        SUBCASE("Escape and the right button cancel: nothing changes, nothing to undo") {
            move(mid);
            REQUIRE(window.startTransform(vp, tools::ModalKind::Grab));
            move(mid + QPoint(100, 30));
            CHECK(minOf(window, "w").x != doctest::Approx(-1.0));
            key(Qt::Key_Escape);
            CHECK_FALSE(window.transforming());
            CHECK(minOf(window, "w").x == doctest::Approx(-1.0));
            CHECK(window.history().undoCount() == 0);
            REQUIRE(window.startTransform(vp, tools::ModalKind::Scale));
            move(mid + QPoint(100, 0));
            sendMouse(vp, QEvent::MouseButtonPress, Qt::RightButton, Qt::RightButton, mid);
            sendMouse(vp, QEvent::MouseButtonRelease, Qt::RightButton, Qt::NoButton, mid);
            CHECK_FALSE(window.transforming());
            CHECK(window.history().undoCount() == 0);
            CHECK(maxOf(window, "w").x == doctest::Approx(1.0));
        }

        SUBCASE("X then typed digits: an exact distance along the axis") {
            move(mid);
            REQUIRE(window.startTransform(vp, tools::ModalKind::Grab));
            key(Qt::Key_X);
            key(Qt::Key_2);
            CHECK(minOf(window, "w").x == doctest::Approx(1.0));   // -1 + 2
            CHECK(minOf(window, "w").y == doctest::Approx(-1.0));
            key(Qt::Key_Backspace);
            key(Qt::Key_Minus);
            key(Qt::Key_3);
            CHECK(minOf(window, "w").x == doctest::Approx(-4.0));
            key(Qt::Key_Return);
            CHECK_FALSE(window.transforming());
            CHECK(window.history().undoCount() == 1);
            window.undo();
            CHECK(minOf(window, "w").x == doctest::Approx(-1.0));
        }

        SUBCASE("typed scale and rotation") {
            move(mid);
            REQUIRE(window.startTransform(vp, tools::ModalKind::Scale));
            key(Qt::Key_2);
            CHECK(maxOf(window, "w").x == doctest::Approx(2.0));   // about the centre of the box
            CHECK(minOf(window, "w").x == doctest::Approx(-2.0));
            key(Qt::Key_Return);
            REQUIRE(window.startTransform(vp, tools::ModalKind::Rotate));
            key(Qt::Key_Y);
            key(Qt::Key_4);
            key(Qt::Key_5);
            // a 4 m cube turned 45 degrees about Y reaches 2 * sqrt 2 in x
            CHECK(maxOf(window, "w").x == doctest::Approx(2.0 * std::sqrt(2.0)).epsilon(1e-6));
            key(Qt::Key_Return);
            CHECK(window.history().undoCount() == 2);
            window.undo();
            window.undo();
            CHECK(maxOf(window, "w").x == doctest::Approx(1.0));
        }

        SUBCASE("the G key starts the operator in the viewport under the cursor") {
            QCursor::setPos(vp->mapToGlobal(mid));
            move(mid);
            pumpFor(50);
            key(Qt::Key_G);
            CHECK(window.transforming());
            key(Qt::Key_Escape);
            CHECK_FALSE(window.transforming());
            window.selection().clear();
            key(Qt::Key_G);
            CHECK_FALSE(window.transforming());   // nothing selected
        }

        SUBCASE("the gizmo: a drag on an arrow moves the selection along that axis") {
            pumpFor(100);
            const auto lines = window.gizmoLinesFor(0);
            CHECK(lines.size() == 3 * 5 * 2);
            {   // the arrows are drawn on top of the scene: red x, green y, blue z
                const auto img = vp->capture();
                REQUIRE(img.ok());
                int red = 0, green = 0;
                for (uint32_t y = 0; y < img.height; ++y)
                    for (uint32_t x = 0; x < img.width; ++x) {
                        const Rgb p = pixel(img, x, y);
                        if (p.r > 220 && p.g < 110 && p.b < 110) ++red;
                        if (p.g > 220 && p.r < 130 && p.b < 130) ++green;
                    }
                CHECK(red > 15);
                CHECK(green > 15);
            }
            const render::Vec3 pivot{0, 0, 0};
            const double len = tools::gizmoLength(vp->rig().active().distanceTo(pivot));
            const QPoint onArrow = screenOf(vp, {len * 0.6, 0, 0});
            move(onArrow);
            sendMouse(vp, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, onArrow);
            CHECK(window.transforming());
            const QPoint to = screenOf(vp, {len * 0.6 + 2.0, 0, 0});
            move(to, Qt::LeftButton);
            CHECK(minOf(window, "w").x == doctest::Approx(-1.0 + 2.0).epsilon(0.3));   // follows the mouse (grid snapping rounds it)
            CHECK(minOf(window, "w").y == doctest::Approx(-1.0));
            sendMouse(vp, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, to);
            CHECK_FALSE(window.transforming());
            CHECK(window.history().undoCount() == 1);
            CHECK(window.selection().items().size() == 1);
            window.undo();
            CHECK(minOf(window, "w").x == doctest::Approx(-1.0));
        }

        SUBCASE("the gizmo in rotate and scale mode") {
            render::ViewportSettings s = vp->rig().settings();
            s.gizmo = render::GizmoMode::Scale;
            vp->rig().applySettings(s);
            vp->viewChanged();
            pumpFor(50);
            CHECK(window.gizmoLinesFor(0).size() == 3 * 13 * 2);
            const double len = tools::gizmoLength(vp->rig().active().distanceTo({0, 0, 0}));
            const QPoint at = screenOf(vp, {len * 0.9, 0, 0});
            move(at);
            sendMouse(vp, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, at);
            REQUIRE(window.transforming());
            move(screenOf(vp, {len * 1.8, 0, 0}), Qt::LeftButton);
            CHECK(maxOf(window, "w").x > 1.5);   // twice as wide, only along x
            CHECK(maxOf(window, "w").y == doctest::Approx(1.0));
            sendMouse(vp, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, mid);
            CHECK(window.history().undoCount() == 1);
            window.undo();
            s.gizmo = render::GizmoMode::Rotate;
            vp->rig().applySettings(s);
            vp->viewChanged();
            pumpFor(50);
            CHECK(window.gizmoLinesFor(0).size() == 3 * 48 * 2);
        }

        SUBCASE("the origin of a brush entity: the square selects it and the gizmo moves it alone") {
            window.selection().select(doc::SelRef::entity("door"));
            pumpFor(100);
            const render::Vec3 origin{4, 0, 0};
            const QPoint sq = screenOf(vp, origin);
            CHECK(window.originSelected().empty());
            move(sq);
            sendMouse(vp, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, sq);
            sendMouse(vp, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, sq);
            CHECK(window.originSelected() == "door");
            CHECK(window.selection().items().size() == 1);
            const double len = tools::gizmoLength(vp->rig().active().distanceTo(origin));
            const QPoint arrow = screenOf(vp, {4 + len * 0.6, 0, 0});
            move(arrow);
            sendMouse(vp, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, arrow);
            REQUIRE(window.transforming());
            move(screenOf(vp, {4 + len * 0.6 + 1.5, 0, 0}), Qt::LeftButton);
            sendMouse(vp, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, mid);
            const auto doc = window.document();
            const int ei = doc->findEntity("door");
            CHECK(doc->entities[static_cast<size_t>(ei)]->typed.origin.x > 5.0);   // the origin moved ...
            CHECK(minOf(window, "door").x == doctest::Approx(3.0));                // ... the brush stayed
            CHECK(window.history().undoLabel() == "Move origin");
            QCursor::setPos(vp->mapToGlobal(mid));   // keys go to the viewport under the cursor
            pumpFor(50);
            key(Qt::Key_Escape);
            CHECK(window.originSelected().empty());
            CHECK(window.selection().items().size() == 1);   // Esc left the origin editing and kept the entity selected
        }
    }
    CHECK(app.device->allocationStats().allocationCount <= app.baseAllocations + 1);
}

// Acceptance of T4.1: dragging a brush of the big level follows the pointer within a frame (Unlit, the default).
TEST_CASE("dragging a brush on lvl2 keeps up with the mouse" * doctest::test_suite("gpu")) {
    const std::filesystem::path root = "F:/Git/SdlWasmEngine/source/Games/ShooterGame/GameData";
    if (!std::filesystem::exists(root / "maps/lvl2.map.json")) return;   // the level is not on this machine
    App app;
    if (!app.ok) return;
    base::JobPool pool(8);
    ui::Services sv = app.services();
    sv.jobs = &pool;
    sv.assetsRoot = root.generic_string();
    {
        ui::MainWindow window(sv);
        window.resize(1200, 800);
        window.show();
        REQUIRE(waitUntil([&] { return allReady(window); }));
        window.openDocument("maps/lvl2.map.json");
        REQUIRE(waitUntil([&] { return window.document() && window.sceneReady(); }, 120000));
        pumpFor(300);
        REQUIRE(window.frameScene());
        pumpFor(300);
        render::ViewportWindow* vp = window.viewports()[0].window;
        // a brush of the world in front of the camera: the picker's hit at the centre
        const QPoint mid(vp->width() / 2, vp->height() / 2);
        window.pickAt(vp, mid, ui::MainWindow::PickOp::Replace);
        REQUIRE(window.selection().items().size() == 1);
        sendMouse(vp, QEvent::MouseMove, Qt::NoButton, Qt::NoButton, mid);
        REQUIRE(window.startTransform(vp, tools::ModalKind::Grab));
        const uint64_t frames0 = app.rt->framesRendered(vp->viewportId());
        using Clock = std::chrono::steady_clock;
        const auto t0 = Clock::now();
        double worst = 0, total = 0;
        const int moves = 90;
        for (int i = 1; i <= moves; ++i) {
            const auto a = Clock::now();
            sendMouse(vp, QEvent::MouseMove, Qt::NoButton, Qt::NoButton, mid + QPoint(i * 3, (i % 20) * 2));
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - a).count();
            worst = std::max(worst, ms);
            total += ms;
            pumpFor(8);
        }
        const double seconds = std::chrono::duration<double>(Clock::now() - t0).count();
        const uint64_t frames = app.rt->framesRendered(vp->viewportId()) - frames0;
        MESSAGE("lvl2 grab: " << moves << " moves, handling avg " << total / moves << " ms, worst " << worst << " ms; " << frames << " frames in " << seconds << " s");
        CHECK(total / moves <= 16.7);   // the pointer's handling (math, edit, scene rebuild request) leaves a frame's time
        CHECK(frames >= static_cast<uint64_t>(moves) / 2);   // and the render thread draws what follows
        window.confirmTransform();
        CHECK(window.history().undoCount() == 1);
        CHECK(waitUntil([&] { return window.sceneReady(); }, 20000));
    }
}
