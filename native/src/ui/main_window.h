// ui/main_window -- the application's top-level window.
//
// Owns: the Qt ADS dock manager (central widget), the viewport docks (layout presets with 1, 2 or 4 viewports: perspective, top,
// front, side), the Stats dock, the View menu, and saving / restoring the layout (preset, dock arrangement, window geometry,
// every viewport's settings and camera poses) in a QSettings, the action registry / keymap / input router of T1.6 (every key and
// mouse event of the application goes through the router; the menus are generated from the registry).
// Thread: UI thread only.
// Spec: 07_EDITOR_PORT.md group C (app/main_window), 10_UI_AND_INPUT.md, 05_RENDERER.md §3, 08_MILESTONES.md T1.4.
#pragma once

#include <QImage>
#include <QMainWindow>
#include <QPointF>
#include <QString>
#include <QTimer>

#include <deque>
#include <functional>
#include <atomic>
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
#include "render/rt_lights.h"
#include "scene/snapshot.h"
#include "tools/details.h"
#include "tools/face_drag.h"
#include "tools/resize2d.h"
#include "tools/edit_ops.h"
#include "tools/gizmo.h"
#include "tools/modal.h"
#include "tools/prefs.h"
#include "tools/texturing/face_edit.h"
#include "tools/texturing/ftt.h"
#include "tools/texturing/tileset_edit.h"
#include "tools/shapes.h"
#include "exporting/launch.h"
#include "tools/brush_edit.h"
#include "tools/mesh/cut.h"
#include "tools/mesh/session.h"
#include "tools/mesh/select.h"
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
class ModelStore;
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
namespace native::exporting { class ExportJob; }
namespace core { struct BspExportResult; }
namespace native::render { struct LineVertex; enum class GizmoMode; struct PickOptions; }

namespace native::ui {

struct BoxDrawing;
struct TransformSession;
class StatsPanel;
class RegistryMenus;
class OutlinerPanel;
class DetailsPanel;
class SurfacePanel;
class TextureBrowserPanel;
class FttWindow;
class TexturePropsDialog;
class TexGroupEditor;
class GroupsPanel;
class LevelSettingsPanel;
class TileSetEditor;
class ReliefEditor;

// The faces the Fast Texture Tool edits: some faces (all, when a whole brush is selected) of one brush.
struct FttTarget {
    std::string entityId, brushId;
    std::vector<uint32_t> faceIds;
};
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
    // The Lit / Lighting only modes can be chosen: the chunked bake runs, or the ray traced Lit path (--rt-lit) is on.
    bool litModesAvailable() const;
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
    // The level settings (worldspawn keys; nullopt removes a key) as one undo step named `label`; false when nothing changed or the edit is refused.
    bool setLevelKeys(const std::string& label, const std::vector<std::pair<std::string, std::optional<std::string>>>& changes);
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

    // ---- File > Export / Play map / Import Quake Map (T9.3, ui/main_window_export.cpp) ----
    // An export (of the map as it is now, in the game folder's format) runs on its own thread; the status bar shows its progress. Only one at a time.
    bool exporting() const;
    // Asks where to put the main file (assets relative, with the exporter's extension) and starts the export.
    void exportWithDialog();
    // Starts an export to `path` (assets relative or absolute inside the game folder); `play` starts the game with the file once it is written. False when it did not start (the reason is reported).
    bool startExport(const std::string& path, bool play, bool bake = true);
    // Export next to the map file, then start the game of Settings > General > Run game with it.
    void playMap();
    void cancelExport();
    // The result of the export that finished last (null before the first).
    std::shared_ptr<const core::BspExportResult> lastExport() const { return lastExport_; }
    // The game's start line (per game folder, kept in the settings).
    exporting::LaunchSettings launchSettings() const;
    void setLaunchSettings(const exporting::LaunchSettings& s);
    // Replaces the open map with a converted Quake .map file (a new, unsaved map); warnings go to the log.
    void importQuakeMapWithDialog();
    bool importQuakeMap(const std::string& file);
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
    // The models of entities (T11.9c): the placements the render thread draws, from the snapshot, the store and the hidden set.
    void refreshModels();
    // A model finished loading (queued from the loading thread): its box changes the entity's pick / outline box, then the scene is rebuilt.
    void modelStoreChanged();
    // The size labels of the selection in a viewport (what refreshSizeLabels sends to the render thread; empty = none).
    std::vector<render::TextItem> sizeLabelsFor(size_t panelIndex);
    void refreshSizeLabels(size_t panelIndex);

