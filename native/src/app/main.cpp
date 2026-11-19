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

#include <memory>
#include <string>
#include <vector>

#include "app/app_options.h"
#include "base/log.h"
#include "base/trace.h"
#include "gpu/device.h"
#include "render/render_thread.h"
#include "render/viewport_window.h"
#include "ui/main_window.h"

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

    // Declared after the render thread and the Qt instance: destroyed first, so every viewport releases its swapchain while
    // both still exist.
    // The layout (preset, docks, geometry, cameras) lives in an ini file in the user's profile. A --screenshot run uses none, so a
    // visual check is never influenced by (or changes) the person's layout.
    const bool screenshotRun = !parsed.options.screenshotPath.empty();
    QSettings layoutSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("MapEditor"), QStringLiteral("MapEditorNative"));
    native::ui::Services services;
    services.device = device.get();
    services.renderThread = &renderThread;
    services.vulkanInstance = &vulkanInstance;
    services.settings = screenshotRun ? nullptr : &layoutSettings;
    services.forceViewports = parsed.options.layoutViewports;
    native::ui::MainWindow window(services);
    window.setStatsRows(device->caps().rows());
    window.show();

    int shotResult = 0;
    if (!parsed.options.screenshotPath.empty()) {
        // Visual-check mode: wait for the first paint, save the window, quit.
        QTimer::singleShot(600, &qtApp, [&] {
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
        });
    }
    const int rc = qtApp.exec();
    native::base::traceStop();
    return shotResult != 0 ? shotResult : rc;
}
