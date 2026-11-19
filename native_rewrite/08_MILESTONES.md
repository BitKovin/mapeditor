# 08 — Milestones and tasks

Work strictly in order. Each task lists **Read** (what to read before starting), **Do**, **Accept** (checks that must pass;
the task is not done otherwise) and, where useful, **Not here** (things to leave for later tasks). Commit per task:
`native: T<id> <summary>`. Tick the status table at the end in the same commit.

Performance numbers refer to the development machine (RTX 4070 Ti, 1440p) and lvl2
(`F:\Git\SdlWasmEngine\source\Games\ShooterGame\GameData\maps\lvl2.map.json`, assets root = its `GameData` folder).

---

## M0 — Setup

### T0.1 Prerequisites
- **Do:** check `QT_DIR`, `VULKAN_SDK`, Visual Studio (02_BUILD_AND_TOOLING.md §1). If Qt is missing, **ask the user** to
  install Qt 6.8 LTS or newer (MSVC 2022 64-bit) and to set `QT_DIR`; wait. Do not install anything yourself.
- **Accept:** `"%QT_DIR%\bin\qmake.exe" -query QT_VERSION` prints ≥ 6.8; `"%VULKAN_SDK%\Bin\glslangValidator.exe" --version` works.

### T0.2 Newer Vulkan headers in the Core
- **Read:** 02 §3.
- **Do:** replace `core/vendored/vulkan/vulkan/*` and `core/vendored/vulkan/vk_video/*` with the SDK 1.4.363 headers; update
  `core/vendored/README.md` (version line).
- **Accept:** `build_core.bat Release notests` builds; `core_tests.exe -tc="*gpu*,*vis*,*raytrace*"` passes (GPU cases run on
  the dev machine); `bsp_export_tool` on `core/assets/maps/demo_lit.map.json` gives the same files as before (09 §2).

### T0.3 Third-party sources
- **Do:** add Qt ADS (latest 4.x release), VMA (latest 3.x), volk (matching SDK 1.4.363) under `native/third_party/` with
  licences and `native/third_party/README.md`.
- **Accept:** files present, licences present, README lists name / version / licence / URL / purpose.

### T0.4 Skeleton build
- **Do:** `native/CMakeLists.txt` (02 §4), `build_native.bat`, `run_native.bat`; `app/main.cpp` showing an empty
  `QMainWindow` titled "Map Editor (native)"; `native_tests` with one passing test; module libraries created empty with
  their header comments.
- **Accept:** `build_native.bat` succeeds from a clean `native/build`; the window opens and closes cleanly;
  `native_tests.exe` passes.

---

## M1 — Window, device, viewports

### T1.1 Base utilities
- **Do:** `base/log.h`, `base/jobs.h` (01 §2: priorities, cancellation by version), `base/hash.h` (64-bit content hash,
  xxhash-style, of bytes and of `nlohmann::json` canonical dump), `base/timing.h`, `--trace` Chrome trace writer.
- **Accept:** tests: jobs run in priority order, cancelled jobs do not run their body, hash of equal JSON values is equal
  regardless of key insertion order.

### T1.2 Vulkan device and caps
- **Read:** 05 §2, 01 §5.
- **Do:** `gpu/device.*` with volk + VMA; extension table of 05 §2 with `DeviceCaps`; graphics + compute queues with global
  priority; `--validation`; a **Stats** dock panel listing device name, driver, API version and every `DeviceCaps` flag.
- **Accept:** on the dev machine every row of the 05 §2 table is "on"; with `--validation` the start-up log has 0
  validation errors; a test `[gpu]` creates and destroys the device 3 times without leaks (VMA stats 0 at the end).

### T1.3 Viewport window and render thread
- **Read:** 05 §3.
- **Do:** `render/viewport_window.*` (QWindow + Vulkan surface via `QVulkanInstance::setVkInstance`), render thread,
  swapchain per viewport, resize handling, redraw-on-dirty policy, clear colour + a test triangle.
