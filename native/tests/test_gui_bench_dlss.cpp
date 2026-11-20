// T6.8: G-frame-4K. Four perspective viewports of 3840x2160 each, the camera flying through a level for 10 seconds, frames counted per viewport.
// A measurement, not a regression test: it only runs when NATIVE_BENCH_ROOT (assets root) and NATIVE_BENCH_MAP (map relative to it) are set, e.g.
//   NATIVE_BENCH_ROOT=F:/Git/SdlWasmEngine/source/Games/ShooterGame/GameData NATIVE_BENCH_MAP=maps/lvl2.map.json native_gui_tests -tc="*G-frame-4K*"
// NATIVE_BENCH_DLSS = off | quality | dlaa (default quality) is the mode of all four viewports; NATIVE_BENCH_SIZE = WxH of one viewport (default 3840x2160);
// NATIVE_BENCH_LOG = a log file (validation messages, DLSS lines). NATIVE_NO_VALIDATION=1 runs without the validation layer.
// Note: a display at 143 Hz caps every viewport at about 143 fps (the Off and 1440p numbers are the display, not the GPU).
// The level is opened in a MainWindow (its scene is the one every viewport of the render thread draws); the four measured viewports are extra
// frameless windows of exactly the asked size, stacked at the origin (the part outside the screen is still rendered and presented).
#include <doctest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <vector>

#include "base/jobs.h"
#include "base/log.h"
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

} // namespace

TEST_CASE("G-frame-4K: four viewports at 4K with DLSS fly through a level" * doctest::test_suite("bench")) {
    const std::string root = envVar("NATIVE_BENCH_ROOT"), mapRel = envVar("NATIVE_BENCH_MAP");
    if (root.empty() || mapRel.empty() || !fs::exists(fs::path(root) / mapRel)) return;   // not asked for
    std::string modeName = envVar("NATIVE_BENCH_DLSS");
    if (modeName.empty()) modeName = "quality";
    int vw = 3840, vh = 2160;
    const std::string sizeS = envVar("NATIVE_BENCH_SIZE");
    if (!sizeS.empty()) sscanf_s(sizeS.c_str(), "%dx%d", &vw, &vh);
    const std::string logPath = envVar("NATIVE_BENCH_LOG");
    if (!logPath.empty()) base::setLogFile(logPath);

    App app;
    if (!app.ok) return;
    base::JobPool pool(8);
    ui::Services sv = app.services();
    sv.jobs = &pool;
    sv.assetsRoot = root;
    sv.forceViewports = 1;
    {
        ui::MainWindow window(sv);
        window.resize(900, 600);
        window.show();
        REQUIRE(waitUntil([&] { return window.viewports().size() == 1; }));
        window.openDocument(mapRel);
        REQUIRE(waitUntil([&] { return window.sceneReady(); }, 180000));
        window.frameScene();
        const std::string pose = window.viewports()[0].window->rig().poseToJson();

        const render::DlssMode mode = render::dlssModeFromName(modeName);
        std::vector<std::unique_ptr<render::ViewportWindow>> shots;
        for (int i = 0; i < 4; ++i) {
            render::ViewportSettings settings;
            settings.dlss = mode;
            auto w = std::make_unique<render::ViewportWindow>(*app.rt, app.qvk, settings);
            w->setFlags(Qt::FramelessWindowHint | Qt::Tool);
            w->rig().poseFromJson(pose);
            w->setGeometry(0, 0, 100, 100);
            w->show();
            const double dpr = w->devicePixelRatio();
            w->setGeometry(0, 0, static_cast<int>(std::lround(vw / dpr)), static_cast<int>(std::lround(vh / dpr)));
            shots.push_back(std::move(w));
        }
        REQUIRE(waitUntil([&] {
            for (auto& w : shots)
                if (w->viewportId() == 0) return false;
            return true;
        }));
        for (auto& w : shots) w->viewChanged();
        pumpFor(4000);   // textures, first frames, DLSS history

        std::vector<render::ViewportId> ids;
        for (auto& w : shots) ids.push_back(w->viewportId());
        for (render::ViewportId id : ids) {
            const auto info = app.rt->dlssInfo(id);
            MESSAGE("viewport " << id << ": DLSS " << std::string(info.active ? "on" : "off") << " renders " << info.renderW << "x" << info.renderH << " for " << info.outW << "x"
                                << info.outH);
        }
        std::vector<uint64_t> before;
        for (render::ViewportId id : ids) before.push_back(app.rt->framesRendered(id));
        const auto t0 = std::chrono::steady_clock::now();
        QElapsedTimer clock;
        clock.start();
        double angle = 0;
        while (clock.elapsed() < 10000) {   // every viewport turns a little each round, so every viewport is dirty all the time
            angle += 0.4;
            for (auto& w : shots) {
                w->rig().drag(std::sin(angle) * 2.0, 0.0, vh);
                w->viewChanged();
            }
            QApplication::processEvents(QEventLoop::AllEvents, 2);
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        double minFps = 1e9, total = 0;
        for (size_t i = 0; i < ids.size(); ++i) {
            const double fps = static_cast<double>(app.rt->framesRendered(ids[i]) - before[i]) / seconds;
            MESSAGE("viewport " << ids[i] << ": " << fps << " fps");
            minFps = std::min(minFps, fps);
            total += fps;
        }
        MESSAGE("G-frame-4K " << modeName << ": slowest viewport " << minFps << " fps, " << total << " viewport frames per second in total (" << seconds << " s, " << vw << "x" << vh
                              << " each)");
        CHECK(minFps >= 100.0);
        shots.clear();
    }
    MESSAGE("log file: " << base::logFilePath());
    CHECK(app.device->validationErrors() == 0);
}
