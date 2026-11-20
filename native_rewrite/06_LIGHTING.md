# 06 — Live lighting: the exact bake, on the GPU, in place

> **2026-11-20: the live lightmap bake described here is frozen** (user's decision, 12_RT_VIEWPORT.md §11 A). Lit is the ray-traced
> viewport of 12. What stays in force from this file: the light model and parity sources (§0.1, §1), the acceleration structure rules (§5,
> reused by R1), the invalidation table (§7, used by R4 as the region rule) and the export (§13). T6.1–T6.4 code remains but gets no new
> tasks.

Read DESIGN §6.4–§6.11 first: they define the light model, the layout, the live baker's rules and what the user expects to
see. This file defines how the native app runs **the same model** without the transport, with GPU-resident results, and
with less invalidation.

## 0. Principles

1. **Same model as the export.** Lights (`core/include/core/lights.h`), layout rules (`lightmap_uv.h`), texel samples
   (`lightbake.h` `TexelSampleCache`), the estimator (`core/shaders/lightmap_direct.comp`, `lightmap_gather.comp`,
   `lightmap_ao.comp` and their CPU references in `core/src/lightbake.cpp`), light styles (`light_styles.h`), light volumes
   (`lightvol.h`). New shaders are **ports** of the Core shaders with a different data layout, never new maths.
2. **Same live rules as `core::LiveBaker`** (`live_bake.h`, DESIGN §6.6): progressive passes with new random numbers;
   direct light as a running mean; indirect as a moving average over `indirectWindow` passes; the first pass of a stale texel
   replaces its old value; the old value stays visible until then; the bounce source `S = Db + I` is zeroed where invalidated
   (stale light must not circulate); direct light is never denoised. `LiveBaker` stays in the Core as the **reference**:
   tests compare the native GPU bake against it.
3. **Everything stays on the GPU.** Per-texel results, the bounce source, the display pages and the light volume live in
   device memory. The CPU only decides *what* to bake next and uploads geometry deltas.
4. **What the camera sees comes first** (view prioritization), then what is near the change, then the rest.
5. **Invalidate the least possible** — per light class and per region (§7).

## 1. Parity contract

For the same document, settings and `lighting.json`:
- **Layout:** chart *shapes* (which faces form a chart, unfolding, size, texel lattice) equal those of
  `core::buildLightmapLayout` for the faces of each chart group (§3). Chart *placement* (page, x, y) may differ; the texel
  lattice is world-aligned per face (`LightmapFace::texel`), so a chart's texels sit at the same world points wherever it
  is placed.
- **Samples:** the texel samples of a chart equal those `TexelSampleCache::build` makes for that chart (same positions,
  normals, sub-texel offsets, buried texels dropped).
- **Values:** after `maxPasses` passes the native result and `core::LiveBaker`'s result for the same scene agree within the
  noise: mean relative difference < 1 % and 99th percentile < 5 % over texels with value > 0.01 (test maps in
  09_TESTING_AND_PERF.md §1). Against the export's final bake (more samples, denoised): mean < 3 %.
- **Differences on purpose (editor only):** the playable-area visibility culling is skipped (`skipVisibility`, DESIGN §6.11);
  placement differs. **Relief is included** in the live bake (it was left out of the current one only because the Shell had to
  send it as a second snapshot; in-process it is just geometry).

If a parity check fails by more than the bounds and the cause is not noise, stop and ask the user.

## 2. Inputs (`light/inputs.h`, part of the snapshot)

`LightingInputs { std::vector<core::Light> lights; ambient; style table; std::shared_ptr<const core::SolidSet> solids;
core::LightingClasses classes; level settings (worldspawn keys: _light_scale, _ao_*, exposure, sky, quality); live
settings (from lighting.json, DESIGN §6.6 "Config"); albedo library; sky radiance }`. Built by the scene builder with the
same functions the Core's `Scene` uses (`Scene::lightScene`, `Scene::lightSolids`, `LightingClasses::fromProfile`); task
T5.1 lists which `Scene` member functions to split out so they run on a `core::Document` without a `Scene`.

## 3. Incremental layout (`light/layout.*`)

### 3.1 Chart groups
Faces of one (instance, entity) joined by shared edges that pass the merge test of `lightmap_uv.cpp` (same density scale
/ cascade, same style set, dihedral angle ≤ `mergeAngleDeg` or a smooth edge) form a graph. Its **connected components are
chart groups**. The island growth of `buildLightmapLayout` never leaves a component, so charting a component alone gives
the same charts as charting the whole level (the islands are seeded by face area with a stable sort — within one component
the order is the global order restricted to it).

### 3.2 Core refactor (task T5.2)
Split `core::buildLightmapLayout` (`core/src/lightmap_uv.cpp`) into stages **without changing its output**:
```cpp
struct LayoutFaces;                                     // faces (A./B. of the current code): pieces, normals, scale, styles, adjacency
LayoutFaces prepareLayoutFaces(const std::vector<LightSurface>&, const LightingClasses&, const LightmapSettings&);
std::vector<std::vector<int>> chartGroups(const LayoutFaces&);            // connected components (face indices)
struct ChartShape { std::vector<int> faces; int w, h; /* per-face affine maps without the placement offset */ };
std::vector<ChartShape> buildCharts(const LayoutFaces&, const std::vector<int>& group);   // step C. (islands) for one group
LightmapLayout placeCharts(const LayoutFaces&, std::vector<ChartShape>, const LightmapSettings&);  // packing (lightmap_pack.h)
// buildLightmapLayout = prepare -> groups -> buildCharts per group (in the current global order) -> placeCharts
```
Acceptance: `core_tests -tc="*lightmap*"` passes; `buildLightmapLayout(...).toJson()` is byte-identical before and after for
`core/assets/maps/*.map.json` and lvl2 (dump to files, compare); export parity (09 §2).

### 3.3 Live re-layout per change set
1. Dirty faces = faces of changed brushes (old and new versions) and faces whose cascade / style set changed.
2. Every live group containing a dirty face is dissolved. Its remaining faces + the new faces are regrouped with
   `chartGroups` run on that subset plus the faces they share an edge with (edge lookup: a hash of quantized edge
   endpoints per entity, kept up to date incrementally). If a new group touches an untouched group, dissolve that one too
   and repeat until stable.
3. `buildCharts` for every new group (jobs, in parallel).
4. Placement: §3.4. Unchanged groups keep their charts and placement.

### 3.4 Live placement (`light/page_allocator.*`)
- Pages are layers of 2D-array images of `atlasSize²` (from lighting.json; default 1024).
- Each page has a rectangle allocator that supports **free** (guillotine with a free-rectangle list, merging free neighbours;
  padding per `LightmapSettings::padding` included in the rectangle, as today).
- New charts: first fit, tallest first. No page has room → add a page (grow the image arrays by 8 layers, copy old layers on
  the GPU).
- **Repack:** when used area / allocated area < 60 % and the editor was idle 5 s, or on the user's request (Lighting menu),
  pack all charts with `placeCharts` and **move** texel data (GPU image copy per chart rectangle, all layers and buffers);
  no re-bake.
- A chart that moved needs new lightmap UVs in the render meshes: the layout publishes `LayoutVersion` with a per-chart
  offset table; the renderer's vertex shader adds the offset of the vertex's chart (vertex attribute `chartId`), so moving a
  chart never re-uploads vertices.

## 4. Texel samples (`light/samples.*`)

- **Core refactor (T5.3):** expose per-chart generation from `TexelSampleCache` (`core/include/core/lightbake.h`): 
  `TexelSamples buildChartSamples(const LightmapLayout&, int chart, const std::vector<LightSurface>& piecesOfChart, const SolidSet*)`,
  equal to the slice of `buildTexelSamples` for that chart (test against `core/tests/texel_reference.h`). The "first claim
  wins" rule only matters inside a chart (charts never overlap), so per-chart generation is exact.
- Fine GI grid entries (`giScale`, `buildGiSamples`) per chart likewise.
- GPU storage: one **sample arena** (device-local storage buffer, sub-allocated per chart with VMA virtual blocks):
  `{ float3 pos; uint randomId; oct16x2 normal; uint16 page; uint16 x, y; uint subFirst }` + sub-texel positions
  (`superSamples`) + GI grid entries. Uploaded per changed chart through the upload ring.
- **Chunks:** a chart's texels are cut into chunks of `chunkTexels²` (16², as now). A chunk is the scheduling unit. Chunk
  records `{ chart, rect in page, sample range, grid range, world bounds, state }` live on the CPU (bake thread) and mirrored
  on the GPU.

## 5. Acceleration structures (`light/accel.*`)

- **Occluders** = what `core::buildOccluders` makes today (shadow-casting classes, relief replacing flat pieces, models with
  `castsShadow`; transparency per `core/include/core/raytrace.h`), but **one BLAS per brush** (and per model entity, per
  relief face set) instead of one mesh for the level. lvl2 has ~2k brushes; terrain and imported maps may have ~10k: fine
  for a TLAS.
- Per-instance `instanceCustomIndex` → an object record (buffer device address) with: triangle attributes (lightmap UV +
  page per corner, for the S look-up at a hit; albedo UV + texture index + LOD; transparency record), flags.
- **Builds:** new / changed BLAS with `PREFER_FAST_BUILD`; after 2 s without edits to that object rebuild it with
  `PREFER_FAST_TRACE | ALLOW_COMPACTION` and compact. TLAS: rebuilt (not updated) on the bake queue at the start of any
  step whose BLAS set changed.
- **Cutouts:** with `VK_EXT_opacity_micromap`, build an OMM per cutout texture (4-state, subdivision level from texture
  size, max 9) and attach to the triangles that use it; triangles with translucent textures stay non-opaque and use the
  existing transmittance logic. Without OMM: the current candidate loop (`surfaceAt()` in the shaders) — same results.
- Optional later (M12): `VK_NV_partitioned_acceleration_structure` for the TLAS, `VK_NV_cluster_acceleration_structure` for
  relief BLAS.

## 6. GPU storage of results (`light/results.*`)

Per style layer (base = style 0; more only for styled lights, as `LiveBaker` keeps one baker per style):

| Buffer / image | Per | Content |
|---|---|---|
| `D_local`, `D_sun`, `D_sky` | sample | direct light of point+spot+rect / directional / sky lights: running mean (rgb) + pass count |
| `Db` | sample | bounce-weighted direct (all classes), running mean |
| `I` | sample or GI grid entry | indirect, moving average over `indirectWindow` |
| `AO` | sample | occluded fraction (NaN = not computed), as `LiveBaker::occlusion` |
| `stats` | sample | passes of direct / indirect, sum and sum² of luminance (convergence) |
| `S` | atlas texel (RGBA16F array image) | bounce source = Db + I, raw (never filtered), valid flag in alpha |
| **display pages** | atlas texel (RGBA16F array image) | what the renderer samples: `max(0, (D_local + D_sun + D_sky + ambient + I_shown) · aoFactor)` with gutter fill |

`I_shown` = the à-trous-filtered indirect (§9) or the OIDN result for converged pages. The **ambient** is a constant per
layer (worldspawn `_ambient`), composed at display time, so changing it re-bakes nothing but the bounces it feeds.

## 7. Invalidation (`light/invalidate.*`)

Driven by the ChangeSet (03_DOCUMENT.md §6). `influence(light)` is the Core's (`live_bake.cpp`, in an anonymous namespace: task T6.6 exposes it in `live_bake.h` unchanged), `indirectRadius` from the
live settings (40 m default).

| Change | D_local | D_sun | D_sky | AO | I / S |
|---|---|---|---|---|---|
| point / spot / rect light added, removed or changed | chunks in old ∪ new influence | – | – | – | refresh within `indirectRadius` of the influence (if `bounce` > 0) |
| directional light changed | – | all | – | – | refresh all |
| sky light or sky texture changed | – | – | all | – | refresh all |
| `_ambient` changed | – (display only) | – | – | – | refresh all (the ambient feeds bounces) |
| `_light_scale`, exposure | recompose / display only (exposure never re-bakes, DESIGN §6.7) | | | | `_light_scale`: refresh all |
| brush geometry changed (box B = old ∪ new) | chunks within the largest local light radius of B | all (one cheap shadow ray per sub-sample) | chunks within `skyRadius` (setting, default 64 m) of B | chunks within `_ao_radius` of B | refresh within `indirectRadius` of B; the brush's own new texels start stale |
| face texture / projection only (albedo changes) | – | – | – | – | refresh within `indirectRadius` |
| class change (casts shadows / receives lightmaps) | as geometry | | | | |
| AO settings | – | – | – | all | refresh all |
| quality (`maxPasses`, `directSupersample`) | per `LiveBaker::setQuality` rules | | | | |

"Stale" = the class's next pass replaces instead of averaging; "refresh" = indirect restarts as `LiveBaker::markStale`
does, and **S of those texels is zeroed at once**. Displayed values stay until replaced (no flash). Tests (T6.6, T7.6): moving a
light leaves every texel outside its old ∪ new influence ∪ indirect radius bit-identical in every buffer.

## 8. Publishing to the renderer without races (`light/mailbox.*`)

The bake writes per-sample buffers on the compute queue; the renderer samples display pages on the graphics queue. They
never touch the same memory at the same time:
- After each bake step the bake thread runs a **compose** dispatch that writes, for every chunk changed since the last
  *consumed* mailbox, the chunk's display texels (gutter included) into a **mailbox buffer** (ring of 3 host-invisible
  device buffers, each a list of `{page, x, y, rgba16f}` runs per chunk rectangle) and signals `bakeTimeline = k`.
