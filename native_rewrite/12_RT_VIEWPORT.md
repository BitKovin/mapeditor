# 12 — Ray-traced Lit viewport with temporal accumulation

Written 2026-11-20 on the user's instruction: *the light build is not instant because it still uses the chunked approach; the viewport
itself should ray trace with temporal accumulation, designed for an RT viewport, with literally no delay — not just a new frontend for
the bake.* **Status: approved 2026-11-20** with the user's decisions in §11 (RT only; Streamline and DLSS Ray Reconstruction first; NVIDIA
SDK code may be used and changed freely). The task order is in 08 (M6 / M7, tasks T6.7, T6.8, R1–R8).

## 0. Why the chunked bake cannot be instant

The live bake (06, T6.1–T6.4) computes light **per lightmap texel**, in chunks, on a separate thread, and only then shows it:

| Cost the user waits for | Where it comes from |
|---|---|
| layout, samples, chunks of the whole level before the first pass | 06 §3–§4, per snapshot epoch |
| texels the camera never sees are baked too (lvl2: millions) | priority helps (T6.5) but the work is still per texel, not per pixel |
| a change re-bakes every texel in the light's influence, visible or not | 06 §7 |
| bake thread → mailbox → scatter → next frame | 06 §8, one or more frames of lag by design |
| the bake needs a *complete* snapshot epoch (Core inputs rebuilt) | `LightingService::setInputs` |

The export does its **own** full bake (06 §13: "the live results are not written to the export"). So the live lightmap is only a
*preview* — and a preview is cheaper and faster computed **per screen pixel, where the camera looks, in the render thread, in the frame**.

## 1. What the RT viewport is

The Lit and Lighting-only modes of a perspective viewport compute the light of **each visible pixel** every frame with ray queries on the
render thread's own TLAS, using the **same light model as the bake** (06 §0.1: `lights.h`, `lightmap_direct.comp`, `lightmap_ao.comp`,
`lightmap_gather.comp`), and **accumulate over frames** in screen space. The picture is the Monte-Carlo estimate of exactly the integral
the bake estimates per texel, evaluated at the pixel's surface point instead of at a texel centre.

- **No bake step before the first frame.** Open a level → the first Lit frame is already lit (one sample per pixel, filtered).
- **No queue between an edit and the light.** The render thread uploads the changed lights / geometry and traces them in the same frame.
- **Work scales with pixels, not with the level.** lvl2 and a 6 × 6 room set cost the same per frame at the same resolution.
- **Converges to the reference.** With a still camera and no edits the accumulation is a plain running mean (Blender Cycles / Omniverse
  "Interactive" behaviour): after enough frames the pixel equals the bake's converged value for that point (§9 parity), and tracing then
  **stops** (a converged viewport costs only its raster pass).

What it does **not** show: lightmap resolution itself (texel density, seams, leaks between charts). That is what the final export bake
shows (§11 A: the live lightmap preview is frozen).

## 2. Light model per pixel (parity with the bake)

Shown colour (Lit) = `clamp(albedo · L · exposure + brightness)`, Lighting-only = `clamp(L · exposure + brightness)`, as `mesh.frag` does now, with

```
L = max(0, (ambient + Σ_styles w_s(t) · D_s + I) · aoFactor(o))        (06 §6 display value; aoFactor = lights.h formula on the mean occlusion o)
```

Each term is estimated per pixel per frame with the bake's own estimator, so its **expected value equals the bake's**:

| Term | Bake (per texel per pass) | RT viewport (per pixel per frame) |
|---|---|---|
| point / spot (sphere source) | `evalLight(centre)` × mean visibility over `areaSamples` disc points | light chosen by ReSTIR (§4), `evalLight` × visibility of **one** random disc point (same `sample2` distribution) |
| rect | Arvo form factor × visibility over rect points | same, one rect point |
| directional | `n·l` × visibility over cone directions | one cone direction |
| sky | cosine rays × `skyRadiance` where they escape | one cosine ray |
| dark lights (negative brightness) | summed signed, clamped at the end | ReSTIR target `|p̂|`, contribution keeps its sign; clamp only in `L` |
| AO occlusion `o` | mean of hit weights `(1 − d/r)^falloff` over `_ao_rays` | one ray, running mean; `aoFactor` applied to the **mean** (nonlinear: never to single samples) |
| indirect `I` | cosine rays, hit → bilinear `S` from the atlas × albedo(hit) × `albedo` × `window(d, far)` | one cosine ray, hit → `S` from the **world-space radiance cache** (§5) × the same factors |
| transparency | `transmit()` / `surfaceAt()` | the same functions on the RT scene's object records |