    // ---- transform tools (T4.1) ----
    // G / R / S on the selection in `viewport` (the pointer then moves the selection; typed digits, X / Y / Z constrain; confirm / cancel end it): one undo step.
    // False when nothing transformable is selected or one is running.
    // In mesh mode the same G / R / S move the picked vertices, and E / I / Ctrl+B (Extrude, Inset, Bevel) and Ctrl+R (LoopCut, `edge` = the edge whose ring is cut) work on
    // the picked faces / edges; see MeshSession.
    bool startTransform(render::ViewportWindow* viewport, tools::ModalKind kind, const std::optional<tools::mesh::Edge>& edge = std::nullopt);
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
    // `extrude`: mesh mode, face element, Shift on the move arrow: the picked faces are extruded along the arrow instead of moved.
    bool beginGizmoDrag(render::ViewportWindow* viewport, QPointF p, bool extrude = false);

    // ---- face drag and 2D resize (T11.2c) ----
    // The handle of the one selected face (object mode, select tool): an arrow along the face's outward normal from the centre of the face.
    struct FaceHandle {
        std::string entityId, brushId;
        uint32_t faceId = 0;
        render::Vec3 pivot, dir;
    };
    std::optional<FaceHandle> faceHandle() const;
    // The eight handles on the bounding rectangle of the selected brushes in an orthographic view (select tool, no face handle); none elsewhere.
    struct ResizeInfo {
        tools::V3 min, max;
        tools::ScreenAxis right, up;
        double metresPerPixel = 0;
    };
    std::optional<ResizeInfo> resizeInfo(const render::ViewportWindow* viewport) const;
    // The face of the brush under `p` is selected and dragged along its normal (Alt + left button), or the arrow of the selected face is dragged (`alt` false). False when
    // nothing was hit.
    bool beginFaceDrag(render::ViewportWindow* viewport, QPointF p, bool alt);
    // A press on a handle of the 2D resize of `viewport`.
    bool beginResizeDrag(render::ViewportWindow* viewport, QPointF p);

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

