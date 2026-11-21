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
#include <optional>
#include <set>
#include <unordered_set>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "app/actions/keymap.h"
#include "app/actions/palette.h"
#include "app/actions/registry.h"
#include "app/actions/router.h"
#include "render/math3d.h"
#include "scene/snapshot.h"
#include "tools/details.h"
#include "tools/edit_ops.h"
#include "tools/gizmo.h"
#include "tools/modal.h"
#include "tools/prefs.h"
#include "tools/shapes.h"
#include "tools/visibility.h"
#include "ui/app_actions.h"
#include "ui/input_probe.h"

class QSettings;
class QVulkanInstance;
class QEvent;
class QKeyEvent;

namespace ads {
class CDockManager;
class CDockWidget;
class CDockAreaWidget;
} // namespace ads
namespace native::gpu { class Device; }
namespace native::base { class JobPool; }
namespace native::doc {
class DocumentVersion;
class History;
class PrefabLibrary;
class PrefabResolver;
class Selection;
struct ChangeSet;
struct Expansion;
struct LoadResult;
} // namespace native::doc
namespace native::scene {
struct SceneSnapshot;
class SceneBuilder;
} // namespace native::scene
namespace native::light {
class LightLink;
class LightingService;
} // namespace native::light
namespace core {
struct GameProfile;
}
namespace core::texplan {
class TexEnvironment;
} // namespace core::texplan
namespace core {
struct EntityDefinitions;
} // namespace core
namespace native::render {
class Picker;
class RenderThread;
struct TextItem;
class ViewportWindow;
} // namespace native::render

namespace native::doc { struct SelRef; }
namespace native::render { struct LineVertex; enum class GizmoMode; struct PickOptions; }

namespace native::ui {

struct BoxDrawing;
struct TransformSession;
class StatsPanel;
class RegistryMenus;
class OutlinerPanel;
class DetailsPanel;
class PlacePanel;
class StatusFields;
class KeyHelpOverlay;

// What the window needs from the application (owned by main(); the window never outlives them). All null in widget-only tests.
struct Services {
    gpu::Device* device = nullptr;
    render::RenderThread* renderThread = nullptr;
    bool rtLit = false;                       // --rt-lit: Lit / Lighting only are ray traced per pixel (R2); the chunked bake does not run
    QVulkanInstance* vulkanInstance = nullptr;
    QSettings* settings = nullptr;     // where the layout is kept; null = nothing is saved or restored
    int forceViewports = 0;            // 1, 2 or 4: ignore the saved preset (the saved dock arrangement is not used either)
    std::string keymapPresetPath;      // the preset JSON (assets/keymaps/default.json); empty = no bindings
    std::string userKeymapPath;        // the user's changes against the preset (%APPDATA%/mapeditor_native/keymaps/<name>.json); may not exist
    base::JobPool* jobs = nullptr;     // loads and saves run here (not on the UI thread); null = synchronously (tests)
    std::string assetsRoot;            // --assets; empty = the saved setting ("assets/root" in `settings`)
    bool verifyButtons = true;         // the system's button state overrules a drag that lost its release (tests that inject presses turn it off)
    bool watchInput = false;           // while something is held, ask the system every 100 ms whether it still is (repairs releases that never arrived); the app turns it on
};

enum class LayoutPreset { One = 1, Two = 2, Four = 4 };

struct ViewportPanel {
    ads::CDockWidget* dock = nullptr;
    QWidget* container = nullptr;          // hosts the viewport's native window
    render::ViewportWindow* window = nullptr;   // owned by container
    bool hasSizeLabels = false;                 // the render thread holds size labels for this viewport
    bool hasGizmo = false;                      // ... and gizmo lines
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
    // Ends the holds whose key or button the probe says is up (`probe(trigger, known)`: trigger "btn:2" / "key:W"), and unlocks a viewport pointer whose
    // button is up. The no-argument form asks the operating system (ui/input_probe). Called by a timer while something is held (Services::watchInput).
    void reconcileInput(const InputProbe& probe);
    void reconcileInput();
    void setVerifyButtons(bool on);
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
    // Whether the lit render modes can work: the device has ray queries and a second queue, and the lighting service runs.
    bool lightingAvailable() const;
    // Whether DLSS Super Resolution can run on this device (Streamline loaded, RTX GPU, plugin present).
    bool dlssAvailable() const;
    // Whether the bake of the latest snapshot finished (true without a service); --screenshot waits for it in the lit modes.
    bool lightingSettled() const;
    // One line about the live lighting ("baking 120 / 1577 chunks", "error: ..."); shown in the Stats panel.
    std::string lightingText() const;
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
    // File > New: an empty map (no entities) that has no path yet, so Save asks for one. Unsaved changes are confirmed first; false when declined.
    bool newDocument();
    void openWithDialog();
    void saveAsWithDialog();
    void chooseAssetsRootWithDialog();
    // Makes `version` the current document (after a load, and for undo / redo / edits from T2.4 on). `saved` marks it as the saved
    // state (a load or a save), otherwise it counts as modified when it differs from the saved one.
    void setDocument(std::shared_ptr<const doc::DocumentVersion> version, bool saved);
    // ---- selection (T2.5) ----
    // What is selected and the edit mode. Follows the document: an edit, undo or redo keeps what still exists, a new document clears it.
    doc::Selection& selection() { return *selection_; }

