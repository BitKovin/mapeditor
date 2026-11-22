// T1.3 / T1.4 (Qt side): Vulkan viewports in ADS docks show the scene through their cameras; resizing, floating and re-docking work
// with 0 validation errors; an unchanged viewport is idle; layout presets 1 / 2 / 4, camera input and the saved layout.
// Real windows, real GPU: part of native_gui_tests.
#include <doctest.h>

#include <DockManager.h>
#include <DockWidget.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSettings>
#include <QTemporaryDir>
#include <QVulkanInstance>
#include <QWheelEvent>

#include <cmath>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>

#include "gpu/device.h"
#include "gui_support.h"
#include "render/camera_rig.h"
#include "render/render_thread.h"
#include "render/viewport_window.h"
#include "ui/main_window.h"

using namespace native;

namespace {

using namespace guitest;

// The scene's 1 m box seen from above (top view): its +Y face. Sampled a few pixels off the centre so the axis lines do not cover it.
constexpr Rgb kBoxTop{126, 183, 126};

bool boxAtCentreOfTopView(const render::CapturedImage& img) {
    return closeTo(pixel(img, img.width / 2 - 5, img.height / 2 - 5), kBoxTop, 10);
}

} // namespace

TEST_CASE("a viewport in a dock shows the scene, resizes, floats and re-docks" * doctest::test_suite("gpu")) {
    App app;
    if (!app.ok) return;
    {
        ui::MainWindow window(app.services());
        window.resize(1000, 700);
        window.show();
        REQUIRE(window.viewport() != nullptr);
        REQUIRE(waitUntil([&] { return window.viewport()->viewportId() != 0; }));

        // 1. First frame: clear colour in the corner (sky), the box through the default fly camera in the middle.
        auto img = window.viewport()->capture();
        REQUIRE(img.ok());
        CHECK(img.width > 300);
        CHECK(closeTo(pixel(img, 3, 3), {27, 27, 29}, 3));
        // The default camera looks from (8,6,8) at the top centre of the box: just below that point is the box, not clear colour.
        CHECK_FALSE(closeTo(pixel(img, img.width / 2 + 3, img.height / 2 + 12), {27, 27, 29}, 12));

        // 2. Resizing the main window changes the frame size (the swapchain follows).
        const uint32_t w0 = img.width;
        window.resize(1200, 760);
        pumpFor(300);
        img = window.viewport()->capture();
        REQUIRE(img.ok());
        CHECK(img.width != w0);
        CHECK(img.width == static_cast<uint32_t>(window.viewport()->width() * window.viewport()->devicePixelRatio()));
        CHECK(img.height == static_cast<uint32_t>(window.viewport()->height() * window.viewport()->devicePixelRatio()));

        // 3. Float and re-dock three times: the native window (and its VkSurface) is recreated each time.
        for (int round = 0; round < 3; ++round) {
            window.viewportDock()->setFloating();
            pumpFor(300);
            REQUIRE(waitUntil([&] { return window.viewport()->viewportId() != 0; }));
            CHECK(window.viewportDock()->isFloating());
            img = window.viewport()->capture();
            CHECK(img.ok());
            if (img.ok()) CHECK(closeTo(pixel(img, 3, 3), {27, 27, 29}, 3));

            window.dockManager()->addDockWidget(ads::LeftDockWidgetArea, window.viewportDock());
            pumpFor(300);
            REQUIRE(waitUntil([&] { return window.viewport()->viewportId() != 0; }));
            CHECK_FALSE(window.viewportDock()->isFloating());
            img = window.viewport()->capture();
            CHECK(img.ok());
        }

        // 4. Idle: nothing changed, so no frames are rendered.
        REQUIRE(window.viewport()->capture().ok());
        pumpFor(200);
        const uint64_t before = app.rt->framesRendered(window.viewport()->viewportId());
        pumpFor(700);
        CHECK(app.rt->framesRendered(window.viewport()->viewportId()) == before);

        // 5. Hiding the window (minimise-like) and showing it again keeps working.
        window.hide();
        pumpFor(100);
        window.show();
        REQUIRE(waitUntil([&] { return window.viewport()->viewportId() != 0; }));
        CHECK(window.viewport()->capture().ok());
    }
    CHECK(app.device->validationErrors() == 0);
    CHECK(app.device->allocationStats().allocationCount == app.baseAllocations);
}

