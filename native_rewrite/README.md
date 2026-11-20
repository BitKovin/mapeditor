# Native editor rewrite — specs and plan

**Status:** approved direction, not started. Written 2026-11-19 from the user's decisions in conversation.
**Audience:** the model (or person) that implements it, one task at a time. Everything needed is in these files; when
something is missing or ambiguous, **stop and ask the user** instead of guessing (see "Rules" below).

## What this is

The editor today is two programs: the **Shell** (TypeScript, WebGL2, runs in a browser / Electron) and the **Core** (C++20,
native, Vulkan). They talk over a WebSocket. Every edit sends the whole document as JSON (lvl2's map is 25 MB), the Core
re-derives everything, bakes light, encodes lightmap tiles and sends them back; the Shell needs a second lighting model
(shadow-map prediction, `shell/src/render/directLight.ts`) to hide the delay. Measured: the GPU is ~10 % of a bake; the rest
is CPU work around it and the transport. Light edits and brush edits in Lit mode are slow.

The rewrite is **one native Windows program**: Qt 6 for the UI, Vulkan for every 3D view and for the light bake, the
document in typed C++ memory, the Core's C++ libraries linked in directly. The Lit view shows the **exact bake** (the same
estimator and layout rules as the export) refined live on the GPU into the very texture the viewport samples.

## Decisions (made by the user — do not reopen)

| Topic | Decision |
|---|---|
| Lit view | **Exact bake, live.** Same lightmap + light volume model the export writes, baked on the GPU in place. A ray-tracing GPU is required. |
| UI toolkit | **Qt 6** (Widgets). Docking with Qt Advanced Docking System. |
| Migration | **Clean break.** The Shell is frozen (kept runnable, not deleted, not edited). The native editor is built up to parity. |
| Priority | **Light edits and brush edits in Lit** must become instant first. |
| **One program** | **One process, one address space, one GPU device.** No frontend / backend split, no IPC, no socket, no serialization between the UI and the engine: tools, renderer, bake and export all work on the same in-memory objects. The Core becomes a library linked into the app. Sending data between halves is what made the current editor slow. |
| Frame rate | **100 fps** is the target for the editor (viewport frames, also while the bake runs and while dragging); the gates in 09 §3 use 10 ms frames (p99 12 ms). |
| Visibility (PVS) | Stays baked at export (`core/src/bsp_vis.cpp`). Not part of this work. |
| Platform | Windows 10/11, MSVC, Vulkan 1.3+ with ray query. Development GPU: RTX 4070 Ti, driver 610.88, Vulkan SDK 1.4.363 at `D:\VulkanSDK\1.4.363.0`. |

## Reading order

1. [01_ARCHITECTURE.md](01_ARCHITECTURE.md) — process, threads, modules, data flow, directory layout.
2. [02_BUILD_AND_TOOLING.md](02_BUILD_AND_TOOLING.md) — prerequisites, CMake, shaders, how to run and test.
3. [03_DOCUMENT.md](03_DOCUMENT.md) — the in-memory document, JSON I/O, commands, undo, selection.
4. [04_SCENE_AND_DERIVED.md](04_SCENE_AND_DERIVED.md) — per-object derived geometry, caches, snapshots.
5. [05_RENDERER.md](05_RENDERER.md) — Vulkan device, viewports, render modes, picking.
6. [06_LIGHTING.md](06_LIGHTING.md) — the live bake (the reason for the rewrite). Read fully before any lighting task.
7. [07_EDITOR_PORT.md](07_EDITOR_PORT.md) — inventory of Shell features and where each goes.
8. [08_MILESTONES.md](08_MILESTONES.md) — **the task list.** Work through it in order.
9. [09_TESTING_AND_PERF.md](09_TESTING_AND_PERF.md) — test strategy, performance gates, parity checks.
10. [10_UI_AND_INPUT.md](10_UI_AND_INPUT.md) — actions, rebindable keys, command palette, UI improvements.
11. [11_NVIDIA_TECH.md](11_NVIDIA_TECH.md) — NVIDIA Streamline (DLSS Quality only, Ray Reconstruction, Reflex), Aftermath, other NVIDIA features; 100 fps target.
12. [12_RT_VIEWPORT.md](12_RT_VIEWPORT.md) — the Lit viewport: per-pixel ray tracing with temporal accumulation (ReSTIR DI / RTXDI, SHaRC, DLSS-RR), replaces the live lightmap bake of 06.