    // ---- prefabs (T2.6) ----
    // The prefab instances of the current document, expanded (null while there is no document; empty without instances). Files are loaded
    // and instances resolved on the job pool, so this lags a document change by a moment; prefabExpansionChanged says when it is new.
    std::shared_ptr<const doc::Expansion> prefabExpansion() const { return expansion_; }
    doc::PrefabLibrary& prefabLibrary() { return *prefabLib_; }

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

    // ---- scene (T3.4) ----
    // The builder has nothing to do, the render thread holds every object and every requested texture is loaded (screenshots, tests).
    bool sceneReady() const;
    // Points every viewport's camera at the whole scene (the solid brushes of the latest snapshot); false while there is none.
    bool frameScene();

    // ---- picking and selecting in the viewports (T3.5) ----
    // Replace = click selects (nothing under the cursor clears), Toggle (Ctrl) adds or removes, Face (Shift) selects the face under the cursor.
    enum class PickOp { Replace, Toggle, Face };
    // The picker over the latest scene snapshot the UI thread has seen; null without a scene builder.
    render::Picker* picker() const { return picker_.get(); }
    // Pick under `p` / everything inside the rectangle a-b (logical pixels of the viewport window) and apply the Radiant rules to the selection.
    void pickAt(render::ViewportWindow* viewport, QPointF p, PickOp op);
    void boxSelectIn(render::ViewportWindow* viewport, QPointF a, QPointF b, PickOp op);
    // The select.pick action: press starts, moves past 4 px turn it into a box select (with a rubber band), release applies it.
    void pickInput(render::ViewportWindow* viewport, const actions::InputEvent& event, PickOp op);
    // The outline lines of the selection are recomputed (the selection or the snapshot changed).
    void refreshOutline();
    // The entity boxes / arrows / gizmos and the size labels of every viewport (the snapshot, the selection or a viewport's view changed).
    void refreshOverlays();
    // The size labels of the selection in a viewport (what refreshSizeLabels sends to the render thread; empty = none).
    std::vector<render::TextItem> sizeLabelsFor(size_t panelIndex);
    void refreshSizeLabels(size_t panelIndex);

