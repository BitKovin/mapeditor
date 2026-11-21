// T6.4: the lit modes in the real window. demo_lit is opened in a MainWindow (assets root with a game profile), the viewport is switched to Lit, the bake runs
// from the window's own snapshots, and the viewport shows light: shaded surfaces, not one flat grey. Real windows, real GPU: part of native_gui_tests.
#include <doctest.h>

#include <QImage>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <set>

#include "base/jobs.h"
#include "gui_support.h"
#include "render/camera_rig.h"

using namespace native;
using namespace guitest;
namespace fs = std::filesystem;

namespace {

std::string envVar(const char* name) {
    char* v = nullptr;
    size_t n = 0;
    std::string out;
    if (_dupenv_s(&v, &n, name) == 0 && v) out = v;
    free(v);
    return out;
}

// The number of distinct 4-bit grey levels on the frame (a flat unlit grey has a handful; a lit room has many).
size_t greyLevels(const render::CapturedImage& img) {
    std::set<int> levels;
    for (size_t i = 0; i + 3 < img.rgba.size(); i += 4) levels.insert((img.rgba[i] + img.rgba[i + 1] + img.rgba[i + 2]) / 3 / 8);
    return levels.size();
}

} // namespace

TEST_CASE("the Lit mode of a window shows the bake of its own snapshots" * doctest::test_suite("gpu")) {
    App app;
    if (!app.ok) return;
    if (!app.device->caps().lit() || app.device->computeQueue() == app.device->graphicsQueue()) {
        MESSAGE("no ray tracing device with a second queue: skipped");
        return;
    }
    // NATIVE_TEST_LIT_ROOT / NATIVE_TEST_LIT_MAP: look at another level (a development aid)
    const std::string envRootS = envVar("NATIVE_TEST_LIT_ROOT"), envMapS = envVar("NATIVE_TEST_LIT_MAP");
    const char* envRoot = envRootS.empty() ? nullptr : envRootS.c_str();
    const char* envMap = envMapS.empty() ? nullptr : envMapS.c_str();
    const fs::path root = envRoot ? fs::path(envRoot) : fs::path(NATIVE_CORE_ASSETS_DIR);
    const std::string mapRel = envMap ? envMap : "maps/demo_lit.map.json";
    if (!fs::exists(root / mapRel)) return;
    base::JobPool pool(8);
    ui::Services sv = app.services();
    sv.jobs = &pool;
    sv.assetsRoot = root.generic_string();
    {
        ui::MainWindow window(sv);
        window.resize(1000, 700);
        window.show();
        REQUIRE(waitUntil([&] { return allReady(window); }));
        window.openDocument(mapRel);
        REQUIRE(waitUntil([&] { return window.sceneReady(); }, 30000));
        CHECK(window.lightingAvailable());
        render::ViewportWindow* vp = window.viewports()[0].window;
        if (!envMap) vp->rig().fly().lookAt({3.55, 1.6, 2.5}, {-0.5, 1.2, -3.5});
        else window.frameScene();
        vp->setRenderMode(render::RenderMode::Lit);
        vp->viewChanged();
        CHECK(waitUntil([&] { return window.lightingSettled(); }, envMap ? 240000 : 120000));
        pumpFor(500);
        const auto lit = vp->capture();
        REQUIRE(lit.ok());
        vp->setRenderMode(render::RenderMode::LightingOnly);
        pumpFor(300);
        const auto only = vp->capture();
        REQUIRE(only.ok());
        vp->setRenderMode(render::RenderMode::Unlit);
        pumpFor(300);
        const auto unlit = vp->capture();
        REQUIRE(unlit.ok());
        const auto info = app.rt->sceneInfo();
        MESSAGE("scene objects " << info.scene.objects << ", drawn from lit meshes " << info.scene.litObjects);
        CHECK(info.scene.litObjects > 0);
        MESSAGE("raw " << info.scene.rawObjects << " queued " << info.scene.queued);
        MESSAGE("lighting: " << window.lightingText());
        MESSAGE("grey levels: lit " << greyLevels(lit) << ", lighting only " << greyLevels(only) << ", unlit " << greyLevels(unlit));
        QImage out(static_cast<int>(lit.width * 3), static_cast<int>(lit.height), QImage::Format_RGBA8888);
        const render::CapturedImage* imgs[3] = {&unlit, &lit, &only};
        for (uint32_t k = 0; k < 3; ++k)
            for (uint32_t y = 0; y < lit.height; ++y) std::memcpy(out.scanLine(static_cast<int>(y)) + k * lit.width * 4, imgs[k]->rgba.data() + static_cast<size_t>(y) * lit.width * 4, lit.width * 4);
        out.save(QString::fromStdString((fs::path(NATIVE_CORE_ASSETS_DIR).parent_path().parent_path() / "native" / "build" / (envMap ? "gui_lit_other.png" : "gui_lit_demo_lit.png")).string()));
        CHECK(greyLevels(only) > 6);   // surfaces lit differently, not one flat grey
    }
    CHECK(app.device->validationErrors() == 0);
}

// A lost GPU device: the window says so once (on the UI thread), lighting stops, the window keeps answering and closes.
TEST_CASE("a lost GPU device is reported once and the window keeps working" * doctest::test_suite("gpu")) {
    App app;
    if (!app.ok) return;
    const fs::path root = NATIVE_CORE_ASSETS_DIR;
    if (!fs::exists(root / "maps/demo_lit.map.json")) return;
    base::JobPool pool(8);
    ui::Services sv = app.services();
    sv.jobs = &pool;
    sv.assetsRoot = root.generic_string();
    {
        ui::MainWindow window(sv);
        int told = 0;
        QString text;
        window.setErrorHandler([&](const QString& message) {
            ++told;
            text = message;
        });
        window.resize(900, 600);
        window.show();
        REQUIRE(waitUntil([&] { return allReady(window); }));
        window.openDocument("maps/demo_lit.map.json");
        REQUIRE(waitUntil([&] { return window.sceneReady(); }, 30000));
        window.viewports()[0].window->setRenderMode(render::RenderMode::Lit);
        pumpFor(500);
        CHECK(told == 0);

        app.device->noteResult(VK_ERROR_DEVICE_LOST, "gui test");
        app.device->noteResult(VK_ERROR_DEVICE_LOST, "gui test again");
        CHECK(waitUntil([&] { return told > 0; }, 5000));
        pumpFor(300);
        CHECK(told == 1);
        CHECK(text.contains(QStringLiteral("GPU device was lost")));
        CHECK(text.contains(QStringLiteral("gui test")));
        // the window still answers: a resize and a mode switch return, and the lighting reports why it stopped
        window.resize(700, 500);
        window.viewports()[0].window->setRenderMode(render::RenderMode::Unlit);
        pumpFor(500);
        CHECK(window.isVisible());
    }
}

TEST_CASE("with --rt-lit the Lit and Lighting only modes can be chosen although the chunked bake does not run" * doctest::test_suite("gpu")) {
    App app;
    if (!app.ok) return;
    base::JobPool pool(4);
    ui::Services sv = app.services();
    sv.jobs = &pool;
    sv.rtLit = true;
    ui::MainWindow window(sv);
    window.resize(900, 600);
    window.show();
    REQUIRE(waitUntil([&] { return allReady(window); }));
    CHECK_FALSE(window.lightingAvailable());   // no bake in this mode
    CHECK(window.litModesAvailable() == app.device->caps().lit());
}
