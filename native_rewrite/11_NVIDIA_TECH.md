# 11 — NVIDIA technologies: Streamline (DLSS, Ray Reconstruction, Reflex) and the rest

Written 2026-11-20 on the user's instruction: **use NVIDIA Streamline and other NVIDIA technologies to improve the editor's performance, and
note down how Streamline is to be used, especially DLSS and Ray Reconstruction.** The platform already requires an NVIDIA-class RTX GPU
(README "Decisions"), so vendor features are allowed; every one of them stays **optional at run time** (feature missing, driver too old, a
non-NVIDIA GPU: the editor runs without it and nothing else changes).

**Frame rate target: 100 fps** (user, 2026-11-20): the hardware is already powerful, so image quality is not traded for speed. **DLSS is allowed only at the Quality preset (or better: DLAA, native resolution)**; Balanced, Performance and Ultra Performance are not offered, not even as a hidden option. Where the 100 fps gate is met without DLSS, DLSS stays off.

Names of Streamline constants and functions below are from memory of the public SDK (github.com/NVIDIA-RTX/Streamline, programming guides
`ProgrammingGuide*.md`); **check each against the SDK headers when the task is done**, and write the corrected name here.

> **As built (T6.7, SDK 2.14.16):** names checked against the headers: `slInit(Preferences)` with `renderAPI = RenderAPI::eVulkan`, features
> `kFeatureReflex` / `kFeaturePCL`, `slIsFeatureSupported(feature, AdapterInfo{vkPhysicalDevice})`, `slGetFeatureFunction(feature, "slReflexSetOptions"...)`
> (the header helpers call the import library, so `gpu/streamline.cpp` resolves them by name from the loaded DLL), `ReflexMode::eLowLatency`,
> `PCLMarker::eSimulationStart ... ePresentEnd`, `slGetNewFrameToken`. Feature ids: DLSS 0, Reflex 3, PCL 4, DLSS-RR `kFeatureDLSS_RR` 1001.
> The interposer's `vkCreateInstance` / `vkCreateDevice` proxies add the extensions the plugins ask for (not `VK_KHR_present_id`: the app adds it).
> The Streamline zip has no Aftermath SDK. The signed production DLLs in `bin/x64` are used (the `development` folder has the unsigned ones).

## 1. What Streamline is, in this app

Streamline (SL) is NVIDIA's open-source integration layer for DLSS Super Resolution (DLSS-SR), DLSS Ray Reconstruction (DLSS-RR, "DLSS-D" in
the plugin names), DLSS Frame Generation, Reflex and PCL (latency markers), NIS and more. It works through an **interposer**: the app loads
`sl.interposer.dll` instead of `vulkan-1.dll`, SL hooks the Vulkan calls, and per-feature plugin DLLs (`sl.dlss.dll`, `sl.dlss_d.dll`,
`sl.reflex.dll`, `sl.pcl.dll`, `sl.common.dll`) are loaded on demand. Consequences for the structure of the app (01 §modules, 05 §device):

- **One place creates the instance and device:** `gpu/device.cpp`. Order: `slInit(prefs)` (before anything Vulkan) → load the interposer's
  `vkGetInstanceProcAddr` into volk (`volkInitializeCustom`) instead of the system loader → ask `slGetFeatureRequirements` for each wanted
  feature → add the instance / device extensions and extra queues it lists → create instance / device → `slSetVulkanInfo(instance, device,
  queue families)`. Features are enabled per use (`slIsFeatureSupported`, `slIsFeatureLoaded`); if SL cannot start, create the device the
  normal way (system loader) and set `Caps::streamline = false`.
