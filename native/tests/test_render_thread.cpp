// T1.3: render thread, swapchain, redraw-on-dirty policy, resize, capture. Uses a plain Win32 window as the surface so the
// tests need no Qt platform plugin; the Qt side (ViewportWindow in a dock) is covered by native_gui_tests.
// Pixels are checked: the clear colour, the grid and the test box really arrive on screen through the viewport's view.
#include <doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
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
#include "render/camera_rig.h"
#include "render/entity_lights.h"
#include "render/model_scene.h"
#include "render/render_thread.h"
#include "model_builder.h"
#include "scene/model_store.h"

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
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClass, L"native test", WS_OVERLAPPEDWINDOW, 50, 50, r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        // The test windows are never shown (the swapchain presents to a hidden window): a test run must not disturb whoever uses the machine.
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
    explicit GpuFixture(bool streamline = false, bool fsr = false) {
        gpu::DeviceOptions o;
        o.validation = true;
        o.streamline = streamline;
        o.fidelityFx = fsr;
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

// The view the tests look through: a fly camera 6 m in front of the 1 m test box, almost level.
render::ViewState frontView(uint32_t w, uint32_t h) {
    render::CameraRig rig;
    rig.fly().lookAt({0, 1.5, 6}, {0, 0.8, 0});
    return rig.viewState(w, h);
}

TEST_CASE("a viewport shows the clear colour, the grid and the box through its view" * doctest::test_suite("gpu")) {
    GpuFixture gpuFx;
    if (!gpuFx.device) return;
    {
        TestWindow window(320, 240);
        VkSurfaceKHR surface = gpuFx.makeSurface(window.hwnd());
        {
            render::RenderThread rt(*gpuFx.device);
            const auto id = rt.addViewport(surface, 320, 240);
            rt.setView(id, frontView(320, 240));
            const auto img = rt.capture(id);
            REQUIRE(img.ok());
            CHECK(img.width == 320);
            CHECK(img.height == 240);
            // Corner: the clear colour (0.105, 0.105, 0.115) in 8 bit (sky above the horizon).
            CHECK(closeTo(pixel(img, 4, 4), {27, 27, 29}, 3));
            // Centre: the box's +Z face, lit: (0.423, 0.423, 0.558). The inside of the far face would be darker (77, 77, 100):
            // that would mean back-face culling is the wrong way round.
            CHECK(closeTo(pixel(img, 160, 120), {108, 108, 142}, 8));
            // Below the horizon the grid draws on the ground: some pixel in the lower part is a grid grey, not clear.
            bool grid = false;
            for (uint32_t y = 200; y < 240 && !grid; ++y)
                for (uint32_t x = 0; x < 320; ++x)
                    if (pixel(img, x, y).r > 60) { grid = true; break; }
            CHECK(grid);
            rt.removeViewport(id);
        }
        vkDestroySurfaceKHR(gpuFx.device->instance(), surface, nullptr);
    }
    CHECK(gpuFx.device->validationErrors() == 0);
}

TEST_CASE("an orthographic top view puts the grid, the axes and the box where the maths says" * doctest::test_suite("gpu")) {
    GpuFixture gpuFx;
    if (!gpuFx.device) return;
    {
        TestWindow window(320, 240);
        VkSurfaceKHR surface = gpuFx.makeSurface(window.hwnd());
        {
            render::RenderThread rt(*gpuFx.device);
            const auto id = rt.addViewport(surface, 320, 240);
            render::ViewportSettings st;
            st.cameraMode = render::CameraMode::Top;
            render::CameraRig rig(st);                       // 16 m view height over 240 px = 15 px per metre
            rt.setView(id, rig.viewState(320, 240));
            const auto img = rt.capture(id);
            REQUIRE(img.ok());
            // The box seen from above: its +Y face (0.496, 0.716, 0.496), 7.5 px around the centre.
            CHECK(closeTo(pixel(img, 156, 114), {126, 183, 126}, 8));
            // Grid spacing doubled to 1 m (0.5 m would be 7.5 px). The X axis (red) runs along row 120, 3 m right of centre.
            bool red = false, blue = false, minor = false;
            for (uint32_t y = 118; y <= 122; ++y) {
                const Rgb p = pixel(img, 160 + 45, y);
                if (p.r > p.g + 60 && p.r > p.b + 60) red = true;
            }
            for (uint32_t x = 158; x <= 162; ++x) {           // the Z axis (blue) runs down column 160; +Z is down in the top view
                const Rgb p = pixel(img, x, 120 + 45);
                if (p.b > p.r + 60 && p.b > p.g + 30) blue = true;
            }
            for (uint32_t x = 233; x <= 237; ++x)             // a minor line 5 m right of centre
                if (pixel(img, x, 40).r > 60) minor = true;
            CHECK(red);
            CHECK(blue);
            CHECK(minor);
            CHECK(closeTo(pixel(img, 4, 4), {27, 27, 29}, 40));   // far corner: grid grey at most, never the box
            // Moving the camera moves the picture: pan the view 4 m to the right, the box leaves the centre.
            rig.ortho(render::OrthoAxis::Top).center = {4, 0, 0};
            rt.setView(id, rig.viewState(320, 240));
            const auto moved = rt.capture(id);
            REQUIRE(moved.ok());
            CHECK_FALSE(closeTo(pixel(moved, 156, 114), {126, 183, 126}, 30));
            CHECK(closeTo(pixel(moved, 156 - 60, 114), {126, 183, 126}, 8));   // the box is now 4 m = 60 px to the left
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
            rt.setView(id, frontView(480, 360));
            img = rt.capture(id);
            REQUIRE(img.ok());
            CHECK(img.width == 480);
            CHECK(img.height == 360);
            CHECK(closeTo(pixel(img, 240, 180), {108, 108, 142}, 8));   // the box stays centred when the viewport is resized
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

// T6.7: the same frame through the Streamline interposer: surface, swapchain, present and the Reflex / PCL markers all go through SL.
TEST_CASE("a viewport frame through Streamline: Reflex markers count the frames and validation stays clean" * doctest::test_suite("gpu")) {
    GpuFixture gpuFx(true);
    if (!gpuFx.device) return;
    if (!gpuFx.device->caps().streamline) { MESSAGE("Streamline unavailable (" << gpuFx.device->caps().streamlineNote << "): skipped"); return; }
    {
        TestWindow window(320, 240);
        VkSurfaceKHR surface = gpuFx.makeSurface(window.hwnd());
        {
            render::RenderThread rt(*gpuFx.device);
            const auto id = rt.addViewport(surface, 320, 240);
            rt.setView(id, frontView(320, 240));
            const auto img = rt.capture(id);
            REQUIRE(img.ok());
            CHECK(closeTo(pixel(img, 4, 4), {27, 27, 29}, 3));
            CHECK(closeTo(pixel(img, 160, 120), {108, 108, 142}, 8));
            for (int i = 0; i < 40; ++i) {   // enough frames for a Reflex report (every 30th frame reads the state)
                rt.markDirty(id);
                sleepMs(8);
            }
            rt.removeViewport(id);
        }
        vkDestroySurfaceKHR(gpuFx.device->instance(), surface, nullptr);
    }
    const gpu::StreamlineStatus st = gpuFx.device->streamline()->status();
    CHECK(st.frames >= 20);
    CHECK(gpuFx.device->validationErrors() == 0);
}

// T6.8: DLSS Super Resolution of a viewport: the scene is drawn smaller, upscaled by DLSS and the overlays are drawn at full size over it.
TEST_CASE("DLSS: a viewport is upscaled from a smaller render, the picture matches the plain one, history resets are counted" * doctest::test_suite("gpu")) {
    GpuFixture gpuFx(true);
    if (!gpuFx.device) return;
    if (!gpuFx.device->caps().dlss) { MESSAGE("DLSS unavailable (" << gpuFx.device->caps().streamlineNote << "): skipped"); return; }
    {
        TestWindow window(320, 240);
        VkSurfaceKHR surface = gpuFx.makeSurface(window.hwnd());
        {
            render::RenderThread rt(*gpuFx.device);
            const auto id = rt.addViewport(surface, 320, 240);
            render::ViewState view = frontView(320, 240);

            // Off: nothing changes, no upscaling is reported.
            rt.setView(id, view);
            const auto plain = rt.capture(id);
            REQUIRE(plain.ok());
            CHECK_FALSE(rt.dlssInfo(id).active);

            // Quality: two thirds of the size, the same picture.
            view.dlss = 1;
            rt.setView(id, view);
            auto img = rt.capture(id);
            REQUIRE(img.ok());
            render::RenderThread::DlssInfo info = rt.dlssInfo(id);
            CHECK(info.active);
            CHECK_FALSE(info.failed);
            CHECK(info.mode == 1);
            CHECK(info.outW == 320);
            CHECK(info.outH == 240);
            CHECK(info.renderW < 320);
            CHECK(info.renderH < 240);
            CHECK(static_cast<double>(info.renderW) / info.outW == doctest::Approx(2.0 / 3.0).epsilon(0.03));
            CHECK(img.width == 320);
            CHECK(img.height == 240);
            CHECK(closeTo(pixel(img, 4, 4), {27, 27, 29}, 6));          // the clear colour survives the upscaler
            CHECK(closeTo(pixel(img, 160, 120), {108, 108, 142}, 14));  // the box face in the middle
            bool grid = false;                                          // the grid is an overlay drawn at full size after the upscaler
            for (uint32_t y = 200; y < 240 && !grid; ++y)
                for (uint32_t x = 0; x < 320; ++x)
                    if (pixel(img, x, y).r > 60) { grid = true; break; }
            CHECK(grid);

            // The loop draws on demand; DLSS asks for a few more frames after a change so its history converges, with no reset in between.
            for (int i = 0; i < 200 && rt.dlssInfo(id).frames < 20; ++i) sleepMs(10);
            info = rt.dlssInfo(id);
            CHECK(info.frames >= 20);
            CHECK(info.resets == 1);   // the first frame only

            // A moving camera does not reset; a camera cut does.
            render::CameraRig rig;
            rig.fly().lookAt({0.3, 1.5, 6}, {0, 0.8, 0});
            view = rig.viewState(320, 240);
            view.dlss = 1;
            rt.setView(id, view);
            for (int i = 0; i < 100 && rt.dlssInfo(id).frames < info.frames + 2; ++i) sleepMs(10);
            CHECK(rt.dlssInfo(id).resets == 1);
            view.cut += 1;
            rt.setView(id, view);
            for (int i = 0; i < 100 && rt.dlssInfo(id).resets < 2; ++i) sleepMs(10);
            CHECK(rt.dlssInfo(id).resets == 2);

            // DLAA renders at the output size.
            view.dlss = 2;
            rt.setView(id, view);
            img = rt.capture(id);
            REQUIRE(img.ok());
            info = rt.dlssInfo(id);
            CHECK(info.active);
            CHECK(info.mode == 2);
            CHECK(info.renderW == 320);
            CHECK(info.renderH == 240);

            // A resize makes new targets and a new history.
            const uint64_t resetsBefore = rt.dlssInfo(id).resets;
            window.resizeClient(400, 300);
            rt.resizeViewport(id, 400, 300);
            view = render::CameraRig().viewState(400, 300);
            view.dlss = 2;
            render::CameraRig rig2;
            rig2.fly().lookAt({0, 1.5, 6}, {0, 0.8, 0});
            view = rig2.viewState(400, 300);
            view.dlss = 2;
            rt.setView(id, view);
            img = rt.capture(id);
            REQUIRE(img.ok());
            CHECK(img.width == 400);
            info = rt.dlssInfo(id);
            CHECK(info.outW == 400);
            CHECK(info.resets > resetsBefore);

            // Back to Off: drawn the normal way again, the picture of the first frame.
            view.dlss = 0;
            rt.setView(id, view);
            img = rt.capture(id);
            REQUIRE(img.ok());
            CHECK_FALSE(rt.dlssInfo(id).active);
            CHECK(closeTo(pixel(img, 4, 4), {27, 27, 29}, 3));
            rt.removeViewport(id);
        }
        vkDestroySurfaceKHR(gpuFx.device->instance(), surface, nullptr);
    }
    CHECK(gpuFx.device->validationErrors() == 0);
}

// T15.1: AMD FSR as the upscaler: it runs on any GPU (so also on this NVIDIA one), so the whole path is tested here: smaller render, upscale, overlays at full size, history resets,
// resize, switching the backend while a viewport runs, and no validation error (which also checks the image layouts the FidelityFX API is given).
TEST_CASE("FSR: a viewport is upscaled by FidelityFX, the picture matches the plain one, the backend can be switched" * doctest::test_suite("gpu")) {
    GpuFixture gpuFx(true, true);
    if (!gpuFx.device) return;
    if (!gpuFx.device->caps().fsr) { MESSAGE("FSR unavailable (" << gpuFx.device->caps().fsrNote << "): skipped"); return; }
    {
        TestWindow window(320, 240);
        VkSurfaceKHR surface = gpuFx.makeSurface(window.hwnd());
        {
            render::RenderThread rt(*gpuFx.device);
            rt.setUpscaler(gpu::UpscalerPref::Fsr);
            CHECK(rt.upscalerKind() == gpu::UpscalerKind::Fsr);
            const auto id = rt.addViewport(surface, 320, 240);
            render::ViewState view = frontView(320, 240);

            rt.setView(id, view);
            const auto plain = rt.capture(id);
            REQUIRE(plain.ok());
            CHECK_FALSE(rt.dlssInfo(id).active);

            // Quality: 1 / 1.5 of the size per dimension, the same picture.
            view.dlss = 1;
            rt.setView(id, view);
            auto img = rt.capture(id);
            REQUIRE(img.ok());
            render::RenderThread::DlssInfo info = rt.dlssInfo(id);
            CHECK(info.active);
            CHECK_FALSE(info.failed);
            CHECK(info.backend == gpu::UpscalerKind::Fsr);
            CHECK(info.version.rfind("FSR", 0) == 0);
            CHECK(info.outW == 320);
            CHECK(info.outH == 240);
            CHECK(info.renderW == 213);
            CHECK(info.renderH == 160);
            CHECK(closeTo(pixel(img, 4, 4), {27, 27, 29}, 6));          // the clear colour survives the upscaler
            CHECK(closeTo(pixel(img, 160, 120), {108, 108, 142}, 14));  // the box face in the middle
            bool grid = false;                                          // the grid is an overlay drawn at full size after the upscaler
            for (uint32_t y = 200; y < 240 && !grid; ++y)
                for (uint32_t x = 0; x < 320; ++x)
                    if (pixel(img, x, y).r > 60) { grid = true; break; }
            CHECK(grid);

            for (int i = 0; i < 200 && rt.dlssInfo(id).frames < 20; ++i) sleepMs(10);
            info = rt.dlssInfo(id);
            CHECK(info.frames >= 20);
            CHECK(info.resets == 1);   // the first frame only

            // A camera cut resets the history; DLAA / native renders at the output size.
            render::CameraRig rig;
            rig.fly().lookAt({0.3, 1.5, 6}, {0, 0.8, 0});
            view = rig.viewState(320, 240);
            view.dlss = 1;
            rt.setView(id, view);
            view.cut += 1;
            rt.setView(id, view);
            for (int i = 0; i < 100 && rt.dlssInfo(id).resets < 2; ++i) sleepMs(10);
            CHECK(rt.dlssInfo(id).resets == 2);
            view.dlss = 2;
            rt.setView(id, view);
            img = rt.capture(id);
            REQUIRE(img.ok());
            info = rt.dlssInfo(id);
            CHECK(info.active);
            CHECK(info.renderW == 320);
            CHECK(info.renderH == 240);

            // Ultra Performance renders at a third of the size.
            view.dlss = 3;
            rt.setView(id, view);
            img = rt.capture(id);
            REQUIRE(img.ok());
            info = rt.dlssInfo(id);
            CHECK(info.active);
            CHECK(info.renderW == 106);
            CHECK(info.renderH == 80);

            // A resize makes new targets and a new FSR context.
            const uint64_t resetsBefore = rt.dlssInfo(id).resets;
            window.resizeClient(400, 300);
            rt.resizeViewport(id, 400, 300);
            render::CameraRig rig2;
            rig2.fly().lookAt({0, 1.5, 6}, {0, 0.8, 0});
            view = rig2.viewState(400, 300);
            view.dlss = 1;
            rt.setView(id, view);
            img = rt.capture(id);
            REQUIRE(img.ok());
            CHECK(img.width == 400);
            info = rt.dlssInfo(id);
            CHECK(info.active);
            CHECK(info.outW == 400);
            CHECK(info.resets > resetsBefore);

            // Switching to DLSS (when the GPU has it) starts the viewport again on the other backend; turning upscaling off draws the normal frame.
            if (gpuFx.device->caps().dlss) {
                rt.setUpscaler(gpu::UpscalerPref::Dlss);
                CHECK(rt.upscalerKind() == gpu::UpscalerKind::Dlss);
                img = rt.capture(id);
                REQUIRE(img.ok());
                info = rt.dlssInfo(id);
                CHECK(info.active);
                CHECK(info.backend == gpu::UpscalerKind::Dlss);
                rt.setUpscaler(gpu::UpscalerPref::Fsr);
                img = rt.capture(id);
                REQUIRE(img.ok());
                CHECK(rt.dlssInfo(id).backend == gpu::UpscalerKind::Fsr);
            }
            rt.setUpscaler(gpu::UpscalerPref::Off);
            CHECK(rt.upscalerKind() == gpu::UpscalerKind::None);
            img = rt.capture(id);
            REQUIRE(img.ok());
            CHECK_FALSE(rt.dlssInfo(id).active);
            CHECK(closeTo(pixel(img, 4, 4), {27, 27, 29}, 3));
            rt.removeViewport(id);
        }
        vkDestroySurfaceKHR(gpuFx.device->instance(), surface, nullptr);
    }
    CHECK(gpuFx.device->validationErrors() == 0);
}

// Without Streamline (what an AMD or Intel machine has) Auto picks FSR.
TEST_CASE("FSR is the upscaler on a device without Streamline" * doctest::test_suite("gpu")) {
    GpuFixture gpuFx(false, true);
    if (!gpuFx.device) return;
    if (!gpuFx.device->caps().fsr) { MESSAGE("FSR unavailable (" << gpuFx.device->caps().fsrNote << "): skipped"); return; }
    CHECK_FALSE(gpuFx.device->caps().dlss);
    {
        TestWindow window(320, 240);
        VkSurfaceKHR surface = gpuFx.makeSurface(window.hwnd());
        {
            render::RenderThread rt(*gpuFx.device);
            CHECK(rt.upscalerKind() == gpu::UpscalerKind::Fsr);
            const auto id = rt.addViewport(surface, 320, 240);
            render::ViewState view = frontView(320, 240);
            view.dlss = 5;   // Balanced
            rt.setView(id, view);
            const auto img = rt.capture(id);
            REQUIRE(img.ok());
            const auto info = rt.dlssInfo(id);
            CHECK(info.active);
            CHECK(info.backend == gpu::UpscalerKind::Fsr);
            CHECK(info.renderW == 188);   // 320 / 1.7
            CHECK(info.renderH == 141);   // 240 / 1.7
            CHECK(closeTo(pixel(img, 160, 120), {108, 108, 142}, 14));
            rt.removeViewport(id);
        }
        vkDestroySurfaceKHR(gpuFx.device->instance(), surface, nullptr);
    }
    CHECK(gpuFx.device->validationErrors() == 0);
}

TEST_CASE("DLSS is ignored in an orthographic view and without Streamline" * doctest::test_suite("gpu")) {
    {
        GpuFixture gpuFx(true);
        if (!gpuFx.device) return;
        TestWindow window(320, 240);
        VkSurfaceKHR surface = gpuFx.makeSurface(window.hwnd());
        {
            render::RenderThread rt(*gpuFx.device);
            const auto id = rt.addViewport(surface, 320, 240);
            render::CameraRig rig;
            rig.setMode(render::CameraMode::Top);
            render::ViewState view = rig.viewState(320, 240);
            view.dlss = 1;   // asked for, but a top view is orthographic
            rt.setView(id, view);
            const auto img = rt.capture(id);
            REQUIRE(img.ok());
            CHECK_FALSE(rt.dlssInfo(id).active);
            rt.removeViewport(id);
        }
        vkDestroySurfaceKHR(gpuFx.device->instance(), surface, nullptr);
        CHECK(gpuFx.device->validationErrors() == 0);
    }
    {
        GpuFixture plain(false);   // no Streamline: the request is ignored and the frame is the normal one
        if (!plain.device) return;
        TestWindow window(320, 240);
        VkSurfaceKHR surface = plain.makeSurface(window.hwnd());
        {
            render::RenderThread rt(*plain.device);
            const auto id = rt.addViewport(surface, 320, 240);
            render::ViewState view = frontView(320, 240);
            view.dlss = 1;
            rt.setView(id, view);
            const auto img = rt.capture(id);
            REQUIRE(img.ok());
            CHECK_FALSE(rt.dlssInfo(id).active);
            CHECK(closeTo(pixel(img, 160, 120), {108, 108, 142}, 8));
            rt.removeViewport(id);
        }
        vkDestroySurfaceKHR(plain.device->instance(), surface, nullptr);
        CHECK(plain.device->validationErrors() == 0);
    }
}

// A lost device (the driver reset the GPU): the render thread stops submitting and waiting, answers requests empty, and a resize or a removal still returns.
TEST_CASE("a lost GPU device stops the viewport without hanging a resize or a removal" * doctest::test_suite("gpu")) {
    GpuFixture gpuFx;
    if (!gpuFx.device) return;
    std::atomic<int> told{0};
    std::string toldWhy;
    std::mutex toldMutex;
    gpuFx.device->setLostHandler([&](const std::string& why) {
        std::lock_guard<std::mutex> lock(toldMutex);
        toldWhy = why;
        ++told;
    });
    {
        TestWindow window(320, 240);
        VkSurfaceKHR surface = gpuFx.makeSurface(window.hwnd());
        {
            render::RenderThread rt(*gpuFx.device);
            const auto id = rt.addViewport(surface, 320, 240);
            rt.setView(id, frontView(320, 240));
            REQUIRE(rt.capture(id).ok());
            CHECK_FALSE(gpuFx.device->lost());

            gpuFx.device->noteResult(VK_ERROR_OUT_OF_DATE_KHR, "not a loss");   // other results are not a loss
            CHECK_FALSE(gpuFx.device->lost());
            gpuFx.device->noteResult(VK_ERROR_DEVICE_LOST, "unit test");
            gpuFx.device->noteResult(VK_ERROR_DEVICE_LOST, "unit test again");
            CHECK(gpuFx.device->lost());
            CHECK(told == 1);   // said once
            {
                std::lock_guard<std::mutex> lock(toldMutex);
                CHECK(toldWhy.find("unit test") != std::string::npos);
            }
            CHECK(gpuFx.device->lostReason().find("unit test") != std::string::npos);

            // requests are answered, empty, and nothing is drawn any more
            const uint64_t frames = rt.framesRendered(id);
            rt.setView(id, frontView(320, 240));
            const auto start = std::chrono::steady_clock::now();
            CHECK_FALSE(rt.capture(id, 10000).ok());
            window.resizeClient(400, 300);
            rt.resizeViewport(id, 400, 300);
            CHECK_FALSE(rt.capture(id, 10000).ok());
            CHECK(rt.framesRendered(id) == frames);
            rt.removeViewport(id);   // returns: the render thread still serves removals
            CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < 8.0);
        }
        vkDestroySurfaceKHR(gpuFx.device->instance(), surface, nullptr);
    }
    gpuFx.device->setLostHandler(nullptr);
}

// T11.9c: the models of entities. A glb triangle is placed between the camera and the test box: the pixel in the middle of the view turns from the box to the model, and goes back when
// the models are taken away. Validation stays clean (buffers, the instance and vertex streams, the push block).
TEST_CASE("a placed model is drawn through the viewport and leaves with its scene" * doctest::test_suite("gpu")) {
    GpuFixture gpuFx;
    if (!gpuFx.device) return;
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("model_gpu_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / "models");
    {
        modeltest::Builder b;
        const int pos = b.floats({-1, 0, 2, 1, 0, 2, -1, 4, 2}, "VEC3");
        using nlohmann::json;
        const json g = b.document({{"meshes", json::array({json{{"primitives", json::array({json{{"attributes", {{"POSITION", pos}}}}})}}})}, {"nodes", json::array({json{{"mesh", 0}}})}});
        const std::vector<uint8_t> bytes = b.glb(g);
        std::ofstream(root / "models/tri.glb", std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    scene::ModelStore store;
    store.setSource(root, nullptr, {});
    const auto slot = store.get("models/tri.glb");
    REQUIRE(slot.model);
    auto models = std::make_shared<render::ModelScene>();
    render::ModelPlacement placement;
    placement.model = slot.model;
    placement.matrix = native::scene::modelMatrix({0, 0, 0}, {0, 0, 0}, {1, 1, 1});
    placement.bounds = slot.model->data.bounds;
    placement.bounds.min = {-1, 0, 2};
    placement.bounds.max = {1, 4, 2};
    models->placements.push_back(placement);
    {
        TestWindow window(320, 240);
        VkSurfaceKHR surface = gpuFx.makeSurface(window.hwnd());
        {
            render::RenderThread rt(*gpuFx.device);
            const auto id = rt.addViewport(surface, 320, 240);
            rt.setView(id, frontView(320, 240));
            const auto before = rt.capture(id);
            REQUIRE(before.ok());
            CHECK(closeTo(pixel(before, 160, 120), {108, 108, 142}, 8));   // the box
            rt.setModels(models);
            const auto with = rt.capture(id);
            REQUIRE(with.ok());
            const Rgb m = pixel(with, 160, 120);
            CHECK_FALSE(closeTo(m, {108, 108, 142}, 20));   // the triangle is in front of the box
            CHECK_FALSE(closeTo(m, {27, 27, 29}, 10));      // and it is not the clear colour either
            CHECK(closeTo(pixel(with, 300, 20), {27, 27, 29}, 3));   // far from the model nothing changed
            rt.setModels(nullptr);
            const auto after = rt.capture(id);
            REQUIRE(after.ok());
            CHECK(closeTo(pixel(after, 160, 120), {108, 108, 142}, 8));
            rt.removeViewport(id);
        }
        vkDestroySurfaceKHR(gpuFx.device->instance(), surface, nullptr);
    }
    CHECK(gpuFx.device->validationErrors() == 0);
    std::error_code ec;
    fs::remove_all(root, ec);
}

// T10.1: a model is lit by the light volume's value of its entity (ambient + directed * max(0, n . dir)), like the game, in the lit modes: a table with a green ambient makes the triangle
// green, the flat look (no table, or a table made for other placements) stays grey, and a table for another scene with the same placements still counts.
TEST_CASE("a model takes the light volume's value of its entry in the lit modes" * doctest::test_suite("gpu")) {
    GpuFixture gpuFx;
    if (!gpuFx.device) return;
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("model_light_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / "models");
    {
        modeltest::Builder b;
        const int pos = b.floats({-1, 0, 2, 1, 0, 2, -1, 4, 2}, "VEC3");
        using nlohmann::json;
        const json g = b.document({{"meshes", json::array({json{{"primitives", json::array({json{{"attributes", {{"POSITION", pos}}}}})}}})}, {"nodes", json::array({json{{"mesh", 0}}})}});
        const std::vector<uint8_t> bytes = b.glb(g);
        std::ofstream(root / "models/tri.glb", std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    scene::ModelStore store;
    store.setSource(root, nullptr, {});
    const auto slot = store.get("models/tri.glb");
    REQUIRE(slot.model);
    const auto sceneOf = [&] {
        auto models = std::make_shared<render::ModelScene>();
        render::ModelPlacement placement;
        placement.model = slot.model;
        placement.matrix = native::scene::modelMatrix({0, 0, 0}, {0, 0, 0}, {1, 1, 1});
        placement.bounds = slot.model->data.bounds;
        placement.bounds.min = {-1, 0, 2};
        placement.bounds.max = {1, 4, 2};
        models->placements.push_back(placement);
        return models;
    };
    const auto models = sceneOf();
    const auto tableFor = [&](const std::shared_ptr<const render::ModelScene>& forScene, float r, float g, float b) {
        auto t = std::make_shared<render::EntityLightTable>();
        t->data = {r, g, b, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f};   // ambient, no directed light, dir up
        t->models = {1};
        t->forScene = forScene;
        return std::shared_ptr<const render::EntityLightTable>(t);
    };
    {
        TestWindow window(320, 240);
        VkSurfaceKHR surface = gpuFx.makeSurface(window.hwnd());
        {
            render::RenderThread rt(*gpuFx.device);
            const auto id = rt.addViewport(surface, 320, 240);
            render::ViewState view = frontView(320, 240);
            view.renderMode = 1;   // Lit: the models have no lightmap, so without a table they are drawn dimmed
            rt.setView(id, view);
            rt.setModels(models);
            const Rgb flat = pixel(rt.capture(id), 160, 120);
            CHECK(std::abs(flat.r - flat.g) <= 6);   // grey
            CHECK(flat.r > 40);

            rt.setEntityLights(tableFor(models, 0.f, 0.8f, 0.f));
            const Rgb green = pixel(rt.capture(id), 160, 120);
            CHECK(green.g > 170);
            CHECK(green.r < 30);
            CHECK(green.b < 30);

            // the scene is rebuilt with the same model at the same place (a hide toggle elsewhere): the entry still counts
            rt.setModels(sceneOf());
            const Rgb again = pixel(rt.capture(id), 160, 120);
            CHECK(again.g > 170);

            // a table for other placements is ignored: back to the flat look
            auto other = std::make_shared<render::ModelScene>();
            rt.setEntityLights(tableFor(other, 0.f, 0.8f, 0.f));
            const Rgb ignored = pixel(rt.capture(id), 160, 120);
            CHECK(std::abs(ignored.r - ignored.g) <= 6);

            // Lighting only shows the light alone: the same green, whatever the albedo
            rt.setEntityLights(tableFor(sceneOf(), 0.f, 0.8f, 0.f));
            view.renderMode = 2;
            rt.setView(id, view);
            const Rgb only = pixel(rt.capture(id), 160, 120);
            CHECK(only.g > 170);
            CHECK(only.r < 30);

            // the Unlit mode ignores it
            view.renderMode = 0;
            rt.setView(id, view);
            const Rgb unlit = pixel(rt.capture(id), 160, 120);
            CHECK(std::abs(unlit.r - unlit.g) <= 6);
            rt.removeViewport(id);
        }
        vkDestroySurfaceKHR(gpuFx.device->instance(), surface, nullptr);
    }
    CHECK(gpuFx.device->validationErrors() == 0);
    std::error_code ec;
    fs::remove_all(root, ec);
}

// T11.9c: an animated model takes the posed-vertex path (a fresh vertex stream per iteration in the upload ring). The joint slides a little along x; the triangle stays over the middle.
TEST_CASE("an animated model keeps being drawn while it plays" * doctest::test_suite("gpu")) {
    GpuFixture gpuFx;
    if (!gpuFx.device) return;
    namespace sc = native::scene;
    auto loaded = std::make_shared<sc::LoadedModel>();
    loaded->path = "test/animated.glb";
    sc::ModelPrimitive p;
    p.positions = {-1, 0, 2, 1, 0, 2, -1, 4, 2};
    p.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1};
    p.uvs = {0, 0, 0, 0, 0, 0};
    p.indices = {0, 1, 2};
    p.joints = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    p.weights = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    p.skin = 0;
    loaded->data.primitives.push_back(p);
    loaded->data.nodes.resize(1);
    loaded->data.roots = {0};
    sc::ModelSkin skin;
    skin.joints = {0};
    skin.inverseBind = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    loaded->data.skins.push_back(skin);
    sc::ModelAnimation anim;
    anim.name = "Slide";
    anim.duration = 1;
    sc::ModelChannel ch;
    ch.node = 0;
    ch.path = sc::ChannelPath::Translation;
    ch.times = {0, 1};
    ch.values = {0, 0, 0, 0.1f, 0, 0};
    anim.channels.push_back(ch);
    loaded->data.animations.push_back(anim);
    loaded->data.bounds.min = {-1, 0, 2};
    loaded->data.bounds.max = {1, 4, 2};
    auto models = std::make_shared<render::ModelScene>();
    render::ModelPlacement placement;
    placement.model = loaded;
    placement.matrix = sc::modelMatrix({0, 0, 0}, {0, 0, 0}, {1, 1, 1});
    placement.animation = 0;
    placement.bounds = loaded->data.bounds;
    models->placements.push_back(placement);
    REQUIRE(models->animated());
    {
        TestWindow window(320, 240);
        VkSurfaceKHR surface = gpuFx.makeSurface(window.hwnd());
        {
            render::RenderThread rt(*gpuFx.device);
            const auto id = rt.addViewport(surface, 320, 240);
            rt.setView(id, frontView(320, 240));
            rt.setModels(models);
            for (int i = 0; i < 4; ++i) {
                const auto img = rt.capture(id);
                REQUIRE(img.ok());
                CHECK_FALSE(closeTo(pixel(img, 160, 120), {108, 108, 142}, 20));
                CHECK_FALSE(closeTo(pixel(img, 160, 120), {27, 27, 29}, 10));
                sleepMs(60);
            }
            rt.removeViewport(id);
        }
        vkDestroySurfaceKHR(gpuFx.device->instance(), surface, nullptr);
    }
    CHECK(gpuFx.device->validationErrors() == 0);
}
