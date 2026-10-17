

---
# HANDOVER - state at the end of this session (start the next chat from here)
**Last milestone zip: `mapeditor-M4_3-e2e-ortho.zip` (previous: M4_2-clipdrag, M4_1-resize2d, M4_0-ortho, M3_7-merge, M3_6-planedrag, M3_5-selkeys). All tests green at M4_3: Core 106 (doctest, GCC build), Shell 284 (vitest), browser e2e 139 PASS lines.**

## Done this session (all in the zip, all with tests)
- N1 prefabs fully working (a-f): instance names + derived defaults, entity `refs`, structured overrides, Core `flattenForCompile`, shared transform walker (angles/scale/overrides), instance selection/transform/Details/overrides, Place > Prefabs (click + drag-drop), reference picker + pipette. Prefab content is drawn opaque.
- Brush mode (Radiant): face-plane drag (Shift+click face, drag normal handle), clipper (X; Enter/Shift+Enter/Ctrl+Enter), make hollow, merge, snap, flip, rotate 90, arrow nudge, rotate + scale gizmos.
- Item 3: separate rotation/scale snap, entity angles + scale in Details, decal projector box + angle arrows, decal size follows instance scale.
- Item 4: convex rule for solids only (Core + Shell), auto func_detail on concave edits (toggle), Make entity / Move to world, right-click context menu.

## Where things are
Core (C++): `core/src/prefab.cpp` (walker, flatten), `document.cpp` (schema, validation), tests in `core/tests`. Shell (TS): `shell/src/model/{expand,refs,brushOps,faceDrag,annotations,rotation}.ts`, `state/actions.ts` (all edit actions), `ui/viewport.ts` (tools, gizmos, context menu), `ui/panels.ts` (Details), `ui/placePanel.ts` (Place + Brush tools), e2e in `shell/e2e/e2e.py`.

## Fixed in M4_3
- `app.ts` kept disposed viewports in its set after a popout round trip, so keys/tools targeted a dead viewport (`liveViewports()` now prunes them).
- An armed clipper now owns LMB regardless of the active tool (the Box tool used to swallow the clicks).
- e2e note: the viewport header overlays the top of the canvas; aim test clicks lower.