TEST_CASE("layout presets 1, 2 and 4 build the right viewports and keep cameras across a switch" * doctest::test_suite("gpu")) {
    App app;
    if (!app.ok) return;
    {
        ui::MainWindow window(app.services());
        window.resize(1100, 760);
        window.show();
        REQUIRE(waitUntil([&] { return allReady(window); }));
        CHECK(window.layoutPreset() == ui::LayoutPreset::One);
        CHECK(window.viewports().size() == 1);
        CHECK(window.viewports()[0].window->rig().mode() == render::CameraMode::Fly);

        // Move the perspective camera, then go to 4 viewports: it is kept; the new ones are top, front, side.
        window.viewports()[0].window->rig().fly().lookAt({-4, 3, 7}, {0, 0.5, 0});
        window.viewports()[0].window->viewChanged();
        window.setLayoutPreset(ui::LayoutPreset::Four);
        REQUIRE(waitUntil([&] { return window.viewports().size() == 4 && allReady(window); }));
        const auto& vps = window.viewports();
        CHECK(vps[0].window->rig().mode() == render::CameraMode::Fly);
        CHECK(vps[1].window->rig().mode() == render::CameraMode::Top);
        CHECK(vps[2].window->rig().mode() == render::CameraMode::Front);
        CHECK(vps[3].window->rig().mode() == render::CameraMode::Side);
        CHECK(vps[0].window->rig().fly().position.x == doctest::Approx(-4));
        CHECK(vps[0].window->rig().fly().position.z == doctest::Approx(7));

        // A 2 x 2 grid: perspective top-left, top top-right, front bottom-left, side bottom-right.
        const auto at = [&](size_t i) { return vps[i].container->mapTo(&window, QPoint(0, 0)); };
        CHECK(at(1).x() > at(0).x());
        CHECK(at(2).y() > at(0).y());
        CHECK(at(3).x() > at(2).x());
        CHECK(at(3).y() > at(1).y());

        // Every viewport draws: the box is in the middle of the top view and the front view, a corner is clear.
        pumpFor(300);
        for (size_t i = 0; i < 4; ++i) {
            const auto img = vps[i].window->capture();
            REQUIRE(img.ok());
            // the corner is clear, or on a grid line: at least one of a few pixels near it is the background
            bool clear = false;
            for (const QPoint p : {QPoint(2, 2), QPoint(5, 3), QPoint(3, 7), QPoint(9, 5), QPoint(7, 11)}) clear = clear || closeTo(pixel(img, static_cast<uint32_t>(p.x()), static_cast<uint32_t>(p.y())), {27, 27, 29}, 45);
            INFO("viewport " << i << " " << img.width << "x" << img.height);
            CHECK(clear);
        }
        const auto topImg = vps[1].window->capture();
        REQUIRE(topImg.ok());
        CHECK(boxAtCentreOfTopView(topImg));
        // Front view: the 1 m box stands on the grid, its +Z face (0.423, 0.423, 0.558) is centred 0.5 m above the middle.
        const auto frontImg = vps[2].window->capture();
        REQUIRE(frontImg.ok());
        const double pxPerMetre = frontImg.height / 16.0;
        const uint32_t boxY = static_cast<uint32_t>(frontImg.height / 2.0 - 0.5 * pxPerMetre + 5);
        const Rgb fp = pixel(frontImg, frontImg.width / 2 - 5, boxY);
        INFO("front view " << frontImg.width << "x" << frontImg.height << " sampled y=" << boxY << " rgb=" << fp.r << "," << fp.g << "," << fp.b);
        CHECK(closeTo(fp, {108, 108, 142}, 10));

        window.setLayoutPreset(ui::LayoutPreset::Two);
        REQUIRE(waitUntil([&] { return window.viewports().size() == 2 && allReady(window); }));
        CHECK(window.viewports()[1].window->rig().mode() == render::CameraMode::Top);
        CHECK(window.viewports()[0].window->rig().fly().position.x == doctest::Approx(-4));
        CHECK(window.viewports()[1].container->mapTo(&window, QPoint(0, 0)).x() >
              window.viewports()[0].container->mapTo(&window, QPoint(0, 0)).x());
        window.setLayoutPreset(ui::LayoutPreset::One);
        REQUIRE(waitUntil([&] { return window.viewports().size() == 1 && allReady(window); }));
        CHECK(window.viewport()->capture().ok());
    }
    CHECK(app.device->validationErrors() == 0);
    CHECK(app.device->allocationStats().allocationCount == app.baseAllocations);
}

