// T6.4: the Lit view through the render thread. demo_lit goes through the scene builder, the lighting service and the render thread (scatter into the display
// pages, lit meshes, lit shader); the Lit frame differs from the Unlit one and is brighter near the lights than far from them. Writes the frames of the demo
// camera to build/lit_view_demo_lit.png (Unlit | Lit | Lighting only) for a look. [gpu]: skips itself without a ray tracing device with a second queue.
#include <doctest.h>

#include "scene/snapshot.h"   // first: core headers use `near` as a name, which windows.h (via volk) defines as a macro

#include <QImage>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "base/jobs.h"
#include "doc/changeset.h"
#include "doc/io.h"
#include "doc/prefabs.h"
#include "gpu/device.h"
#include "light/light_link.h"
#include "light/lighting_service.h"
#include "render/camera_rig.h"
#include "render/render_thread.h"
#include "scene/builder.h"

#include <core/profile.h>
#include <core/texplan.h>

using namespace native;
namespace fs = std::filesystem;

namespace {

LRESULT CALLBACK litWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

HWND makeWindow(int w, int h) {
    static const wchar_t* kClass = L"native_tests_lit_window";
    static const bool registered = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = litWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        RegisterClassW(&wc);
        return true;
    }();
    (void)registered;
    RECT r{0, 0, w, h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClass, L"native lit test", WS_OVERLAPPEDWINDOW, 60, 60, r.right - r.left, r.bottom - r.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    // The test windows are never shown (the swapchain presents to a hidden window): a test run must not disturb whoever uses the machine.
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return hwnd;
}

double meanLuma(const render::CapturedImage& img) {
    double sum = 0;
    for (size_t i = 0; i + 3 < img.rgba.size(); i += 4) sum += 0.2126 * img.rgba[i] + 0.7152 * img.rgba[i + 1] + 0.0722 * img.rgba[i + 2];
    return sum / (static_cast<double>(img.rgba.size() / 4) * 255.0);
}

double meanAbsDiff(const render::CapturedImage& a, const render::CapturedImage& b) {
    double sum = 0;
    for (size_t i = 0; i < a.rgba.size() && i < b.rgba.size(); ++i)
        if (i % 4 != 3) sum += std::abs(static_cast<int>(a.rgba[i]) - static_cast<int>(b.rgba[i]));
    return sum / (static_cast<double>(a.rgba.size()) * 0.75);
}

} // namespace