    // ---- mesh mode (T11.1f, ui/main_window_mesh.cpp) ----
    // Edit mode "mesh": the vertices / edges / faces of the primary selection's brush are selected and edited (Tab, then 1 2 3 for the element kind). The operations are
    // tools/mesh/session on the document; each is one undo step. The selection (mode, element kind, picked elements) lives in doc::Selection.
    bool meshMode() const;
    bool enterMeshMode();     // false (with a status message) when no single brush is selected
    void leaveMeshMode();
    bool toggleMeshMode();
    tools::mesh::SelMode meshElement() const;
    bool setMeshElement(tools::mesh::SelMode element);   // the picked elements are converted by Blender's rules (edges -> the faces they enclose ...)
    // The brush being edited (null outside mesh mode).
    std::optional<tools::mesh::MeshTarget> meshTarget() const;
    tools::mesh::MeshSel meshSelection() const;
    void setMeshSelection(const tools::mesh::MeshSel& sel);
    // An instant tool by id (tools/mesh/session: "connect", "merge", "fill", "dissolve", "delete", "flip", "recalc", "bridge", "smooth", "sharp", "subdivide"; the selection tools
    // "selectall", "selectnone", "invert", "grow", "shrink"). `param`: subdivide's cuts. False when it did nothing (the status bar says why).
    bool runMeshTool(const std::string& id, double param = 1);
    // Runs a mesh operation on the edited brush as one undo step (a concave result moves into func_detail). The mesh selection becomes the operation's.
    bool applyMeshOperation(const std::string& label, const tools::mesh::MeshOpFn& op);
    // Proportional editing (O): unselected vertices within the radius follow G / R / S with a smooth falloff; the mouse wheel changes the radius while a transform runs.
    bool meshProportional() const { return proportional_; }
    void setMeshProportional(bool on);
    double meshProportionalRadius() const { return propRadius_; }
    // The mouse wheel during a running transform: the proportional radius (mesh G / R / S with proportional editing on) or the bevel's segment count. False = not used.
    bool transformWheel(double dy);
    bool transformUsesWheel() const;
    // Ctrl+R: the loop cut through the edge under the pointer of `viewport` (nothing happens, with a hint, when there is none).
    bool startLoopCut(render::ViewportWindow* viewport);
    // Ctrl+M, then X / Y / Z: mirrors the picked vertices (all of them: the whole brush) about their centre; one undo step.
    bool mirrorMesh(int axis);
    // ---- brush operations (T11.2): Hollow, Merge, Snap, Select inside / touching, the clipper ----
    bool hollowSelected(double thickness);
    bool mergeSelected();
    bool snapSelected();
    bool selectByVolume(tools::VolumeMode mode);
    // Clipper (X): click up to three points (two = a plane along the view); Enter keeps the back side, Shift+Enter splits, Ctrl+Enter keeps the front, Esc cancels.
    bool clipperActive() const { return clip_.has_value(); }
    bool startClipper();
    void stopClipper(const QString& message = {});
    void addClipperPoint(const core::Vec3& point, const render::Vec3& viewDir);
    std::vector<core::Vec3> clipperPoints() const;
    bool clipperApply(tools::Keep keep);   // cuts the selected brushes along the plane of the points
    // ---- knife (K): click points on vertices / edges (Ctrl = the edge's middle), Enter or a right click cuts along them, Esc cancels ----
    bool knifing() const { return knife_.has_value(); }
    bool startKnife();
    void addKnifePoint(const tools::mesh::KnifePoint& point);
    bool finishKnife();
    void cancelKnife(const QString& message = {});
    std::vector<tools::mesh::KnifePoint> knifePoints() const { return knife_ ? knife_->points : std::vector<tools::mesh::KnifePoint>{}; }
    // The mesh.pick action: a click selects the element under the cursor (Shift toggles, Ctrl adds the path from the last one, Alt the loop, Ctrl+Alt the ring), a drag selects a box.
    void meshPickInput(render::ViewportWindow* viewport, const actions::InputEvent& event);
    // What a click at `p` (logical pixels of the viewport window) selects; false when nothing was under the cursor (the selection is left alone).
    bool meshClickAt(render::ViewportWindow* viewport, QPointF p, bool shift, bool ctrl, bool alt);
    void meshBoxSelectIn(render::ViewportWindow* viewport, QPointF a, QPointF b, bool shift);
    // The edges of the edited brush as world-space lines (pale blue; smooth edges green; selected ones orange), drawn with the selection outline; empty outside mesh mode.
    std::vector<render::LineVertex> meshEdgeLines() const;
    // The vertex markers of a viewport (a few pixels wide, so they are made per viewport); empty outside mesh mode.
    std::vector<render::LineVertex> meshVertexLines(size_t panelIndex) const;

    // ---- texturing (T11.3d) ----
    // One texturing command on the selection (a selected face; else all faces of the selected brushes / entities), one undo step. `op` and `params`:
    //   apply {texture?}  lift  copyUv  pasteUv  flow  nudge {mode: shift|scale|rotate, key: left|right|up|down|rotcw|rotccw, fine?}  reset  fit  hotspot
    //   justify {edge: left|right|top|bottom|center}  fitAxis {axis, count}  ortho  axial  cam  mirror {axis}  shear {value}  rotate {value degrees}
    //   shift {axis, value texels}  stretch {axis, value multiple of natural}  matchGrid {grid?}  lightmap {factor | value}  repeat {axis, mode?, edge?}
    // False when nothing was done (the status bar says why).
    bool textureCommand(const std::string& op, const nlohmann::json& params = {});
    tools::tex::InfoFn textureInfo() const;   // the texture environment's sizes / properties (empty without an assets folder)
    tools::tex::HotspotFn hotspotSource() const;
    void setTextureInfo(tools::tex::InfoFn info) { textureInfo_ = std::move(info); }       // tests: textures without an assets folder
    void setHotspotSource(tools::tex::HotspotFn fn) { hotspotOverride_ = std::move(fn); }  // tests
    std::optional<tools::tex::FaceRef> selectedFaceRef() const;   // the only selected face
    bool textureTargetsExist() const;                              // texture edits have faces to work on
    std::optional<tools::tex::FaceRef> copiedFace() const { return copiedFace_; }
    const tools::tex::TexSteps& textureSteps() const { return texSteps_; }
    void setTextureSteps(const tools::tex::TexSteps& s) { texSteps_ = s; }