- The **bake and the Core's GPU code share the same device** (one-device rule, 01). SL's interposer must therefore also be what the Core
  sees: T9.2 (the Core on the app's device) takes its function pointers from the app, not from its own loader — nothing to change in the Core
  if it already does that; if it does not, that is part of T9.2.
- Qt: the app creates the `VkInstance` itself and hands it to Qt (`QVulkanInstance::setVkInstance`), so the instance made through SL is what
  Qt windows use. Check this in T1.3's code before T6.7 and fix it there if it is not so.
- SL needs: development builds of the plugins for development (unsigned, on-screen watermark off with the `sl.dlss` development DLLs), the
  **signed** release plugins for anything the user ships. DLSS comes with its own licence and attribution text; accepting it is the user's
  decision (ask before downloading the SDK: file name, source, size — see the "Rules" in the README).
- Streamline binaries go under `native/third_party/streamline/` (headers, `sl.interposer.dll`, plugins, `nvngx_dlss*.dll`) like the other
  third-party pieces (02 §third party); `build_native.bat` copies the DLLs next to the exe. Nothing is committed that the licence forbids
  redistributing; the user is asked once about committing the DLLs or fetching them with a script.

## 2. Where each feature is used

### 2.1 Reflex + PCL — latency (cheap, do first): task T6.7
The editor shares one GPU between the viewport and the live bake; dragging a light while the bake runs is the case where input-to-photon
latency matters most. Use `kFeatureReflex` (`slReflexSetOptions` mode low latency; `slReflexSleep` once per frame in the render thread) and
`kFeaturePCL` markers (`SIMULATION_START/END` around the document / camera update, `RENDERSUBMIT_START/END` around command recording, `PRESENT_START/END`
around present, `TRIGGER_FLASH` is not needed). The bake's compute work already runs with its own budget (06 §9); Reflex does not replace it, it keeps the
render thread's queue short. **Without SL**: the same behaviour is possible with `VK_NV_low_latency2` directly (device extension; the SL
Reflex plugin uses it); T6.7 uses SL when present and `VK_NV_low_latency2` alone when only that is available. Shown in the Stats panel:
Reflex state and the measured render latency (PCL stats).

### 2.2 DLSS Super Resolution — Lit and Unlit perspective viewports: task T6.8 (after the Lit view exists, T6.4)
Why: four viewports at 1440p with the Lit view (bicubic lightmap sampling, per-pixel shading, selection outline) cost the most when many
viewports show large levels; DLSS-SR lets a viewport render at 67 % scale (the Quality preset) and reconstruct. Rules:
- **Only perspective viewports** (the ortho views are line-art on flat colour; DLSS does badly on thin lines and they are cheap). Setting per
  viewport: **Off / Ultra Performance / Performance / Balanced / Quality / DLAA** (`eUltraPerformance`, `eMaxPerformance`, `eBalanced`, `eMaxQuality`, `eDLAA`; render scale
  1/3, 1/2, 0.58, 2/3, 1). The user can choose every preset (menu View > DLSS, settings file key `dlss`, `--dlss`; an unknown value read from a file is Quality); only the
  performance measurements in this document are made with Quality and DLAA. Default **Off** until the user turns it on in
  Settings > Editor (an upscaler changes how the picture looks; the user decides). It is a tool to reach 100 fps where the plain renderer cannot, not a
  default.
- Resources tagged per frame with `slSetTagForFrame`: colour in (render resolution, linear HDR before tone mapping), depth, **motion
  vectors**, colour out (display resolution), plus `slSetConstants` (jitter, camera matrices, reset flag). The camera's motion vectors come from
  the depth reprojection of last frame's camera (cheap, no extra pass: the view state already holds `viewProj` and `prevViewProj`); objects dragged with a gizmo
  or a moving brush get per-object motion (the renderer already knows the previous transform of the objects in the live edit — add it to the
  object record in T6.8). The bake's progressive refinement changes lightmap texels every frame: that is *content change without motion*, so the
  lightmap lookup is **not** part of the jitter-sensitive signal; DLSS sees it as slowly changing colour (fine).
- **Reset flag** on: camera cut (frame selection, view preset change), viewport resize, a mode switch (Unlit / Lit), undo of a large change.
- **Draw order:** scene (render res) → DLSS → full-resolution overlays: grid, gizmos, selection outline, labels, entity boxes, the stale-chunk
  overlay (these are all drawn after the upscaler at output resolution; this is how `topLines_` / the overlay pass are already separated, T3.6).
- Picking stays on its own pass at output resolution (no upscaler involved).
- Texture LOD bias: apply the SL-recommended mip bias (`log2(renderWidth / displayWidth) - 1`) to the material samplers when the scale is
  not 1.
- **Gate:** a new line in 09 §3 "G-frame-4K": 4 viewports at 4K with DLSS **Quality** ≥ 100 fps on lvl2, and the same scene at 1440p DLSS Off
  unchanged (no regression when Off; G-frame stays 100 fps without DLSS). If Quality does not reach 100 fps, profile and report; do not add a lower preset. Measure with the Stats panel (GPU ms per viewport) before and after, in the commit message.

