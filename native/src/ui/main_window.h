// ui/main_window -- the application's top-level window.
//
// Owns: the QMainWindow shell: the viewport dock (T1.3: a Vulkan ViewportWindow in a dock) and the Stats dock (T1.2).
// Geometry persistence and the Qt ADS dock manager arrive with T1.4 (both docks move into it), menus built from the action
// registry with T1.6.
// Thread: UI thread only.
// Spec: 07_EDITOR_PORT.md group C (app/main_window), 10_UI_AND_INPUT.md, 05_RENDERER.md §3.
#pragma once

#include <QMainWindow>

#include <string>
#include <utility>
#include <vector>

class QDockWidget;
class QVulkanInstance;

namespace native::gpu { class Device; }
namespace native::render {
class RenderThread;
class ViewportWindow;
}

namespace native::ui {

class StatsPanel;

// What the window needs from the application (owned by main(); the window never outlives them). All null in widget-only tests.
struct Services {
    gpu::Device* device = nullptr;
    render::RenderThread* renderThread = nullptr;
    QVulkanInstance* vulkanInstance = nullptr;
};

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(const Services& services = {}, QWidget* parent = nullptr);

    static const char* windowTitle();

    // Fills the Stats dock (gpu::DeviceCaps::rows()).
    void setStatsRows(const std::vector<std::pair<std::string, std::string>>& rows);

    // The 3D viewport and its dock; null without services.
    render::ViewportWindow* viewport() const { return viewport_; }
    QDockWidget* viewportDock() const { return viewportDock_; }
    // The widget that hosts the viewport's native window.
    QWidget* viewportContainer() const { return viewportContainer_; }

private:
    StatsPanel* stats_ = nullptr;
    render::ViewportWindow* viewport_ = nullptr;   // owned by viewportContainer_
    QWidget* viewportContainer_ = nullptr;
    QDockWidget* viewportDock_ = nullptr;
};

} // namespace native::ui
