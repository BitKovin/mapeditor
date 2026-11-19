// T1.3: render thread, swapchain, redraw-on-dirty policy, resize, capture. Uses a plain Win32 window as the surface so the
// tests need no Qt platform plugin; the Qt side (ViewportWindow in a dock) is covered by native_gui_tests.
// Pixels are checked: the clear colour and the test triangle really arrive on screen.
#include <doctest.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "gpu/device.h"
#include "render/render_thread.h"

using namespace native;

namespace {

LRESULT CALLBACK testWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

// A small top-level window with a given client size.
class TestWindow {
public:
    TestWindow(int clientW, int clientH) {
        static const wchar_t* kClass = L"native_tests_window";
        static const bool registered = [] {
            WNDCLASSW wc{};
            wc.lpfnWndProc = testWndProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = kClass;
            RegisterClassW(&wc);
            return true;
        }();
        (void)registered;
        RECT r{0, 0, clientW, clientH};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd_ = CreateWindowExW(0, kClass, L"native test", WS_OVERLAPPEDWINDOW, 50, 50, r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        pump();
    }
    ~TestWindow() { if (hwnd_) DestroyWindow(hwnd_); }
    HWND hwnd() const { return hwnd_; }
    void resizeClient(int w, int h) {
        RECT r{0, 0, w, h};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(hwnd_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        pump();
    }
    static void pump() {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }

private:
    HWND hwnd_ = nullptr;
};

struct GpuFixture {
    std::unique_ptr<gpu::Device> device;
    GpuFixture() {
        gpu::DeviceOptions o;
        o.validation = true;
        std::string err;
        device = gpu::Device::create(o, &err);
        if (!device) MESSAGE("no Vulkan device (" << err << "): GPU test skipped");
    }
    VkSurfaceKHR makeSurface(HWND hwnd) {
        VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        ci.hinstance = GetModuleHandleW(nullptr);
        ci.hwnd = hwnd;
        VkSurfaceKHR s = VK_NULL_HANDLE;
        REQUIRE(vkCreateWin32SurfaceKHR(device->instance(), &ci, nullptr, &s) == VK_SUCCESS);
        return s;
    }
};

struct Rgb { int r, g, b; };
Rgb pixel(const render::CapturedImage& img, uint32_t x, uint32_t y) {
    const size_t i = (static_cast<size_t>(y) * img.width + x) * 4;
    return {img.rgba[i], img.rgba[i + 1], img.rgba[i + 2]};
}
bool closeTo(Rgb a, Rgb b, int tol) { return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol; }

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

} // namespace

TEST_CASE("a viewport shows the clear colour and the test triangle" * doctest::test_suite("gpu")) {
    GpuFixture gpuFx;
    if (!gpuFx.device) return;
    {
        TestWindow window(320, 240);
        VkSurfaceKHR surface = gpuFx.makeSurface(window.hwnd());
        {
            render::RenderThread rt(*gpuFx.device);
            const auto id = rt.addViewport(surface, 320, 240);
            const auto img = rt.capture(id);
            REQUIRE(img.ok());
            CHECK(img.width == 320);
            CHECK(img.height == 240);
            // Corner: the clear colour (0.105, 0.105, 0.115) in 8 bit.
            CHECK(closeTo(pixel(img, 4, 4), {27, 27, 29}, 3));
            // Centroid of the triangle: the average of its three vertex colours.
            CHECK(closeTo(pixel(img, 160, 144), {128, 136, 119}, 14));
            // Outside the triangle, left of its lower-left corner.
            CHECK(closeTo(pixel(img, 20, 230), {27, 27, 29}, 3));
            // A pixel near each corner is dominated by that vertex's colour.
            const Rgb top = pixel(img, 160, 100);       // near (0, -0.6): red
            CHECK(top.r > top.g + 40);
            const Rgb bottomRight = pixel(img, 250, 186);  // near (0.6, 0.6): green
            CHECK(bottomRight.g > bottomRight.r + 40);
            const Rgb bottomLeft = pixel(img, 70, 186);    // near (-0.6, 0.6): blue
            CHECK(bottomLeft.b > bottomLeft.r + 40);
            rt.removeViewport(id);
        }
        vkDestroySurfaceKHR(gpuFx.device->instance(), surface, nullptr);
    }
    CHECK(gpuFx.device->validationErrors() == 0);
}

TEST_CASE("an idle viewport renders nothing; marking it dirty renders exactly one frame" * doctest::test_suite("gpu")) {
    GpuFixture gpuFx;
    if (!gpuFx.device) return;
    {
        TestWindow window(256, 256);
        VkSurfaceKHR surface = gpuFx.makeSurface(window.hwnd());
        {
            render::RenderThread rt(*gpuFx.device);
            const auto id = rt.addViewport(surface, 256, 256);
            REQUIRE(rt.capture(id).ok());
            sleepMs(100);
            const uint64_t before = rt.framesRendered(id);
            sleepMs(600);                                    // idle: the GPU must not be used
            CHECK(rt.framesRendered(id) == before);
            CHECK(rt.totalFrames() == before);

            rt.markDirty(id);
            for (int i = 0; i < 200 && rt.framesRendered(id) == before; ++i) sleepMs(5);
            sleepMs(150);
            CHECK(rt.framesRendered(id) == before + 1);

            // Many dirty marks in a row coalesce: never more frames than marks, at least one.
            const uint64_t b2 = rt.framesRendered(id);
            for (int i = 0; i < 100; ++i) rt.markDirty(id);
            sleepMs(300);
            const uint64_t burst = rt.framesRendered(id) - b2;
            CHECK(burst >= 1);
            CHECK(burst <= 100);
            CHECK(rt.completedFrames() >= rt.totalFrames());
            rt.removeViewport(id);
        }
        vkDestroySurfaceKHR(gpuFx.device->instance(), surface, nullptr);
    }
    CHECK(gpuFx.device->validationErrors() == 0);
}

TEST_CASE("resizing recreates the swapchain and the frame has the new size" * doctest::test_suite("gpu")) {
    GpuFixture gpuFx;
    if (!gpuFx.device) return;
    {
        TestWindow window(300, 200);
        VkSurfaceKHR surface = gpuFx.makeSurface(window.hwnd());
        {
            render::RenderThread rt(*gpuFx.device);
            const auto id = rt.addViewport(surface, 300, 200);
            auto img = rt.capture(id);
            REQUIRE(img.ok());
            CHECK(img.width == 300);
            const uint64_t created = rt.swapchainRecreations(id);

            window.resizeClient(480, 360);
            rt.resizeViewport(id, 480, 360);
            img = rt.capture(id);
            REQUIRE(img.ok());
            CHECK(img.width == 480);
            CHECK(img.height == 360);
            CHECK(closeTo(pixel(img, 240, 216), {128, 136, 119}, 14));   // the triangle scales with the viewport
            CHECK(rt.swapchainRecreations(id) > created);

            window.resizeClient(200, 150);
            rt.resizeViewport(id, 200, 150);
            img = rt.capture(id);
            REQUIRE(img.ok());
            CHECK(img.width == 200);
            CHECK(img.height == 150);

            // A zero-sized viewport (minimised dock) is skipped without errors and comes back.
            rt.resizeViewport(id, 0, 0);
            rt.markDirty(id);
            sleepMs(100);
            rt.resizeViewport(id, 200, 150);
            img = rt.capture(id);
            CHECK(img.ok());
            rt.removeViewport(id);
        }
        vkDestroySurfaceKHR(gpuFx.device->instance(), surface, nullptr);
    }
    CHECK(gpuFx.device->validationErrors() == 0);
}

TEST_CASE("two viewports render independently" * doctest::test_suite("gpu")) {
    GpuFixture gpuFx;
    if (!gpuFx.device) return;
    {
        TestWindow w1(200, 150), w2(260, 180);
        VkSurfaceKHR s1 = gpuFx.makeSurface(w1.hwnd());
        VkSurfaceKHR s2 = gpuFx.makeSurface(w2.hwnd());
        {
            render::RenderThread rt(*gpuFx.device);
            const auto a = rt.addViewport(s1, 200, 150);
            const auto b = rt.addViewport(s2, 260, 180);
            rt.setClearColor(b, 0.5f, 0.0f, 0.0f);
            const auto ia = rt.capture(a);
            const auto ib = rt.capture(b);
            REQUIRE(ia.ok());
            REQUIRE(ib.ok());
            CHECK(closeTo(pixel(ia, 3, 3), {27, 27, 29}, 3));
            CHECK(closeTo(pixel(ib, 3, 3), {128, 0, 0}, 3));
            sleepMs(100);
            const uint64_t fa = rt.framesRendered(a), fb = rt.framesRendered(b);
            rt.markDirty(a);
            sleepMs(200);
            CHECK(rt.framesRendered(a) == fa + 1);
            CHECK(rt.framesRendered(b) == fb);               // the other one stayed idle
            rt.removeViewport(a);
            rt.removeViewport(b);
        }
        vkDestroySurfaceKHR(gpuFx.device->instance(), s1, nullptr);
        vkDestroySurfaceKHR(gpuFx.device->instance(), s2, nullptr);
    }
    CHECK(gpuFx.device->validationErrors() == 0);
}

TEST_CASE("removing and re-adding a viewport on the same window works (re-docking)" * doctest::test_suite("gpu")) {
    GpuFixture gpuFx;
    if (!gpuFx.device) return;
    {
        TestWindow window(220, 160);
        {
            render::RenderThread rt(*gpuFx.device);
            for (int round = 0; round < 3; ++round) {
                VkSurfaceKHR surface = gpuFx.makeSurface(window.hwnd());
                const auto id = rt.addViewport(surface, 220, 160);
                const auto img = rt.capture(id);
                CHECK(img.ok());
                rt.removeViewport(id);
                vkDestroySurfaceKHR(gpuFx.device->instance(), surface, nullptr);
            }
        }
    }
    CHECK(gpuFx.device->validationErrors() == 0);
    CHECK(gpuFx.device->allocationStats().allocationCount == 0);   // capture staging buffers were freed
}