> **As built (T6.8, SDK 2.14.16):** `sl.dlss.dll` + `nvngx_dlss.dll` are copied next to the exe and `kFeatureDLSS` is requested in `slInit`; `slDLSSGetOptimalSettings` /
> `slDLSSSetOptions` come through `slGetFeatureFunction`, `slSetConstants` / `slEvaluateFeature` / `slFreeResources` through `GetProcAddress` on the interposer. Tags are
> local to the evaluate call (`slEvaluateFeature` inputs: viewport, `kBufferTypeScalingInputColor`, `kBufferTypeDepth`, `kBufferTypeMotionVectors`,
> `kBufferTypeScalingOutputColor`, lifecycle `eOnlyValidNow`, explicit extents). Options: `colorBuffersHDR = eFalse` (the scene is linear but already display-referred),
> `useAutoExposure = eTrue`. Constants: matrices copied unchanged from the column-major `Mat4`, jitter in pixels (Halton 2,3, `max(8, 8 (out/render)^2)` phases, the
> scene is shifted by the same value), `mvecScale = (1/renderW, 1/renderH)` with pixel-space vectors prev - cur from the compute pass `dlss_mvec.comp` (camera motion
> only), `depthInverted = false`, `motionVectorsJittered = false`. There are no per-object motion vectors: a change of the scene (`GpuScene::changeSerial`) resets the
> history and the loop draws `phases + 6` more frames. Settings: per viewport, no Settings > Editor default yet; `--dlss`, View > DLSS, JSON `dlss` in the viewport settings.
> The overlays are drawn after the resolve at the output size; the resolve writes the (nearest) render-size depth to the output-size depth buffer so they depth-test.
> **Measured gate (RTX 4070 Ti, 143 Hz display, lvl2, 4 x 3840x2160):** Quality 85.4 fps per viewport, DLAA 79.7, Off 140.1, Quality 1440p 141 (Off and 1440p are capped by the
> display refresh). The DLSS evaluate costs about 1.2 ms per 4K viewport on this GPU (without it the same run is at the display cap), so the gate (100 fps) is not met with four
> 4K viewports (the measurements use Quality and DLAA only; the lower presets are there for the user). See 08 T6.8.
>
> **As built (R2, Ray Reconstruction):** `sl.dlss_d.dll` + `nvngx_dlssd.dll` are copied next to the exe, `kFeatureDLSS_RR` (1001) is requested in `slInit`, `slDLSSDGetOptimalSettings` / `slDLSSDSetOptions` come through
> `slGetFeatureFunction`. Options (set every frame: they carry `worldToCameraView` / `cameraViewToWorld`): mode Quality / DLAA, `colorBuffersHDR = eFalse`, `normalRoughnessMode = ePacked`. Tags: `kBufferTypeScalingInputColor`
> (the noisy radiance), `kBufferTypeDepth`, `kBufferTypeMotionVectors`, `kBufferTypeScalingOutputColor`, `kBufferTypeAlbedo` (diffuse albedo), `kBufferTypeSpecularAlbedo` (a black image: no specular yet),
> `kBufferTypeNormalRoughness` (world normal, roughness 1 in w). **Tagging `kBufferTypeNormals` alone, for the packed mode, crashes inside the plugin** (a null dereference in `slEvaluateFeature`); the packed mode
> needs `kBufferTypeNormalRoughness`. The first RR evaluate of a viewport makes NGX clear its own image (`nv.ngx.dlssd.resource`) right after a layout transition of its own and the synchronization validation reports a
> WRITE_AFTER_WRITE hazard; `gpu::Device` counts messages naming `[nv.ngx.` as warnings. The viewport's DLSS setting (Quality / DLAA) selects RR in the ray-traced Lit path; with DLSS off the a-trous fallback runs.


### 2.3 DLSS Ray Reconstruction — only for a ray-traced *screen-space* image
RR is a denoiser (and upscaler) for **noisy ray-traced images in screen space**: it takes the noisy radiance of a few rays per pixel together
with guide buffers (diffuse albedo, specular albedo, normals, roughness, depth, motion vectors, and for specular the hit distance) and returns a
clean image. It is **not** a lightmap denoiser: our light is gathered per *texel of the atlas*, not per screen pixel, and there are no per-pixel
guide buffers for a texel. So RR does **not** replace `atrous.comp` / OIDN for the live bake (06 §12): that stays as specified. Using RR there
would need a fake screen-space image of the lightmap and would lose the "exact bake" parity.

**Superseded 2026-11-20:** the user made the ray-traced viewport *the* Lit mode and chose RR as its denoiser (12_RT_VIEWPORT.md §7, §11;
tasks R2–R8 in 08). The text below is the earlier proposal, kept for its notes on inputs; where it differs, 12 wins.
- (earlier) A **"Path-traced preview"** viewport mode: primary rays (or rasterised G-buffer) + 1-2 ray-query bounces from the *camera* using the same
  TLAS, lights and albedo library as the bake, denoised and upscaled by DLSS-RR. It shows dynamic things the baked light cannot: glossy
  reflections of the real scene, translucent glass, and the *current* un-baked state of an edit instantly (a moved brush is lit correctly
  the moment it moves, before the bake catches up). It is the answer to "Lit view lags behind a brush edit" without waiting for chunks, and it is a
  preview only (the export stays the baked result).