    // ---- transform tools (T4.1) ----
    // G / R / S on the selection in `viewport` (the pointer then moves the selection; typed digits, X / Y / Z constrain; confirm / cancel end it): one undo step.
    // False when nothing transformable is selected or one is running.
    bool startTransform(render::ViewportWindow* viewport, tools::ModalKind kind);
    bool transforming() const { return transform_ != nullptr; }
    bool canStartTransform() const;
    void confirmTransform() { endTransform(true); }
    void cancelTransform() { endTransform(false); }
    void toggleTransformAxis(int axis);
    // The entity whose origin is selected (clicked square of a brush entity): the move gizmo then moves the origin alone. Empty = none.
    std::string originSelected() const { return originSel_; }
    void leaveOrigin();
    // The gizmo of a viewport as lines (what the render thread draws); empty = none.
    std::vector<render::LineVertex> gizmoLinesFor(size_t panelIndex);
    void refreshGizmos();
    void refreshGizmo(size_t panelIndex);
    // Press on a gizmo handle / the origin square of `viewport` at `p` (what the left button does before the pick); false when nothing was hit.
    bool beginGizmoDrag(render::ViewportWindow* viewport, QPointF p);

    // ---- the box / shape tool (T4.2) ----
    enum class Tool { Select, Box };
    Tool tool() const { return tool_; }
    void setTool(Tool tool);
    tools::ShapeId shape() const { return shape_; }
    void setShape(tools::ShapeId shape);   // also switches to the box tool
    const tools::ShapeParams& shapeParams() const { return shapeParams_; }
    void setShapeParams(const tools::ShapeParams& params);
    void turnShape(int quarterTurns = 1);
    const std::string& activeTexture() const { return activeTexture_; }
    void setActiveTexture(const std::string& texture) { activeTexture_ = texture; }
    bool drawingBox() const;
    // The box.draw action: press starts the rectangle, moves follow, release moves on to the height.
    void boxInput(render::ViewportWindow* viewport, const actions::InputEvent& event);
    // The second click (or a call from a test) finishes the drawing: one undo step, the new brushes selected. False when nothing was built.
    bool commitBox();
    void cancelBox();
    // The lines of the drawing in progress for a viewport (the rectangle, then the shape); empty when it is drawn elsewhere.
    std::vector<render::LineVertex> boxPreviewLines(size_t panelIndex) const;

    // ---- delete, duplicate, group, clipboard, hide (T4.3) ----
    // Each is one undo step on the selection (prefab-internal and face selections are left alone); false when refused or nothing to do (the status bar says why).
    bool deleteSelected();
    bool duplicateSelected();   // the copies are selected
    bool groupSelected();       // brushes -> a new func_group
    bool ungroupSelected();     // the brushes of func_group entities (or any selected brushes) -> worldspawn
    // The OS clipboard holds the selection as the Shell's clipboard JSON (level-editor/clipboard), so both editors paste each other's copies.
    bool copySelected();
    bool cutSelected();
    bool pasteClipboard(const core::Vec3& offset = {0, 0, 0});   // the pasted items are selected
    // What is hidden: a view setting (not saved with the map, not undone). Hidden objects are not drawn, not picked and not selected.
    const tools::HideState& hideState() const { return hide_; }
    bool hideSelected();
    void unhideAll();
    void setHideRules(const std::vector<tools::HideRule>& rules);
    void setHiddenKeys(const std::set<std::string>& keys);
    // The objects of the latest snapshot that are hidden now (what the render thread skips and the picker ignores).
    std::shared_ptr<const std::unordered_set<scene::ObjectKey, scene::ObjectKey::Hash>> hiddenObjects() const { return hiddenSet_; }