The surface point comes from the raster G-buffer (§3), not from a texel centre, so there is no `superSamples` and no buried-texel rule.
All shader maths is shared GLSL (`native/shaders/light_common.glsl` grows into the common include), so the bake shaders and the RT
viewport cannot drift apart.

## 3. Frame structure (render thread, per Lit viewport)

```
 (scene changed?)  RT scene update: BLAS of changed objects, TLAS rebuild            (graphics queue, same frame as the raster sync)
 raster G-buffer   existing mesh pass + MRT: normal (oct16), albedo (RGBA8), object id, linear depth, motion vectors
 history reset     per pixel: world position inside a change region of class c -> history of c restarts (§6)
 reproject         previous frame's history -> this frame (motion vectors; reject on depth / normal / object id mismatch)
 rt_lit.comp       one ray-query kernel per pixel: ReSTIR local lights + shadow ray, sun ray, sky ray, AO ray, GI ray + cache query
 cache update      sparse: refresh the radiance-cache cells queried in the last frames (§5)
 accumulate        per signal running mean / capped moving average, moments for variance
 filter            variance-guided a-trous on the demodulated signals, weight fading to 0 as the history grows (§7)
 compose           L per §2 -> mesh.frag's lit output; translucent pass (§8); overlays after (as now)
```

- **Primary visibility is rasterised**: the viewport already rasterises the scene with the exact triangles, textures, cutouts and
  selection; a G-buffer adds MRT outputs only. Primary rays would duplicate that work.
- **The RT scene is built from the render thread's `GpuScene`** (the meshes that are drawn), not from the Core's `buildOccluders` on the
  bake thread: what is traced is what is drawn, in the same frame. BLAS inputs are the existing vertex / index buffers
  (`ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY` usage), so no geometry copy. Instance **mask bits** carry the classes (casts shadows,
  receives light, sky, translucent, editor-only), so a class that is drawn but does not cast shadows is skipped by the ray mask, as
  `buildOccluders` skips it. The T6.3 code (`light/accel.*`: arenas, one BLAS per object, TLAS by `instanceCustomIndex`, object records)
  is reused; its input changes from Core occluders to `GpuScene` objects.
- **Edits:** a dragged brush rebuilds one small BLAS (`PREFER_FAST_BUILD`) and the TLAS (thousands of instances: sub-millisecond) in
  the frame the snapshot arrives; light edits only rewrite the light buffer (`packLights`). The fast-trace rebuild after 2 s idle stays.

## 4. Direct light: ReSTIR DI on the bake's lights

- **Light grid.** Point, spot and rect lights have a hard radius (`window(d, r) = 0` beyond it), so a world-space grid listing, per
  cell, the lights whose radius sphere touches it is **exact** (no light is ever wrongly culled). Rebuilt on the GPU when lights change
  (a compute pass over the light buffer; thousands of lights cost microseconds).
- **Initial candidates:** M (default 8) lights of the pixel's cell drawn uniformly, weighted by the unshadowed target
  `p̂ = |luminance(evalLight)|` (RIS, no rays).
- **Temporal reuse:** combine with the reprojected reservoir of the previous frame (M capped at 20 × the initial M); **dropped** for
  pixels whose direct history is reset by §6 (a moved light never survives in a reservoir).
- **Spatial reuse:** k = 3 neighbours within 30 px with similar depth / normal; **unbiased** combination (pairwise MIS or the 1/Z rule
  with a shadow ray per neighbour candidate, per "A Gentle Introduction to ReSTIR", SIGGRAPH 2023) so the converged mean equals the bake,
  not a biased look-alike.