- Inputs (all produced by the existing rasteriser, no extra scene pass): G-buffer (albedo, specular albedo from the material's metal/rough
  fields when it has them, world normal, roughness, depth), motion vectors as in 2.2, the noisy radiance from a compute / ray-query pass
  that reuses `direct.comp`'s light evaluation (06 §10) per pixel; tags: `kBufferTypeScalingInputColor`, `kBufferTypeAlbedo`,
  `kBufferTypeSpecularAlbedo`, `kBufferTypeNormalRoughness`, `kBufferTypeDepth`, `kBufferTypeMotionVectors`,
  `kBufferTypeSpecularHitDistance` (names to verify). RR replaces DLSS-SR in this mode (it also upscales); it needs the DLSS-RR model
  `nvngx_dlssd.dll`.
- Cost / risk: an extra ray-traced pass per frame in that viewport only; off by default; one viewport at a time; RR runs at the same scale rule as DLSS-SR
  (Quality or native only) and the 100 fps target applies to the other viewports while it is on. If the user does not want a
  new mode, RR is simply not built and nothing else in the plan depends on it.
- If the mode is built, NVIDIA's open-source real-time denoiser **NRD** is *not* needed (RR replaces it); it would only be a fallback for GPUs
  without RR (non-RTX): then the mode is simply unavailable.

### 2.4 What is deliberately not used
- **DLSS Frame Generation / Multi Frame Generation:** adds latency and invents frames; the editor's interactions (dragging, picking, typing
  numbers into the transform) must show the real state every frame. Not used.
- **NIS / sharpening, DeepDVC, Latewarp:** no need.
- NRD: not needed, RR denoises the RT viewport (12 §11 C). The live lightmap gather / OIDN of 06 are frozen with the chunked bake.

## 3. NVIDIA technology outside Streamline (and where it already is in the plan)

| Technology | Use | Where |
|---|---|---|
| Shader Execution Reordering (`VK_EXT_ray_tracing_invocation_reorder`) | bounce gather on the RT pipeline | T7.2 |
| Opacity Micromaps (`VK_EXT_opacity_micromap`) | cutout textures in the TLAS | T8.4 |
| Partitioned TLAS, cluster acceleration structures (RTX Mega Geometry) | big maps, relief BLAS rebuilds | M12 |
| OIDN on CUDA | converged-page denoise (OIDN picks CUDA on NVIDIA); the OptiX denoiser is an alternative only if OIDN loses on the gate in 09 | T7.4 |
| `VK_NV_low_latency2` | Reflex without SL | T6.7 |
| **Nsight Aftermath** | GPU crash dumps: on `VK_ERROR_DEVICE_LOST` write a dump next to the log with the shader names (`VK_NV_device_diagnostics_config` + `VK_NV_device_diagnostic_checkpoints`), so a device-lost report from the user is debuggable. Dev-only (off by default, `--gpu-crash-dumps`) | T6.7 |
| **Nsight Graphics / NVTX** | frame and GPU trace: NVTX ranges around render passes / bake passes (`VK_EXT_debug_utils` labels are already in 05; NVTX adds CPU ranges for the bake thread) | with the first G-light measurement (T6.5) |
| NVIDIA Texture Tools / Neural Texture Compression | not an editor-performance item; the export owns texture formats | — |

## 4. Order and acceptance (the tasks are in 08)

1. **T6.7** Streamline bootstrap (+ Reflex/PCL, Aftermath): `slInit` before Vulkan, device through the interposer, graceful fallback, Stats panel
   rows; **ask the user before downloading the SDK** (name, source, size). Accept: with SL the editor opens, `--bench open` timing unchanged,
   Reflex reports active; with `--no-streamline` and with the interposer missing, the editor runs as before (test both).
2. **T6.8** DLSS-SR for perspective viewports (setting, resources, reset rules, overlays after). Accept: screenshots Off / Quality at 1440p (user
   looks), G-frame-4K (Quality, 100 fps), no validation errors, no ghosting trail when a brush is dragged (user confirms with a recording or shots).
3. **R1–R8** (08 M7): the ray-traced Lit viewport of 12, with DLSS-RR from R2 on (T6.8 first: NVIDIA integrates DLSS-SR before RR).
4. Everything else in the table is already a task.

Rules for all three: a feature failing at run time (init error, missing DLL, unsupported GPU) is logged once, shown in the Stats panel as
"unavailable: reason", and the editor continues without it. Settings keys live under `render/streamline/*` in the editor's QSettings.