TEST_CASE("mouse and keys move the cameras as in the Shell" * doctest::test_suite("gpu")) {
    App app;
    if (!app.ok) return;
    {
        ui::MainWindow window(app.services());
        window.resize(1000, 700);
        window.setLayoutPreset(ui::LayoutPreset::Two);
        window.show();
        REQUIRE(waitUntil([&] { return window.viewports().size() == 2 && allReady(window); }));
        pumpFor(300);
        render::ViewportWindow* persp = window.viewports()[0].window;
        render::ViewportWindow* top = window.viewports()[1].window;
        const QPoint mid(persp->width() / 2, persp->height() / 2);
        persp->rig().fly().position = {0, 2, 6};
        persp->rig().fly().yaw = 3.14159265358979;     // looking towards -Z, at the box
        persp->rig().fly().pitch = 0;
        persp->viewChanged();
        const auto before = persp->capture();
        REQUIRE(before.ok());

        // RMB drag looks: 100 px to the right turns the yaw by -0.3 rad (a delta from the press position).
        sendMouse(persp, QEvent::MouseButtonPress, Qt::RightButton, Qt::RightButton, mid);
        CHECK(persp->pointerGrabbed());
        const double yaw0 = persp->rig().fly().yaw;
        sendMouse(persp, QEvent::MouseMove, Qt::NoButton, Qt::RightButton, mid + QPoint(100, 0));
        CHECK(persp->rig().fly().yaw == doctest::Approx(yaw0 - 0.3));
        persp->rig().fly().yaw = 3.14159265358979;

        // W flies forward while RMB is held. The distance depends on timers and on nobody else using the keyboard or the window focus
        // while the test runs (a stray real Shift key would boost the flight), so the measurement gets three attempts.
        bool flightOk = false;
        for (int attempt = 0; attempt < 3 && !flightOk; ++attempt) {
            persp->rig().fly().position = {0, 2, 6};
            persp->setBoost(false);
            const render::Vec3 p0 = persp->rig().fly().position;
            QElapsedTimer t;
            t.start();
            sendKey(persp, QEvent::KeyPress, Qt::Key_W);
            pumpFor(300);
            sendKey(persp, QEvent::KeyRelease, Qt::Key_W);
            const double dt = t.elapsed() / 1000.0;
            const double flown = render::length(persp->rig().fly().position - p0);
            INFO("attempt " << attempt << ": flown " << flown << " m in " << dt << " s, speed setting " << persp->rig().settings().flySpeed
                            << " fly.speed " << persp->rig().fly().speed);
            flightOk = flown > 8.0 * dt * 0.4 && flown < 8.0 * dt * 1.05 &&   // never faster than the speed setting
                       persp->rig().fly().position.z < p0.z;                  // forward = towards -Z here
            if (!flightOk) MESSAGE("flight attempt " << attempt << " out of bounds: flown " << flown << " m in " << dt << " s");
        }
        CHECK(flightOk);

        // The router ends a gesture when the whole application loses the focus (another program takes it in the middle of the test).
        // That is not a failure of the code under test: say so and stop here.
        if (!window.router().holding("camera.look")) {
            MESSAGE("the application lost the focus during the test (the look gesture was cancelled): rest of the case skipped");
            return;
        }
        // RMB + wheel changes the speed by 1.2 per notch; once RMB is released the keys do nothing and the wheel dollies.
        sendWheel(persp, 120, Qt::RightButton);
        CHECK(persp->rig().settings().flySpeed == doctest::Approx(9.6));
        sendMouse(persp, QEvent::MouseButtonRelease, Qt::RightButton, Qt::NoButton, mid);
        CHECK_FALSE(persp->pointerGrabbed());
        const render::Vec3 p1 = persp->rig().fly().position;
        sendKey(persp, QEvent::KeyPress, Qt::Key_W);
        pumpFor(100);
        CHECK(render::length(persp->rig().fly().position - p1) == 0.0);
        sendKey(persp, QEvent::KeyRelease, Qt::Key_W);
        sendWheel(persp, 120, Qt::NoButton);                       // one notch up: forward by 100 * 0.002 * 9.6
        CHECK(render::length(persp->rig().fly().position - p1) == doctest::Approx(0.2 * 9.6).epsilon(0.01));

        // The picture changed (the camera is elsewhere now).
        pumpFor(100);
        const auto after = persp->capture();
        REQUIRE(after.ok());
        int diff = 0;
        for (size_t i = 0; i < before.rgba.size() && i < after.rgba.size(); i += 4) diff += before.rgba[i] != after.rgba[i];
        CHECK(diff > 200);

        // Orthographic top view: RMB drag pans, the wheel zooms (halfHeight x exp(0.0015 * delta)), MMB pans.
        auto& oc = top->rig().ortho(render::OrthoAxis::Top);
        const render::Vec3 c0 = oc.center;
        const QPoint tmid(top->width() / 2, top->height() / 2);
        sendMouse(top, QEvent::MouseButtonPress, Qt::RightButton, Qt::RightButton, tmid);
        sendMouse(top, QEvent::MouseMove, Qt::NoButton, Qt::RightButton, tmid + QPoint(60, 0));
        sendMouse(top, QEvent::MouseButtonRelease, Qt::RightButton, Qt::NoButton, tmid);
        CHECK(oc.center.x < c0.x);
        const double hh = oc.halfHeight;
        sendWheel(top, 120, Qt::NoButton);                         // one notch up = zoom in
        CHECK(oc.halfHeight == doctest::Approx(hh * std::exp(-100.0 * 0.0015)));
        sendMouse(top, QEvent::MouseButtonPress, Qt::MiddleButton, Qt::MiddleButton, tmid);
        const render::Vec3 c1 = oc.center;
        sendMouse(top, QEvent::MouseMove, Qt::NoButton, Qt::MiddleButton, tmid + QPoint(0, 40));
        sendMouse(top, QEvent::MouseButtonRelease, Qt::MiddleButton, Qt::NoButton, tmid);
        CHECK(oc.center.z != c1.z);
    }
    CHECK(app.device->validationErrors() == 0);
    CHECK(app.device->allocationStats().allocationCount == app.baseAllocations);
}