- **Accept:** a viewport in a dock shows the triangle; resizing / undocking / redocking works without validation errors;
  GPU usage is ~0 % when nothing changes (Task Manager / `nvidia-smi`).

### T1.4 Docking layout and cameras
- **Read:** `shell/src/render/camera.ts`, DESIGN §6.1, §17, REQUIREMENTS §2, §35.
- **Do:** Qt ADS main window; layout presets 1 / 2 / 4 viewports (perspective, top, front, side); camera controls with the
  Shell's bindings (fly, orbit, pan, zoom; ortho pan / zoom); per-viewport settings struct; layout saved in QSettings.
- **Accept:** port of camera behaviour checked by hand against the Shell (same keys / mouse do the same); layout survives a
  restart.

### T1.5 Upload ring and arenas
- **Do:** `gpu/upload_ring.*` (64 MB per frame in flight, host-visible, sub-allocation, retire by timeline value),
  `gpu/arena.*` (VMA virtual blocks over big device buffers).
- **Accept:** test `[gpu]`: 10 000 random allocate / free / upload cycles, data read back equal, no leaks.

### T1.6 Action registry and input router
- **Read:** 10_UI_AND_INPUT.md §1–§4.
- **Do:** `app/actions/registry.*` (ActionDef, kinds, parameterised actions), `app/actions/contexts.*` (the context stack
  of 10 §3), `app/actions/router.*` (resolution most specific first, text fields first, sequences, hold, double tap,
  mouse and wheel chords, logical / physical keys, non-Latin layouts matched by US key position), keymap layers (preset +
  user diff, 10 §4.2) loaded from JSON, the `default` preset with the bindings that exist so far (file, edit, camera,
  layout). Menus and toolbars are built from the registry. From this task on, **every** new command is an action; no
  widget or tool handles a key itself.
- **Accept:** tests: the same input resolves to different actions in different contexts; a more specific context
  shadows a general one; a user diff adds / removes bindings over the preset; `Ctrl+Z` works with a Cyrillic layout
  active (simulated scan code); sequences time out after 1 s; a focused text field receives letters while Ctrl chords still
  reach the router.

---

## M2 — Document

### T2.1 Per-node parsing in the Core
- **Read:** 03 §2, `core/src/document.cpp`.
- **Do:** if `documentFromJson` cannot parse one entity / brush / instance on its own, expose `entityFromJson`,
  `instanceFromJson` next to the existing `brushFromJson` (no behaviour change).
- **Accept:** `core_tests -tc="*document*"` passes.

### T2.2 Load and save with round trip
- **Read:** 03 §1–§3.
- **Do:** `doc/document.*`, `doc/io.*` (parallel parse, save = dump(2) + "\n", atomic rename), File > Open / Save / Save As
  (QFileDialog in the assets root), recent files, assets root setting.
- **Accept:** round-trip test (03 §3) byte-identical for every map in `core/assets/maps`, `core/assets/prefabs` and lvl2;
  lvl2 loads in < 3 s to a `DocumentVersion` (log timing).

### T2.3 JSON writers
- **Read:** `shell/src/model/document.ts` (serialization), `core/src/document.cpp` (reader).
- **Do:** `doc/json_write.*`: `writeBrush`, `writeEntity`, `writeInstance` (03 §2 rule: patch only changed keys into a copy
  of the old raw).
- **Accept:** for every brush / entity / instance of the test maps: `write(read(raw), raw) == raw`; after changing one
  vertex the output differs from the input exactly at that vertex.

### T2.4 Commands, history, change sets
- **Read:** 03 §5–§6.
- **Do:** `doc/commands.*`, `doc/changeset.*`; Edit > Undo / Redo with labels.
- **Accept:** tests: diff of identical versions is empty; one changed brush → one `ChangedBrush` with correct boxes; diff
  cost on lvl2 with one change < 1 ms (timed in test); undo / redo restores pointer-identical versions; interaction merging.