    // ---- the Place, Outliner and Details panels (T4.4) ----
    ads::CDockWidget* placeDock() const { return placeDock_; }
    ads::CDockWidget* outlinerDock() const { return outlinerDock_; }
    ads::CDockWidget* detailsDock() const { return detailsDock_; }
    OutlinerPanel* outliner() const { return outliner_; }
    DetailsPanel* details() const { return details_; }
    PlacePanel* place() const { return place_; }
    // The game profile's entity definitions (null until an assets root with a profile is set) and its prefab folder (assets-relative).
    const core::EntityDefinitions* entityDefinitions() const { return entityDefs_.get(); }
    std::string prefabRoot() const { return prefabRoot_; }
    // Hide / show what an Outliner row stands for (its key, see tools/visibility). A view setting, not an undo step.
    void toggleHiddenKey(const std::string& key);
    // An Outliner click: selects the thing (Toggle: Ctrl-click adds or removes it); a hidden one only says how to get it back. False when it was not selected.
    bool selectFromOutliner(const doc::SelRef& ref, bool hidden, bool toggle);
    // A Details edit (one undo step; refusals go to the status bar). The primary selection stays.
    bool editDetails(const tools::DetailsEdit& edit, const tools::EditValue& value);
    // Where a new entity of `classname` goes: resting on the surface in the middle of the active view (else on the ground, else at the camera's target).
    render::Vec3 placementPoint(const std::string& classname) const;
    // A click in the Place panel: adds a point entity / a prefab instance there and selects it.
    bool placeEntity(const std::string& classname, const std::vector<std::pair<std::string, std::string>>& keyvalues = {});
    bool placePrefab(const std::string& file);

    // ---- status bar and settings (T4.5) ----
    // The permanent fields of the status bar (tool, grid, selection, map, game folder).
    StatusFields* statusFields() const { return statusFields_; }
    QSettings* settings() const { return services_.settings; }
    // What a NEW viewport starts with (Settings > Editor); kept in the settings. Open viewports keep theirs.
    const tools::EditorPrefs& editorPrefs() const { return prefs_; }
    void setEditorPrefs(const tools::EditorPrefs& prefs);
    // Reads game_profile.json and the entity definitions again (after the profile was edited, or its files changed): the scene's texture and relief
    // environment, the picker's class boxes, the Place panel. A malformed profile is reported and the previous one stays.
    void reloadProfile() { setupScene(); }
    // Why the last profile load failed ("" = it loaded).
    const std::string& profileError() const { return profileError_; }
    // Empties File > Open Recent.
    void clearRecentFiles();
    // The Settings dialog (modal), on a page (0 General, 1 Game profile, 2 Editor, later pages after those).
    void openSettings(int page = 0);
    // Asked before the game folder is switched while the map has unsaved changes; the default is a message box. Tests install an answer.
    void setConfirmHandler(std::function<bool(const QString& title, const QString& text)> handler) { confirmHandler_ = std::move(handler); }
    bool confirm(const QString& title, const QString& text);

    // ---- keymaps (T4.6) ----
    // The user keymaps live in one folder (the one of Services::userKeymapPath), one file per name; one is active. Without that path there are no files:
    // changes still apply, they are just not kept.
    bool hasKeymapFiles() const { return !keymapDir_.empty(); }
    const std::string& keymapName() const { return keymapName_; }
    // The name that has a file, plus the active one; sorted.
    std::vector<std::string> keymapNames() const;
    // Each returns "" or why it failed. Switching saves the current layer first; the new one applies at once (menus and hints follow).
    std::string switchKeymap(const std::string& name);
    std::string newKeymap(const std::string& name);       // empty layer, becomes the active one
    std::string deleteKeymap(const std::string& name);    // deleting the active one falls back to "default"
    std::string importKeymap(const std::string& file, const std::string& name);   // validated, stored under `name`, becomes the active one
    std::string exportKeymap(const std::string& file) const;
    // After the user layer was edited (the binding editor): saves it, refreshes the menus and tells the listeners.
    void keymapEdited();

