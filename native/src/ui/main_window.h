// ui/main_window -- the application's top-level window.
//
// Owns: the Qt ADS dock manager (central widget), the viewport docks (layout presets with 1, 2 or 4 viewports: perspective, top,
// front, side), the Stats dock, the View menu, and saving / restoring the layout (preset, dock arrangement, window geometry,
// every viewport's settings and camera poses) in a QSettings, the action registry / keymap / input router of T1.6 (every key and
// mouse event of the application goes through the router; the menus are generated from the registry).
// Thread: UI thread only.
// Spec: 07_EDITOR_PORT.md group C (app/main_window), 10_UI_AND_INPUT.md, 05_RENDERER.md §3, 08_MILESTONES.md T1.4.
#pragma once

#include <QMainWindow>
#include <QPointF>
#include <QTimer>

#include <string>
#include <utility>
#include <vector>

#include "app/actions/keymap.h"
#include "app/actions/registry.h"
#include "app/actions/router.h"
#include "ui/app_actions.h"

class QSettings;
class QVulkanInstance;
class QEvent;
class QKeyEvent;

namespace ads {
class CDockManager;
class CDockWidget;
} // namespace ads
namespace native::gpu { class Device; }
namespace native::render {
class RenderThread;
class ViewportWindow;
} // namespace native::render

namespace native::ui {

class StatsPanel;
class RegistryMenus;

// What the window needs from the application (owned by main(); the window never outlives them). All null in widget-only tests.
struct Services {
    gpu::Device* device = nullptr;
    render::RenderThread* renderThread = nullptr;
    QVulkanInstance* vulkanInstance = nullptr;
    QSettings* settings = nullptr;     // where the layout is kept; null = nothing is saved or restored
    int forceViewports = 0;            // 1, 2 or 4: ignore the saved preset (the saved dock arrangement is not used either)
    std::string keymapPresetPath;      // the preset JSON (assets/keymaps/default.json); empty = no bindings
    std::string userKeymapPath;        // the user's changes against the preset (%APPDATA%/mapeditor_native/keymaps/<name>.json); may not exist
};

enum class LayoutPreset { One = 1, Two = 2, Four = 4 };

struct ViewportPanel {
    ads::CDockWidget* dock = nullptr;
    QWidget* container = nullptr;          // hosts the viewport's native window
    render::ViewportWindow* window = nullptr;   // owned by container
};

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(const Services& services = {}, QWidget* parent = nullptr);
    ~MainWindow() override;

    static const char* windowTitle();

    // Fills the Stats dock (gpu::DeviceCaps::rows()).
    void setStatsRows(const std::vector<std::pair<std::string, std::string>>& rows);

    // The viewports in preset order (perspective first); empty without services.
    const std::vector<ViewportPanel>& viewports() const { return panels_; }
    // The first viewport (the perspective one); null without services.
    render::ViewportWindow* viewport() const { return panels_.empty() ? nullptr : panels_[0].window; }
    ads::CDockWidget* viewportDock() const { return panels_.empty() ? nullptr : panels_[0].dock; }
    QWidget* viewportContainer() const { return panels_.empty() ? nullptr : panels_[0].container; }
    ads::CDockManager* dockManager() const { return dockManager_; }
    ads::CDockWidget* statsDock() const { return statsDock_; }
    // The viewport the last mouse press went to (what the camera menu acts on); the first one at start.
    render::ViewportWindow* activeViewport() const;

    // The action system: registry (every command), keymap (preset + user layer) and the router all input goes through.
    actions::Registry& registry() { return registry_; }
    actions::Keymap& keymap() { return keymap_; }
    actions::Router& router() { return router_; }
    RegistryMenus* menus() const { return menus_; }
    // Layout actions need viewports (render services).
    bool canLayout() const { return services_.renderThread && services_.vulkanInstance; }
    // Writes the user keymap layer to Services::userKeymapPath (created on demand); false without a path or on a write error.
    bool saveUserKeymap() const;
    std::string keymapError() const { return keymapError_; }

    LayoutPreset layoutPreset() const { return preset_; }
    // Rebuilds the viewport docks; existing viewports keep their settings and cameras, new ones get the defaults.
    void setLayoutPreset(LayoutPreset preset);

    // Writes the layout to the QSettings of the services (also done when the window closes and shortly after every change).
    void saveLayout();

protected:
    void closeEvent(QCloseEvent* e) override;
    // Application-wide key filter: every key press / release that reaches a window is offered to the router first.
    bool eventFilter(QObject* watched, QEvent* e) override;

private:
    struct SavedViewport {
        std::string settingsJson, poseJson;
    };
    void buildViewports(LayoutPreset preset, const std::vector<SavedViewport>& saved);
    void destroyViewports();
    std::vector<SavedViewport> captureViewports() const;
    void loadKeymap();
    UiEnv currentUiEnv() const;
    render::ViewportWindow* viewportUnderCursor() const;
    void setContextsFor(render::ViewportWindow* viewport);
    bool routeKey(const QKeyEvent& e, bool press);
    bool routeViewportEvent(size_t index, QEvent* e, QPointF delta);
    void cancelInput();
    void scheduleSave();
    void updateDockTitle(size_t index);

    Services services_;
    ads::CDockManager* dockManager_ = nullptr;
    ads::CDockWidget* statsDock_ = nullptr;
    StatsPanel* stats_ = nullptr;
    std::vector<ViewportPanel> panels_;
    LayoutPreset preset_ = LayoutPreset::One;
    int active_ = 0;
    QTimer saveTimer_;
    bool building_ = false;
    actions::Registry registry_;
    actions::Keymap keymap_;
    actions::Router router_{registry_, keymap_};
    RegistryMenus* menus_ = nullptr;
    render::ViewportWindow* holdViewport_ = nullptr;   // the viewport a running hold (a drag, flight keys) belongs to
    std::string keymapError_;
};

} // namespace native::ui
