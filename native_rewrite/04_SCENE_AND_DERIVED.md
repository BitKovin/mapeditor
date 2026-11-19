# 04 — Scene builder: derived data per object

## 1. What is derived, and from what

| Derived product | Computed by (existing code) | Unit of caching | Depends on |
|---|---|---|---|
| Resolved prefab entities | `core::resolvePrefabs` (`prefab.h`) | prefab instance | instance node, prefab file version |
| Carved surface pieces (`core::LightSurface`) | `core::collectSurfaces` (`derived.cpp`), `core::csgSubtract` (`csg.h`) | **target brush** | the brush, every subtractive brush whose box overlaps it and whose carve scope applies (`core::carveScopeApplies`) |
| Decal fragments | `core::collectDecals`, `core::clipToDecal` (`decal.h`) | decal entity | the decal entity, the surfaces its box touches |
| Texture layout per face (UVs, repeat plan, texture groups, tile sets) | `core/include/core/texplan.h` (C++ ports of the Shell's `uv.ts`, `uvmesh.ts`, `texgroup.ts`, `tileset.ts`, `layout.ts`, `layoutPlan.ts`) | face | face, texture properties file, texture group file |
| Relief meshes | `texplan.h` relief section (`texplan_relief.cpp`, port of `reliefApply.ts`) | face side | face, neighbours at convex corners (see `texplan.h` "What stands next to the outline edges"), texture's relief |
| Smooth-edge normals | `core::brushSmoothing` (`smoothing.h`) | brush | brush |
| Solids (light-blocking volumes) | `core::collectSolids` (`derived.h`) | brush | brush, class rules |
| Render mesh (triangles, albedo UVs, texture ids, outline edges) | new: `scene/render_mesh.cpp` from the pieces above | brush (+ its decals) | the products above |
| Lightmap charts, texel samples, BLAS input | 06_LIGHTING.md | chart group / chart / brush | surfaces, layout settings |

Nothing in this table is re-implemented in native code. Where the Core function works on the whole document, task T3.1
splits it so it can run for a subset (see §3).

## 2. The cache

`scene/cache.h`:

```cpp
template <class Key, class Value> class DerivedCache {   // thread-safe; values immutable once stored
    // get(key, inputsHash) -> shared_ptr<const Value> or null (miss / stale)
    // put(key, inputsHash, value)
    // retain(keys still in the scene)  -> drops the rest (called once per published snapshot)
};
```

Each product has its own cache keyed by object identity (`instancePath + entityId + brushId [+ faceId + side]`), validated by
an **inputs hash**: the hash of everything the product depends on (column "Depends on"), e.g. for carved pieces of brush B:
`hash(B.contentHash, entity transform, sorted contentHash of every applicable subtractor overlapping B)`. A product is
recomputed exactly when its inputs hash changes. This makes undo / redo and "move it back" free (old values hit the cache
if they were not evicted; evict least-recently-used beyond a memory budget of 2 GB for all caches together).

## 3. Core refactor for per-object derivation (task T3.1)

`core::collectSurfaces(doc, prefabs)` builds targets for the whole document. Add, in `core/include/core/derived.h`:

```cpp
struct BrushRef { std::string instance, entityId, brushId; };   // new; instance = instanceString() of the path ("" = the document)
// The same pieces collectSurfaces() makes, for the target brushes accepted by `which` only (the carving still sees every
// subtractor of the document). The union over a partition of all brushes equals collectSurfaces() (same pieces, same order
// within one brush).
std::vector<LightSurface> collectSurfacesFor(const Document& doc, const std::vector<ResolvedEntity>& prefabs,
                                             const std::function<bool(const std::string& instance, const std::string& entityId,
                                                                      const std::string& brushId)>& which);
// Subtractive brushes that can carve `target` (box overlap + carve scope): its carving dependencies.
std::vector<BrushRef> carversOf(const Document& doc, const std::vector<ResolvedEntity>& prefabs, const BrushRef& target);
```
Implement by filtering inside `buildTargets` (do not copy its body). Keep `collectSurfaces` as a call to
`collectSurfacesFor` with "accept all". Test in `core/tests/test_derived.cpp`: for `core/assets/maps/demo.map.json` and a
generated map with subtractive brushes, the union of `collectSurfacesFor` over single brushes equals `collectSurfaces`
(compare sorted by key + vertices). Existing derived / BSP tests must still pass.

The native side must **not** build a `core::Document` for every edit (copying 25 MB). Keep one `core::Document` per
`DocumentVersion`, built lazily and **structurally shared** where the Core types allow; if the Core's `Document` needs a
deep copy, keep a single mutable `core::Document` owned by the scene builder and patch the changed entities / brushes into
it from the ChangeSet before each build (it is only read by the builder's jobs, which run after the patch). Document which
one was chosen in `scene/README.md`.

## 4. The builder (`scene/builder.h`)

Runs on the UI thread's request, works in jobs:

1. **Receive** `(DocumentVersion, ChangeSet)`.
2. **Expand dependencies:** changed brushes → plus brushes carved by changed subtractors (old and new boxes) → plus decals
   whose box touches changed surfaces → plus faces whose relief depends on a changed neighbour (convex-corner rule).
3. **Schedule** one job per affected brush (priority: brushes visible in any viewport first, then by distance to the active
   camera). Each job computes carved pieces → texture layout → relief → render mesh; checks the version counter between
   steps and stops if its brush changed again.
4. **Lighting products** (06_LIGHTING.md §3–§4) run after the surfaces of their chart group are done.
5. **Publish** a `SceneSnapshot` when (a) all jobs of the version finished, or (b) 8 ms passed since the last publish and
   some finished (partial publish: finished objects take their new products, the others keep their previous ones; the
   renderer draws raw authored meshes for objects that have no product yet).

## 5. Snapshot (`scene/snapshot.h`)

```cpp
struct ObjectProducts {                                  // per brush (and per point entity with a model)
    std::shared_ptr<const RenderMesh> render;            // null = not built yet: draw the raw brush
    std::shared_ptr<const std::vector<core::LightSurface>> surfaces;
    core::Aabb bounds;
    uint64_t inputsHash;
};
struct SceneSnapshot {
    uint64_t version;                                    // the DocumentVersion it was built from
    std::shared_ptr<const nat::DocumentVersion> doc;
    std::unordered_map<ObjectKey, ObjectProducts> objects;
    std::shared_ptr<const LightingInputs> lighting;      // lights, solids, classes, settings (06_LIGHTING.md §2)
    ChangeSet sincePrevious;                             // merged change set since the previous published snapshot
};
```
Published by swapping an `std::atomic<std::shared_ptr<const SceneSnapshot>>`. Render and bake threads read it at the start
of their frame / step.

## 6. Points to check while implementing (ask the user if unclear)

- **Where the export gets relief today.** DESIGN §6.11 says the export takes the relief the Shell last sent
  (`Router::lastRelief_`), while `core/src/texplan_relief.cpp` is a C++ port of the Shell's relief code. In the native app
  there is no Shell: the relief must come from the Core's texplan functions for both the live view and the export. Task T3.3
  compares the Core port's output with the Shell's (`shell/tests/reliefApply.test.ts`, `reliefBake.test.ts`, the shared
  vectors in `testdata/texplan_vectors.json`) before relying on it.
- **Terrain** (`shell/src/model/terrain.ts`, DESIGN §8.k) is a `func_detail` entity with many small brushes; it is ordinary
  brush data for the builder.
- **Models** (`misc_model`, `core/include/core/obj_model.h`; `.glb` previews in `shell/src/model/gltf.ts`): `.obj` models
  are surfaces for lighting (Core already reads them); `.glb` are preview-only. Both are per-entity products.

## As built (T3.2)

`native/src/scene/` (see its README). Differences from the sketch above: `ObjectKey` = (instance string, entity id, brush id) so prefab brushes
are objects too; products are the carved `LightSurface` pieces, bounds and inputs hash (render meshes from T3.4, relief T3.3, lighting later);
the snapshot is swapped under a mutex (`shared_ptr` copy), not `std::atomic<shared_ptr>`; `complete=false` marks a partial publish. The affected set
is computed against the last *complete* snapshot, so superseded builds never lose a change. Jobs use no priority by distance yet (one level).
Decal and relief-neighbour dependencies join the affected set when those products exist.

## As built (T3.3)

Relief is a second builder wave (`scene/relief`, see its README section): it needs the surfaces of the touching objects, so it starts when all surface jobs of a build are done. `ObjectProducts::relief` is a list of `core::ReliefMesh` (per authored face side) with `reliefHash`; null until a `ReliefEnv` (texture infos + groups, from T3.4's texture lookup) is set. The Core's `texplan` port equals the Shell's current relief output (vectors regenerated, see 08 T3.3); the Core export keeps its own inline copy of the same loop in `bsp_export.cpp` (not refactored).