### T2.5 Selection
- **Read:** 03 §8, `shell/src/state/store.ts` (selection parts).
- **Do:** `doc/selection.*`, modes object / face / mesh, signals to UI.
- **Accept:** tests from `shell/tests/store.test.ts` selection cases, converted.

### T2.6 Prefabs
- **Read:** 03 §7, DESIGN §5.4, §18.1.
- **Do:** prefab file cache on the IO thread, `core::resolvePrefabs` per instance, file watcher → ChangeSet `Prefab`.
- **Accept:** converted `shell/tests/expand.test.ts` pass through the Core resolver; editing a prefab file on disk updates
  an open map within 1 s.

---

## M3 — Scene and the Unlit view

### T3.1 `collectSurfacesFor` in the Core
- **Read:** 04 §3.
- **Accept:** as listed in 04 §3; `core_tests -tc="*derived*,*csg*,*bsp*"` pass (bsp tests that are slow may be filtered to
  the derived / surface ones — list which ran in the commit message).

### T3.2 Scene builder, caches, snapshots
- **Read:** 04 §2, §4, §5.
- **Do:** `scene/cache.*`, `scene/builder.*`, `scene/snapshot.*`; dependency expansion (carvers, decals, relief neighbours).
- **Accept:** tests: moving a subtractive brush rebuilds exactly the brushes it overlaps (old and new box); undo after a
  move hits the cache (no recompute, counted); building all of lvl2 from scratch < 5 s on 16 threads (log).

### T3.3 Relief from the Core
- **Read:** 04 §6.
- **Do:** relief meshes through `texplan.h`; compare with the Shell's vectors (`testdata/texplan_vectors.json`,
  `shell/tests/reliefApply.test.ts`, `reliefBake.test.ts`, `reliefWalls.test.ts`).
- **Accept:** converted vectors pass; if any differ, **stop and report** which (the user decides whether the Core port or
  the Shell is right).

### T3.4 Render meshes, textures, Unlit
- **Read:** 05 §4, §5 (Unlit, sky), `shell/src/render/renderer.ts` (Unlit shaders), `core/include/core/textures.h`.
- **Do:** `scene/render_mesh.*`, `render/gpu_scene.*` (arenas, indirect draw, culling), bindless textures loaded through the
  Core's texture lookup, Unlit mode, sky faces, subtractive brushes translucent, raw-brush fallback while products are
  pending.
- **Accept:** lvl2 opens to a first Unlit frame in < 10 s; 4 viewports at ≥ 60 fps while orbiting; screenshots of
  `core/assets/maps/demo.map.json` and `sky_test.map.json` side by side with the Shell's Unlit view look the same (save
  both shots in a scratch folder, show them to the user and ask them to confirm; do not commit screenshots).

### T3.5 Picking and selection display
- **Read:** 05 §6.
- **Do:** CPU picking, click / shift-click / box select (Radiant conventions, REQUIREMENTS §3), outlines.
- **Accept:** picking test on generated boxes; picking on lvl2 < 2 ms per click (log).

### T3.6 Grid, ortho views, overlays part 1
- **Do:** grid (`orthoGrid.ts`), grid size keys, entity boxes and icons, light gizmos, size labels, text atlas.
- **Accept:** converted `sizeLabels`, `pointBox`, `arrows` tests; visual check against the Shell on demo.map.json.

---

## M4 — Basic editing

### T4.1 Transform tools
- **Read:** `model/transform.ts`, `tools/modal.ts`, `render/gizmo.ts`, DESIGN §8, §17.
- **Do:** gizmo, G / R / S modal operators, typed values, axis constraints, grid snapping; each as Commands with
  interaction merging.
- **Accept:** converted `transform.test.ts`, `modal.test.ts`, `originGizmo.test.ts`; dragging a brush on lvl2: the brush
  follows the mouse every frame (frame time ≤ 16.7 ms while dragging, Unlit).