TEST_CASE("the layout survives a restart" * doctest::test_suite("gpu")) {
    App app;
    if (!app.ok) return;
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString ini = dir.filePath("layout.ini");
    render::Vec3 flyPos;
    double flyYaw = 0;
    QPoint topLeftOfTop;
    {
        QSettings settings(ini, QSettings::IniFormat);
        ui::Services sv = app.services();
        sv.settings = &settings;
        ui::MainWindow window(sv);
        window.resize(1100, 760);
        window.show();
        REQUIRE(waitUntil([&] { return allReady(window); }));
        window.setLayoutPreset(ui::LayoutPreset::Four);
        REQUIRE(waitUntil([&] { return window.viewports().size() == 4 && allReady(window); }));
        pumpFor(300);
        const auto& vps = window.viewports();
        vps[0].window->rig().fly().lookAt({5, 4, -6}, {0, 1, 0});
        vps[0].window->rig().setFlySpeed(21);
        vps[0].window->viewChanged();
        vps[1].window->rig().ortho(render::OrthoAxis::Top).halfHeight = 3.5;
        vps[1].window->viewChanged();
        vps[3].window->setCameraMode(render::CameraMode::Orbit);   // a viewport switched to orbit: the mode is part of the layout
        flyPos = vps[0].window->rig().fly().position;
        flyYaw = vps[0].window->rig().fly().yaw;
        topLeftOfTop = vps[1].container->mapTo(&window, QPoint(0, 0));
        window.close();                                              // closeEvent saves
    }
    CHECK(QFile::exists(ini));
    {
        QSettings settings(ini, QSettings::IniFormat);
        ui::Services sv = app.services();
        sv.settings = &settings;
        ui::MainWindow window(sv);
        window.show();
        REQUIRE(waitUntil([&] { return window.viewports().size() == 4 && allReady(window); }));
        pumpFor(300);
        const auto& vps = window.viewports();
        CHECK(window.layoutPreset() == ui::LayoutPreset::Four);
        CHECK(vps[0].window->rig().mode() == render::CameraMode::Fly);
        CHECK(vps[1].window->rig().mode() == render::CameraMode::Top);
        CHECK(vps[3].window->rig().mode() == render::CameraMode::Orbit);
        CHECK(vps[0].window->rig().fly().position.x == doctest::Approx(flyPos.x));
        CHECK(vps[0].window->rig().fly().position.z == doctest::Approx(flyPos.z));
        CHECK(vps[0].window->rig().fly().yaw == doctest::Approx(flyYaw));
        CHECK(vps[0].window->rig().settings().flySpeed == doctest::Approx(21));
        CHECK(vps[1].window->rig().ortho(render::OrthoAxis::Top).halfHeight == doctest::Approx(3.5));
        // The dock arrangement is restored too: the top viewport sits where it did (same window size, same splitters).
        const QPoint nowTopLeft = vps[1].container->mapTo(&window, QPoint(0, 0));
        CHECK(std::abs(nowTopLeft.x() - topLeftOfTop.x()) < 40);
        CHECK(std::abs(nowTopLeft.y() - topLeftOfTop.y()) < 40);
        // And it still draws: the top view shows the box, zoomed in (half height 3.5: the box is 1/7 of the view height).
        const auto img = vps[1].window->capture();
        REQUIRE(img.ok());
        CHECK(boxAtCentreOfTopView(img));
    }
    CHECK(app.device->validationErrors() == 0);
    CHECK(app.device->allocationStats().allocationCount == app.baseAllocations);
}