- **Final:** one shadow ray to a fresh random point on the chosen light (§2 table) with `transmit()`.
- **Sun and sky** are global and few: one ray each per frame, outside the reservoir (no reuse needed, low variance). Later optimisation
  if the ray budget is short: put them into the reservoir as candidates (one shadow ray for all direct light).
- **Light styles:** direct light is accumulated **per style layer** (style 0 plus up to 3 more styles present in the view; more →
  those lights are sampled with their current style value and the history of that layer is kept short). Compose multiplies the layers by
  the animated style values (`light_styles.h`), exactly like the game's per-style lightmaps, so a flicker never smears the accumulation.
- **Implementation: RTXDI** (NVIDIA, `third_party/rtxdi`, the `Rtxdi/` shader headers). The editor is private and for personal use, so
  the user allows using and changing SDK code freely (§11 C; if the editor is ever published, these parts are rewritten). The headers are
  HLSL: the RT kernels that include them are compiled with **DXC to SPIR-V** (`D:\VulkanSDK\1.4.363.0\Bin\dxc.exe`, `-spirv
  -fspv-target-env=vulkan1.3`), the rest of the shaders stay GLSL; the bake's light functions (`evalLight`, `transmit`, …) are ported to
  HLSL once, in a shared header that the RTXDI bridge (`RAB_*` callbacks: light sampling, target function, visibility) calls. Our light
  types map onto RTXDI's polymorphic light records with our own sampling (a disc point for sphere sources, a rect point, a cone direction).

> **As built (lighting rules of the classes, after R3):** the Lit picture follows the game profile's classes like the bake does (`makeRtRules` in render/rt_lights.cpp): a class without lightmaps does not receive light (the G-buffer marks it with `normal.w = 0.6`; it is shown with the unlit look of the chunked Lit mode and nothing is traced for it), a class that does not block light (or an object without a casting surface) is in the TLAS with the receiver mask only, so shadow and AO rays pass it. The rules reach the GPU as per-object flags (`GpuScene::setRules`, draw `firstInstance`, `RtObject::flags`). Limit: faces textured with the sky material still block light (the rule is per object, not per triangle), glass / cutout transparency is not in the shadow rays yet.
> **As built (R6):** see 08 R6; the cache is per viewport (SHaRC's levels follow the viewport's camera), cleared on a restart, and its value is the bake's S = Db + I with every update path vertex feeding Db and each segment's throughput = albedo(hit) * `albedo` * window.
> **As built (R3):** see 08 R3. The reservoir is streamed per pixel and frame over the lights of the pixel's grid cell and is not kept between frames (reuse = R5); the history is the running mean of D, not of a reservoir.

## 5. Bounces: world-space radiance cache for S

The bake gathers bounce light from `S = Db + I` stored in the lightmap atlas. The RT viewport stores `S` in a **spatial hash grid** in
world space: **SHaRC** (NVIDIA RTXGI 2.0, `third_party/sharc`, HLSL headers compiled with DXC as in §4; changed where our model needs it,
e.g. the cell value is `S`, not outgoing radiance):

- **Cell key** = quantised position (cell size grows with distance to the camera: `size = base · 2^floor(log_b(distance))`, SHaRC's
  `sceneScale` / log base) + a normal bucket. 2^22 entries (~64 MB) by default; LRU-style eviction by last-used frame.
