# 03 — The document in memory

## 1. Requirements

- Loads and saves schema 0.2 `.map.json` **byte-identically** to the current editor for an unchanged document:
  `json.dump(2) + "\n"`, keys sorted (nlohmann's default `std::map` order), atomic write (temp file + rename), exactly as
  `core/src/scene.cpp` `saveDocument` does.
- **Unknown keys survive** (DESIGN §3 "Core keeps the raw JSON as canonical"): the Shell stores editor-only data the Core
  never modelled (face projection parameters, relief options, visgroups, …). The native document keeps every key it does
  not model, at every level (document, entity, brush, face, prefab instance), and writes them back unchanged.
- Edits are cheap on a 25 MB document: an edit copies only the nodes it changes (structural sharing).
- Undo / redo of any edit; drags coalesce into one undo step.
- Thread safety by immutability: the UI thread builds new versions; other threads only read published versions.

## 2. Types (`native/src/doc/document.h`)

```cpp
namespace nat {
using Json = nlohmann::json;

struct FaceNode {                        // one face of a brush
    core::BrushFace face;                // typed (core/brush.h): id, loop, uvs, texture, projection, look ...
    Json raw;                            // the face's JSON as loaded / last written (unknown keys live here)
};
struct BrushNode {
    core::Brush brush;                   // typed: id, op, vertices, faces (as core::BrushFace), smoothEdges
    Json raw;                            // the brush's JSON (faces included)
    uint64_t contentHash = 0;            // hash of `raw` (base/hash.h); computed once when the node is made
};
struct EntityNode {
    core::Entity typed;                  // without brushes (typed.brushes stays empty; brushes live below)
    std::vector<std::shared_ptr<const BrushNode>> brushes;
    Json raw;                            // the entity's JSON WITHOUT its "brushes" array
};
struct InstanceNode { core::PrefabInstance typed; Json raw; };

struct DocumentVersion {
    uint64_t version = 0;                // strictly increasing per process
    std::vector<std::shared_ptr<const EntityNode>> entities;
    std::vector<std::shared_ptr<const InstanceNode>> instances;
    Json rootRaw;                        // top-level keys other than entities / prefabInstances
    std::string path;                    // assets-relative, "" = unsaved
    // lookup tables, rebuilt lazily per version (not saved): entity id -> index, (entity id, brush id) -> index
};
}
```

Rules:
- A node is never modified after it is published. An edit makes a new node (copying the old one), changes it, and makes a
  new `DocumentVersion` whose vectors point at the old nodes except the replaced ones.
- `raw` is the source of truth for saving. When a tool changes typed fields, `doc/json_write.cpp` writes those fields into a
  copy of the old `raw` (only the keys the tool changed), so unknown keys stay. Writers per type:
  `writeBrush(const core::Brush&, const Json& oldRaw) -> Json`, `writeEntity(...)`, `writeInstance(...)`. The key names and
  value formats are those of `shell/src/model/document.ts` (the Shell's writer) — port its serialization, compare with
  `core/src/document.cpp` (the reader) for every field.
- Reading uses the Core: `core::brushFromJson`, `core::documentFromJson` pieces (split per node, see T2.1).

*As built (T2.2):* namespace `native::doc` (not `nat`). `FaceNode` is not a separate node: faces live in the brush's `raw` and in the typed `core::Brush`. `EntityNode` also has `hasBrushesKey` (a file with `"brushes": []` is written back with it) and `DocumentVersion` has `hasEntitiesKey` / `hasInstancesKey` (a file without `prefabInstances` gets none). The lookup tables are built on the first lookup of a version, so an edit changes its copy of a version *before* anything looks things up in it and then publishes it. `DocumentVersion` is copyable (an edit starts from a copy; the copy gets its own tables).

## 3. Load and save (`doc/io.cpp`)

- **Load:** read file on the IO thread → `nlohmann::json::parse` → per entity in parallel (`core::parallelFor`): typed parse
  of the entity and its brushes, `contentHash` of each brush's raw → `DocumentVersion` → validate with
  `core::validateDocument` (on a typed `core::Document` assembled from the nodes; done once on load) → hand to the UI thread.
  On a validation error: dialog with the message, nothing replaced.
- **Save:** assemble the JSON tree from the nodes' `raw` (entities get their `brushes` array back) → dump(2) + "\n" → temp
  file → rename. On the IO thread; the UI only snapshots the version pointer.
- **Save details:** the temp file is removed when a save fails; the final rename is retried for about half a second because a virus scanner or the indexer can hold the old file open for a moment on Windows.
- **Round-trip test (T2.2):** for every map in `core/assets/maps/*.map.json`, `core/assets/prefabs/*.map.json` and lvl2:
  load → save to a temp file → bytes equal to the original file. (If an original file was not written by this editor and
  differs only by formatting, re-save it once with the current Core first and compare against that.)
  *Result:* lvl2 and one of the six shipped files are byte-identical to the editor's format. The other shipped maps / prefabs were
  written by generator scripts (their own key order and indentation), so the test compares them with their canonical form
  (`dump(2) + "\n"` of the parsed original), checks the saved JSON is the same data as the original and that load + save is idempotent.
  The repository assets are not rewritten.

## 4. Validation on edit

Full `validateDocument` is for load and save. After an edit, validate only what changed:
- each new / changed brush: `Brush::checkStructure()`, and `checkConvexSolid()` when its entity class requires convex
  brushes (`core::EntityClassRegistry::requiresConvexBrushes`);
- id uniqueness: the edit's new ids against the version's id tables.
A tool must not produce an invalid brush (DESIGN §5.1); if validation fails anyway, the command is rejected (not
committed) and the status bar shows why. This replaces the Core's "gatekeeper" role in-process.

## 5. Commands and undo (`doc/commands.h`)

```cpp
struct Command {
    std::string label;                         // "Move", "Clip", "Set texture" ... (Edit menu: "Undo Move")
    std::shared_ptr<const DocumentVersion> before, after;
};
class History {                                // UI thread only
public:
    void commit(Command c);                    // pushes, clears redo
    void beginInteraction(std::string label);  // a drag starts: later commits until end merge into one Command
    void endInteraction();
    bool undo(); bool redo();                  // switch the current version; emit the ChangeSet between the two versions
    // limits: at most 500 commands; the oldest are dropped first
};
```

Undo and redo are version switches; because nodes are shared, keeping both versions costs only the changed nodes. The
ChangeSet for an undo is computed by diffing the two versions (§6), so derived data and lighting update exactly as for a
forward edit.

*As built (T2.4):* `doc/edit.h` `VersionEditor` is the one way to make an edited version (replace / add / remove brush, entity, instance; validation of §4 happens there; every new node is parsed back from its written JSON so typed == what loading the JSON gives); it is not in the task list text but tools and commands need it. `History::commit` refuses a command whose `before` is not the current version. Undo / redo are refused while an interaction runs. Save As to a new path resets the history (a version carries its path). `MainWindow` owns the `History` (`commitEdit`, `undo`, `redo`, signal `documentEdited(ChangeSet)`); `ActionDef::dynamicLabel` gives the menu entry text "Undo Move".

## 6. Change sets (`doc/changeset.h`)

```cpp
enum ChangeKind : uint32_t {
    Geometry = 1,       // vertices / faces / op of a brush, brush added / removed
    Surface  = 2,       // texture, projection, uvs, look of faces (no geometry change)
    Keys     = 4,       // entity keyvalues / refs (lights: Light; others may change classes)
    Transform= 8,       // entity origin / angles, instance transform
    Light    = 16,      // a light entity changed (any of its keys or transform)
    Class    = 32,      // an entity's classname changed (lighting class may change)
    Prefab   = 64,      // instance added / removed / file changed
};
struct ChangedBrush { std::string instance, entityId, brushId; uint32_t kinds; core::Aabb before, after; };
struct ChangedEntity { std::string instance, entityId; uint32_t kinds; core::Aabb before, after; };
struct ChangeSet {
    uint64_t fromVersion, toVersion;
    std::vector<ChangedBrush> brushes;
    std::vector<ChangedEntity> entities;
    bool all = false;   // a new document, a game profile change: everything is new
};
ChangeSet diff(const DocumentVersion& a, const DocumentVersion& b);   // pointer identity first, then content hash
```

`diff` compares node pointers (unchanged = same pointer) and only descends into replaced nodes; it must cost
O(changed objects), plus O(entities) pointer compares. Boxes are world-space (after entity origin and instance transform).

*As built (T2.4):* boxes are `doc::Box` (not `core::Aabb`, whose header drags in the live bake). An entity box is its origin (a point); a brush box is its vertex bounds; instances are reported as `ChangedEntity{instance = id, entityId = ""}` with `Prefab` (file or overrides changed, added, removed) or `Transform`. Brush kinds: `Geometry` = vertices / faces' loops and ids / op / carve / smooth; `Surface` = texture, uvs, look hash, projection, lightmap scale; an added or removed brush is `Geometry | Surface`. A changed `rootRaw` (level settings) sets `all`. Changes of keys the editor does not model (raw only) are not reported.

## 7. Prefab instances

- Prefab **files** are loaded by the IO thread (cached by normalized path + mtime; cycle-checked with
  `core::normalizeAssetPath` / `core::PrefabResolveError` semantics). A file watcher (`QFileSystemWatcher`) reloads a prefab
  that changed on disk and produces a ChangeSet with `Prefab` for every instance of it.
- **Expansion** uses the Core's resolver (`core::resolvePrefabs`, `core/include/core/prefab.h`), per instance, cached by
  (instance node pointer, file version). The Shell's `shell/src/model/expand.ts` is a mirror of the same rules and its tests
  (`shell/tests/expand.test.ts`) are the behavioural spec.
- Overrides, references, effective instance names: `core::effectiveInstanceNames`, DESIGN §18.1.

*As built (T2.6):* `doc/prefabs.h`: `PrefabLibrary` (parsed prefab files by normalized path, version number per load, stat check = `refresh()`) and `PrefabResolver` (per instance: a one-instance `core::Document` through `core::resolvePrefabs`, cached by the `InstanceNode` pointer + the document path + the versions of every file the instance read; failures become `problems` of that instance only). Qt-free; the main window runs both on the job pool (`runBackground`, which does not count as `busy()`), keeps `prefabExpansion()` and emits `prefabExpansionChanged` / `prefabsChanged(ChangeSet)`. Files are found changed by polling their time and size every 300 ms (not `QFileSystemWatcher`: a stat of a few files is cheap, and the watcher misses atomic replace-by-rename on Windows). A problem inside a nested include fails the whole instance that contains it. Problems go to the log.

## 8. Selection (`doc/selection.h`)

Not part of the document (not saved, not undone — same as the Shell). Elements:

```cpp
struct SelRef {
    std::vector<std::string> instancePath;   // [] = the document itself
    std::string entityId;
    std::string brushId;                     // "" = the whole entity
    std::vector<uint32_t> faces;             // face mode
    std::vector<uint32_t> vertices, edges;   // mesh mode (edge = index into the brush's half-edge list, see tools)
};
```
Modes: object (entities / brushes), face, mesh (vertex / edge / face), as in `shell/src/state/store.ts` and DESIGN §8.
Selection changes notify panels and the renderer (outline), never the scene builder.

*As built (T2.5):* `SelRef` has one `std::optional<uint32_t> faceId` (as the Shell: several faces = several refs) instead of a `faces` vector, and the mesh picks are a separate `MeshSel {vertices, edges, faces}` of the primary brush (as the Shell's `meshSel`; `edges` are opaque ids of the mesh tools). A prefab instance of the document is `{instancePath = {id}, entityId = ""}`. `doc::Selection` (UI thread, one listener, `revision()`) ports `select` / `toggleSelect` / `selectMany` / `setEditMode` and the `applyDoc` follow rule; the main window owns it (`selection()`, signal `selectionChanged`) and calls `follow` from the history listener before `documentChanged` / `documentEdited` are emitted (undo / redo also drop the mesh picks, a new document resets). Converting mesh picks between vertex / edge / face kinds is left to the mesh tools (picks are dropped).

## 9. View state that is not document

Hidden objects (`shell/src/model/visibility.ts` rules), per-viewport settings (`shell/src/render/viewportSettings.ts`),
camera positions, grid size, layout: stored in the user settings (`QSettings` under `mapeditor_native`), never in the map.

## 10. Clipboard

Copy / cut / paste as JSON text in the OS clipboard with the same format the Shell uses (`shell/src/state/clipboard.ts`),
so the user can copy between the old Shell and the native editor during the transition.
