// T1.3 (Qt side): a Vulkan viewport in a dock shows the triangle; resizing, floating and re-docking work with 0 validation
// errors; an unchanged viewport is idle. Real windows, real GPU: part of native_gui_tests.
#include <doctest.h>

#include <QApplication>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QVulkanInstance>

#include <cstdlib>
#include <functional>
#include <memory>
#include <string>

#include "gpu/device.h"
#include "render/render_thread.h"
#include "render/viewport_window.h"
#include "ui/main_window.h"

using namespace native;

namespace {

struct Rgb { int r, g, b; };
Rgb pixel(const render::CapturedImage& img, uint32_t x, uint32_t y) {
    const size_t i = (static_cast<size_t>(y) * img.width + x) * 4;
    return {img.rgba[i], img.rgba[i + 1], img.rgba[i + 2]};
}
bool closeTo(Rgb a, Rgb b, int tol) { return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol; }

void pumpFor(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

bool waitUntil(const std::function<bool()>& cond, int timeoutMs = 4000) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        if (cond()) return true;
        QApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return cond();
}

// Device, Qt's wrapper of its VkInstance and the render thread, in the order main() creates them.
struct App {
    std::unique_ptr<gpu::Device> device;
    QVulkanInstance qvk;
    std::unique_ptr<render::RenderThread> rt;
    bool ok = false;
    App() {
        gpu::DeviceOptions o;
        o.validation = true;
        std::string err;
        device = gpu::Device::create(o, &err);
        if (!device) { MESSAGE("no Vulkan device (" << err << "): GUI GPU test skipped"); return; }
        qvk.setVkInstance(device->instance());
        QByteArrayList ext = {"VK_KHR_surface", "VK_KHR_win32_surface"};
        if (device->validationActive()) ext.push_back("VK_EXT_debug_utils");
        qvk.setExtensions(ext);
        if (!qvk.create()) { MESSAGE("QVulkanInstance::create failed: GUI GPU test skipped"); return; }
        rt = std::make_unique<render::RenderThread>(*device);
        ok = true;
    }
    ui::Services services() { return {device.get(), rt.get(), &qvk}; }
};

} // namespace

TEST_CASE("a viewport in a dock shows the triangle, resizes, floats and re-docks" * doctest::test_suite("gpu")) {
    App app;
    if (!app.ok) return;
    {
        ui::MainWindow window(app.services());
        window.resize(1000, 700);
        window.show();
        REQUIRE(window.viewport() != nullptr);
        REQUIRE(waitUntil([&] { return window.viewport()->viewportId() != 0; }));

        // 1. First frame: clear colour in the corner, the triangle's colours inside.
        auto img = window.viewport()->capture();
        REQUIRE(img.ok());
        CHECK(img.width > 300);
        CHECK(closeTo(pixel(img, 3, 3), {27, 27, 29}, 3));
        // Centroid of the test triangle for any size: x = w/2, y = 0.6 * h.
        CHECK(closeTo(pixel(img, img.width / 2, static_cast<uint32_t>(img.height * 0.6)), {128, 136, 119}, 16));

        // 2. Resizing the main window changes the frame size (the swapchain follows).
        const uint32_t w0 = img.width;
        window.resize(1200, 760);
        pumpFor(200);
        img = window.viewport()->capture();
        REQUIRE(img.ok());
        CHECK(img.width != w0);
        CHECK(img.width == static_cast<uint32_t>(window.viewport()->width() * window.viewport()->devicePixelRatio()));
        CHECK(img.height == static_cast<uint32_t>(window.viewport()->height() * window.viewport()->devicePixelRatio()));

        // 3. Undock and redock three times: the native window (and its VkSurface) is recreated each time.
        for (int round = 0; round < 3; ++round) {
            window.viewportDock()->setFloating(true);
            pumpFor(200);
            REQUIRE(waitUntil([&] { return window.viewport()->viewportId() != 0; }));
            img = window.viewport()->capture();
            CHECK(img.ok());
            if (img.ok()) CHECK(closeTo(pixel(img, 3, 3), {27, 27, 29}, 3));

            window.viewportDock()->setFloating(false);
            pumpFor(200);
            REQUIRE(waitUntil([&] { return window.viewport()->viewportId() != 0; }));
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
    CHECK(app.device->allocationStats().allocationCount == 0);
}