- At frame start the render thread takes the newest mailbox whose value is ≤ the completed `bakeTimeline`, records a
  scatter dispatch (graphics queue) that copies it into the display page images, a barrier, then the draws; it then marks
  mailbox k consumed (atomic). It never applies older mailboxes (each mailbox holds the full current values of every chunk
  dirty since the last consumed one).
- A mailbox slot is reused only after the render frame that consumed it signalled `renderTimeline` (checked without
  blocking; if no slot is free the bake skips composing this step and keeps the chunks dirty).
- Chart moves (§3.4) travel in the same mailbox together with the new `LayoutVersion`, so UVs and texels switch in the
  same frame.

## 9. Scheduling and budget (`light/scheduler.*`, bake thread)

**Visibility feedback.** The Lit fragment shader marks the chunk of the texel it samples in a `visibleChunks` bitset
(storage buffer, `atomicOr`) for 1 pixel in 16 (`(x & 3) == 0 && (y & 3) == 0`). The chunk is found through a per-page
`chunkMap` image (R32UI, one entry per 16×16 texel cell). The bitset is copied to host-visible memory each frame and read by
the bake thread one frame later; a chunk counts as visible for 0.5 s after it was last marked.

**Priorities** (highest first), each ordered by distance to the active camera:
1. visible chunks with a stale class (any of D_local, D_sun, D_sky, AO, I stale);
2. visible chunks below `maxPasses`;
3. non-visible stale chunks within `indirectRadius` of the last change;
4. light volume probes near the last change (§11);
5. other non-visible stale chunks;
6. non-visible chunks below `maxPasses`;
7. idle work: fast-trace BLAS rebuilds, OIDN on converged pages, repack.