    // ---- discoverability (T4.7) ----
    // The UiEnv a menu or the palette runs actions with: this window and its active viewport.
    UiEnv uiEnv() const { return currentUiEnv(); }
    // Runs an action by id as a menu entry would (no key involved); false when unknown or disabled.
    bool runAction(const std::string& id, const nlohmann::json& params = nullptr);
    // The command palette (a modal dialog); the chosen action runs after it closed.
    void openPalette();
    // The recently used palette entries (kept in the settings as "palette/recent").
    std::vector<actions::Recent> paletteRecent() const;
    void setPaletteRecent(const std::vector<actions::Recent>& list);
    // The key help sheet over the active viewport (built from the contexts active now; made again when the keymap changes).
    void showKeyHelp(bool show);
    bool keyHelpShown() const;
    KeyHelpOverlay* keyHelp() const { return keyHelp_; }
    // The context of the mode that owns the keys now: "modal:transform" while a transform runs, else "tool:box" / "tool:select". Its bindings are the status bar hints.
    std::string modeContext() const;
    std::string hintText() const;

signals:
    // The game folder was changed.
    void assetsRootChanged();
    // The editor preferences were changed.
    void editorPrefsChanged();
    // A viewport changed (its settings or camera) or another one became the active one.
    void viewportsChanged();
    // The effective keymap changed (an edit in the binding editor, another user keymap, an import).
    void keymapChanged();
    // The entity definitions or the prefab folder changed (a new assets root).
    void entityDefinitionsChanged();
    // The hide keys or rules changed.
    void hiddenChanged();
    // The tool, the shape or its parameters changed.
    void toolChanged();
    // A modal transform started or ended (the keys that are active change with it).
    void transformChanged();
    // The current document changed (opened, saved under a new path, replaced): panels refresh their title and contents.
    void documentChanged();
    // The current version changed (an edit, undo, redo, or a new document: `all`): what differs from the previous one.
    void documentEdited(const native::doc::ChangeSet& changes);
    // The selection, the edit mode or the mesh selection changed.
    void selectionChanged();
    // The expansion of the prefab instances was recomputed (the document changed or a prefab file did).
    void prefabExpansionChanged();
    // A prefab FILE changed on disk (found by the poll every 300 ms): the instances that read it, as `Prefab` changes of the current version.
    void prefabsChanged(const native::doc::ChangeSet& changes);

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
    // The GPU device was lost (the driver reset it or hit an error): the viewports stop drawing, the map stays in memory. Said once, on the UI thread.
    void onDeviceLost(const QString& why);
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
    void updatePrefabs();
    void pollPrefabs();
    // The scene (T3.4): the builder that turns document versions into snapshots for the render thread, its texture environment, the sky.
    void setupScene();
    void submitScene(const doc::ChangeSet& changes);
    void updateSky();
    void onSnapshot(std::shared_ptr<const scene::SceneSnapshot> snapshot);   // UI thread: the picker and the outline follow the builder
    // transform tools (main_window_transform.cpp)
    struct GizmoTarget {
        bool valid = false;
        render::Vec3 pivot;
        double length = 1;
        render::GizmoMode mode{};
        std::string originEntity;
    };
    std::vector<doc::SelRef> transformSelection() const;
    size_t panelIndexOf(const render::ViewportWindow* vp) const;
    GizmoTarget gizmoTarget(render::ViewportWindow* vp);
    tools::Axis gizmoAxisAt(render::ViewportWindow* vp, const GizmoTarget& g, QPointF p);
    bool originHit(render::ViewportWindow* vp, QPointF p);
    std::optional<render::Vec3> originHandle() const;
    void updateTransform();
    void showTransformStatus();
    void endTransform(bool confirm);
    bool transformKey(const QKeyEvent& e, bool press);
    bool transformMouse(size_t panelIndex, const actions::InputEvent& e);
    bool boxMouse(size_t panelIndex, const actions::InputEvent& e);
    void boxMove(QPointF p);
    void updateBoxPreview();
    void statusBox();
    void refreshHidden();
    void createToolDocks();                          // main_window_panels.cpp
    void layoutToolDocks(ads::CDockAreaWidget* statsArea);
    render::PickOptions pickOptions() const;
    bool applyEdit(const tools::EditResult& result, bool selectResult = true);
    // Like runJob, but invisible to busy(): housekeeping that must not disable the file actions.
    void runBackground(std::function<void()> work, std::function<void()> done);