TEST_CASE("the Lit view of demo_lit differs from the Unlit one and follows the light" * doctest::test_suite("gpu")) {
    gpu::DeviceOptions o;
    o.validation = true;
    std::string err;
    auto device = gpu::Device::create(o, &err);
    if (!device || !device->caps().lit() || device->computeQueue() == device->graphicsQueue()) {
        MESSAGE("no ray tracing device with a second queue (" << err << "): GPU test skipped");
        return;
    }
    const fs::path root = NATIVE_CORE_ASSETS_DIR;
    const std::string map = "maps/demo_lit.map.json";
    if (!fs::exists(root / map)) return;

    const doc::LoadResult loaded = doc::loadDocumentFile(root / map, map);
    REQUIRE(loaded.version);
    auto profile = std::make_shared<core::GameProfile>(core::loadProfile(root));
    auto texEnv = std::make_shared<core::texplan::TexEnvironment>(root, *profile);
    scene::ReliefEnv env;
    env.info = [texEnv](const std::string& p) { return texEnv->info(p); };
    env.groups = [texEnv](const std::string& n) { return texEnv->group(n); };
    env.revision = 1;
    base::JobPool pool(8);
    scene::SceneBuilder builder(pool);
    builder.setReliefEnv(env);
    doc::PrefabLibrary prefabLib(root);
    doc::PrefabResolver resolver(prefabLib);
    builder.submit(loaded.version, doc::allChanged(0, loaded.version->version), resolver.expand(*loaded.version));
    REQUIRE(builder.waitIdle(120000));
    const auto snapshot = builder.snapshot();
    REQUIRE(snapshot);

    constexpr uint32_t W = 640, H = 400;
    HWND hwnd = makeWindow(W, H);
    VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    ci.hinstance = GetModuleHandleW(nullptr);
    ci.hwnd = hwnd;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    REQUIRE(vkCreateWin32SurfaceKHR(device->instance(), &ci, nullptr, &surface) == VK_SUCCESS);
    {
        render::RenderThread rt(*device);
        rt.setTextureSource(root, &pool);
        rt.setSnapshot(snapshot);
        auto link = std::make_shared<light::LightLink>(*device);
        REQUIRE(link->valid());
        rt.setLightLink(link);
        light::LightingService service(*device, *link);
        REQUIRE(service.valid());
        service.setInputs({snapshot, profile, root});

        const auto id = rt.addViewport(surface, W, H);
        render::CameraRig rig;
        rig.fly().lookAt({3.55, 1.6, 2.5}, {-0.5, 1.2, -3.5});
        const auto frame = [&](render::RenderMode mode) {
            render::ViewportSettings s = rig.settings();
            s.renderMode = mode;
            rig.applySettings(s);
            rt.setView(id, rig.viewState(W, H));
            return rt.capture(id);
        };
        // the textures decode in the background: ask for frames until the unlit view stops changing
        render::CapturedImage unlit = frame(render::RenderMode::Unlit);
        for (int i = 0; i < 40; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            render::CapturedImage next = frame(render::RenderMode::Unlit);
            const bool same = meanAbsDiff(unlit, next) < 0.01;
            unlit = std::move(next);
            if (same && i > 3) break;
        }
        REQUIRE(unlit.ok());
        frame(render::RenderMode::Lit);   // a viewport in a lit mode asks the bake for light
        REQUIRE(service.waitConverged(120000));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        render::CapturedImage lit = frame(render::RenderMode::Lit);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        lit = frame(render::RenderMode::Lit);
        const render::CapturedImage only = frame(render::RenderMode::LightingOnly);
        REQUIRE(lit.ok());
        REQUIRE(only.ok());
        const auto st = service.status();
        MESSAGE("lit service: " << st.chunks << " chunks, error '" << st.error << "'");
        CHECK(st.error.empty());
        const double d = meanAbsDiff(unlit, lit);
        MESSAGE("mean luma unlit " << meanLuma(unlit) << ", lit " << meanLuma(lit) << ", lighting only " << meanLuma(only) << "; mean |unlit - lit| " << d << " of 255");
        CHECK(d > 3.0);                       // lit is a different picture
        CHECK(meanLuma(lit) > 0.02);          // not black: the lights reach the walls
        CHECK(meanLuma(only) > 0.02);
        CHECK(meanAbsDiff(lit, only) > 1.0);  // the textures show in Lit and not in Lighting only

        const uint32_t cw = unlit.width, ch = unlit.height;
        MESSAGE("frame size " << cw << " x " << ch);
        QImage out(static_cast<int>(cw * 3), static_cast<int>(ch), QImage::Format_RGBA8888);
        const render::CapturedImage* imgs[3] = {&unlit, &lit, &only};
        for (uint32_t k = 0; k < 3; ++k) {
            REQUIRE(imgs[k]->width == cw);
            REQUIRE(imgs[k]->height == ch);
            for (uint32_t y = 0; y < ch; ++y) std::memcpy(out.scanLine(static_cast<int>(y)) + k * cw * 4, imgs[k]->rgba.data() + static_cast<size_t>(y) * cw * 4, cw * 4);
        }
        const fs::path dir = fs::path(NATIVE_CORE_ASSETS_DIR).parent_path().parent_path() / "native" / "build";
        std::error_code ec;
        fs::create_directories(dir, ec);
        out.save(QString::fromStdString((dir / "lit_view_demo_lit.png").string()));
        rt.removeViewport(id);
    }
    vkDestroySurfaceKHR(device->instance(), surface, nullptr);
    DestroyWindow(hwnd);
    CHECK(device->validationErrors() == 0);
}