### T4.2 Box tool and shapes
- **Accept:** converted `tools.test.ts` (box parts), `batch33.test.ts` shape cases.

### T4.3 Delete, duplicate, copy, paste, group, hide
- **Accept:** converted `clipboard.test.ts`, `hideRules.test.ts`; copy in the Shell → paste in native works and vice versa.

### T4.4 Entities: place, details, typed properties
- **Read:** `ui/properties.ts`, `ui/placePanel.ts`, `core/include/core/entdef.h`, REQUIREMENTS §12 (lights).
- **Do:** Place panel (entities, lights, prefabs), Details panel with typed editors from the .ent, Outliner (search,
  fold, eye, reveal).
- **Accept:** converted `outlinerSearch`, `outlinerFold`, `outlinerEye`, `outlinerReveal`, `detailsRefs`, `lights` tests.

### T4.5 Status bar, settings dialog (basic)
- **Do:** assets root, game profile, editor prefs pages (others come with their features).
- **Accept:** settings persist; changing the assets root reloads profile and entity definitions.

### T4.6 Binding editor and keymap presets
- **Read:** 10 §4–§5.
- **Do:** Settings > Keymap (table, filters, record, conflict dialog, reverse lookup, reset, import / export, several user
  keymaps), the `default` preset completed with every binding of DESIGN §17, REQUIREMENTS (texture keys, nudge, selection
  keys) and `shell/src/ui/bindings.ts` that exists so far. Ask the user whether to ship the `blender`, `hammer`,
  `radiant`, `unreal` presets now or later.
- **Accept:** every action registered so far can be rebound, including camera navigation and modal operator keys;
  rebinding applies without restart; the conflicts filter is empty for the `default` preset; a keymap exported and
  imported on a clean profile gives the same effective keymap.

### T4.7 Command palette, hints, key help
- **Do:** command palette (10 §6), bindings in menus and tooltips, status-bar hints of the active tool / modal operator,
  the key help overlay.
- **Accept:** every enabled action can be found and run from the palette; the hints change when the binding changes.

**M4 gate:** block out a small level from scratch (boxes, a light, a player start), save, reopen in the old Shell: it
opens and looks the same.

---

## M5 — Core refactors for live lighting

### T5.1 Lighting inputs without `Scene`
- **Read:** 06 §2, `core/src/scene.cpp` (`lightScene`, `lightSolids`, `lightSurfaces`, albedo / sky helpers).
- **Do:** move the bodies of those member functions into free functions in `core/` taking a `core::Document`, resolved
  prefabs, the profile and the assets root; `Scene` calls them.
- **Accept:** `core_tests -tc="*scene*,*lighting_service*"` pass; export parity (09 §2).

### T5.2 Staged lightmap layout
- **Read:** 06 §3.1–§3.2.
- **Accept:** as listed in 06 §3.2.

### T5.3 Per-chart texel samples
- **Read:** 06 §4.
- **Accept:** test: for demo_lit and lvl2, concatenating `buildChartSamples` over all charts equals `buildTexelSamples`
  (positions, normals, sub-texel data, texel indices); `core_tests -tc="*texel*"` pass.

---

## M6 — Live direct light (light edits instant)

### T6.1 Live layout (full) and page allocator
- **Read:** 06 §3.4.
- **Do:** `light/layout.*` running the staged layout over the whole level on load (incremental comes in M8),
  `light/page_allocator.*`, display page images, `chartMap` / chunk records.
- **Accept:** chart shapes equal the Core's `buildLightmapLayout` for lvl2 (compare); allocator tests (alloc / free /
  merge / grow).

### T6.2 Sample arena and chunks
- **Do:** per-chart samples uploaded (06 §4), chunk table CPU + GPU.
- **Accept:** read-back of the arena equals the Core's samples for demo_lit.

### T6.3 Acceleration structures per brush
- **Read:** 06 §5.
- **Do:** `light/accel.*`: BLAS per brush / model / relief set, object records, TLAS; transparency as the Core (OMM comes
  in M8).
