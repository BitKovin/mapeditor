# 01 — Architecture

## 1. Goals and non-goals

Goals, in priority order:
1. **Nothing interactive waits.** Input → visible result in the same or the next frame for every edit. The UI thread never
   blocks on geometry, lighting, the GPU or the disk.
2. **Lit mode is the real bake, live.** Moving a light: its final direct light (shadows included) on screen within 1–2
   frames for what the camera sees; bounces converge in about 1–2 s. Moving / reshaping a brush: the same, plus geometry
   re-lit around it. Numbers in 09_TESTING_AND_PERF.md §3.
3. **One uniform program — no frontend / backend split.** One process, one document, one geometry pipeline, one lighting
   model, one GPU device. UI, tools, renderer, bake and export share the same memory; nothing is serialized or sent between
   them (JSON only for files, the clipboard and the optional automation socket that lets *outside* scripts drive the editor).
4. **Export unchanged.** File > Export produces the same files as `core/src/bsp_export.cpp` does today.

Non-goals (for this rewrite): Linux/macOS builds, a non-ray-tracing GPU fallback for Lit (Unlit must still work without ray
tracing), multiple users editing one document, any change to the map file format, any change to the engine.

## 2. Process and threads

One executable `mapeditor_native.exe`. Threads:

| Thread | Owns | Must never |
|---|---|---|
| **UI** (Qt main thread) | Qt widgets, input, the `Document`, the undo stack, the selection, tool state | wait for a GPU fence, a job, a file |
| **Render** | the graphics queue, swapchains, per-viewport frame loop, render pipelines | mutate the document |
| **Bake** | the async compute queue (falls back to the graphics queue when the device has one queue family), the live light scheduler | block the render thread longer than a queue submit |
| **Job pool** | CPU work for derived data (CSG, surfaces, layout, texel samples, BLAS input) | touch Qt or Vulkan objects other than mapped staging memory it was handed |
| **IO** | file reads / writes, texture decode, prefab loading | — |

Use the Core's `core::parallelFor` (`core/include/core/parallel.h`) inside jobs. The job pool itself is a small task queue in
`native/src/base/jobs.h` (fixed worker threads = hardware threads − 3, minimum 1; tasks have a priority; a task can be
cancelled by bumping a version counter it checks).

## 3. Data flow

```
 input ──► Tool ──► Command ──► Document (UI thread, mutable)
                                   │ commit
                                   ▼
                              ChangeSet + DocumentVersion (immutable, structurally shared)
                                   │
                                   ▼
                        SceneBuilder (jobs, per changed object)
                          ├─ derived surfaces (CSG, texplan, relief, decals)
                          ├─ render meshes
                          ├─ lightmap charts (incremental re-layout by chart group)
                          ├─ texel samples (per chart)
                          └─ BLAS inputs
                                   │ publish (latest wins)
                                   ▼
                          SceneSnapshot (immutable, versioned)
                         ┌─────────┴──────────┐
                         ▼                    ▼
                 Render thread           Bake thread
                 (draws latest)          (uploads deltas, rebuilds BLAS/TLAS,
                         ▲                schedules texels, runs passes)
                         │  lightmap atlas images, light volume texture
                         └────────── shared GPU memory ─┘
```

- **Immediate feedback before derived data is ready** (DESIGN §5.6 rule 1): while a brush's derived surfaces are being
  rebuilt, the renderer draws the brush's raw mesh from the `DocumentVersion` (it is already there). Derived results swap in
  when published. No frame ever waits for them.
- **Latest wins.** When a new `DocumentVersion` arrives while jobs for an older one run, jobs whose object changed again are
  cancelled (version check); results for objects that did not change again are kept.
- **Lighting is never shown "wrong-new".** A texel keeps its last value until the bake writes the new one (no black flash),
  exactly like the current live baker (DESIGN §6.6 "No flash to black").

## 4. Modules and directory layout

New top-level folder `native/` (the code). These specs live in `native_rewrite/`.

