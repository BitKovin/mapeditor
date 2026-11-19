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
#include <QString>
#include <QTimer>

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
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
namespace native::base { class JobPool; }
namespace native::doc {
class DocumentVersion;
class History;
class Selection;
struct ChangeSet;
struct LoadResult;
} // namespace native::doc
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
    base::JobPool* jobs = nullptr;     // loads and saves run here (not on the UI thread); null = synchronously (tests)
    std::string assetsRoot;            // --assets; empty = the saved setting ("assets/root" in `settings`)
};

enum class LayoutPreset { One = 1, Two = 2, Four = 4 };

struct ViewportPanel {
    ads::CDockWidget* dock = nullptr;
    QWidget* container = nullptr;          // hosts the viewport's native window
    render::ViewportWindow* window = nullptr;   // owned by container
};

class MainWindow : public QMainWindow {
    Q_OBJECT
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

    // ---- the document (T2.2) ----
    // The current document version (null until one is opened). Versions are immutable: keep the pointer as long as needed.
    std::shared_ptr<const doc::DocumentVersion> document() const { return document_; }
    // The document was changed since it was opened / saved.
    bool isDirty() const;
    // The folder maps are opened from and saved to (paths in documents are relative to it). Persisted in the settings.
    std::string assetsRoot() const { return assetsRoot_; }
    void setAssetsRoot(const std::string& folder);
    // Opens a map: a path relative to the assets root, or an absolute path inside it. Loads on the job pool (the window stays
    // responsive, the status bar says "Loading ...") or synchronously without one; replaces the document when done. Errors go to
    // the error handler and leave the current document alone.
    void openDocument(const std::string& path);
    // Saves to the document's path; a document without one is saved with saveDocumentAs through a dialog.
    void saveDocument();
    // Saves under another path (relative to the assets root, or absolute inside it); the document takes the new path.
    void saveDocumentAs(const std::string& path);
    // The dialogs behind the File menu: QFileDialog restricted to the assets root.
    void openWithDialog();
    void saveAsWithDialog();
    void chooseAssetsRootWithDialog();
    // Makes `version` the current document (after a load, and for undo / redo / edits from T2.4 on). `saved` marks it as the saved
    // state (a load or a save), otherwise it counts as modified when it differs from the saved one.
    void setDocument(std::shared_ptr<const doc::DocumentVersion> version, bool saved);
    // ---- selection (T2.5) ----
    // What is selected and the edit mode. Follows the document: an edit, undo or redo keeps what still exists, a new document clears it.
    doc::Selection& selection() { return *selection_; }

    // ---- edits and undo (T2.4) ----
    // The undo history of the document (reset whenever a document is opened or saved under another path).
    doc::History& history() { return *history_; }
    // Makes `after` (built from document() with a doc::VersionEditor) the current version as one undo step named `label`. False when it
    // was not built from the current version or changed nothing. Listeners hear documentEdited with the change set.
    bool commitEdit(const std::string& label, std::shared_ptr<const doc::DocumentVersion> after);
    bool canUndo() const;
    bool canRedo() const;
    // "Undo Move" / "Redo Clip" for the Edit menu; "Undo" / "Redo" when there is nothing to undo / redo.
    std::string undoText() const;
    std::string redoText() const;
    void undo();
    void redo();
    // A load or a save is running (the file actions are disabled meanwhile).
    bool busy() const { return pendingJobs_ > 0; }
    // Absolute paths of the maps opened or saved last, newest first (at most 10).
    std::vector<std::string> recentFiles() const { return recent_; }
    // Where problems are shown; the default is a message box. Tests install a recorder.
    void setErrorHandler(std::function<void(const QString&)> handler) { errorHandler_ = std::move(handler); }

    LayoutPreset layoutPreset() const { return preset_; }
    // Rebuilds the viewport docks; existing viewports keep their settings and cameras, new ones get the defaults.
    void setLayoutPreset(LayoutPreset preset);

    // Writes the layout to the QSettings of the services (also done when the window closes and shortly after every change).
    void saveLayout();

signals:
    // The current document changed (opened, saved under a new path, replaced): panels refresh their title and contents.
    void documentChanged();
    // The current version changed (an edit, undo, redo, or a new document: `all`): what differs from the previous one.
    void documentEdited(const native::doc::ChangeSet& changes);
    // The selection, the edit mode or the mesh selection changed.
    void selectionChanged();

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
    // documents
    struct ResolvedPath {
        std::string relative;        // assets-relative, forward slashes
        std::string absolute;
        std::string error;           // empty when the path is inside the assets root
    };
    ResolvedPath resolveAssetPath(const std::string& path) const;
    void reportError(const QString& message);
    void updateTitle();
    void addRecent(const std::string& absolutePath);
    // Runs `work` on the job pool and then `done` on the UI thread (both inline without a pool).
    void runJob(std::function<void()> work, std::function<void()> done);
    void drainCompleted();
    void finishOpen(uint64_t serial, const std::string& relative, const doc::LoadResult& result);
    void saveTo(const std::string& relative, const std::string& absolute);
    void scheduleSave();
    void updateDockTitle(size_t index);
    void onHistoryChanged();

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

    std::shared_ptr<const doc::DocumentVersion> document_;
    std::unique_ptr<doc::History> history_;
    std::unique_ptr<doc::Selection> selection_;
    uint64_t savedVersion_ = 0;
    std::string assetsRoot_;
    std::vector<std::string> recent_;
    int pendingJobs_ = 0;
    uint64_t openSerial_ = 0;
    std::function<void(const QString&)> errorHandler_;
    // Results of finished jobs, handed to the UI thread by a timer that runs only while jobs are pending.
    struct Completed {
        std::mutex mutex;
        std::deque<std::function<void()>> queue;
    };
    std::shared_ptr<Completed> completed_ = std::make_shared<Completed>();
    QTimer completedTimer_;
};

} // namespace native::ui