- **Accept:** test `[gpu]`: closest-hit results of 100 000 random rays against the native TLAS equal `core::TriangleBvh`
  (`closestHit`) on demo_lit within 1e-4 m (hit / miss identical).

### T6.4 Direct and AO passes, accumulation, compose, mailbox, Lit view
- **Read:** 06 §6, §8, §10; `core/shaders/lightmap_direct.comp`, `lightmap_ao.comp`; `LiveBaker::refineOnGpu`, `renderTile`.
- **Do:** `direct.comp` (three classes), `ao.comp`, `accumulate.comp` (direct only for now), `compose.comp`, mailbox +
  render-side scatter, Lit and Lighting-only modes (bicubic sampling, exposure, styles off for now).
- **Accept:** parity (06 §1) of direct light + AO after `maxPasses` against `core::LiveBaker` with bounces off on
  `demo_lit.map.json` and a generated room set (`bench_bake --rooms 6` scene); no validation errors; Lit view of demo_lit
  looks like the Shell's Lit view with bounces off (user confirms screenshots).

### T6.5 Scheduler, visibility feedback, budget
- **Read:** 06 §9.
- **Do:** `visibleChunks` from the Lit shader, priorities, Frostbite budget controller, interaction detection, background
  setting, "Show stale chunks" overlay.
  Also the benchmark mode `--bench <scenario>` of 09 §3 (scenarios `open`, `orbit`, `light_drag`, `sun_turn`; `brush_move` and
  `undo` are added in T8.3).
- **Accept:** perf gate **G-light** (09 §3) for direct light; frame time stays ≤ 16.7 ms while baking with the mouse held
  down.

### T6.6 Light editing invalidation (direct)
- **Read:** 06 §7 (light rows).
- **Do:** expose the Core's `influence(const Light&)` (now in an anonymous namespace in `core/src/live_bake.cpp`) in
  `live_bake.h` without changing it; implement the light rows of 06 §7.
- **Accept:** test: after moving a point light, every buffer value of samples outside old ∪ new influence is bit-identical
  to before; G-light passes on lvl2 for point, spot, rect and directional lights.

---

## M7 — Bounces, denoise, light styles

### T7.1 Gather (compute fallback)
- **Do:** straight port of `lightmap_gather.comp` to the new layout; `I`, `S` update in `accumulate.comp`; fine GI grid.
- **Accept:** parity with bounces on (06 §1) against `core::LiveBaker` on demo_lit and the room scene.

### T7.2 Gather on the ray tracing pipeline with SER
- **Do:** `gather.rgen/rmiss/rchit`, SBT, SER path and non-SER path; choose at start-up from `DeviceCaps`.
- **Accept:** same results as T7.1 for a fixed schedule (bit-identical or within 1e-5 relative); GPU time of the gather on
  lvl2 visible chunks measured for: compute, RT without SER, RT with SER — written to the commit message. Keep the
  fastest as default.

### T7.3 Preview denoise
- **Do:** `atrous.comp` on composed chunks (06 §12).
- **Accept:** no light bleeding across charts (test: two charts with 0 and 10 light side by side stay 0 and 10); user
  confirms the look.

### T7.4 OIDN on converged pages
- **Do:** as 06 §12, with external memory when available.
- **Accept:** converged lvl2 view equals the Core's denoised live result in look (user confirms); denoise never blocks the
  bake thread (timed).

### T7.5 Light styles
- **Do:** per-style layers (06 §6), style patterns animated in the renderer (`light_styles.h`, `lightStyles.ts`).
- **Accept:** converted `lightStyles.test.ts`; a styled light flickers in Lit as in the engine.

### T7.6 Full invalidation table
- **Do:** every row of 06 §7.
- **Accept:** one test per row (synthetic scene): only the expected classes / regions change; S zeroed where required;
  no displayed texel goes to 0 during a re-bake (test samples the display between steps).