```
native/
  CMakeLists.txt            builds mapeditor_native.exe and native_tests.exe; add_subdirectory(../core) for core_lib
  src/
    base/                   jobs.h, version.h, log.h, hash.h (content hashes), timing.h
    doc/                    document model, JSON I/O, ids, commands, undo, selection, prefab expansion (03_DOCUMENT.md)
    scene/                  SceneBuilder, per-object caches, snapshots (04_SCENE_AND_DERIVED.md)
    gpu/                    device, queues, memory (VMA), descriptors, upload ring, acceleration structures, pipeline cache
    render/                 viewport renderer, render modes, picking, gizmos, overlays, text (05_RENDERER.md)
    light/                  live lighting: layout, samples, scheduler, passes, caches, denoise, light volume (06_LIGHTING.md)
    tools/                  editing tools (07_EDITOR_PORT.md)
    ui/                     Qt panels and windows
    app/                    main.cpp, MainWindow, docking, menus, actions, settings
    automation/             local JSON socket for scripts / AI (08_MILESTONES.md M9)
  shaders/                  GLSL sources; compiled to SPIR-V at build time
  tests/                    doctest cases (headless; GPU tests skip when no device)
  third_party/              qtads (source), vma (header) — see 02_BUILD_AND_TOOLING.md
```

Dependency rule (enforced by CMake target structure): `base` ← `doc` ← `scene` ← `light`, `render` ← `tools` ← `ui` ← `app`.
`gpu` depends only on `base`. `light` depends on `gpu` and `scene`; `render` depends on `gpu`, `scene` and `light` (the mailbox, the display pages, the lit mesh set). Nothing below `ui` includes a Qt header
except `render/viewport_window.*` (the Vulkan surface) — so `doc`, `scene`, `light` are testable without Qt.

Core is linked as `core_lib` (a static library: the same process, the same memory). Native code includes `core/...` headers
and calls them. The Core's own GPU code (`core/src/gpu_bake.cpp`: export bake, visibility) is given **the app's Vulkan
device** instead of creating its own (task T9.2), so there is one device. The Core's networking and transport parts
(`ws_server`, `router`, the tile / mirror side of `lighting_service`) are not used by the native app.

## 5. GPU model

- **One `VkDevice`** created by `native/src/gpu/device.cpp` with the extensions in 05_RENDERER.md §2.
- **Queues:** one graphics+present queue (render thread), one compute queue from a different family when the device has
  one (bake thread). Resources shared by both use `VK_SHARING_MODE_CONCURRENT` (simpler than ownership transfers; the cost
  is negligible for these sizes).
- **Synchronisation between bake and render:** two timeline semaphores, `bakeTimeline` (signalled by every bake step) and
  `renderTimeline` (signalled by every frame). The bake never writes what the renderer samples: it composes changed texels
  into a **mailbox** buffer; the render thread copies the newest *completed* mailbox (it checks
  `vkGetSemaphoreCounterValue`, it never waits for an unfinished step) into the display pages on the graphics queue before
  drawing. Full protocol: 06_LIGHTING.md §8.
- **Priority:** request `VK_QUEUE_GLOBAL_PRIORITY_HIGH` for the graphics queue and `MEDIUM` for the compute queue
  (`VK_KHR_global_priority`); if refused, create without it.
- **Memory:** Vulkan Memory Allocator (VMA, MIT, header-only) for every buffer and image.
- **Shaders:** GLSL 460 + extensions, compiled with `glslangValidator` at build time to SPIR-V embedded as C++ arrays
  (reuse `core/tools/spv_to_header.cmake`).

## 6. Error handling

- Vulkan errors are fatal only at start-up (no device → message box, app exits). After start-up a failed bake submission
  disables live lighting with a status-bar message and a log line; the editor keeps working Unlit.
- `VK_ERROR_DEVICE_LOST` → recreate the device once, re-upload everything from the latest snapshot; a second loss within a
  minute disables Lit.
- Document load errors are shown in a dialog with the Core's validation message; the previous document stays open.

## 7. Coding conventions

- C++20, MSVC `/W4 /permissive-`, warnings as errors in `native/` (not in third_party).
- Maths: `core::Vec3` (double) for document and CPU geometry (the Core and the file format are double); `float` only in GPU
  buffers.
- Ownership: `std::shared_ptr<const T>` for anything in a snapshot; never a raw pointer across threads.
- No exceptions across thread boundaries: jobs catch and return an error value.
- Every module header begins with a comment block: what it owns, which thread may call it, which spec section it implements.