Background that stays authoritative: `DESIGN.md` (behaviour of every feature), `REQUIREMENTS.md` (what the user asked for,
with the user's words), `MCP_DESIGN.md` (AI level design). When this folder and DESIGN.md disagree about *behaviour*, DESIGN.md
wins; about *architecture of the native app*, this folder wins.

## Rules for the implementer

0. **Never add a process boundary.** No helper process, no local server between parts of the editor, no JSON (or any
   other encoding) to hand data from one part to another. Data moves by pointer (immutable snapshots, 01_ARCHITECTURE.md §3).
   JSON is only for files on disk, the clipboard and the optional automation socket for outside scripts (M9).
1. **One task at a time** from `08_MILESTONES.md`, in order. A task is done only when its acceptance checks pass. Tick it in
   the status table at the end of `08_MILESTONES.md` in the same commit.
2. **Only what the task asks.** No extra features, no refactors outside the task, no changes to the Shell (`shell/`) or the
   Electron app (`desktop/`). Core (`core/`) changes are allowed only where a task names them, and must keep every existing
   Core test passing and the export output identical (09_TESTING_AND_PERF.md "Export parity").
3. **Never duplicate an algorithm the Core already has.** Texture layout (`texplan.h`), CSG (`csg.h`), prefabs (`prefab.h`),
   lights (`lights.h`), lightmap layout (`lightmap_uv.h`), baking (`lightbake.h`, `live_bake.h`), light volumes (`lightvol.h`),
   export (`bsp_export.h`, `exporter.h`) are called, not rewritten. If a function is not reachable in the shape a task needs,
   the task says how to split it.
4. **Ask the user** (do not guess) when: a behaviour is not described in DESIGN.md / REQUIREMENTS.md / these files; a task's
   acceptance check cannot pass for a reason outside the task; something needs installing (Qt, packages); a decision table
   above would have to change.
5. **Tests are targeted.** Do not run the whole `core_tests.exe` (the CPU lighting tests take very long); run the cases
   the task names (`core_tests.exe -tc="*name*"`). Native tests: `native_tests.exe -tc="*name*"`.
6. **Processes:** stop only processes you started, by PID. Never `Stop-Process -Name` / `taskkill /IM` (the user's own
   editor may be running).
7. **Git:** commit per task with a message `native: <task id> <what>`. No AI attribution lines (no `Co-Authored-By` for
   an AI, no "Generated with"). Do not push unless asked.
8. **Big test maps live outside the repo** (e.g. lvl2 at
   `F:\Git\SdlWasmEngine\source\Games\ShooterGame\GameData\maps\lvl2.map.json`). Read them, never modify them; write
   outputs to a scratch folder and delete them afterwards.
9. **Comments and docs:** follow the Core's style: plain sentences that say *why*, references to DESIGN/REQUIREMENTS
   sections. Every new module starts with a header comment saying what it owns.

## Glossary

- **Document** — the map as authored (entities, brushes, prefab instances). Schema 0.2 JSON on disk.
- **Derived data** — everything computed from the document (CSG results, surfaces, render meshes, lightmap layout,
  texel samples, acceleration structures). Never saved.
- **Change set** — what one committed edit changed (object ids + kinds + world boxes). Drives every incremental update.
- **Snapshot** — an immutable, versioned view of the scene handed to the render and bake threads.
- **Chart / page / atlas** — lightmap terms, see `core/include/core/lightmap_uv.h`. A page is one `atlasSize²` layer.
- **Chart group** — a connected set of faces that may be unfolded into charts together (same instance + entity, joined by
  shared edges under the merge rules). The unit of incremental re-layout (06_LIGHTING.md §3).
- **Texel sample** — a lightmap texel that lies on a surface (`core::TexelSamples`).
- **S (bounce source)** — the atlas of last-pass radiance the bounce gather reads (DESIGN §6.6).
- **BLAS / TLAS** — bottom / top level acceleration structures (Vulkan ray tracing).
- **SER** — shader execution reordering (`VK_EXT_ray_tracing_invocation_reorder`).
- **OMM** — opacity micromaps (`VK_EXT_opacity_micromap`).
