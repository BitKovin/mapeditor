# scene

Turns document versions into scene snapshots (T3.2). Spec: `native_rewrite/04_SCENE_AND_DERIVED.md`.

- `snapshot.h` — `ObjectKey` (instance string, entity id, brush id), `ObjectProducts` (carved `core::LightSurface` pieces, bounds, inputs hash;
  `render` joins in T3.4), `SceneSnapshot` (immutable; unrebuilt objects share products with the previous snapshot).
- `cache.h` — `DerivedCache<Key, Value>`: products stored under (key, inputsHash), LRU by bytes (2 GB default), `retain()` drops brushes that left.
- `builder.h/.cpp` — `SceneBuilder`. `submit(version, changes, prefabs)`:
  1. Index every brush of the version and its prefab expansion (box, solid/cutter, content hash, class).
  2. Affected set, relative to the last *complete* snapshot: changed brushes; all brushes of entities whose class changed; all brushes of prefab
     instances whose expansion changed; and every solid brush overlapping the old or new box of a changed subtractive brush.
  3. One pool job per affected brush: inputs hash = brush hash + class + sorted hashes of the subtractors `core::carversOf` reports; cache hit
     -> no work, else `core::collectSurfacesFor` for that brush alone.
  4. Publish when all jobs are done; partial publishes every >= 8 ms while they finish (finished objects new, the rest from the last complete one).
- The builder owns one `core::Document` that is patched per change set (entity replaced from the version's nodes; rebuilt when an entity came
  or went). Jobs read it; it is only patched while no job runs. A submit while jobs run cancels them (generation counter), merges its changes into
  `pending_` and the last finishing job starts the next build.
- Not yet derived here (later tasks add their inputs to the affected set): relief meshes (T3.3), render meshes (T3.4), decals.