    // ---- Fast Texture Tool (T11.3g): the UV island of the selected faces, edited as one piece ----
    std::optional<FttTarget> fttTarget() const;                       // the selected faces of one brush (a selected brush: all its faces)
    const core::Brush* fttBrush(const FttTarget& target) const;
    bool fttApply(const std::string& label, const tools::tex::Xform& xform);   // one undo step
    size_t fttUnwrap(tools::tex::UnwrapMode mode);                    // the number of faces that changed
    // A drag: begin, any number of fttDragTo (each is the transform from the start), end (keep = false puts everything back). One undo step.
    bool fttBeginDrag(const std::string& label);
    bool fttDragTo(const FttTarget& target, const tools::tex::Xform& xform);
    void fttEndDrag(bool keep);
    bool fttDragging() const { return fttDrag_.has_value(); }
    void openFastTextureTool();
    void toggleFastTextureTool();
    FttWindow* fttWindow() const { return fttWindow_; }

    // ---- texture properties (T11.4): the sidecar JSON next to a texture ----
    using TexPropsRead = std::function<nlohmann::json(const std::string&)>;
    using TexPropsWrite = std::function<std::string(const std::string&, const nlohmann::json&)>;
    nlohmann::json textureProps(const std::string& texture) const;                              // as stored (an empty object without a sidecar)
    std::string saveTextureProps(const std::string& texture, const nlohmann::json& props);    // "" = written; the scene is rebuilt with the new properties
    void setTexturePropsStore(TexPropsRead read, TexPropsWrite write) { propsRead_ = std::move(read); propsWrite_ = std::move(write); }   // tests: no assets folder
    QImage loadTextureImage(const std::string& texture) const;                                  // the texture picture (null when it cannot be read)
    TexturePropsDialog* openTextureProperties(const std::string& texture, bool show = true);
    TexturePropsDialog* propsDialog() const { return propsDialog_; }

    // ---- texture groups (T11.4b): rule groups and texture sets in <assets>/<texGroupRoot>/<name>.texgroup.json ----
    struct TexGroupStore {
        std::function<std::vector<std::string>()> list;
        std::function<std::optional<nlohmann::json>(const std::string&)> read;
        std::function<std::string(const std::string&, const nlohmann::json&)> write;
        std::function<std::string(const std::string&)> remove;
    };
    std::vector<std::string> texGroupList() const;
    std::optional<nlohmann::json> texGroupRead(const std::string& name) const;
    std::string texGroupWrite(const std::string& name, const nlohmann::json& group);   // "" = written; the scene is rebuilt (faces that use the group are laid out again)
    std::string texGroupRemove(const std::string& name);
    void setTexGroupStore(TexGroupStore store) { groupStore_ = std::move(store); }      // tests: no assets folder
    bool applyTexGroup(const std::string& name);                                          // to the selected faces / brushes; one undo step
    TexGroupEditor* openTexGroupEditor(const std::string& name, bool show = true);        // "" = a new group
    TexGroupEditor* groupEditor() const { return groupEditor_; }
    TileSetEditor* openTileSetEditor(const std::string& name, tools::tex::KitKind kind = tools::tex::KitKind::Wall, bool show = true);   // "" = a new set of `kind`
    TileSetEditor* tileSetEditor() const { return tileSetEditor_; }
    ReliefEditor* openReliefEditor(const std::string& texture, bool show = true);   // the relief of a texture (a fresh editor each time)
    ReliefEditor* reliefEditor() const { return reliefEditor_; }