**M7 gate:** G-light including bounces (09 §3).

---

## M8 — Brush edits in Lit

### T8.1 Incremental re-layout
- **Read:** 06 §3.3.
- **Accept:** property test: 200 random edits (move / resize / delete / add brushes) on demo_lit; after each, the live chart
  groups and shapes equal a full `buildLightmapLayout` of the same document (placement ignored).

### T8.2 Chart moves and repack
- **Do:** chart offset table, `chartId` vertex attribute, repack with GPU copies, mailbox carries `LayoutVersion`.
- **Accept:** after a repack the display is pixel-identical (screenshot diff) and no texel was re-baked (counter).

### T8.3 Geometry invalidation and BLAS policy
- **Do:** geometry rows of 06 §7; fast-build on edit, fast-trace rebuild after 2 s idle, compaction.
- **Accept:** perf gate **G-brush** (09 §3).

### T8.4 Cutouts with opacity micromaps
- **Do:** OMM per cutout texture (06 §5).
- **Accept:** shadows through a grate texture equal the non-OMM path (texel diff within noise); gather time on a foliage
  test map reported with and without OMM.

### T8.5 Relief in the live bake
- **Accept:** a relief wall's lightmap matches the Core's export bake of the same map (look, user confirms); editing the
  relief's texture properties re-bakes only faces using it.

---

## M9 — Export in-process, one GPU device, automation

### T9.1 Typed export input (Core)
- **Why:** one program, no serialization between its parts (README decision "One program").
- **Read:** `core/include/core/bsp_export.h` (`BspExportRequest` takes the document as JSON), `core/src/bsp_export.cpp`
  (`prepareExport`).
- **Do:** add a request form that takes a typed `std::shared_ptr<const core::Document>`, the resolved prefabs and the relief
  meshes; the JSON form parses and calls it. No behaviour change.
- **Accept:** export parity (09 §2) through both forms; `core_tests -tc="*export*"` passes.

### T9.2 The Core's GPU code on the app's device (Core)
- **Read:** `core/src/gpu_bake.cpp` (`Context`, `makeContext`, `contextFor`), `core/src/gpu_bake.h`.
- **Do:** `core::gpu::useExternalDevice(...)` (instance, physical device, device, a compute queue + family, the loader's
  `vkGetInstanceProcAddr`): from then on `contextFor` wraps those handles without owning them; pipelines and pools are
  created on that device. Without the call the Core behaves as today (its own tools and tests).
- **Accept:** a native test `[gpu]` runs the Core's GPU tests' scenes through the injected device with the same results;
  only one `VkDevice` exists in the native process (log at start-up and during an export).

### T9.3 Export, play map, import
- **Read:** 06 §13, `core/exporter.h`, `bsp_export.h`, `map_import.h`, REQUIREMENTS §25–§31, §38.
- **Accept:** exporting lvl2 from native gives the same files as `bsp_export_tool` on the saved map (09 §2); progress and
  cancel work; Play map launches the game set in settings; File > Import Quake Map works on `core/assets` samples.

### T9.4 Automation socket (for outside scripts only)
- **Why:** this is not a split of the editor (it is off by default and the editor never uses it itself). The `level-blockout` skill (`.claude/skills/level-blockout/SKILL.md`) drives the editor through Core + Shell
  today (open a map, set the camera, take shots). The native app must offer the same without a browser.
- **Do:** `automation/server.*` (QTcpServer on 127.0.0.1 only, `--automation-port`), newline-delimited JSON:
  `open {path}`, `save {path?}`, `get_document`, `set_document {doc}` (validated, one undo step), `camera {viewport, eye,
  target, fov?}`, `settings {viewport, key, value}`, `screenshot {viewport, path, width?, height?}` (renders off-screen at that
  size, waits until visible chunks converge or `timeoutMs`), `stats`, `lighting {pause|resume|wait_converged}`.