## Fixed in M4_4
- **Subtractive brushes inside prefabs** were never drawn carved: `render/renderer.ts` looked carved meshes up with `carvedKey(entity, brush)` and no instance, so prefab content always fell back to the authored geometry (picking already used the instance). `DrawEntity` now carries `instance` (full path, set in `ui/viewport.ts render()`), used for fills and outlines. Core was fine (test_derived has prefab cases).
- **Alt+drag extend face** (brush mode): `extendFace` in `model/faceDrag.ts` builds a prism brush over the selected face (same entity, op and carve scope; end cap = face id 0, base = 1, walls 2..; walls continue the neighbouring face's UV map). Wired in `ui/viewport.ts selectDown/faceDragMove/onUp` (drag kind 'face' with `extend`); on release the new end cap becomes the selected face. Alt+LMB pointerdown is `preventDefault`ed.
- **Concave -> func_detail during a drag** (G/R/S, gizmo): when the brush moved into the new func_detail, `Store.applyDoc` dropped the selection (its entity no longer held the brush), so the next drag update found no mesh target and returned the unchanged start document (convex shape, nothing selected). `applyDoc` now follows a brush to whichever entity holds it (selection and multi-selection; also on undo/redo), and `ViewportPanel.meshDoc` looks the brush up from `dragBrush` (entity + brush at drag start) instead of the live selection.
- Tests: Shell 291 (was 284). Core unchanged, 106 pass (rebuilt with GCC). Browser e2e was NOT run for M4_4 (no Chromium in the sandbox): run `bash shell/e2e/run_e2e.sh` and check Alt+drag, a prefab with a subtractive brush, and a concave vertex drag by hand.

## Open / next
1. **MSVC build is unverified**: none of the C++ since M2.5 was built with MSVC (only GCC on Linux). First thing: run `build_core.bat`, fix any errors.
2. Orthographic views are DONE (`OrthoCamera` in `render/camera.ts`, camera modes top/front/side in the viewport header, adaptive grid in `render/orthoGrid.ts`; RMB/MMB pan, wheel zoom). Side/edge 2D resize handles are DONE (`model/resize2d.ts`; drag handles in `ui/viewport.ts`: `resizeInfo`, `handleHit`, `resizeOverlay`, drag kind 'resize'). Clipper point dragging is DONE (`tools/clipperPoints.ts`; in `ui/viewport.ts`: `clipClick` grabs a point within 10 px, `clipDragMove`; in ortho views points are placed on the plane through the selection centre and dragged in the view plane, depth kept). The ortho views, 2D handles and clipper drag now have browser e2e checks (e2e.py section 'orthographic views'). (Object-mode Ctrl+A / Ctrl+I / Esc selection keys are DONE: `selectAllItems`, `invertSelectionItems` in actions. Keyboard shortcuts only work while the canvas has focus (e2e focuses it explicitly). Select inside/touching are DONE: `model/selectVolume.ts`, `selectByVolume` in actions.) (Texture-shift keys are DONE: `nudgeTexture` in `state/actions.ts`, bound in `ui/viewport.ts onKey`, steps in `Store.texSteps` + Texture panel.)
3. DONE: face drag on convex brushes falls back to `planeDrag` (re-intersects the planes) at corners where 3+ faces meet; concave brushes still refuse there (use vertex mode).
4. DONE: merge compares the hull volume with the exact union volume (`unionVolume`, inclusion-exclusion over convex intersections in `model/brushOps.ts`), so overlapping brushes merge only when the hull adds no new space.
5. Compile pipeline (M6) should call `flattenForCompile`; Shell does not call it yet.
6. e2e flake: "RMB drag looks around without moving the eye" fails now and then in headless runs (timing). Run e2e one at a time (it takes ~100 s); do not run two in parallel.
7. Environment notes: cmake/ninja come from `pip install cmake ninja`; e2e needs `pip install playwright pillow websockets` and `playwright install chromium`; run `bash shell/e2e/run_e2e.sh` (log in /tmp/e2e.log).


## Added after M4_4 (2026-10-03)
- Entity boxes come from the `.ent` `box` (Quake units/axes -> metres, Y-up; `model/entityBox.ts`, DESIGN 4). Entities spawn like prefabs (click or drag), with a half-transparent ghost (`Frame.ghost`) and preview icons (`render/icons.ts`); prefab previews load through the new Core message `read_prefab` (`state/prefabPreview.ts`).
- Popout: floating groups are disabled; any group opens in a real window (header button, or drag a tab out of the browser window). See `popOut()` in `app.ts`.
- REQUIREMENTS.md was re-synced with the code; section 17 holds the open batch (direction arrows, target arrows, prefabs carving the world, alt-drag resize, targetname/target in Details, brush-entity origin).
- Tests: Shell 315 pass, typecheck clean, Core 107 pass. The old e2e (shell/e2e/e2e.py) was not rerun after the popout change; the new behaviour was checked with ad-hoc browser scripts.

## Added after M4_4 (2026-10-03, batch task 3)
- Prefab subtractors now carve the root document (world) around them, per their carve scope; never a sibling prefab. `core/src/derived.cpp`: partition rule; `carved[].subtractorInstances` and `fragments[].instance` say which instance owns each subtractor / cut wall (ids repeat across instances). Shell: `model/derived.ts` looks brushes up per instance, `PickTri.instance`, `render/picking.ts` resolves a cut wall to the prefab's entity. Core 108 pass, Shell 318 pass. Browser e2e not run (no Chromium).
- Note: the door frame's opening also carves its own frame (scope `self`, default) so the frame is a ring; set `carve.self=false` on the opening for an uncut slab.

## Added after M4_4 (2026-10-03, batch tasks 1 and 2)
- Direction arrows + link arrows: `shell/src/model/arrows.ts` (pure; `entityArrows` -> coloured line groups), drawn by `Renderer` via `Frame.overlays`, built in `ViewportPanel.arrowOverlays()`. View menu options `showDirections`, `showLinks` (per viewport). `entityAnnotations` now only does decals.
- Links resolve `refs` like Core's flatten: `ResolvedEntity.refScopes` (new, from `expandPrefabs`) keeps the scope each ref is relative to; plain-name keyvalues (`target: "door01"`) also link by targetname. Selected entities (or content of a selected instance) draw brighter arrows; links touching the selection are always shown.
- Shell 335 pass, typecheck + vite build clean. Browser e2e not run (no Chromium here); look at it by hand: place a point entity, a button with a `target` ref, a door prefab.

## Added after M4_4 (2026-10-03, popout input fix)
- Bug: a viewport in a popout window got no WASD, Shift, Esc, and no app shortcuts (Ctrl+Z, Q, B, Del, F). Cause: page-level key listeners were bound once to the MAIN window's document; a popout is a second Document and events there never reach them. Also the right-click menu and its outside-click/Esc handling lived on the main window.
- Fix: `ViewportPanel.bindDocument()` binds fly/Shift/Esc/View-menu-close listeners to `element.ownerDocument` and re-binds (releasing held fly keys) whenever the pointer or focus reaches the canvas after the element moved; `app.ts` registers the app shortcut handler on every popout document (`hookPopout`); `showMenu(x, y, items, doc)` and `openDialog(..., {doc})` take the target document; the rAF/fly-timer bookkeeping remembers its window (a pending frame of a closed window no longer blocks `invalidate`).
- Tests: `tests/popoutInput.test.ts` (jsdom, second Document). Shell 341 pass. Not checked in a real browser popout (no Chromium here): fly with RMB+WASD in a popout, Ctrl+Z, right-click menu.
- Known gap: other panels' dialogs (file dialog, settings) still open in the main window even if triggered from a popout.


## Added after M4_4 (2026-10-03, lightmapping batch, REQUIREMENTS §18)
- **Step 1 done: lightmap UV generation.** `core/include/core/lightmap_uv.h`, `src/lightmap_uv.cpp`: `buildLightmapLayout(surfaces, LightingClasses, LightmapSettings)` -> faces with affine world->page-texel maps, charts (unfolded islands), MaxRects packing into equal square pages, bucketed by (instance, entity, texture) with Morton order inside a bucket. `collectSurfaces` in `derived.cpp` gives the typed carved surfaces (it shares `buildTargets` with `deriveGeometry`, so the two cannot drift). `Scene::lightmapLayout()` ties it to the open document. `LightmapLayout::toJson()` is the future Shell contract (not wired into the Router yet).
- **Step 1b done:** `lighting_classes.json` (`core/lighting_classes.h`): data-driven lightmapped / castShadows classnames, defaults = structural classes.
- Tests: Core 122 pass (was 108), GCC on Linux. MSVC build unverified.
- **Next: step 2** (compute-shader point light, no RT): see REQUIREMENTS §18 and DESIGN §6.5. The sandbox has no GPU; use `VULKAN_RT_SANDBOX_GUIDE.md` (lavapipe) for GPU-path tests.
- **Steps 2 and 3 done** (point; spot / directional / rect / sky / ambient; CPU reference + Vulkan compute, 137 Core tests incl. GPU-vs-CPU on lavapipe). Visual check: `lightbake_tool <assets> <map> <outDir>`. Lights are summed into one layer: light styles (8 per surface) are later.
- **Step 4 done: ray-traced shadows (no bounces).** `core/raytrace.{h,cpp}` (occluders + BVH), shadow sampling in `lightbake.cpp`, `lightmap_direct_rq` (RAY_QUERY variant of `shaders/lightmap_direct.comp`), acceleration structure + ray query host code in `gpu_bake.cpp`. 148 Core tests pass on Linux (GCC, lavapipe).
- **Windows build changes:** shaders are compiled by CMake (`shaders/manifest.txt`) when `glslangValidator` is found (PATH, `%VULKAN_SDK%\Bin`, `C:\VulkanSDK\*\Bin`); otherwise `src/prebuilt/core_shaders` is used and CMake refuses it when it is out of date. Install the Vulkan SDK (LunarG) to edit shaders on Windows. `tools/compile_shaders.py` refreshes the prebuilt headers (commit them after shader edits).
- Try it: `build\lightbake_tool.exe core\assets maps\demo_lit.map.json out --validate` writes `out\pageN.png` (sRGB debug view) and prints the device used; with the Vulkan validation layer installed it also reports validation messages.
- **Next: step 5 (bounces / GI).** Then the real-time architecture of REQUIREMENTS §18.1: chunked lightmap storage, dirty tracking, progressive refinement, camera-priority scheduling, texture residency.
- **Step 5 done: bounces.** `evaluateGiCpu` (reference), `shaders/lightmap_gather.comp`, ping-pong atlas buffers in `gpu_bake.cpp`; `lightbake_tool ... --bounces 3`. 155 Core tests pass (GCC, lavapipe). `samples/lightbake/` has direct-only and 3-bounce pages of `demo_lit.map.json`.
- **Next (REQUIREMENTS §18.1, real-time goal):** (1) chunked lightmap storage + dirty tracking per face/chart and per light (what does an edit invalidate?), (2) a progressive scheduler: refine samples/bounces over frames, camera-distance priority, directional/sky lights near the camera first, (3) persistent GPU resources (keep the TLAS / BLAS, update instead of rebuild; texel buffers resident, VRAM budget 11 GB, down to 7), (4) texture residency (load only used textures), (5) Shell transport of pages (Lit view) and lightvols.
- **Profile lighting lists:** `game_profile.json` `"lighting": {"blockLight": [...], "receiveBakedLight": [...]}` (worldspawn, func_group, func_detail always included). `lighting_classes.json` no longer exists.
- **User patch applied** (loader messages as notes, mtime fix, non-degenerate gather scene, opt-in diagnostic) and the root cause of the remaining GPU/CPU differences fixed (buried texels are skipped). 167 tests pass, 1 skipped (the diagnostic).


---
## HANDOVER FOR THE NEXT CHAT (written 2026-10-04, end of the lightmapping session)

### Where things stand
Lightmapping goals of REQUIREMENTS §18 are done in Core and first wired into the editor:
1. Lightmap UV generation (charts, unfolding, packing), 2. compute-shader point light, 3. spot / directional / rect / sky / ambient,
4. hardware ray-traced shadows (Vulkan ray query, CPU BVH reference + fallback), 5. bounces (gather shader, ping-pong atlas),
6. **live progressive baker** (`core/live_bake.h`: chunks, dirty tracking, camera-priority scheduler, content-keyed inheritance),
7. **LightingService + Router** (`lighting` request; `lit_mesh`, `lightmap_tiles`, `lighting_status` messages; 50 ms server tick),
8. **Shell "Lighting only" view** (`render/lightmapStore.ts`, renderer lit pass with a WebGL2 texture array, viewport mode + progress label),
9. light-blocking / light-receiving classes in `game_profile.json` (`lighting.blockLight`, `lighting.receiveBakedLight`),
10. texels buried inside solid brushes are skipped (`SolidSet`), shaders compile with the Core (CMake + glslang, hash-checked fallback).

Verified in the sandbox (Linux, GCC, Mesa lavapipe, no GPU): Core 173 tests pass (1 opt-in diagnostic skipped), Shell typecheck clean, 363 tests pass.
Verified by the user on Windows / MSVC / RTX 4070 Ti: everything up to the bounce tests (their patch is merged).

**NOT verified:** (a) the Shell's WebGL lit pass was never run in a browser (no browser here): look at it first and report. (b) MSVC build of
files added after the bounce step (`live_bake`, `lighting_service`, `SolidSet` in `derived.cpp`, multi-tick `ws_server.cpp`, tests). Only
standard C++ is used (no `M_PI`, no `windows.h` min/max, no GCC extensions) but nobody compiled them with MSVC. (c) Live lighting evaluates
chunks on the CPU: correct but slow on big maps. (d) Lit mode with textures (`lit`) is not implemented, only `lightingOnly`.

### Resume in a fresh sandbox (see VULKAN_RT_SANDBOX_GUIDE.md)
```
apt-get update 2>&1 | tail -1; apt-get install -y --no-install-recommends mesa-vulkan-drivers vulkan-validationlayers libvulkan-dev vulkan-tools glslang-tools spirv-tools cmake ninja-build
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
cd mapeditor/core && cmake -S . -B /home/claude/build -G Ninja && cmake --build /home/claude/build
/home/claude/build/core_tests            # ~100 s on 1 core; run in the background with a log (a tool call is limited to 300 s)
cd ../shell && npm ci && npx tsc --noEmit && npx vitest run
```
Handy: `lightbake_tool <assets> maps/demo_lit.map.json out --bounces 3 --validate` writes PNG pages. `core_tests -tc="name*"` filters,
`-d` prints durations, `-ns -tc="bounce gpu diag*"` runs the opt-in GPU/CPU ray-by-ray diagnostic.

### Next steps, in priority order (the user's goal: see retro baked lighting update live in the editor, minutes -> seconds, hardware RT first)
1. **Check the Lighting-only view in the real editor** (user): run `build_core.bat`, start Core + Shell, pick View mode "Lighting only", open
   `maps/demo_lit.map.json`. Fix whatever is wrong (the pass is untested on real WebGL; likely suspects: UNPACK_SKIP_* with texSubImage3D on
   array textures, attribute locations, culling/winding of the lit mesh).
2. **Persistent GPU context for LiveBaker** (the big one): keep device / queue / pipelines alive between passes, keep the TLAS (rebuild or
   refit only when geometry changes), keep sample / atlas buffers resident, dispatch per chunk batch; add a `sampleOffset` + per-sample
   `seed` buffer to the shaders (`lightmap_direct.comp`, `lightmap_gather.comp`; CPU already has `TexelSamples::seed` and
   `BakeOptions::sampleOffset`). Make `LiveBaker::refine` use an evaluator interface (CPU now, GPU new). Test GPU-vs-CPU per chunk like
   `test_bounce.cpp` (use `SolidSet`, avoid degenerate scenes: lights on edge planes, coplanar contact faces).
3. **VRAM budget**: 11 GB default, configurable down to 7 (`LiveSettings`/profile): atlas buffers per page are `atlas^2 * 16 B` each, the two
   ping-pong buffers must fit `maxStorageBufferRange` (today: otherwise CPU fallback): chunk / page-wise gather instead. Texture residency
   for the Lit view: load only textures used by visible surfaces, unload unused.
4. **Lit view with textures** (`lit` render mode): modulate the Shell's textured material by the lightmap; per-fragment albedo into the bounce.
5. **Light volumes** for brush entities that do not receive baked light (profile list) and for point entities: grid of samples + RT.
6. Quality: light styles (8 per surface), edge-aware filtering of the gather, firefly clamp, denoiser; lightmap UV editing and density tools in
   the Shell; shipping format (EXR / engine export); Embree is NOT used (BVH is the fallback).
7. Small debts: `lighting_status` only counts chunks (no ETA), `lit_mesh` is resent whole on every geometry edit (diff it for big maps; its pixels are not resent, see `keepPages`), `Light.style`
   and `bounce` per light are stored but styles are unused, camera reporting only from the perspective viewport.

### Rules that must keep holding
- Update REQUIREMENTS.md / DESIGN.md before code; tick a box only when code and tests exist (see CLAUDE.md).
- GPU tests: validation output is a failure; AS barriers are not verifiable on lavapipe (reviewed by hand in `gpu_bake.cpp`).
- A test that compares two back ends must avoid degenerate scenes; add a noise-calibrated tolerance instead of a magic constant.


---
## HANDOVER (2026-10-05, end of session M5c, archive packed WITH an unresolved item)

Archive: `mapeditor-M5c_gpu-live-bake.zip` (previous: `M5b_editor-lighting-view`). Start the next chat by reading this section.

### Done this session (code in the zip)
- **Shaders take a per-sample random id and a sample offset** (`shaders/lightmap_direct.comp`, `lightmap_gather.comp`): the host stores `TexelSamples::seedOf(i)` as raw bits in `pos.w` and the per-sample pass offset (`TexelSamples::passOffset`, new) in `nrm.w`; push constants gained `sampleOffset` (`PushConstants`, `GatherPush` in `gpu_bake.cpp`). A chunk evaluated alone now draws exactly what a full bake draws, on GPU and CPU. SPIR-V headers in `src/prebuilt/core_shaders` were regenerated with `tools/compile_shaders.py`.
- **`gpu::evaluateDirect` has an optional `db` out-parameter** (bounce-weighted direct light, shader `gi` bit 2 = write `resB` only) so the live baker can get Db from the GPU.
- **`LiveBaker` evaluates direct light in one batched call per `step()`** (`refineBatch`, `samplesOf` in `live_bake.cpp`): all chosen chunks go into one sample set (own seeds, own pass offsets); GPU ray query when `LiveSettings::useGpu` and `gpu::needsRayTracing`, otherwise the CPU BVH. `evaluator()`, `gpuError()`, `gpuValidation()` report what ran. A failed GPU call sets `gpuDisabled_` until the next `setGeometry` / `setLights`. The bounce gather is still CPU per chunk. `directCpu` honours `passOffset`.
- New test `bake gpu: a sample subset keeps its random ids and sampleOffset advances the draws (CPU and GPU agree)` in `tests/test_lightbake.cpp`: PASSES (CPU subset == full bake bit for bit; GPU vs CPU 0 mismatches of 50347 at offsets 0 and 7).

### State of verification (read this)
- Baseline before my edits: Core 173 pass / 1 skipped, Shell 363 pass, typecheck clean.
- After my edits the Core **compiles** (GCC, no warnings seen) and these pass: `bake gpu*`, `bounce gpu*`, `bake*`, `shadows*`, `solids*`, `bounce*` (run after the shader change, before the LiveBaker rewrite), and the new subset test.
- **UNRESOLVED:** the full set `live*,lighting*,bake gpu*,bounce gpu*` ended with `test_lighting_service.cpp:192: test case CRASHED: SIGTERM`. That was my own `timeout 250` killing a slow run, NOT confirmed as a real crash, but it was not re-run. The LiveBaker rewrite (`refineBatch`) is therefore **not verified by `test_live_bake.cpp` / `test_lighting_service.cpp`**. First thing next chat: run `core_tests -tc="live*" -d` and `-tc="lighting*" -d` separately in the background (log to file, one tool call is limited to 300 s) and fix what fails. Likely suspects: the GPU path now runs inside live tests on lavapipe and is slow (try `LiveSettings::useGpu=false` in those tests to compare), batching changes the order of accumulation vs. the old per-chunk loop, `gpuDisabled_` handling.
- The old per-chunk `refine()` is gone; `step()` now calls `refineBatch(chosen)`.
- Shell untouched this session (363 tests passed at session start). The WebGL lit pass in a real browser and the MSVC build of everything after the bounce step are still unverified (see the older handover above).

### Next steps (priority order)
1. Run and fix the live/lighting tests as described. Add a live-baker test that compares GPU-evaluated and CPU-evaluated steps (same chunks, same passes) with a noise-calibrated tolerance.
2. Persistent GPU context: `evaluateDirect` still creates instance, device, pipelines and the BLAS/TLAS on every call (about the cost of a full setup per step). Keep them alive between steps, rebuild/refit the TLAS only when `setGeometry` ran, keep sample buffers resident. This is the real speed-up; today's batching only removes the per-chunk call overhead.
3. GPU gather in the live path (`gatherCpu` per chunk is the remaining CPU cost), chunk-wise so the atlas buffers fit `maxStorageBufferRange`.
4. VRAM budget (11 GB, down to 7), texture residency, textured Lit mode, light volumes, light styles (see REQUIREMENTS §18.1/§18.2 and the older handover).
5. Check the Lighting-only view in a real browser; run `build_core.bat` on MSVC.

### Resume in a fresh sandbox
Same commands as the previous handover (apt packages for lavapipe, glslang, cmake, ninja; `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`; build into /home/claude/build; `npm ci` in shell). Run `core_tests` detached: `setsid nohup core_tests > log 2>&1 < /dev/null &`.


---
## HANDOVER (2026-10-05, end of session M5d, persistent GPU context started)

Archive: `mapeditor-M5d_gpu-context.zip` (previous: `M5c_gpu-live-bake`). Start the next chat by reading this section.

### Verified at the start of this session (before any edit)
- The unresolved M5c item is resolved: `core_tests -tc="live*"` 9/9 pass (about 10 s), `-tc="lighting*"` 7/7 pass. The earlier SIGTERM was only the `timeout`. Each lighting-service test takes 35 to 74 s on lavapipe (GPU path runs inside them on a 1 core sandbox); a single `lighting*` run needs about 6 minutes, so run it detached (`setsid nohup ... &`) and poll.

### Done this session (one change, `core/src/gpu_bake.cpp` + `gpu_bake.h`)
- **Persistent GPU context.** New `Context` struct (loader, instance, debug messenger, physical device choice, device, queue, limits, direct and gather compute pipelines) is created on the first `evaluateDirect` call and cached per (useRT, validation) key in `g_contexts` (guarded by `g_mutex`, so calls are serialised). Per-call objects (buffers, BLAS/TLAS, descriptor pool/sets, command pool, fence) still live in a per-call `Cleanup`. A failed call erases its context. New `gpu::releaseContexts()` in `gpu_bake.h`.
- The refactor was done by a script that cut `run()` into `makeContext`, `Context::directPipe`, `Context::gatherPipe` and the shortened `run()`. It was NOT reviewed by reading the final file line by line.

### State of verification (read this)
- Core builds with GCC, no errors or warnings shown (output filtered for error/warning).
- After the refactor: `core_tests -tc="bake gpu*,bounce gpu*"` 7/7 pass (3.3 M assertions). The run took about 280 s wall time.
- **NOT re-run after the refactor:** `live*`, `lighting*`, the rest of the suite, any run with the Vulkan validation layer (`gpuValidation`), and `lightbake_tool --validate`. Reuse of one device across calls may change what the validation layer reports (messages are collected in `Context::collector`, cleared at the start of each call, and `Result::validation` copies them after the call). Check that destroy-time messages of per-call objects are not lost or double counted.
- **Not measured:** no timing was taken of the speed-up. Each call still builds the BLAS and TLAS and all sample buffers; only device, instance and pipeline creation are saved. Compare a `live*` / `lighting*` run time against the numbers above (35 to 74 s per lighting test) to see the effect.
- MSVC build of everything after the bounce step and the WebGL "Lighting only" view in a real browser are still unverified (see older handovers). Shell untouched this session (363 tests at last measurement).

### Next steps (priority order)
1. Re-run `live*` and `lighting*` detached, and `bake gpu*` with validation on; review the new `Context` code in `gpu_bake.cpp` by reading it (destruction order: `Context` members are declared collector, lib, f, cleanup, so cleanup runs before the library unloads; the per-call `Cleanup` must finish before the context is reused).
2. Keep the TLAS/BLAS between calls: rebuild only when `LiveBaker::setGeometry` ran (needs a geometry revision handle passed into `evaluateDirect`, or a small `GpuSession` object owned by `LiveBaker` instead of the global cache). Keep sample/atlas buffers resident and only upload changed chunks.
3. GPU gather in the live path, chunk-wise so atlas buffers fit `maxStorageBufferRange`; VRAM budget (11 GB, down to 7); texture residency; textured Lit mode; light volumes; light styles (REQUIREMENTS §18.1/§18.2).
4. Live-baker test comparing GPU and CPU evaluated steps with a noise-calibrated tolerance.
5. Check the Lighting-only view in a real browser; run `build_core.bat` on MSVC.

### Resume in a fresh sandbox
Same as the M5c section: apt packages (mesa-vulkan-drivers vulkan-validationlayers libvulkan-dev vulkan-tools glslang-tools spirv-tools cmake ninja-build g++), `export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`, `cmake -S core -B /home/claude/build -G Ninja && cmake --build /home/claude/build` (about 4 minutes on 1 core, run detached), tests detached with a log file.


### Addendum (M5d, same session): Lighting-only view mostly black in the editor
- Symptom (user screenshot): after baking, most floor and wall surfaces black, only some strips lit near the lights.
- Cause found by reading code (NOT reproduced in a browser, none here): `Renderer.syncLighting` uploaded `store.dirtySince(0).rects` into a freshly created texture. After a second mesh (any edit, or lighting toggled off/on) `applyMesh` resets the change log, so `dirtySince(0)` returns `full: true` with NO rectangles; nothing was uploaded and only tiles that refined afterwards appeared.
- Fix: `LightmapStore.fullRects()` (whole pages), used for a new texture and for `full`. Test added in `tests/lightmapStore.test.ts`. Shell: typecheck clean, 364 tests pass.
- Still to check in the real editor: whether this fully explains the screenshot. Other suspects if it does not: `UNPACK_SKIP_*` with `texSubImage3D` on array textures, black bleed from LINEAR filtering at chart edges, the textured door/frame visible in a Lighting-only view (prefab content may still be drawn by the normal pass; look at `litMode` handling in `render/renderer.ts draw()`).

### Addendum (M5e): the real cause of the mostly black Lighting-only view
- Reproduced in headless Chromium (real Shell + real Core, `?debug`, render mode `lightingOnly`): Core converged, store held the lit texels, screen mostly black, console full of `GL_INVALID_OPERATION: glTexSubImage3D: invalid unpack params combination`.
- Cause: `Renderer.uploadRects` used UNPACK_ROW_LENGTH / SKIP_PIXELS / SKIP_ROWS with `texSubImage3D`. Every rectangle with skipRows > 0 failed (521 of 689 uploads), so most tiles never reached the GPU texture. Core logic (scheduling, staleness, tile messages) was verified correct on CPU and on lavapipe GPU, including light and geometry drags.
- Fix: `packRect()` in `lightmapStore.ts` copies each dirty rectangle into a tight buffer; `uploadRects` uploads it with default unpack state. Tests in `tests/lightmapStore.test.ts`. 685 of 685 uploads succeed in the probe; Shell 367 tests pass.


---
## HANDOVER (2026-10-05, fix4: lightmap edge leaks)

Symptom: dark or bright edges at corners / surface borders in the Lighting-only view (screenshot: wall base dark, door jambs bright).
Cause (found with `lightbake_tool` + `layout.json`): faces unfolded into one island sat next to each other with no gutter, so bilinear filtering at the edge of one face
read texels of another (lit floor against dark wall at a 90 degree fold, unrelated faces touching in the chart, partly covered edge texels never baked).
Fixes (all in Core, Shell untouched):
1. `buildTexelSamples` (`lightbake.cpp`): two passes. Pass 0 = texel centre inside the fragment (as before). Pass 1 = conservative rasterisation: texels that only partly overlap a fragment are baked at the nearest point inside it, pulled `kEdgeInset` (5 mm) towards the fragment centroid. Centre-inside texels always win.
2. `buildLightmapLayout` (`lightmap_uv.cpp`): faces of one island that are NOT joined by a shared edge keep `2 * padding + 1` texels apart (`isolationGap`, `Island::polyFace`, `adjacentFaces`).
3. `LightmapSettings::mergeAngleDeg` default 90 -> 15: corners are separate charts (own gutter); coplanar and nearly flat faces still merge.
Tests: new `bake: every texel a bilinear lookup can touch inside a face is baked...` (test_lightbake.cpp); `test_live_bake.cpp` World pins `mergeAngleDeg = 90` and its chunk assertions use chunk bounds instead of centres (a chunk can now hold texels of several faces).
Verified here (Linux, GCC, no Vulkan device so GPU cases fall back to CPU): Core 178 pass / 1 skipped, Shell 367 pass. NOT verified: the real editor view, MSVC, GPU path on hardware.


---
# Session M5d: light volumes, HDR transport and the Lit view (latest)
**Done (all green: Core 207 cases, Shell 380 tests, `shell/e2e/run_e2e.sh` 140 checks, `shell/e2e/run_lit.sh`):**
- Core `lightvol.h/.cpp`: `ProbeBaker` = ray traced probes on a resident `gpu::LiveSession` (ray query direct + GPU gather over the baker's atlas), CPU fallback `bakeProbesCpu` (compared in `test_lightvol`); fit to ambient + directed + direction; x2 byte packing; trilinear grid; `entityLights`. `LightingService` bakes it after the lightmap converged and publishes `lightvol` + `entity_light`. `LiveBaker` got read accessors.
- **HDR**: `lightmap_tiles` are RGBA16F linear (`LiveBaker::renderTile` -> half floats, `floatToHalf/halfToFloat`), `exposure` only in `lighting_status`. Shell: pages are `Uint16Array`, RGBA16F texture, shaders multiply albedo (linear) x light x exposure and clamp only in the final sRGB encode. No tone mapping, no constants (REQUIREMENTS §9).
- `lit_mesh` has a face table; Shell `render/litTextured.ts` projects the albedo UVs; entities that are not lightmapped use `entity_light` (`ENTLIT_FS`).
- **Browser verification is possible in the sandbox**: Chromium is preinstalled in `/opt/pw-browsers` (`PLAYWRIGHT_BROWSERS_PATH=/opt/pw-browsers`), WebGL2 via SwiftShader; Vulkan for the Core via lavapipe (`apt install mesa-vulkan-drivers`, `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`). `core/build` may be a symlink to the real build dir.
**Known limits:** light volume values saturate at 2 (the format); decals are not drawn in Lit / Lighting-only; no exposure UI (Level settings panel pending); probe preview overlay not built; the probe grid waits for the lightmap to converge.
**Next:** light styles (REQUIREMENTS §9: up to 8 per surface, chart arrangement, discard the weakest), probe preview overlay, decals in Lit, Level settings panel.


---
# Session M5e: light styles and decals in Lit
**Done (green: Core 218 cases, Shell 395, `shell/e2e/run_e2e.sh` 140 checks, `run_lit.sh lit`, `run_lit.sh styles`):**
- Light styles end to end, see DESIGN §6.8: table (`light_styles.h`), per-face reach analysis with shadow rays + influence ranking + 8-slot budget (`light_style_analysis.h`), style-aware charts (`LightmapSettings::faceStyles`, `LightmapChart::styles`, `layoutForStyle`), one `LiveBaker` per style layer in `LightingService`, per-style light volume probes, `light_styles` message, tiles with `style`, lit mesh with a per-vertex style mask (12 floats), Shell texture array with compact slots, animation at 20 Hz, `Viewport.setStyleTime`.
- Decals in Lit: face lightmap maps in the face table, `buildLitDecals` / `buildEntityDecals`, alpha-blended pass.
- Light `style` range is 0..63 now (was 0..7): the 8-slot budget only means something with more than 8 ids; ids 0..7 behave as before.
**Known limits:** style layers occupy the same page rectangles as the base in the editor (sparse, engine export must pack densely: M6); GI of a style onto charts that do not carry the layer is dropped; no UI to edit patterns (profile JSON) or pause styles; decals are not shown in Lighting-only.
**Next (user request): proper light sources with preview gizmos** (REQUIREMENTS light section).


---
# Session M5f: light sources
**Done (green: Core + Shell 410 + browser suites e2e / lit / styles / lights):** modern light entity classes with typed keys in `entities.ent` (parser: `default/min/max/step`), Core test that the .ent defaults equal the baker's, typed Properties editor in Details (`ui/properties.ts`), Place > Lights with the five lights (spot / sun / rect start pointing down), preview gizmos (`model/lights.ts`, View > Light gizmos), `shell/e2e/lights.py`. See DESIGN §6.9.
Two UI bugs found by the browser test and fixed: the Details panel did not refresh after focus left an edited input (reset button stayed disabled), and kept showing the previous entity when the selection changed while an input had focus.
**Not done:** dragging handles in the viewport to change the radius / cone angle; IES profiles / light functions; gizmo colour for animated styles; the Level settings panel (exposure, lightvol grid) is still missing.


---
## HANDOVER (2026-10-08, fix11: textured bounces, decal orientation, rotate/scale of entity properties, no dark flash after edits)

**1. Textured light bounces (bindless).** A bounce ray that hits a surface now picks up that surface's colour: `albedo = texture(face albedo uv) * LiveSettings::albedo`.
- Core `albedo.h/.cpp`: own decoders (TGA all common types, PNG non-interlaced incl. palette / grey / 16 bit, baseline JPEG 4:4:4 / 4:2:0 / grey; progressive JPEG and interlaced PNG are NOT supported: such a texture is white), images cut to <= 256 px with a linear-light mip chain, `AlbedoLibrary` (cache, reload when size / mtime change, sandboxed names).
- `BrushFace::projection` is parsed from the document (`projection: {scale, shift, rotate}`, same defaults as the Shell) and `projectAlbedoUv()` equals the Shell's `planarCoordsProjected` (tested by hand-computed values). `LightSurface::projection` carries it; `buildOccluders` stores per triangle texture id + albedo uv + uv/lightmap area ratio; `attachAlbedo(mesh, loader)` loads the textures, compacts ids and fixes the level of detail per triangle (one lightmap texel's footprint), so CPU and GPU sample the same mip.
- CPU: `gatherCpu` multiplies by `AlbedoImage::sample` (bilinear, trilinear between mips, repeat). GPU: `lightmap_gather.comp` binding 9 (per triangle record), 10 (sampler), 11 (`texture2D albedoTex[]`, variable count, partially bound, `nonuniformEXT` index, `textureLod`). Textures are RGBA8_SRGB images sub-allocated in <= 256 MB blocks (one upload per set, kept by the Vulkan context while the set is unchanged: geometry edits do not re-upload). Needs Vulkan 1.2 descriptor indexing (`runtimeDescriptorArray`, `shaderSampledImageArrayNonUniformIndexing`, `descriptorBindingPartiallyBound`, `descriptorBindingVariableDescriptorCount`): a device without them no longer counts as ray query capable and the CPU path is used. More textures than `maxPerStageDescriptorSampledImages` (capped at 16384) -> the extra ones are white.
- Wiring: `LiveSettings::albedoLoader` / `useTextures` (lighting.json key `useTextures`, default true), `LightingService::albedoLoader()`, `BakeOptions::albedoLoader` for offline bakes.
- Tests: `tests/test_albedo.cpp` (decoders on the real asset files + embedded JPEGs, mips, library, red wall bleeds red, GPU == CPU with 306 textures; the GPU case runs on lavapipe: `apt install mesa-vulkan-drivers`). Prebuilt SPIR-V regenerated (`python3 tools/compile_shaders.py`).
- Limits: no alpha-tested textures (opaque), texture edits on disk are picked up at the next geometry rebuild (not by themselves), the albedo of decals is not used for bounces.

**2. Decals** were upside down (= rotated 180 degrees and mirrored): `v` ran along +up, but image rows run down. `decal.cpp` now uses `v = 0.5 - r.up / height`; test in `test_derived.cpp`.

**3. Rotate / scale tools change entity properties** (`model/transform.ts: mapEntityProps`, called for every point entity by every transform: modal R / S, gizmos, 90 degree rotate): rotation composes into `angles` for any axis (not only yaw); scale changes light `radius` (attenuation) and `source_size` (point / spot), rect light `width` / `height` (own axes), decal `width` / `height` / `depth` (own axes), other entities `modelscale_vec` (own axes) or `modelscale` / `radius` when only those exist. A non-uniform scale scales radius by the cube root of the volume factor. Defaults for unset keys are the .ent defaults (radius 10, width / height 1, depth 0.5). Mirrors do not turn angles. Tests: `tests/transform.test.ts`.

**4. Lit view after an edit.** (a) `LiveBaker::setGeometry` gives every new texel the light of the nearest old texel that faced about the same way (any orientation as a last resort, search radius 6 m); it stays stale and is replaced by the first pass. (b) `LightingService` counted a mesh as ready when 10 % of the chunks were REFINED; it now counts chunks that SHOW something (`chunkHasData`: refined, inherited or extrapolated), so after an edit the new lit mesh and tiles go out in the same tick instead of up to 3 s later. Only the very first build still waits. Test: `live: after a geometry edit new texels start from the light of their neighbours`.
**Not verified:** the real editor in a browser, Windows / MSVC, real GPU hardware (only lavapipe). The full Core suite under lavapipe is very slow (one GPU bake test takes many minutes); the albedo, bounce, live and Shell suites were run.
### Live lighting latency (edit to pixels), state after the latency work
Done: bake thread with latest-wins cancellation, snapshot per message, texel cache, slot inheritance, dirty chunks + mirror diff, `keepPages`, progressive
flush, GPU buffer pool, parallel resident arrays, denoise on its own thread, `bench_live`, `timingsMs` in the status. Measured with noise instead of light on
a 1 core machine (9 rooms, 460k texels, 8 texels/m): first tiles about 700 ms -> about 150 ms after the edit, all passes about 840 -> about 370 ms; the real
GPU was not available, so session creation and GPU time on real hardware are not measured (read `timingsMs` in the status on a real machine first).
Open, in the order of expected gain:
1. Tighten the stale region: a changed box only matters to chunks that can see it from a light (hull of light and chunk bounds, swept box for
   directional lights; `sky` stays global). Today every chunk within the largest light radius goes stale, a sun makes everything stale. Prove
   conservativeness with a test that compares per-chunk direct light before / after an edit.
2. `TexelSampleCache::build` assembly (about 30 to 40 ms at 460k texels, serial because of the first-claim-wins order) and `buildLightmapLayout`
   (about 16 ms): parallelise assembly in three phases (hidden test per candidate, serial claim, parallel copy) / cache the layout by input hash.
3. Merge loops of `refineOnGpu` / `refineBatch` (per sample CPU work per step) on all cores.
4. Resident sample upload is a full copy per session (about 84 bytes per texel); keep stable resident slots per chunk to upload only changed chunks.
5. The OIDN denoise thread is not covered by ThreadSanitizer (OIDN uses RTLD_DEEPBIND, which the sanitizer runtime refuses).


---
### Merge note: live latency work on top of fix11 (textured bounces, light styles, light volumes, HDR tiles)
The latency patch was developed against fix6 and merged into the lit-stale-fix state of fix11. Where fix11 and the patch touched the same code:
- Tiles are RGBA16F (`renderTile` returns halves, no exposure and no sRGB table; the patch's sRGB table is gone). The mirror holds `uint16_t` pages.
- The service engine handles several `LiveBaker`s (base + style layers): dirty chunks, tiles, staleness, convergence and denoise are per layer; the
  mirror has pages per style. `keepPages` requires the same layer set.
- `LiveBaker::setGeometry` keeps fix11's extrapolation (new texels borrow the light of the nearest old texel that faced about the same way) on top of
  the slot inheritance; those texels are dirty (new) and stale.
- Shell: `LightmapStore` caches (stale analysis, lit draw) are keyed on `meshSerial` (the mesh and face table), not on `serial` (which only moves when
  the pages are replaced); `meshSerial` moves with every mesh. The renderer refills its vertex buffer through `syncLitPlain` after a kept-pages mesh.
- `tsc --noEmit` is clean (an unused parameter in `model/transform.ts` was renamed).
- Not run here: the GPU path (no Vulkan driver in this environment), the OIDN denoise thread, the browser suites (`shell/e2e`).


---
### Lightvol sample preview (View > Lightvol samples)
Shell `render/lightvolPreview.ts` builds the overlay geometry: one smooth-shaded sphere per baked probe at its grid position (icosphere, subdivision level 1 = 80 triangles, radius 0.12 cell). Every vertex carries its unit normal and a colour = the light a surface facing along that normal receives, `ambient + directed * max(0, n . dir)`, through the Lit view's own transfer function (exposure, clamp, sRGB); the GPU interpolates it smoothly. A white line leaves the probe towards its directed light, half a cell long at full strength (lines have a zero normal and are not shaded). Invalid probes are skipped; above 6000 probes (`PROBE_PREVIEW_MAX`, lowered from 20000 because a sphere has 10x the triangles of the old diamond) every n-th is drawn. Vertex layout is `PREVIEW_STRIDE` = 9 floats (position, colour, normal). `renderer.ts: drawLightvolSamples` uploads it once per `LightvolStore.serial` / exposure and draws it depth-tested after the mesh overlays; the probe shader adds a gentle fixed key-light cue (70 to 100 % brightness) so an ambient-only probe still reads as a sphere. The View entry turns lighting on by itself (the probes only exist while lighting runs), so it also works in the Unlit view. Only the base style layer is shown. Test: `tests/lightvolPreview.test.ts` (sphere count and centre, unit normals, top brighter than bottom for an upward directed light, unshaded lines); browser check `shell/e2e/overlays.py`. Unit tests and typecheck run here; not verified in a browser.


---
### Lightmap texel density per face (Texture panel > Lightmap texel density)
`BrushFace::lightmapScale` (document key `lightmapScale` on a face, absent = 1, clamped to 1/16 .. 16; `kMinLightmapScale` / `kMaxLightmapScale` in `brush.h`) is carried by `LightSurface::lightmapScale` into the layout (`lightmap_uv.cpp`: the face's `Face::scale`, multiplied by a `LightmapSettings::densityScale` override if one exists). Faces with different densities never share a chart (existing rule). A change is a geometry change for the service (`sameSurface` compares it), so the layout is rebuilt and the mesh is resent. Shell: `Face.lightmapScale`, `model/uv.ts: lightmapScaleOf / clampLightmapScale`, `actions.ts: editLightmapScale` (1 is stored as absent, no-op edits are no undo step), controls in `ui/texturePanel.ts` (field, x2, /2, Reset; shows the first selected face when several are selected). Tests: `test_lightmap_uv` (density + clamp), `test_document` (lenient parsing), `test_lighting_service` (more texels reach the baker), `tools.test.ts` (action, clamp, undo). Not verified in a browser.


---
### Lightmap density overlay (View > Lightmap density)
Shell `render/lightmapDensity.ts` builds per-triangle texels-per-metre from the lit mesh (`triangleDensity`, memoised per `meshSerial` in `densityOf`). `renderer.ts: drawLightmapDensity` draws a texel checker (one cell per lightmap texel) with a log2 colour ramp (1 to 64 texels/m) over the mesh with polygon offset. The viewport header appends a legend with the min / max density while the overlay is on. The View entry turns lighting on by itself. Tests: `tests/lightmapDensity.test.ts`, browser check `shell/e2e/overlays.py` (ramp pixels, legend changes after a `lightmapScale` edit, lightvol overlay, no console errors). GPU path, OIDN thread and the older e2e suites were not run here.

---
### Point entity boxes, decal selection, entity selection (Shell)
- **Boxes follow `angles`.** `model/pointBox.ts`: `pointBox(entity, boxOf)` = the class box (decals: `decalLocalBox`, the projector volume: width x, depth y, height z) with `rot = rotationFromAngles(angles)` about the origin. `worldCorners` feeds the renderer (`cornerLines` / `cornerTriangles`, ghost included), `rayOrientedBox` in `render/picking.ts` hits it (ray taken into the box frame), `boundsOfBox` gives the axis-aligned box for link arrows and prefab icon bounds, `directionArrow` starts at the turned centre and measures the span through the box in its own frame.
- **No box for lights and decals.** `drawsBox(entity)` is false for modern lights and decals: the renderer draws neither fill nor outline (the class-name label stays). They are still picked through `pointBox`, so a light keeps its click target and a decal is clicked by its bounds (`annotations.ts: decalBoundsLines`, the same edges the View > Origins annotation draws). A selected decal outlines its bounds in orange even with Origins off (`viewport.ts: decalSelectionOverlay`). A decal's bounds now take clicks over a wall behind it, as the old box did.
- **Sole brush = entity.** `state/store.ts: wholeEntityIfSole` / `soleBrushOf`. Used by the viewport click, right-click, double-click, the outliner brush rows and the Tab (mesh edit) switch; the properties panel shows the brush section for such an entity. Tests: `tests/pointBox.test.ts`.
- **Retexture -> bounce re-bake: fixed** (the texture / projection hash was added in an earlier session; on 2026-10-14 `lookHash` added the newer UV data too, REQUIREMENTS §22 P3).

## Texturing and UV milestone (2026-10-09)
Done: part A (REQUIREMENTS §19 A1-A7) except browser verification of the window canvas and the viewport heat map pass.
Next: run the window in a real browser; then part B in order: B1 Surface Inspector layout (shift / stretch / rotate with steps, Match Grid, Width/Height/Reset/Fit, projection buttons, pixels per meter readout, Texture properties button), B2 justify, B3 copy UV by right-click, B4 new UV storage + UV lock in mesh mode, B5 Flow / Fast Texture Tool / hotspots / lift.

Update 2026-10-10: B1 (Surface Inspector, except Ortho / Cam / Patch Natural) and B2 (justify) done. Next: B3 right-click copy UV, then B4 (new UV storage; unlocks Ortho / Cam), B5.

Update 2026-10-10 (2): B3 right-click copy UV done. Next: B4 new UV storage (unlocks Project Ortho / Cam), then B5.

Update 2026-10-10 (3): UV origin bottom-left (A9) and edge-bleed fix (A10) done; verify in a browser that the black line is gone at several distances (mips above level 2 may show a faint remainder; if so raise `edgeInsetTexels` or pad the mip chain).

Update 2026-10-10 (4): B4 done (kinds ortho / cam, shear, per-face repetition override, mesh-mode vertex-move UV lock). Next: B5 (Flow, Fast Texture Tool, hotspots, lift material).

Update 2026-10-10 (5): Flow, Lift material, Hotspots done. Remaining in B5: Fast Texture Tool (incl. mirror). Then: browser verification of all the new UI (texture properties window incl. hotspot drawing, Surface Inspector, right-click menu, heat map pass).


## Relief editor (2026-10-10) - handover
Done: `model/relief.ts`, `model/reliefSession.ts`, `render/reliefView.ts`, `ui/reliefEditor.ts` (popup, `openReliefEditor(env, path)`), `TextureProps.relief`, Core `sanitizeRelief` (+ test).
Finished in this pass: "Relief..." button in the Texture panel (next to Properties) and in the Content Browser right-click menu (`app.ts openRelief`, saves through `writeTextureProps`); CSS for `.reliefwin`, `.rl-*` in `style.css`; `tests/relief.test.ts` (stored form, presets, report, session history/modal/confine/resize). Shell 608 pass, typecheck and vite build clean, Core texture/relief tests pass (GCC).
NOT verified in a real browser (no Chromium here): WebGL view, picking, modal tools, layout. Check by hand: open Relief from the Texture panel, apply a preset, G/E/I, Apply, reopen.
NOT done (not requested yet): subtractive brushes rendered in lit mode bug, texture group presets, applying relief to world geometry.

## Fix: subtractive brushes drawn lit (2026-10-10)
Cause: `render/litStale.ts analyzeLit` redraws every face of a changed brush as a lit overlay. A subtractor owns pieces in the Core's lit mesh (the walls it cut), so after it was moved or edited its whole authored volume appeared lit. Fix: a stale `op === 'subtract'` brush drops its old pieces and adds no overlay. Core was already right (`collectSurfaces` only builds targets from additive brushes; subtractors only take part as cutters / solids). Test: `tests/litStale.test.ts` (subtractive section). Not verified in a browser.

## Relief on brushes (2026-10-10)
The relief saved with a texture is now drawn on every face that uses it (Unlit view). `shell/src/model/reliefApply.ts`: `reliefTriangle` tiles the relief over a face triangle (one tile per UV repeat; relief x -> u, relief y up -> v down, depth along the face normal in metres), cuts it at the face border and the tile border, and emits side walls cut along their footprint; normals come from the displaced geometry, walls keep facing into pits / out of bumps, also with mirrored projections. Used in `renderer.ts upload` (plain brushes) and `model/derived.ts buildCarvedMesh` (carved fragments, incl. cut walls). Repeat behaviour: see "Relief and repeat modes" below. Budget: 4096 repeats per triangle, 300k triangles per mesh build, beyond that the face stays flat. Cache invalidation already works: the texture signature contains the relief, so Apply in the Relief editor redraws at once.
Lit / Lighting-only: see "Relief in the Lit views" below. Not done: picking, outlines, selection and the lightmap UVs use the flat face; the editor's relief tile is the texture's natural size while a face may scale it (x / y follow the face, depth stays in metres). Not verified in a browser. Tests: `tests/reliefApply.test.ts`, `tests/reliefCarved.test.ts`.

## Relief in the Lit views (2026-10-10)
The Lit and Lighting-only views now show the relief. No Core change: the lit mesh stays flat and the Shell lays the relief over its pieces (`render/litTextured.ts displaceLit`, same geometry as the Unlit view: `reliefTriangle`). Why this works without new charts: the lightmap uv is affine in the position on the face plane (DESIGN 6.4), every relief vertex has a flat-plane footprint, so its lightmap u / v are interpolated from the flat triangle's corners (barycentric key = the albedo uv the relief vertex carries); page and style mask come from the triangle. Used in `buildLitDraw` (Lit, also for the stale-brush overlay) and `buildReliefPlainMesh` (Lighting-only, `LightmapStore.plainMesh`, cached by mesh / document / texture revision).
SUPERSEDED by "Relief in the bake" below: the shade factor is gone, the relief is lit by the bake.
Tests: `tests/litRelief.test.ts`. Shell 626 pass, typecheck clean. The lit shaders compile and link in headless Chromium (SwiftShader); the rendered result itself was not looked at in a real Lit view (needs Core + baked lightmaps).

## Relief and repeat modes (2026-10-10)
A face whose texture has a relief (or whose repeat plan pulls in an alternate texture that has one) no longer ignores mirror, anchor, alternate texture and edge clamp. The repetition plan (`model/uvmesh.ts`) already cuts every triangle into pieces with exact texture coordinates and the texture each piece samples; the relief is now laid over each piece by those coordinates (`model/reliefApply.ts planWithRelief`, `planHasRelief`; `FacePlan.textures` lists the textures a plan can sample). Consequences: a mirrored tile gets a mirrored relief (continuous at the joint), an anchored edge keeps its relief glued to the face edge, the band of an alternate texture takes the alternate's relief (or none), a clamped tile's relief is cut where the tile's inset is (about 1% of the tile at each border, so the first and last texel row of a tile show no relief). Faces without a plan keep the old path (relief by the projection's uv).
Used in `renderer.ts upload`, `derived.ts buildCarvedMesh`, and in the lit views (`litTextured.ts planTriangles`, in `buildLitDraw` and `buildReliefPlainMesh`: lightmap coordinates are interpolated from the source triangle by the piece's barycentric weights, so lighting is unchanged). `textureSignature` now includes an alternate texture's relief, so editing it rebuilds the brushes that use it.
Known limits: no skirt is generated at a piece border inside a face (anchor joint, clamp inset). A relief whose height at the tile border is not 0 can therefore show a step or a gap at an anchor joint or between clamped tiles; mirror joints are always continuous, and presets and any relief with zero border depth (the editor warns about this) join cleanly. Per-face repeat overrides count (they are part of the plan). Not verified in a browser.
Tests: `tests/reliefRepeat.test.ts` (mirror, anchor, alternate texture, clamp, signature, Lit and Lighting-only). Shell 637 pass, typecheck clean.

## Relief in the bake (2026-10-10)
Relief is lit like geometry: it casts and receives shadows, slopes are shaded by their real normal, and it reuses the lightmap coordinates of the flat face. Editors (brush, mesh, picking) still work on the source geometry only.
How: the Core bakes the flat pieces; the Shell sends the displaced triangles it draws. `shell/src/model/reliefBake.ts` (`reliefFaces`: per authored face side with a relief, the same `reliefTriangle` / repetition-plan output the viewport draws; `reliefMessage`; `reliefSignature`) -> `relief_geometry` message (core/protocol.h `kReliefGeometry`; faces keyed instance|entity|brushInstance|brush|face|side, plane n/d, base64 float32 triangles). `Store.syncRelief()` (called by the viewport each lighting frame; debounced 150 ms, sends only when the signature changed, resent after reconnect / document_sync). Core: `core/relief_geometry.{h,cpp}` (`ReliefMesh` with its own TriangleBvh, `ReliefSet::assign/find`, `displaceSample`), `Scene::setReliefGeometry` attaches the mesh to `LightSurface::relief` in `lightSurfaces()` only when the face's plane still matches (an out of date message is ignored), Router -> `lighting_.markDirty()`.
In the bake: (1) `buildOccluders` emits the relief triangles INSTEAD of the flat pieces of that face (a pit must not be covered by the flat face), once per face; lightmap / albedo coordinates of those come from each corner's footprint on the flat plane. (2) `TexelSampleCache::build` `claim`: after the buried-in-solid test (done on the FLAT point: a pit lies inside the brush) the texel position/normal become the relief's (vertical ray from above onto the face's relief); the supersample frame follows the hit triangle's tangent plane when the whole texel lies on one triangle, else stays level. (3) `sameSurface` / `diffSurfaces` include the relief hash and bounds, so a relief edit rebakes the region.
Limits: pit walls map to border texels (their footprint has no area), so they take the light of the floor/rim next to them; relief finer than a lightmap texel is lit per texel, not per pixel; a brush the Core carved but whose derived result is stale is sent without relief until the result catches up (a few frames).
Also fixed (same pass): (a) carved geometry vanished in Lit: the walls a subtractor cuts have the REVERSED normal of its face, `analyzeLit` compared signed and dropped them as stale (`render/litStale.ts sameOrientation`); (b) "level stops rendering above N vertices": `renderer.ts gpu()` did `tri.push(...verts)`, which throws (call stack) beyond ~120k numbers; now a loop. Look for other `push(...bigArray)` before adding similar code.
Verified: Core 257 test cases pass (g++, no cmake: see /home/claude/build/Makefile pattern, include `src/prebuilt` for the shader headers); Shell tests run through a small vitest shim (no node_modules here), all relief/lit/store tests pass. NOT verified in a browser or with the GPU path (the GPU bake takes the same occluder mesh and texel samples, so it should follow, but it was not run).

## Update 2026-10-11: Fast Texture Tool built; texture groups built
- Fast Texture Tool (Shift+Q): see REQUIREMENTS §6 (B5) and DESIGN "Fast Texture Tool". Shell: 730 tests pass. NOT run in a browser: canvas drawing, pointer feel, cursor shapes, the floating window's dragging. Check first: handles are grabbable at every zoom, wheel zoom under the pointer, Esc during a drag, that the viewport follows the drag live.
- Texture groups (REQUIREMENTS §20): done except a visual node editor and per-cell bounce albedo; see §20 T8 / T9.
- Still open in the UV area: Patch Natural (waits for patches), Project Cam is built but the Cam button needs a viewport camera (shown disabled without one), lightmap UV editing in the UV window (§9), verify UV continuity of subtractive faces with the new UV model (§6), hotspots / texture groups in the exporter (M6).
- UV continuity (2026-10-11): fixed carved / bake / Lit / stale UVs after a move with UV lock (`faceUvAt`, DESIGN "Which UV is authoritative"). Open follow-up: Core's textured-bounce albedo (`projectAlbedoUv`) still reads the projection. Shell: 734 tests pass.

## Update 2026-10-12: simple texture sets
- Built: wall sets and 9-slice frames with a visual editor, Simple / Advanced tabs, the building block textures as a shipped example (REQUIREMENTS §20b). Shell: 784 tests pass. NOT run in a browser: drag and drop of cards, canvas thumbnails, dialog stacking of the editor over the texture picker. Check first: dragging a card between rows, Add textures... sorting, the preview following edits.
- Open: see §20b S7. Core needs no change (the file is stored as before; `texGroupRoot` folders of any depth are listed).
- 2026-10-12 fixes: (1) saving a simple set failed in Core ("a group needs a root node"): Core fixed (rebuild Core to get it; the Shell also writes a placeholder root so the old Core works). (2) Max stretch for texture sets (REQUIREMENTS §20b S2b). Shell: 803 tests pass, `npm run build` passes, Core texture group tests 30 cases pass.


## Update 2026-10-14: UV tools checked in a real browser; mesh editing batch (REQUIREMENTS §21)
- **How to run it here (Windows):** `coreuild\mapeditor_core.exe 8787` from `core\`, and the Shell dev server (`.claude/launch.json` "shell" = `npm --prefix shell run dev -- --port 5173`). The page is scaled: CSS pixels = screenshot pixels x 1.28 in the 800 px browser pane (matters when dispatching pointer events from page JS; the pane's own drag does not pass Shift, so Shift+drag was tested with `PointerEvent`s and a stubbed `setPointerCapture`).
- **UV tools, browser pass:** Fast Texture Tool window draws (texture backdrop, island, handles, info line); move / rotate handle / Mirror U / undo work and are live in the viewport; Shift+Q opens it. **Bug found and fixed:** Lightmap mode treated the Core's face map as texels, but Core sends page units (0..1, `lighting_service.cpp` divides by the atlas size): every chart was "0 x 0 texels, 0.0078 texels/m" on an empty page. `model/lightmapView.ts` now takes the atlas size (`texelOf / densityOf / chartOf / chartsOf(..., atlas)`); after the fix the pillar shows 3 x 19 texel charts at 8 texels/m over the baked page. Also fixed: `tests/texgroup.test.ts` built Windows paths from `URL.pathname` (`F:\F:\...`), now `fileURLToPath`.
- **Mesh editing, all six items of §21 built with tests and each tried in the browser:** E1 loop / ring / path selection (Alt+click or double-click, Ctrl+Alt, Ctrl+click), E2 Connect (J) and Subdivide (panel, with Cuts), E3 Mirror (Ctrl+M then X / Y / Z), E4 proportional editing (O, wheel in G / R / S), E5 Knife (K, Enter / right-click / Esc), E6 Shift+drag the move gizmo arrow = extrude. Code: `model/meshSelect.ts`, `model/meshCut.ts`, `meshOps.ts` (`mirrorVerts`, `falloff`, `proportionalVerts`, `extrudeFaces(..., along)`), `meshTopology.finish(..., splitFrom)`, viewport wiring, Mesh Edit panel (Cuts, Proportional + radius). Shell: 844 tests pass, typecheck clean. DESIGN §8.x has the how.
- **Not built / open:** hold-and-drag path selection (Ctrl+click path covers it), Bridge edge loops, knife points inside a face (would make faces non-convex), the wheel changing the proportional radius during a GIZMO drag (panel only), a falloff circle drawn in the viewport. Still open from before: Patch Natural (needs patches), Core textured-bounce albedo ignores repetition and stored UVs (A8), the retexture -> bounce rebake gap (`diffSurfaces` hashes vertices only).
- Noticed, not investigated: in the Lit view the func_detail pillar looks almost black next to the lit floor (its charts are baked and non-empty). Worth a look in the next lighting session.

## Update 2026-10-14 (second): parametric shapes, Level settings, retexture detection (REQUIREMENTS §22)
- **P1 parametric shapes:** cylinder / wedge / arch / stairs keep their generator (`shape` key on each part brush, `model/parametric.ts`); Details > Shape edits parameters, size and orientation; Place > Shapes has Orientation; a click on a part selects the whole shape; any non-transform change to a part (or moving one part alone) turns the shape into brushes (store-wide `reconcileShapes`). Browser: drew stairs, changed steps and orientation, moved it, edited again.
- **P2 Level settings:** Level tab (next to Details / Texture). Values are worldspawn keys (DESIGN 8.z). Core: `_light_scale`, baked AO (`_ao_*`), quality overrides, colour correction in `lighting_status`. Browser: AO, general brightness and display brightness seen in the Lit view (GPU path, RTX). Rebuild the Core to get it (`build_core.bat`).
- **P3:** UV-only edits (kind / basis / shear / uvs / repeat / layout) now re-bake the bounce light around the face (`lookHash`).
- Tests: Shell 864 pass; Core 268 pass (`build_core.bat` runs them; MSVC + Vulkan SDK at D:\VulkanSDK are installed here).
- **Open / next candidates:** A8 (bounce albedo uses the Core's simple projection: port the Shell's UV model or send per-face UV affine maps), M6 exporter (where `_bake_quality` and parametric shapes' flattening apply), hold-and-drag path select, bridge, a gizmo-drag wheel for the proportional radius, Patch Natural (needs patches). The pillar looking near black in Lit is still not investigated.

## Update 2026-10-14 (third): transparency in light baking (REQUIREMENTS §23)
- Texture property `transparency` (Auto / Opaque / Cutout / Translucent with cutoff, opacity, tint) in the texture properties window; baked on the CPU and the GPU (same packed data, exact agreement in tests), direct shadows (all light types incl. sky), bounce rays, AO and light volumes; Unlit and Lit views cut out / blend the same way. Example textures `core/assets/textures/dev/grate.png` (alpha holes) and `dev/glass.png` + `glass.png.json` (translucent). DESIGN 8.t has the how.
- Tests: Core 273 pass (new `tests/test_transparency.cpp`, incl. GPU one-shot and live session on the RTX 4070 Ti), Shell 868 pass. Rebuild the Core to get it.
- Open: picking through cutout holes, translucent faces of light-volume-lit brush entities are drawn opaque, a texture file edited outside the editor (not through its properties) does not trigger a re-bake, the A8 projection gap applies to the alpha lookup too (faces whose UVs come from the newer UV model sample the alpha with the Core's simple projection).
- 2026-10-14, user's correction ("half transparency, like glass, incl. coloured glass"): Auto now makes half transparent alpha into glass (translucent, alpha = opacity, light coloured per texel); example `dev/stained_glass.png`. Core 275 / Shell 868 tests pass. Checked in the browser: a 5 x 5 m `dev/stained_glass` slab under the ceiling light throws red / green / blue / yellow light with dark lead lines onto the floor and walls in Lit, and is drawn see-through in Unlit and Lit.

## Update 2026-10-15: missing / incomplete features pass (REQUIREMENTS §24)
- Statuses re-synced with the code (light styles, lightvols, persistent GPU session, decal lightmaps, lighting classes in the profile JSON, probe preview were built but still ⬜).
- C1 Alt+drag a face resizes its brush (§17 #4). C2 references are keyvalue rows in Details, targetname on top (§17 #5). C3 the Core samples faces with stored uvs through their own UV map (A8, bounce colour + transparency). C4 right-click: Group / Ungroup, Copy / Paste UV, Hide / Unhide all, Properties.
- Tests: Shell 875, Core 276 pass. Browser: context menu, Hide, Unhide all.
- Still open (biggest first): M6 BSP export (classic + new format, EXR lightmaps, lightvol x2 storage, flatten at compile, `_bake_quality`), Lit view hides hidden brushes only after a Core-side filter, A8 rest (repetition behaviour in the Core), lightmap chart editing (placement), lightmap packing that keeps neighbouring faces together, VRAM budget, Scythe selection modes, bulk editing of mixed selections, texture set S7 items.

## Update 2026-10-16: BSP export, the new `rBSP` format (REQUIREMENTS §25, DESIGN §12)
- **Export works end to end:** toolbar `Export BSP` -> path dialog -> the Core bakes the final lighting on its own copy of the document (headless `LightingService`, pass target and probe rays x `_bake_quality`) and writes `<name>.bsp` + `<name>/lm_NNNN.exr`. Demo: `core/assets/maps/demo_lit.bsp` (+ `demo_lit/lm_0000.exr`) exported from the browser in 1.9 s (75 surfaces, 17 brushes, 48 vis cells, light grid 3840 points). The map file was not saved.
- **Format:** `rBSP` v2 = Raven lump list with 8 style slots (vertex 128 B, surface 204 B, grid point 58 B), lightarray lump starts with the grid's own bounds (40 B header; the user plans playable-area light volume bounds), lightmap lump empty (external EXR, half float, ZIP, own writer on vendored miniz).
- **Visibility:** sampled cells, no portals, no leaks (the user: "modern system that supports open levels"): kd tree of cubic cells (`_vis_cell`, default 4 m, at most 4096 open cells), rays from sample points per cell against the opaque static world, symmetric + dilated by one cell.
- **Collision:** convex pieces of every additive brush, concave ones split by their own planes, subtractors cut out (`bsp_convex.cpp`).
- **Engine** (`SdlWasmEngine/source/Engine/BSP/Quake3Bsp.*`, new `ExrLightmap.*`, `Texture.hpp` pixel sizes for RGBA16F / RGBA32F): reads rBSP v2. Compiled with the engine's flags (standalone, objects in the scratch folder; the engine's build tree was not touched; a CMake reconfigure picks up the new .cpp through its GLOB). **Not run in the engine.** Original files are unchanged apart from these edits (CRLF and BOM kept).
- **OIDN:** the user's `oidn-2.5.1.x64.windows.zip` is unpacked in `core/third_party_bin/`; CMake copies its DLLs next to the executables. With the denoiser active two old tests needed fixes (raw-noise test now disables it; an all-black page is no longer sent through OIDN, which added a faint haze).
- Tests: Core 282 pass (new `tests/test_bsp.cpp`: EXR, convex pieces, compile, visibility, lightmaps + grid, router export), Shell 878 pass (new `tests/exportBsp.test.ts`), production build clean.
- **Open (E10):** classic IBSP; texture repetition modes in the export; decals and reliefs; brush contents (triggers, water, clip); vertex colours; one light direction per grid point; engine renders 4 of 8 slots and its BSP shader multiplies the light volume colour by 1.1 (shader change needs the engine's shader build); timing of the visibility on a large map.

## Update 2026-10-16 (second): the rBSP export runs in the engine (REQUIREMENTS §26)
- **Engine test build:** `%TEMP%\mebuild\build.bat` (own CMake build dir; `GAME_NAME=SlasherGame_mapeditor`, `GAME_PATH=source/Games/SlasherGame`). The engine's CMake puts the exe in `F:\Git\SdlWasmEngine\Build\SlasherGame_mapeditor\windows\bin` (a new folder; delete it when no longer needed). A rebuild re-creates its GameData: run `%TEMP%\mebuild\copy_test_data.sh` to copy the test maps / editor textures in again; fmod.dll + fmodstudio.dll were copied from `source/binariesToInclude`. Run: `bin\SlasherGame_mapeditor.exe -working_dir <windows folder> -map engine_test.bsp`.
- **Test map:** `core/assets/maps/engine_test.map.json` = demo_lit + `info_player_start` + a decal + a style 1 light (the demo map itself has no player start; without one SlasherGame shows nothing). Export: `core\build\bsp_export_tool.exe core\assets maps\engine_test.map.json maps\engine_test.bsp --lighting core\build\lighting.json`.
- **Engine changes** (SdlWasmEngine, no git used there): `Engine/BSP/Quake3Bsp.h/.cpp` (rBSP v2, 8 slots, EXR pages, explicit light grid, decal flag + draw order, no x2 / x1.1 tweaks for rBSP), new `Engine/BSP/ExrLightmap.h/.cpp`, `Engine/Texture.hpp` (RGBA16F / RGBA32F pixel sizes), `Engine/MapData.h/.cpp` (`EntityData::DirectSpace`, `AddLegacyAngle`), `Engine/Entities/StaticMeshEntity.h` (scale in direct space), `Games/SlasherGame/GameData/shaders/source/bsp/fs_bsp.sc` (+ its compiled shaders, rebuilt by ShaderCompilationAutomation).
- **Core:** 8 lightmaps in 4 UV sets, contents rules from `game_profile.json` `bsp`, decals, entities in engine space, `bsp_export_tool`. The flaky threaded denoise test (it only runs since OIDN is installed) now waits for the service to be quiet before comparing. Tests: Core 285 pass (twice in a row).
- **Open:** the other games' BSP shaders still have 4 slots; the engine does not read contents flags (it uses texture names); reliefs and texture repetition modes in the export; one light direction per grid point.

## Update 2026-10-16 (third): export finishing touches (REQUIREMENTS §27)
- **All games' BSP shaders** have 8 slots (copied from SlasherGame, compiled, 0 failed each).
- **Repeat modes, texture group layouts and reliefs are exported** as the editor shows them: the Shell's texture layout code is ported to C++ (`core/src/texplan_*.cpp`) and checked against shared vectors (`testdata/texplan_vectors.json`, regenerate with `UPDATE_TEXPLAN_VECTORS=1` in the Shell's `tests/texplanVectors.test.ts`).
- **One light direction per grid point** in the Core, the protocol (`entity_light`), the Lit view and the export.
- **Fixes after the user's test in the game:** holes beside walls (partly buried pieces were dropped by their centre), and the game looked darker (gamma-space multiply in `fs_bsp.sc`; now linear for rBSP v2).
- Tests: Core 293 pass, Shell 879 pass, typecheck clean.
- **Open:** entities / characters in the game are lit from the light grid by the game's model shaders in gamma space (darker than the editor's Lit view); the engine does not read contents flags; `Build/SlasherGame_mapeditor` is a test folder that can be deleted.

## Update 2026-10-17: enclosed sky and skyboxes (REQUIREMENTS §28, DESIGN §12 "Sky")
- **Auto sky on export:** Level settings > Sky: `off / box / box per area`, margin (default 8 m), skybox. `box` = 6 inward sky quads around the level plus the margin (the floor is sky too), `box per area` = one box per group of touching brushes, boxes that meet are merged. Only the export has them.
- **Sky faces** (automatic and hand-placed, `*cube` / `*sky*`): no lightmap, contents 0, never buried, not occluders (bake and visibility), in every cluster's leafs; the Core's lighting never sees them (`Scene::lightSurfaces()`).
- **The engine's convention, pinned down from its code** (`Quake3Bsp.cpp`, `TextureCube.hpp`, `fs_bsp_cube.sc`): `env/<last part>` faces `_lf _rt _up _dn _ft _bk` (or a panorama), `_up` / `_dn` turned on load, sampled with `(-x, y, z)` in the editor's own space. Core `core/sky.h` and Shell `model/sky.ts` share the mapping; tests check up, north, east, ... and replay the engine's panorama conversion. No engine change.
- **Texture browser:** skyboxes listed (`env/`, a "Skyboxes" entry), SKY badge, cross preview; assign like any texture; the views draw sky faces by direction (Unlit and Lit) and, with Auto sky on, the level's sky behind everything.
- **Sky light from the skybox:** `light_sky` takes its light per direction from the level's skybox (32 x 8 cell average over the upper hemisphere + an irradiance table), CPU and GPU; uniform when there is none.
- Test map `core/assets/maps/sky_test.map.json` with a labelled test skybox `core/assets/env/test_sky_cube_*.png` (each face names its direction and its edges), exported to `maps/sky_test.bsp`.
- Tests: Core sky + bsp cases + related lighting cases pass (23 cases), Shell 902 pass, typecheck clean. Prebuilt SPIR-V of `lightmap_direct` refreshed.
- **Open:** checked in the editor, not yet in the game; the engine's far plane (3000 m) clips sky boxes larger than that (warning; suggested fix in DESIGN §12); a non-cubemap sky texture stays flat in the game; the checked-in `lightmap_gather.spv.h` is stale against its source (its hash does not match; untouched here).

## Update 2026-10-17 (second): the editor as a desktop app (REQUIREMENTS §29, DESIGN §11)
- **`build_app.bat [notests]`** builds the Core (Release, in `core\build_release`), the Shell and the Electron app into `release\LevelEditor\` - one folder to zip and publish; nothing to install on the target PC. `LevelEditor.exe [assetsFolder] [initialMap]`.
- **`desktop/`**: `main.cjs` (starts the Core hidden on a free port, serves `shell\dist` on 127.0.0.1, popouts, unsaved-changes question, Core log + restart dialog, window state), `paths.cjs` (layout, Core arguments, static server), `package.mjs` (stages the Core, its tools, DLLs, MSVC runtime, `core\assets`, the Shell; runs @electron/packager), `tests/` (`npm test`, node:test, 5 pass). `npm start` runs it from the working tree.
- Checked with the packaged app: Core starts, Shell connects (WebGL2), popout window opens, killing the Core shows the dialog and Restart reconnects, closing ends the Core. Built with `notests` (the Core's full suite was not run for this; Shell typecheck clean).
- **Open:** no app icon yet (put `desktop\icon.ico`, it is picked up); settings live inside the folder (`resources\core\settings.json`), so it must not go to Program Files; no installer / code signing / auto-update; the Electron `vulkan-1.dll` in the app root is not on the Core's DLL path (the Core uses the system Vulkan loader, as before).

## Update 2026-10-17 (third): smooth lightmaps (REQUIREMENTS §30)
- **Cause of the "sharp pixels" in diffuse light:** not the bake (super sampling works: 1 vs 8 sub-texel positions changes ~33k texels of demo_lit, CPU, GPU and live), but bilinear filtering, whose slope jumps at every texel border.
- **Fix:** bicubic B-spline lightmap lookup (4 bilinear fetches) in the editor's Lighting-only / Lit views and in the game's `fs_bsp.sc` for rBSP v2 (all three games, recompiled, 0 failed); lightmap padding 2 (default and minimum), since the lookup reads 2 texels around a chart. Old exported maps were packed with padding 1: re-export them.
- Tests: Core layout / settings / bsp / live bake cases (49) pass, Shell 902 pass, typecheck clean; checked in the editor's Lit view. **Open:** not checked in the game.

## Fix 2026-10-17: two Cores on one port
- The user saw no sky in the editor nor in the export: the Shell was talking to an OLD Core (a build from before the sky work, started from `.claude/worktrees/happy-brattain-1e55ef` the day before) that still listened on 8787 next to the new one from `run.bat`. Asio's default SO_REUSEADDR let both bind the port on Windows.
- `ws_server.cpp`: the port is bound with SO_EXCLUSIVEADDRUSE on Windows (reuse_address elsewhere); `main.cpp`: a port in use ends the Core with "cannot listen on 127.0.0.1:<port> ... Is another mapeditor_core already running" (exit 1). Checked both ways (a fixed Core refuses a taken port; an old build cannot join a fixed Core's port).

## Update 2026-10-17 (fourth): several exporters, OBJ (REQUIREMENTS §31, DESIGN "Exporters")
- **Exporter interface** (`core/exporter.h`): id, name, extension, run; `prepareExport` (bake + rBSP in memory) is shared. Registry: `rbsp`, `obj`.
- **The format is the game's:** `game_profile.json` `"exporter": "obj"` (default `rbsp`). File > Export and Play map follow it; `export_map` message (alias `export_bsp`).
- **OBJ exporter:** `<name>.obj` (geometry read back from the compiled rBSP, metres / Y up, `vl` = lightmap UVs), `<name>.json` (entities), `<name>.lightvol` (binary light volume), `<name>/lm_NNNN.exr`. `bsp_export_tool ... maps/x.obj` works from the command line.
- Tests: Core exporters (3) + bsp / sky / router / settings cases (39) pass; Shell 906 pass (new `tests/exportFormat.test.ts`); typecheck clean; menu checked in the browser.
- **Open:** the OBJ carries the base light style's lightmap coordinates only (the other styles' pages are written but not referenced); no .mtl file.

## Update 2026-10-17 (fifth): settings in the game folder (REQUIREMENTS §32)
- `<assets>/editor_settings.json` + `<assets>/lighting.json`; `settings.json` next to the Core keeps the game folder to open and the recent maps.