    Services services_;
    ads::CDockManager* dockManager_ = nullptr;
    ads::CDockWidget* statsDock_ = nullptr;
    StatsPanel* stats_ = nullptr;
    ads::CDockWidget *placeDock_ = nullptr, *outlinerDock_ = nullptr, *detailsDock_ = nullptr;
    OutlinerPanel* outliner_ = nullptr;
    DetailsPanel* details_ = nullptr;
    PlacePanel* place_ = nullptr;
    std::string prefabRoot_ = "prefabs";
    std::string profileError_;
    std::string keymapDir_, keymapName_ = "default";
    void initKeymapFiles();
    std::string activeKeymapPath() const;
    tools::EditorPrefs prefs_;
    StatusFields* statusFields_ = nullptr;
    KeyHelpOverlay* keyHelp_ = nullptr;
    std::function<bool(const QString&, const QString&)> confirmHandler_;
    std::vector<ViewportPanel> panels_;
    LayoutPreset preset_ = LayoutPreset::One;
    int active_ = 0;
    QTimer saveTimer_;
    QTimer statsTimer_;   // refreshes the Stats panel rows that change (lighting)
    std::vector<std::pair<std::string, std::string>> deviceRows_;
    std::string statsShown_;
    void refreshStats();
    QTimer inputTimer_;
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
    std::shared_ptr<doc::PrefabLibrary> prefabLib_;        // shared with the jobs that use it: they may outlive the window
    std::shared_ptr<doc::PrefabResolver> prefabResolver_;
    std::shared_ptr<const doc::Expansion> expansion_;
    std::unique_ptr<scene::SceneBuilder> builder_;
    std::shared_ptr<const core::GameProfile> profile_;     // the loaded game profile (the lighting service reads it)
    std::shared_ptr<light::LightLink> lightLink_;          // bake -> render hand-over (null without a lit-capable device)
    std::unique_ptr<light::LightingService> lighting_;     // the bake thread (stopped before the link and the render thread go)
    std::unique_ptr<render::Picker> picker_;
    struct PickDrag {
        render::ViewportWindow* viewport = nullptr;
        QPointF start, current;
        bool dragging = false;
    } pickDrag_;
    std::unique_ptr<TransformSession> transform_;
    std::unique_ptr<BoxDrawing> box_;
    Tool tool_ = Tool::Select;
    tools::ShapeId shape_ = tools::ShapeId::Box;
    tools::ShapeParams shapeParams_;
    std::string activeTexture_ = "dev/grey";
    std::string originSel_;
    tools::HideState hide_;
    std::shared_ptr<const std::unordered_set<scene::ObjectKey, scene::ObjectKey::Hash>> hiddenSet_;
    struct GizmoHover {
        size_t panel = 0;
        int axis = -1;
    } gizmoHover_;
    std::shared_ptr<core::texplan::TexEnvironment> texEnv_;
    double glyphScale_ = 0;                                  // the pixel ratio the glyph atlas was made for
    std::shared_ptr<core::EntityDefinitions> entityDefs_;   // the game profile's entity definitions (class boxes, colours, keys)
    uint64_t reliefRevision_ = 0;
    QTimer prefabTimer_;
    uint64_t expandSerial_ = 0;
    bool prefabPolling_ = false;
    int backgroundJobs_ = 0;
    uint64_t savedVersion_ = 0;
    std::string assetsRoot_;
    std::vector<std::string> recent_;
    int pendingJobs_ = 0;
    uint64_t openSerial_ = 0;
    std::function<void(const QString&)> errorHandler_;
    bool deviceLostShown_ = false;
    // Results of finished jobs, handed to the UI thread by a timer that runs only while jobs are pending.
    struct Completed {
        std::mutex mutex;
        std::deque<std::pair<bool, std::function<void()>>> queue;   // (counts as busy, done)
    };
    std::shared_ptr<Completed> completed_ = std::make_shared<Completed>();
    QTimer completedTimer_;
};

} // namespace native::ui