- **Accept:** a Python script in `native/tests/automation_smoke.py` opens demo.map.json, takes two shots, reads stats.
  Updating the skill's SKILL.md to use it is a **separate request to the user** (ask; do not edit the skill unasked).

---

## M10 — Light volume

### T10.1 Probe residency and bricks
### T10.2 Probe passes and fit (GPU port of `fitProbeDirectFirst`, or CPU per brick first)
### T10.3 Entity lighting in Lit, light volume overlay
- **Read:** 06 §11, DESIGN §6.7, §6.10.
- **Accept (whole milestone):** probe values after convergence match the Core's `ProbeBaker` on demo_lit (per probe:
  ambient / directed within 3 %, direction within 5°); lvl2's full grid (18 M probes, no cap) works without stalling the
  editor; converted `lightvolSample.test.ts`, `lightvolPreview.test.ts`; doors / lifts lit as in the Shell.

---

## M11 — The rest of the editor

Port groups B and C of 07_EDITOR_PORT.md in this order (each item is a task `T11.<n>`, accepted when its converted Shell
tests pass and the user has tried it):
1. Mesh mode: topology, select (loop / ring / path), ops, cut, bevel, bridge, UV lock.
2. Brush operations: clipper, hollow, merge, face drag (Alt), 2D resize, select inside / touching.
3. Texturing: Surface Inspector, texture browser / content browser, Fast Texture Tool, flow, hotspots, texel density view,
   lightmap UV view, copy surface.
4. Texture properties window, texture groups editor, tile set editor.
5. Relief editor.
6. Parametric shapes.
7. Prefabs authoring: prefabize, instance overrides, references, prefab previews.
8. Terrain.
9. Decals, models (`misc_model`, .glb previews).
10. Level settings panel, full settings dialog (lighting config), hide menu, context menus, remaining key bindings.
11. UI improvements marked **[plan]** in 10_UI_AND_INPUT.md §7 (U1 adjust last operation, U2 tool options, U3 undo
    history, U5 snapping menu, U6 view cube, U7 camera bookmarks, U8 multi-object details, U9 selection tools, U11 texture
    browser, U12 lighting panel, U13 measure, U14 notifications, U15 theme and scale). Items marked **[ask]**: ask the user
    first.

---

## M12 — Optional performance extensions (only if a gate in 09 §3 fails after M8, or the user asks)

- `VK_NV_partitioned_acceleration_structure` for the TLAS (maps with > 50 000 instances).
- `VK_NV_cluster_acceleration_structure` for relief / terrain BLAS rebuilds.
- GPU texel-sample rasterization (conservative rasterization in lightmap space) — must equal T5.3's output exactly.
- `VK_EXT_descriptor_heap` binding model.
Each needs a measurement before and after in the commit message.

---

## M13 — Cut-over

- Feature checklist: every ✅ item of REQUIREMENTS.md is either working in native (with where) or listed as "not ported"
  with the user's agreement.
- Packaging: `build_native.bat Release` + `windeployqt` into `release/native/` (like `build_app.bat` does for Electron).
- The user decides when the Shell is retired.

---

## Status