Within a step, a chunk runs only the passes it needs (a chunk whose only stale class is D_sun runs the sun pass).

**Budget** (Frostbite's controller, Ray Tracing Gems ch. 23 §2.7): GPU time per step target = 4 ms while the user interacts
(mouse button down, a key held, camera moving) and 12 ms otherwise, measured with timestamps; `sampleRatio *= 0.9 ·
clamp(target / measured, 0.25, 1)` when over by more than 15 %, `/= 0.9` when under; texel count per step = ratio ×
`gpuTexelsPerStep`. Hard cap 50 ms per submission (far below the driver timeout). When the window is not active: budget 0
unless the setting "Bake in background" is on (default on).

## 10. Passes (`native/shaders/`)

| Shader | Port of | Notes |
|---|---|---|
| `direct.comp` (ray query) | `core/shaders/lightmap_direct.comp` | one dispatch per class needed; writes the class's buffer and its `Db` share; `superSamples` sub-texel positions; area / sky / rect sampling unchanged |
| `ao.comp` | `lightmap_ao.comp` | |
| `gather.rgen` + `gather.rmiss` + `gather.rchit` (ray tracing pipeline) | `lightmap_gather.comp` | cosine-weighted rays, R2 sequence with hashed start (`sampleGi`), hit → bilinear S at the hit's lightmap UV × albedo (textured when flag bit 1) × distance window. With SER (`GL_EXT_shader_invocation_reorder`, in glslang of SDK 1.4.363): `hitObjectTraceRayEXT` → `reorderThreadEXT(hit, hint, bits)` with the hit's texture index as hint → read hit attributes with `hitObjectGet*EXT` → shade in the ray-gen shader. Without SER: same code without the reorder call. Compute fallback: a straight port of `lightmap_gather.comp` |
| `accumulate.comp` | `LiveBaker::refineOnGpu` merge logic + `accumulateFine` | running mean / moving average / first-pass replace; writes S |
| `compose.comp` | `LiveBaker::renderTile` | display value, gutter fill (average of baked neighbours, alpha 0 = no value) into the mailbox |
| `atrous.comp` | new (Frostbite §2.8, Fig. 11) | 3 iterations of a 5×5 à-trous filter on I in lightmap space, edge stops: same chart id, luminance ratio; only chunks being composed plus one chunk of border |
| `lightvol_*.comp` | probe parts of `lightmap_direct.comp` / `lightmap_gather.comp` + `core::fitProbeDirectFirst` | §11 |

Random numbers: per sample `randomId` and pass offset as today (`Job::offset = pass × samplesPerPass`), so results are a
deterministic function of (scene, pass count) for a fixed schedule — tests rely on it.

## 11. Light volume (`light/lightvol.*`)

- Grid: `core::buildLightvolGrid` with the level's settings; probe validity: `core::solidProbes` (DESIGN §6.7, §6.10).
- Each probe = `normals` (32) samples with the same estimator as texels (direct per class + gather from S), resident like
  texel samples; probes are grouped in bricks of 4×4×4 for scheduling (priority 4, §9), invalidated like texels (§7, by
  probe position).
- Fit: the Core's fitting (`fitProbeDirectFirst`, DESIGN §6.7 "Fit") ported to `lightvol_fit.comp` — or run on the CPU per
  brick from a read-back when the GPU port is not done yet (task order in 08_MILESTONES.md).
- Output: 3D textures `ambient` (RGBA16F), `directed` (RGBA16F), `dir` (RGBA8 oct) for the renderer: brush entities that
  are lit from the volume sample it once at the centre of their bounds (DESIGN §6.7), models likewise.
- Huge grids (lvl2: 18 M probes at 1 m): never cap them (the user's rule); bricks are baked lazily, nearest to the camera
  and the change first; memory: only bricks touched so far are resident (sparse table of bricks).

## 12. Denoising

- Preview: `atrous.comp` every compose (cheap, chunk-local). DLSS Ray Reconstruction (11_NVIDIA_TECH.md) is a screen-space denoiser and does **not** denoise
  lightmap texels; it is only for the optional path-traced preview view.
- Converged pages (every chunk at `maxPasses`): OIDN `RTLightmap` filter on its GPU device (CUDA on NVIDIA, as
  `core/src/denoise.cpp` already chooses), fed from the GI grid exactly like `LiveBaker::beginDenoise` /
  `runDenoise` / `finishDenoise`. With `VK_KHR_external_memory_win32`, share the input / output buffers with OIDN
  (`oidnNewSharedBufferFromWin32Handle`) instead of copying through host memory. On a worker thread; a result for a page
  that was refined meanwhile is dropped (same rule as the Core).

## 13. Export

File > Export / Play map: hand the scene builder's typed `core::Document`, resolved prefabs and relief meshes **directly**
(no JSON: task T9.1 adds that entry point to `prepareExport`) to the Core's exporter (`core/exporter.h`, `bsp_export.h`)
with the lighting options, running on the app's Vulkan device (task T9.2), on a worker thread, with progress in the status bar (the Shell's
`exportStatus.ts` behaviour). The live bake pauses during an export (frees the GPU) and resumes after. The export does its
own full bake; the live results are not written to the export.

## 14. UI

Status bar: `Lighting: 87 % · 3 stale chunks visible · 4.1 ms/step · 9.8 Grays/s`. Lighting menu: Pause / Resume, Re-bake
all, Repack lightmaps, Show stale chunks (overlay tinting stale chunks red), Show chunk visibility (overlay). Stats panel:
per-pass GPU ms, rays/s, sample counts, pages, BLAS count / memory, mailbox lag (frames between bake and display).
