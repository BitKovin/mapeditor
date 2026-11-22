// app/main -- entry point of mapeditor_native.exe.
//
// Owns: QApplication, command line, the main window, the development --screenshot mode. Thread: UI thread (main).
// Spec: 01_ARCHITECTURE.md §2, 02_BUILD_AND_TOOLING.md §6.
#include <QApplication>
#include <QPainter>
#include <QSettings>
#include <QVulkanInstance>
#include <QImage>
#include <QMessageBox>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "app/app_options.h"
#include "automation/server.h"
#include "base/jobs.h"
#include "base/log.h"
#include "base/trace.h"
#include "gpu/device.h"
#include "render/viewport_settings.h"
#include "render/render_thread.h"
#include "render/viewport_window.h"
#include "ui/main_window.h"
#include "ui/input_probe.h"
#include "ui/theme.h"

int main(int argc, char** argv) {
    QApplication::setOrganizationName(QStringLiteral("MapEditor"));
    QApplication::setApplicationName(QStringLiteral("MapEditorNative"));
    QApplication qtApp(argc, argv);

    std::vector<std::string> args;
    const QStringList qargs = QCoreApplication::arguments();
    for (int i = 1; i < qargs.size(); ++i) args.push_back(qargs[i].toStdString());
    const native::app::ParseResult parsed = native::app::parseArgs(args);
    if (!parsed.error.empty() || parsed.options.help) {
        const QString text = QString::fromStdString((parsed.error.empty() ? std::string() : parsed.error + "\n\n") +
                                                    native::app::usageText());
        QMessageBox::information(nullptr, QStringLiteral("Map Editor (native)"), text);
        return parsed.error.empty() ? 0 : 2;
    }

    if (!parsed.options.logFile.empty() && !native::base::setLogFile(parsed.options.logFile))
        native::base::logWarning("cannot open the log file " + parsed.options.logFile);
    if (!parsed.options.traceFile.empty() && !native::base::traceStart(parsed.options.traceFile))
        native::base::logWarning("cannot write the trace file " + parsed.options.traceFile);
    native::base::traceSetThreadName("ui");

    // One Vulkan device for the whole process; failing here is fatal (01_ARCHITECTURE.md §6).
    native::gpu::DeviceOptions deviceOptions;
    deviceOptions.validation = parsed.options.validation;
    deviceOptions.streamline = !parsed.options.noStreamline;   // NVIDIA Streamline (Reflex; DLSS later); falls back to the plain loader
    deviceOptions.gpuCrashDumps = parsed.options.gpuCrashDumps;
    if (!parsed.options.logFile.empty()) {
        const size_t slash = parsed.options.logFile.find_last_of("/\\");
        deviceOptions.crashDumpDir = slash == std::string::npos ? std::string(".") : parsed.options.logFile.substr(0, slash);   // next to the log
    }
    std::string deviceError;
    std::unique_ptr<native::gpu::Device> device = native::gpu::Device::create(deviceOptions, &deviceError);
    if (!device) {
        QMessageBox::critical(nullptr, QStringLiteral("Map Editor (native)"),
                              QStringLiteral("No usable Vulkan device:\n\n") + QString::fromStdString(deviceError));
        return 3;
    }

    // Qt wraps the application's VkInstance so it can create surfaces for the viewport windows (05_RENDERER.md §3).
    QVulkanInstance vulkanInstance;
    vulkanInstance.setVkInstance(device->instance());
    QByteArrayList instanceExtensions = {"VK_KHR_surface", "VK_KHR_win32_surface"};
    if (device->validationActive()) instanceExtensions.push_back("VK_EXT_debug_utils");
    vulkanInstance.setExtensions(instanceExtensions);
    if (!vulkanInstance.create()) {
        QMessageBox::critical(nullptr, QStringLiteral("Map Editor (native)"), QStringLiteral("Qt could not use the Vulkan instance."));
        return 3;
    }
    native::render::RenderThread renderThread(*device);
    // the ray traced Lit view is the default wherever the GPU can do ray queries (--no-rt-lit / --no-lighting opt out, --rt-lit forces it)
    const bool rtLit = parsed.options.rtLit || (!parsed.options.noRtLit && !parsed.options.noLighting && device->caps().lit());
    if (rtLit) {
        native::render::RtLitOptions rtOptions;
        rtOptions.lightmapTexelsPerMeter = parsed.options.rtTexel;
        rtOptions.bounces = parsed.options.rtBounces;
        renderThread.setRtLit(true, rtOptions);
    }

    // Declared after the render thread and the Qt instance: destroyed first, so every viewport releases its swapchain while
    // both still exist.
    // The layout (preset, docks, geometry, cameras) lives in an ini file in the user's profile. A --screenshot run uses none, so a
    // visual check is never influenced by (or changes) the person's layout.
    const bool screenshotRun = !parsed.options.screenshotPath.empty();
    QSettings layoutSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("MapEditor"), QStringLiteral("MapEditorNative"));
    native::base::JobPool jobs;   // loads, saves and (later) all CPU work; outlives the window
    native::ui::Services services;
    services.jobs = &jobs;
    services.assetsRoot = parsed.options.assetsRoot;
    services.device = device.get();
    services.renderThread = &renderThread;
    services.rtLit = rtLit;
    services.vulkanInstance = &vulkanInstance;
    services.settings = screenshotRun ? nullptr : &layoutSettings;
    native::ui::applyAppearance(native::ui::readAppearance(screenshotRun ? nullptr : &layoutSettings));   // a screenshot run shows the default look
    services.forceViewports = parsed.options.layoutViewports;
    services.watchInput = true;
    native::ui::setInputLogging(parsed.options.logInput);
    // Keys: the shipped preset next to the exe, the person's changes in %APPDATA%\mapeditor_native\keymaps (none in a screenshot run).
    services.keymapPresetPath = (QCoreApplication::applicationDirPath() + QStringLiteral("/keymaps/default.json")).toStdString();
    const QString appData = qEnvironmentVariable("APPDATA");
    if (!screenshotRun && !appData.isEmpty())
        services.userKeymapPath = (appData + QStringLiteral("/mapeditor_native/keymaps/default.json")).toStdString();
    native::ui::MainWindow window(services);
    window.setStatsRows(device->caps().rows());
    window.show();
    if (!parsed.options.renderMode.empty()) {
        const auto mode = parsed.options.renderMode == "lit" ? native::render::RenderMode::Lit
                          : parsed.options.renderMode == "lightingOnly" ? native::render::RenderMode::LightingOnly : native::render::RenderMode::Unlit;
        for (const native::ui::ViewportPanel& panel : window.viewports()) panel.window->setRenderMode(mode);
    } else if (rtLit && window.litModesAvailable()) {
        // the ray traced preview is the default look: perspective viewports start in Lit once (the person's later choice is saved with the layout and wins); a screenshot run always shows it
        const QString migrated = QStringLiteral("migrations/rtLitDefault");
        if (screenshotRun || !layoutSettings.value(migrated, false).toBool()) {
            for (const native::ui::ViewportPanel& panel : window.viewports()) {
                const auto cam = panel.window->rig().mode();
                if (cam == native::render::CameraMode::Fly || cam == native::render::CameraMode::Orbit) panel.window->setRenderMode(native::render::RenderMode::Lit);
            }
            if (!screenshotRun) layoutSettings.setValue(migrated, true);
        }
    }
    if (!parsed.options.dlss.empty()) {
        const native::render::DlssMode mode = native::render::dlssModeFromName(parsed.options.dlss);
        for (const native::ui::ViewportPanel& panel : window.viewports()) panel.window->setDlssMode(mode);
    }
    if (!parsed.options.map.empty()) window.openDocument(parsed.options.map);
    std::unique_ptr<native::automation::Server> automation;   // outside scripts (T9.4); only when asked for, on 127.0.0.1
    if (parsed.options.automationPort > 0) {
        automation = std::make_unique<native::automation::Server>(window);
        if (!automation->listen(parsed.options.automationPort)) {
            QMessageBox::critical(nullptr, QStringLiteral("Map Editor (native)"), QStringLiteral("Cannot listen on 127.0.0.1:%1: %2").arg(parsed.options.automationPort).arg(QString::fromStdString(automation->error())));
            return 3;
        }
    }

    int shotResult = 0;
    if (!parsed.options.screenshotPath.empty()) {
        // Visual-check mode: wait until the scene is complete (map built, geometry and textures on the GPU; 20 s at most), frame it, save the
        // window, quit.
        auto shoot = [&] {
            QImage img = window.grab().toImage();
            // QWidget::grab() does not include native child windows: paste every viewport's own frame over its container.
            for (const native::ui::ViewportPanel& panel : window.viewports()) {
                const native::render::CapturedImage frame = panel.window->capture();
                if (!frame.ok()) continue;
                const QImage vp(frame.rgba.data(), static_cast<int>(frame.width), static_cast<int>(frame.height),
                                static_cast<qsizetype>(frame.width) * 4, QImage::Format_RGBA8888);
                QPainter p(&img);
                const QPoint topLeft = panel.container->mapTo(&window, QPoint(0, 0));
                p.drawImage(QRect(topLeft, panel.container->size()), vp);
            }
            if (img.isNull() || !img.save(QString::fromStdString(parsed.options.screenshotPath))) shotResult = 4;
            QApplication::quit();
        };
        auto* poll = new QTimer(&qtApp);
        auto* waited = new int(0);
        auto* framed = new bool(false);
        QObject::connect(poll, &QTimer::timeout, &qtApp, [&, poll, waited, framed, shoot] {
            *waited += 100;
            const bool expectScene = !parsed.options.map.empty();
            if (*waited < 600) return;
            if (expectScene && !window.sceneReady() && *waited < 20000) return;
            if (expectScene && !parsed.options.renderMode.empty() && parsed.options.renderMode != "unlit" && !window.lightingSettled() && *waited < 120000) return;
            if (expectScene && !*framed) {
                *framed = window.frameScene();
                *waited = std::max(*waited, 19000) - 400;   // a few more frames with the camera in place
                return;
            }
            poll->stop();
            shoot();
        });
        poll->start(100);
    }
    const int rc = qtApp.exec();
    native::base::traceStop();
    return shotResult != 0 ? shotResult : rc;
}