| Task | Done | Commit |
|---|---|---|
| T0.1 | yes (Qt 6.11.1 at `D:\Qt.11.1\msvc2022_64`, found by `build_native.bat` when `QT_DIR` is unset) | with T0.2 |
| T0.2 | yes (demo_lit / demo / engine_test `--no-bake` exports byte-identical; 40 `*gpu*,*vis*,*raytrace*` cases pass) | `native: T0.2` |
| T0.3 | yes (Qt ADS 5.1.1, VMA 3.5.0, volk 1.4.363) | `native: T0.3` |
| T0.4 | yes (clean build, 6 tests, `--screenshot` of the empty window exits 0) | `native: T0.4` |
| T1.1 | yes (20 tests: priority order, cancellation, throwing jobs, xxHash64 vectors, canonical JSON hash, log, trace JSON validity) | `native: T1.1` |
| T1.2 | yes (every 05 §2 row on except graphics HIGH priority, which the driver does not list for this family; 0 validation errors; 3 create/destroy cycles leave 0 VMA allocations; Stats panel checked in a screenshot) | `native: T1.2` |
| T1.3 | yes (11 `[gpu]` cases incl. pixel checks of clear colour + triangle, idle = 0 frames, dirty = exactly 1 frame, resize, 2 viewports, re-add on one window; Qt `native_gui_tests`: dock shows triangle, window resize, 3x float / re-dock, hide / show; 0 validation errors; app screenshot checked) | `native: T1.3` |
| T1.4 | yes (headless: 10 camera / settings / rig cases - fly, orbit, ortho maths ported from camera.ts, ray through a picked point, JSON round trips, Shell input rules; `[gpu]`: grid, axes and box land where the maths says in perspective and top views; `native_gui_tests`: ADS docks, float / re-dock, presets 1 / 2 / 4 keep cameras, real mouse / key / wheel events drive the cameras, layout and cameras survive a restart; 0 validation errors; screenshots of 1 / 2 / 4 viewports checked; the test triangle is replaced by a grid + axes + 1 m box scene) | `native: T1.4` |
| T1.5 | yes (`[gpu]`: ring slices / alignment / exhaustion / retire by timeline value; arena alloc / free / alignment / full / reuse; 10 000 random allocate-free-upload operations on a 1.5 MB arena that fills up 149 times, whole arena read back and every live block compared 11 times, ring slices reused while batches are in flight; 0 validation errors, 0 leaked VMA allocations) | `native: T1.5` |
| T1.6 | yes (headless: 12 action cases - chord grammar round trips, context resolution most specific first, disabled specific falls back to general, user diff adds / removes / replaces, `Ctrl+Q` with a Cyrillic layout via scan code, sequences time out after 1 s, double tap, holds with hold contexts, text-field rule, mouse and wheel chords, conflicts; adapter cases for Qt key / mouse / wheel events, scan table, text widgets; the shipped `default` preset names real actions in known contexts with no conflicts; `native_gui_tests`: menus built from the registry with the keymap's keys and enabled / checked state, layout and camera entries act, Ctrl+Q exits, user keymap rebinds / is saved and read back / a broken one is reported, a focused text field gets letters while Ctrl chords reach the router, Shift boost and look-release end flight; the cameras are driven only through the router) | `native: T1.6` |
| T2.1 | yes (`core::entityFromJson` and `core::instanceFromJson` exposed next to `brushFromJson`; `documentFromJson` calls them, no behaviour change; new core test: one-node parse equals the whole-document parse, entity without `brushes`, strictness; `core_tests -tc="*document*,*prefab*,*override*,*carve*,*refs*"` 63 cases pass) | `native: T2.1` |
| T2.2 | | |
| T2.3 | | |
| T2.4 | | |
| T2.5 | | |
| T2.6 | | |
| T3.1 | | |
| T3.2 | | |
| T3.3 | | |
| T3.4 | | |
| T3.5 | | |
| T3.6 | | |
| T4.1 | | |
| T4.2 | | |
| T4.3 | | |
| T4.4 | | |
| T4.5 | | |
| T4.6 | | |
| T4.7 | | |
| T5.1 | | |
| T5.2 | | |
| T5.3 | | |
| T6.1 | | |
| T6.2 | | |
| T6.3 | | |
| T6.4 | | |
| T6.5 | | |
| T6.6 | | |
| T7.1 | | |
| T7.2 | | |
| T7.3 | | |
| T7.4 | | |
| T7.5 | | |
| T7.6 | | |
| T8.1 | | |
| T8.2 | | |
| T8.3 | | |
| T8.4 | | |
| T8.5 | | |
| T9.1 | | |
| T9.2 | | |
| T9.3 | | |
| T9.4 | | |
| T10.1–T10.3 | | |
| M11 (T11.1–T11.11) | | |
| M12 | | |
| M13 | | |