    // ---- delete, duplicate, group, clipboard, hide (T4.3) ----
    // Each is one undo step on the selection (prefab-internal and face selections are left alone); false when refused or nothing to do (the status bar says why).
    bool deleteSelected();
    bool duplicateSelected();   // the copies are selected
    bool groupSelected();       // brushes -> a new func_group
    bool ungroupSelected();     // the brushes of func_group entities (or any selected brushes) -> worldspawn
    // Turn into prefab (T11.7a): the selection is written as a prefab file under the prefab folder (named `name`, else asked for) and replaced by one instance of it at the
    // selection's pivot, one undo step; the instance is selected. An existing file of that name is only replaced after a yes. False when refused (the status bar says why).
    bool turnIntoPrefab(const std::string& name = {});
    // Asked for the prefab name (title, label, suggestion); an empty answer or nullopt cancels. The default is an input dialog; tests install an answer.
    using NameHandler = std::function<std::optional<std::string>(const QString& title, const QString& label, const QString& suggestion)>;
    void setNameHandler(NameHandler handler) { nameHandler_ = std::move(handler); }
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
    ads::CDockWidget* surfaceDock() const { return surfaceDock_; }
    SurfacePanel* surface() const { return surface_; }
    ads::CDockWidget* texturesDock() const { return texturesDock_; }
    TextureBrowserPanel* textureBrowser() const { return textureBrowser_; }
    GroupsPanel* groupsPanel() const { return groupsPanel_; }
    LevelSettingsPanel* levelPanel() const { return levelPanel_; }
    ads::CDockWidget* levelDock() const { return levelDock_; }
    ads::CDockWidget* groupsDock() const { return groupsDock_; }
    std::shared_ptr<const core::GameProfile> gameProfile() const { return profile_; }
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
    void prefabFileWritten();     // a prefab file was written (Turn into prefab): the Place panel lists it
    void texGroupsChanged();   // a group file was written or removed
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
    bool startMeshTransform(render::ViewportWindow* viewport, tools::ModalKind kind, const std::optional<tools::mesh::Edge>& edge);
    // The version a mesh session gets from `op` (applied to the brush of the base version with the starting selection); null when it is refused (the last good one stays).
    std::shared_ptr<const doc::DocumentVersion> meshAfter(TransformSession& t, const tools::mesh::MeshOpFn& op);
    void meshApplied(const TransformSession& t);   // after a commit: the mesh selection the operation produced
    // Fills t.mesh for a session of `kind` on the picked elements (false, with a hint in the status bar, when they do not fit the tool).
    bool setupMeshSession(TransformSession& t, tools::ModalKind kind, const std::optional<tools::mesh::Edge>& edge);
    std::vector<render::LineVertex> gizmoLinesOnly(size_t panelIndex);
    std::vector<doc::SelRef> transformSelection() const;
    size_t panelIndexOf(const render::ViewportWindow* vp) const;
    GizmoTarget gizmoTarget(render::ViewportWindow* vp);
    tools::Axis gizmoAxisAt(render::ViewportWindow* vp, const GizmoTarget& g, QPointF p);
    bool originHit(render::ViewportWindow* vp, QPointF p);
    std::optional<render::Vec3> originHandle() const;
    void updateFaceDrag(TransformSession& t);     // main_window_facedrag.cpp
    void updateResize(TransformSession& t);
    std::vector<render::LineVertex> faceDragLines(size_t panelIndex) const;
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
    ads::CDockWidget *placeDock_ = nullptr, *outlinerDock_ = nullptr, *detailsDock_ = nullptr, *surfaceDock_ = nullptr, *texturesDock_ = nullptr;
    SurfacePanel* surface_ = nullptr;
    TextureBrowserPanel* textureBrowser_ = nullptr;
    GroupsPanel* groupsPanel_ = nullptr;
    LevelSettingsPanel* levelPanel_ = nullptr;
    ads::CDockWidget* levelDock_ = nullptr;
    ads::CDockWidget* groupsDock_ = nullptr;
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
    NameHandler nameHandler_;
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
    std::shared_ptr<const render::ObjectRules> rtRules_;   // the last rules given to the render thread (a snapshot with the same rules does not restart the accumulation)
    std::unique_ptr<render::RtLightsProducer> rtLights_;   // the ray traced Lit path lights (null without --rt-lit); stopped before the render thread goes
    std::unique_ptr<light::LightingService> lighting_;     // the bake thread (stopped before the link and the render thread go)
    std::unique_ptr<render::Picker> picker_;
    std::unique_ptr<scene::ModelStore> modelStore_;   // the models of entities; reset before the render thread goes
    std::shared_ptr<std::atomic<bool>> modelChangeQueued_ = std::make_shared<std::atomic<bool>>(false);
    struct PickDrag {
        render::ViewportWindow* viewport = nullptr;
        QPointF start, current;
        bool dragging = false;
    } pickDrag_;
    struct MeshDrag {
        render::ViewportWindow* viewport = nullptr;
        QPointF start, current;
        bool dragging = false;
        bool shift = false, ctrl = false, alt = false;
        bool loop = false;   // a double click selects the loop, like Alt+click
    } meshDrag_;
    struct ClipPoint {
        core::Vec3 p;
        render::Vec3 dir;   // the view direction it was placed with (two points: the plane contains it)
    };
    struct ClipState {
        std::vector<ClipPoint> pts;
        std::optional<size_t> drag;   // the point being dragged with the left button
    };
    std::optional<ClipState> clip_;
    bool applyBrushEdit(const tools::BrushEdit& edit);
    std::optional<tools::Plane> clipperPlane() const;
    bool clipperMouse(size_t panelIndex, const actions::InputEvent& e);
    std::vector<render::LineVertex> clipperLines(size_t panelIndex) const;
    struct KnifeState {
        std::shared_ptr<const doc::BrushNode> node;   // the brush the cut began on: the cut is dropped when it is not the edited one any more
        std::vector<tools::mesh::KnifePoint> points;
        std::optional<tools::mesh::KnifePoint> hover;
    };
    std::optional<KnifeState> knife_;
    bool knifeMouse(size_t panelIndex, const actions::InputEvent& e);
    std::optional<tools::mesh::KnifePoint> knifePointAt(render::ViewportWindow* viewport, QPointF p, bool ctrl) const;
    std::vector<render::LineVertex> knifeLines(size_t panelIndex) const;
    bool proportional_ = false;
    double propRadius_ = 1;
    std::optional<tools::mesh::ActiveElem> meshActive_;   // the element clicked last: where a Ctrl+click path starts
    std::unique_ptr<TransformSession> transform_;
    std::unique_ptr<BoxDrawing> box_;
    Tool tool_ = Tool::Select;
    tools::ShapeId shape_ = tools::ShapeId::Box;
    tools::ShapeParams shapeParams_;
    std::string activeTexture_ = "dev/grey";
    tools::tex::InfoFn textureInfo_;
    tools::tex::HotspotFn hotspotOverride_;
    std::optional<tools::tex::FaceRef> copiedFace_;   // Copy UV
    tools::tex::TexSteps texSteps_;
    struct FttDrag {
        std::shared_ptr<const doc::DocumentVersion> base;
        std::string label;
    };
    std::optional<FttDrag> fttDrag_;
    FttWindow* fttWindow_ = nullptr;
    TexPropsRead propsRead_;
    TexPropsWrite propsWrite_;
    TexturePropsDialog* propsDialog_ = nullptr;
    TexGroupStore groupStore_;
    TexGroupEditor* groupEditor_ = nullptr;
    TileSetEditor* tileSetEditor_ = nullptr;
    ReliefEditor* reliefEditor_ = nullptr;
    void rebuildTextureEnvironment();
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
    void finishExport();
    void launchGame(const std::string& bspRelative);
    std::shared_ptr<exporting::ExportJob> exportJob_;   // shared_ptr: main_window.cpp does not see the type (it pulls in Core headers)
    QTimer* exportTimer_ = nullptr;
    bool playAfterExport_ = false;
    std::string exportOut_;
    std::shared_ptr<const core::BspExportResult> lastExport_;
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
