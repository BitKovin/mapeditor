// Shared by the GUI tests (real windows, real GPU): pixels, event pumping, the device / Qt Vulkan instance / render thread bundle and
// helpers that send synthetic mouse, key and wheel events to a window.
#pragma once

#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QVulkanInstance>
#include <QWheelEvent>
#include <QWindow>

#include <cmath>
#include <functional>
#include <memory>
#include <string>

#include "gpu/device.h"
#include "render/render_thread.h"
#include "render/viewport_window.h"
#include "ui/main_window.h"

namespace guitest {

using namespace native;

struct Rgb { int r, g, b; };
inline Rgb pixel(const render::CapturedImage& img, uint32_t x, uint32_t y) {
    const size_t i = (static_cast<size_t>(y) * img.width + x) * 4;
    return {img.rgba[i], img.rgba[i + 1], img.rgba[i + 2]};
}
inline bool closeTo(Rgb a, Rgb b, int tol) { return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol; }

inline void pumpFor(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

inline bool waitUntil(const std::function<bool()>& cond, int timeoutMs = 10000) {
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
    // VMA allocations the render thread itself holds (upload ring, scene arenas, texture table): what is left when every viewport is gone.
    decltype(gpu::AllocationStats::allocationCount) baseAllocations = 0;
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
        baseAllocations = device->allocationStats().allocationCount;
        ok = true;
    }
    ui::Services services() {
        ui::Services s;
        s.device = device.get();
        s.renderThread = rt.get();
        s.vulkanInstance = &qvk;
        s.keymapPresetPath = std::string(NATIVE_ASSETS_DIR) + "/keymaps/default.json";
        s.verifyButtons = false;   // events are injected without real buttons; the real moves that follow must not end the gestures
        return s;
    }
};

inline void sendMouse(QWindow* w, QEvent::Type type, Qt::MouseButton button, Qt::MouseButtons buttons, QPoint local,
                      Qt::KeyboardModifiers mods = Qt::NoModifier) {
    const QPoint global = w->mapToGlobal(local);
    QMouseEvent e(type, QPointF(local), QPointF(global), button, buttons, mods);
    QApplication::sendEvent(w, &e);
}

inline void sendKey(QWindow* w, QEvent::Type type, int key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QKeyEvent e(type, key, mods);
    QApplication::sendEvent(w, &e);
}

inline void sendWheel(QWindow* w, int angleY, Qt::MouseButtons buttons) {
    const QPoint local(w->width() / 2, w->height() / 2);
    QWheelEvent e(QPointF(local), QPointF(w->mapToGlobal(local)), QPoint(), QPoint(0, angleY), buttons, Qt::NoModifier,
                  Qt::NoScrollPhase, false);
    QApplication::sendEvent(w, &e);
}

inline bool allReady(ui::MainWindow& window) {
    for (const auto& p : window.viewports())
        if (p.window->viewportId() == 0) return false;
    return !window.viewports().empty();
}

} // namespace guitest