- **Cell value** = running estimate of `S` at that place: `Db` (direct light weighted by each light's `bounce` and the near-field factor,
  as `bounceNearFactor`) + `I` (one gather ray whose hit is again looked up in the cache — every frame adds a bounce of depth, as the
  bake's "every pass adds one more bounce").
- **Update:** each frame, for a sparse set of pixels (1 in 16, rotated), a short path is traced (direct at the hit with one light from the
  grid + one gather ray); results are added to the cells along it, then a resolve pass folds them into a moving average over about
  `indirectWindow` × 4 frames (the bake's window, scaled because a cell sees fewer samples per frame than a texel per pass).
- **Query:** the per-pixel GI ray's hit reads `S(hit)` and multiplies with the hit's **texture** albedo × `albedo` × `window(d, far)`, so
  texture detail of bounce colour is kept; the cache's coarseness only blurs *where* the bounce light lands, like the atlas does.
- **Invalidation:** cells inside a change region (§6, indirect radius) are **zeroed** (the bake's "S of those texels is zeroed at once":
  stale light must not circulate), then refill within ~10–20 frames.
- Shared by all viewports (world space), so a second Lit viewport starts with a filled cache.

## 6. History invalidation from the ChangeSet (no heuristic lag)

> **As built (R4 replaced, 2026-11-21):** no regions for the direct / AO signals. The accumulation restarts completely when `GpuScene::lightingSerial()` changes (a drawn object's shape, a lighting rule or the set of drawn objects) or the lights / trace settings change; an edit that keeps every shape (texture, projection) restarts nothing, because albedo is not in the accumulated light. The text below is the design for the radiance cache (R6) if it needs regions.

A game denoiser guesses that lighting changed (A-SVGF gradients, colour clamping) and lags or ghosts. The editor **knows** what changed:
every ChangeSet (03 §6) becomes, on the render thread, a list of world-space **change regions per signal class**, using the table of
06 §7 (light influence old ∪ new; geometry box grown by the light radius / `_ao_radius` / `indirectRadius`; sun → all of `D_sun`;
`_ambient` / exposure → compose only, nothing resets). The reset pass restarts the history of exactly those classes for pixels whose
G-buffer position lies in a region, and the cache zeroes its cells there. Everything outside keeps converging. Tests check both
directions (§9). Camera motion never resets anything (it only reprojects).

## 7. Accumulation and denoising

- Signals are accumulated **demodulated** (light without albedo; the model is purely diffuse, so albedo factors out exactly): `D_s` per
  style layer, `I`, `o`, each with its own sample count `N_c` and luminance moments.
- Mean: `h = h + (x − h) / min(N_c, N_cap)` with `N_cap` = 4096 while nothing changes (plain mean: converges to the reference) and
  per-class short caps only while that class is being edited continuously (e.g. `D` of a light being dragged: cap 4 until the drag ends).
- **Converged stop:** a pixel whose every class reached `N_target` (setting, default 1024, or a variance threshold like Omniverse's
  adaptive sampling "target error") is not traced; a viewport with every pixel converged skips `rt_lit` and the cache update.
- **Denoiser: DLSS Ray Reconstruction** (user's decision §11 B: Streamline first, it gives the best result; 11 §2.3). RR gets, per frame,
  the noisy radiance of this frame's samples (`albedo · L`, before exposure) and the guide buffers: diffuse albedo, specular albedo = 0,
  normal + roughness = 1 (packed), **linear** depth, motion vectors; it accumulates and upscales itself (Quality preset or DLAA only).
  Rules: RR's reset flag on the frame a change region covers most of the view (§6) and on camera cuts; while the camera is still, the
  view blends from RR's output to our own running mean (`w = min(1, N / 64)` per pixel), which is the converged reference and lets tracing
  stop. With RR on, no other denoiser runs (NVIDIA's rule).
- **Fallback without RR** (Streamline missing or failed at run time, `--no-streamline`, tests): edge-aware 5 × 5 à-trous (SVGF: depth,
  normal, object id, variance-guided luminance stop), 4 iterations on the demodulated signals with blend weight `1 − min(1, N_c / 32)`,
  so tests and a non-SL run still give a readable, unbiased-when-converged image.

> **As built (R2):** see 08 R2. The RR input is `albedo * L` (not demodulated) with exposure and brightness applied before RR, the albedo is also the diffuse albedo guide; RR is selected by the viewport's DLSS setting
> (Quality: render size from `slDLSSDGetOptimalSettings`, DLAA: native), the a-trous filter runs at the output size when DLSS is off or unavailable. Translucent faces and cutout holes are not in the G-buffer (the blended pass
> draws them Unlit over the result until R3). The history is a per-pixel running mean of the occlusion with reprojection by world position (4 taps checked by distance and normal); it is reset by the triggers listed in 08 R2.


## 8. Other surfaces

- **Translucent surfaces** (blended pass, no G-buffer, so no accumulation): the fragment shader reads the cache `S` for the indirect part
  and evaluates the direct light of at most 4 strongest lights of its grid cell with one ray-query shadow test each (centre of the light).
  Noise-free and approximate; translucent faces are rare.
- **Models** (prefab / model entities): lit per pixel like brushes (better than the game, which lights them from the light volume,
  DESIGN §6.7). A later option "Game look for models" samples the light volume instead.
- **Classes without lightmaps** and sky faces: as now (unlit / sky shading).

## 9. Correctness tests and gates

| Test | Check |
|---|---|
| RT scene | 100 000 random rays against the render-side TLAS equal `core::TriangleBvh` closest hits on demo_lit and lvl2 (T6.3's test, new input) |
| Direct parity | demo_lit, bounces off: 2 000 random pixels; CPU reference = `lightbake.cpp`'s `evalLight` + `visibility` at the G-buffer position with 4 096 samples; RT after 4 096 frames: mean rel. difference < 1 %, p99 < 5 % |
| Against the bake | same view, `core::LiveBaker` converged (bounces off) sampled through the lightmap at the same pixels: mean < 3 % (the difference is texel interpolation) |
| AO parity | as direct, occlusion `o` and `aoFactor` |
| Bounce parity | bounces on, against `core::LiveBaker` sampled at the pixels: mean < 5 %, user confirms the look |
| ReSTIR unbiased | converged mean with reuse equals converged mean without reuse (RIS only) within the noise |
| Invalidation | move a light: in the next frame no pixel shows the old light inside old ∪ new influence; every pixel outside keeps its history bit-identical |
| Latency | light drag and brush drag: the frame that receives the snapshot shows the new light (counted in frames, `--bench light_drag`) |
| Validation | 0 Vulkan validation errors |

**Perf gates** (09 §3, new): **G-rt-move**: lvl2, one 1440p Lit viewport, camera flying, RTX 4070 Ti: ≥ 100 fps with every other
viewport ortho (target split: raster 2 ms, `rt_lit` ≤ 5 ms, cache 1 ms, accumulate + filter 1 ms). **G-rt-still**: converged viewport
costs no more than Unlit + 0.5 ms. **G-rt-edit**: dragging a light keeps ≥ 100 fps. If a gate fails: profile and report; options are
DLSS Quality (11 §2.2, also cuts rays by ~55 %), sun / sky in the reservoir, checkerboard tracing for non-active viewports — never a
lower DLSS preset.

Ray budget estimate: up to 5 rays per pixel per frame (local, sun, sky, AO, GI) + ~0.1 for the cache = ~19 M rays per frame at 1440p;
shadow and AO rays are short and coherent. Measure in R1–R3 before promising the gate; the converged stop makes the still case free.

## 10. Tasks (replace T6.5, T6.6, T7.1–T7.4, T7.6, T7.7 of the lightmap path; order in 08)

Streamline comes first (§11 B), and NVIDIA's guide integrates DLSS-SR before RR, so: T6.7 → T6.8 → R1 … R8. T6.8 also builds what the
RT path needs anyway (jitter, motion vectors, linear depth, render / output resolution split, overlays after the upscaler).

| Task | Do | Accept |
|---|---|---|
| **T6.7** Streamline bootstrap | as 08 / 11 §2.1 (interposer, Reflex + PCL, fallback, Aftermath) | as 08 |
| **T6.8** DLSS-SR (Quality / DLAA) | as 08 / 11 §2.2 | as 08 |
| **R1** RT scene on the render thread | BLAS from `GpuScene` buffers, class masks, object records (vertex addresses, texture ids, transparency), TLAS per frame when changed; reuse `light/accel.*`; DXC in the shader build (HLSL kernels next to GLSL) | ray test of §9; one-object edit rebuilds one BLAS in the same frame |
| **R2** G-buffer, accumulation, DLSS-RR | MRT outputs, history buffers, `N_c`, converged stop, RR evaluation (tags of §7) and the still-camera blend; à-trous fallback; first signal: AO; Lit / Lighting-only switch to the RT path (AO only at this step) | AO parity (without RR); still camera converges and stops tracing; RR on: image clean while flying (user looks); resize / mode switch resets |
| **R3** Direct light (RIS, no reuse yet) | `rt_lit` kernel, shared HLSL light header (port of `evalLight`, `transmit`, …), light grid, all light types, dark lights, transparency, sun, sky | direct parity (CPU and LiveBaker); validation 0 |
| **R4** ChangeSet regions | 06 §7 table → regions per class; reset pass; RR reset rule | invalidation and latency tests |
| **R5** ReSTIR DI with RTXDI | RTXDI runtime + `RAB_*` bridge on our lights, temporal + spatial reuse, unbiased mode | unbiased test; noise (RMSE after 1 frame) on lvl2 ≥ 3× lower than R3 |
| **R6** SHaRC and bounces | SHaRC update / resolve / compaction / query on `S`, GI ray per pixel, invalidation by region | bounce parity; cache cells zeroed in regions |
| **R7** Styles, translucency | per-style layers, translucent pass | user confirms the look; styled light flickers without smear |
| **R8** Budget and gates | per-frame ray budget across viewports (active first), `--bench` scenarios (`open`, `orbit`, `light_drag`, `sun_turn`) | G-rt-move, G-rt-still, G-rt-edit |

## 11. Decisions (user, 2026-11-20)

- **A. RT only.** Lit and Lighting-only use the RT viewport only. The chunked live bake (T6.1–T6.4 code: `light/layout`, `samples`,
  `passes`, `results`, `mailbox`, `lit_mesh`, `light_link`, `lighting_service`, `render/lightmap_view`) is **frozen**: no further tasks
  (T6.5, T6.6, T7.1–T7.4, T7.6 dropped); it keeps compiling and its tests keep passing until it is removed. `light/accel.*` is reused by
  R1. The export's own bake (06 §13, M9) is unchanged.
- **B. Streamline first**, DLSS Ray Reconstruction as the denoiser ("it provides the best result"); the à-trous filter is only the
  fallback when RR is unavailable.
- **C. NVIDIA SDK code may be used and changed freely** (RTXDI, SHaRC, NRD, Streamline): the editor is private, for personal use. If it is
  ever published, those parts get an own rewrite. NRD is not needed (RR denoises).

## Sources

- Bitterli et al., *Spatiotemporal reservoir resampling for real-time ray tracing with dynamic direct lighting* (ReSTIR DI, SIGGRAPH 2020);
  Wyman et al., *A Gentle Introduction to ReSTIR: Path Reuse in Real-time* (SIGGRAPH 2023 course).
- RTXDI: https://github.com/NVIDIA-RTX/RTXDI (Vulkan through NVRHI + DXC; NVIDIA RTX SDKs licence).
- SHaRC: https://github.com/NVIDIA-RTX/SHARC, integration guide `docs/Integration.md` (update / resolve / compaction / query passes,
  `sceneScale`); RTXGI 2.0: https://github.com/NVIDIA-RTX/RTXGI.
- Schied et al., *Spatiotemporal Variance-Guided Filtering* (HPG 2017) and *Gradient Estimation for Real-Time Adaptive Temporal Filtering*
  (A-SVGF, HPG 2018): https://cg.ivd.kit.edu/atf.php.
- NRD (ReLAX / ReBLUR): https://github.com/NVIDIA-RTX/NRD.
- DLSS Ray Reconstruction inputs: https://github.com/nvpro-samples/vk_denoise_dlssrr ; Streamline DLSS 4 integration:
  https://developer.nvidia.com/blog/how-to-integrate-nvidia-dlss-4-into-your-game-with-nvidia-streamline.
- Omniverse RTX modes (Real-Time 2.0 vs Interactive path tracing with accumulation and adaptive target error):
  https://docs.omniverse.nvidia.com/materials-and-rendering/latest/rtx-renderer_pt_overview.html.
