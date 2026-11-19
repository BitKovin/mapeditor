# 05 — Renderer

## 1. Responsibilities

Draw every viewport from the latest `SceneSnapshot` (04_SCENE_AND_DERIVED.md §5), the current selection and tool overlays,
at the display's refresh rate while anything moves and **not at all** while nothing changes. Never wait for the bake.

## 2. Device (`native/src/gpu/device.cpp`)

Instance: Vulkan 1.3, `VK_KHR_surface`, `VK_KHR_win32_surface`, `VK_EXT_debug_utils` (when `--validation`).
Pick the discrete GPU with ray query; else the first device with Vulkan 1.3 (Unlit only; Lit menu entries disabled with a
tooltip saying why).

| Extension / feature | Need | Used for | Without it |
|---|---|---|---|
| Vulkan 1.3 core: dynamic rendering, synchronization2, timeline semaphores, buffer device address, descriptor indexing (runtime arrays, update-after-bind, partially bound), scalar block layout, 8/16-bit storage | required | everything | app does not start |
| `VK_KHR_swapchain` | required | viewports | — |
| `VK_KHR_acceleration_structure`, `VK_KHR_deferred_host_operations` | required for Lit | BLAS / TLAS | Lit off |
| `VK_KHR_ray_query` | required for Lit | direct light, AO, view prioritization, picking aids | Lit off |
| `VK_KHR_ray_tracing_pipeline` | required for Lit | the bounce gather pass (needed for SER) | gather falls back to the ray-query compute version |
| `VK_EXT_ray_tracing_invocation_reorder` | optional | SER in the gather ray-gen shader | no reorder call (same results) |
| `VK_EXT_opacity_micromap` | optional | cutout textures in the acceleration structure | alpha test in an any-hit / candidate loop (as the Core's shaders do today) |
| `VK_KHR_ray_tracing_position_fetch` | optional | hit triangle positions from the AS | read vertices through buffer device address |
| `VK_KHR_global_priority` | optional | high-priority graphics queue (the driver's per-family list from `vkGetPhysicalDeviceQueueFamilyProperties2` decides: requesting a priority it does not list crashed NVIDIA 610.88; on the dev machine HIGH is not listed for graphics, MEDIUM is for compute) | normal priority |
| `VK_EXT_host_image_copy` | optional | texture upload, lightmap read-back for "save preview" | staging buffer copies |
| `VK_KHR_external_memory_win32` | optional | sharing lightmap buffers with OIDN's CUDA device | copy through host memory |
| `VK_EXT_shader_atomic_float` | optional | float atomics in sample accumulation | integer fixed-point atomics |
| `VK_NV_partitioned_acceleration_structure`, `VK_NV_cluster_acceleration_structure` | optional, later (milestone M12) | faster TLAS / relief BLAS updates | plain KHR builds |
| `VK_EXT_descriptor_heap` | not used in v1 | — | descriptor indexing is the binding model |

All of these are exposed by the development GPU (RTX 4070 Ti, driver 610.88; checked with `vulkaninfo` on 2026-11-19). The
device creation code must check every optional feature bit and record what is on in a `DeviceCaps` struct that the rest of
the code reads; nothing else queries Vulkan features. The **Stats** panel lists `DeviceCaps`.

**Binding model (v1):** one global descriptor set with update-after-bind runtime arrays: `textures[]` (albedo, sky, UI),
`lightmapPages[]` (one 2D-array image per style layer set, §6 of 06_LIGHTING.md), `storage buffers` via buffer device
address in push constants / a per-frame uniform. Adding a texture = write one array element. (Descriptor heaps would also
work but add a second, newer path; not worth it until profiling says descriptor writes cost time.)

## 3. Viewports in Qt (`render/viewport_window.*`)

- Each viewport is a `QWindow` subclass with `setSurfaceType(QSurface::VulkanSurface)`, wrapped for docking with
  `QWidget::createWindowContainer`. One `QVulkanInstance` wraps the app's `VkInstance` (`QVulkanInstance::setVkInstance`)
  so `QVulkanInstance::surfaceForWindow` returns the `VkSurfaceKHR`. **Do not use `QVulkanWindow`** (it creates its own
  device and frame loop).
- The render thread owns each viewport's swapchain (present mode MAILBOX when available, else FIFO; 3 images). Qt resize /
  expose events set an atomic "needs resize" flag; the render thread recreates the swapchain before its next frame.
- Input events arrive on the UI thread and go to the active tool (07_EDITOR_PORT.md). The camera of each viewport is a small
  struct behind a mutex; the render thread copies it at frame start.
- **Redraw policy:** a viewport renders a frame only when its *dirty* flag is set: camera changed, a snapshot was published
  that touches what it sees, a lightmap page it samples was flipped (06_LIGHTING.md §8), selection / tool overlay changed,
  animated light styles are on, or a gizmo is hovered. Otherwise it presents nothing (GPU idle → full GPU for the bake).
- Layout presets (1, 2, 4 viewports; perspective / top / front / side) and per-viewport settings: port
  `shell/src/render/viewportSettings.ts` and DESIGN §6.1 / §6.2.

## 4. Scene data on the GPU (`render/gpu_scene.*`)

- **Arenas:** one large vertex buffer and one index buffer (sub-allocated with VMA virtual blocks), one storage buffer of
  per-object draw records `{firstIndex, indexCount, vertexOffset, objectId, flags, textureIndex range, transform}`.
- **Deltas only:** when a snapshot arrives, upload the render meshes of changed objects through a per-frame upload ring
  (64 MB, host-visible), free the old ranges after the frames that used them retired.
- **Draws:** per pass one `vkCmdDrawIndexedIndirect` over a GPU-written list (frustum culling in a compute pass over object
  bounds). Thousands of brushes cost one draw call per pass.
- **Vertex format:** position (float3), normal (oct-encoded 2×16), albedo UV (float2), lightmap UV (float2, 0 when none),
  lightmap page (uint16), texture index (uint16), face flags (uint16). Lightmap UVs refer to the live layout
  (06_LIGHTING.md §3); when a chart moves the bake thread publishes new UVs for its triangles as a small patch buffer.

**As built (T3.4).** `gpu_scene` keeps one 1 GiB vertex and one 512 MiB index arena; an object's mesh is uploaded when its pointer in the snapshot changes
(nearest to the origin first, at most three quarters of a ring slice per frame), its vertex block starts at a multiple of 40 bytes so draws address it by
`vertexOffset`, old ranges are freed once the timeline passed the last submit that may read them. Culling is on the CPU (frustum planes against object boxes,
two thousand boxes on lvl2) and writes three `VkDrawIndexedIndirectCommand` lists into the frame's ring slice (solid, translucent, volume); the compute pass
of this section is for scenes where that loop shows up in the frame time. Textures: `texture_table` (4096 `sampler2D` elements, update after bind, a meta
buffer with the transparency mode); an element shows the placeholder until the decoded image is uploaded, which waits for the GPU to be idle. Skyboxes take
six consecutive elements (or one for a panorama) and the fragment shader does the cube lookup itself. The window system: viewports have a D32 depth image;
the render thread makes one upload command buffer per loop iteration (scene sync, textures) before the viewports' frames.

## 5. Render modes and overlays

Behaviour is defined by DESIGN §6.2 and the Shell code it names; the native renderer reproduces it:

| Mode / overlay | Spec source |
|---|---|
| Unlit (flat colour per texture, fixed light) | `shell/src/render/renderer.ts` (UNLIT shaders), DESIGN §6.2 |
| Lit: `albedo × L × exposure`, bicubic lightmap sampling, light volume for non-lightmapped entities, light styles | DESIGN §6.7 "Lit view", REQUIREMENTS §30, `shell/src/render/litTextured.ts`, `lightStyles.ts` |
| Lighting only | DESIGN §6.7 |
| Models (`misc_model` .obj / .glb) with the engine's minimum brightness | `shell/src/render/modelRenderer.ts`, `shell/tests/minBrightness.test.ts` |
| Sky faces: cubemap sampled by direction exactly as the engine | `core/include/core/sky.h`, `shell/src/model/sky.ts`, REQUIREMENTS §28 K10 |
| Subtractive brushes as translucent volumes, playable area volumes | DESIGN §5.6 rule 1, §6.10 |
| Grid (ortho and perspective), selection outlines, gizmos, entity boxes / arrows / handles, light gizmos, size labels | `shell/src/render/orthoGrid.ts`, `gizmo.ts`, `shell/src/model/arrows.ts`, `entityHandles.ts`, `sizeLabels.ts`, REQUIREMENTS §17, §33 |
| Lightmap density, texel density, light volume samples overlays | `shell/src/render/lightmapDensity.ts`, `shell/src/model/texelDensity.ts`, `lightvolPreview.ts` |

The Shell's direct-light prediction (`directLight.ts`, `directLightPlan.ts`, `litStale.ts`) is **not ported**: the live bake
replaces it (06_LIGHTING.md).

Text (labels, sizes): a glyph atlas made once with `QPainter` into an R8 image, drawn as textured quads.

## 6. Picking (`render/picking.*`)

On the CPU, immediately on the UI thread (no GPU read-back latency): ray from the cursor → bounding-volume tree over object
bounds of the snapshot (rebuilt incrementally on publish) → exact ray / triangle test on the candidate render meshes
(two-sided Möller–Trumbore as `shell/src/render/picking.ts`). Mesh mode: nearest vertex / edge in screen space
(`shell/src/tools/meshPick.ts`). Objects without a render mesh yet are picked on their raw brush.

**As built (T3.5).** `render/picking`: `Picker` builds a bounding-volume tree (leaf of at most four, median split on the longest axis) over the objects of a scene
snapshot on `setSnapshot()` (0.3 ms for the 2124 items of lvl2, so it is rebuilt, not updated: the incremental update of this section is not needed yet) and
tests the candidates two-sided Moller-Trumbore. A solid brush is hit on its carved `core::LightSurface` pieces (a hole is clickable, a cut wall names the
subtractor's brush and face), a subtractive brush on its authored faces from the document, prefab content on its pieces and then reported as the instance. Point
entities are hit by a 0.5 m box until the entity definitions (T5) give the real boxes (no `angles` turn yet). Lvl2: 0.01 ms average, 0.035 ms worst per ray
over 400 rays. `boxSelect` takes what lies completely inside the screen rectangle (item boxes against the four side planes of the rectangle and the depth range,
built from the rows of the view-projection). The Radiant rules live in `ui/main_window` (`pickAt`, `boxSelectIn`, `pickInput`) and the action `select.pick`
(`LMB drag` / `Ctrl+LMB drag` / `Shift+LMB drag`, rebindable): a click selects the brush (a sole brush of an entity selects the entity), nothing under the cursor
clears, Ctrl toggles, Shift selects the face under the cursor, a drag of more than 4 px selects the box (Ctrl / Shift add to the selection). The outline
(`render/selection_outline`: brush edges orange, selected face yellow, prefab instance and point entity boxes) and the rubber band are coloured line lists
(`LineVertex`, 16 B) written into the upload ring per frame and drawn by two more `mesh_pass` pipelines (world lines: depth-tested with a bias, no depth write;
screen lines: no depth, identity matrix). The picker follows the builder's listener through a queued call on the UI thread; the render thread gets the lines
through `setSelectionLines` and `setRubberBand`.

**As built (T3.6).** Overlays are lists of coloured vertices (`LineVertex`) or text quads (`LabelVertex`, 32 B) the render thread puts into the upload ring each frame and
`mesh_pass` draws after the scene: `FrameOverlays` = selection outline (depth tested, biased), entity lines (boxes, arrows, gizmos; depth tested), translucent box
fills (triangles, no depth write), class-name labels (depth tested at the anchor), size labels (over everything), rubber band (clip space). Pure code lives in
`scene/` and is converted from the Shell: `point_box` (class box from the entity definitions, `angles` turn it, decal projector volume, `drawsBox`), `arrows`
(direction and link arrows, `refs` / `refScopes` from the prefab resolver - one small Core addition), `light_gizmo` (point / spot / sun / area shapes),
`size_labels` (box labels per axis without repeating a number, face width / height, overlap filter); `render/entity_overlay` builds the world-space lists for the
whole level once per snapshot / selection change (shared by all viewports); the size labels are per viewport (`MainWindow::sizeLabelsFor`, projected with the view,
metres per pixel from the unprojection, the view axis of an orthographic view skipped). Text: `ui/glyph_atlas_qt` draws printable ASCII with QPainter (12 px
semi-bold sans-serif at the window's device pixel ratio, R = glyph, G = glyph with outline) into an RGBA8 image that `TextureTable::addImage` takes like any other
texture; `label.vert` places the glyph cell by `anchor` + pixel offset, `label.frag` draws white text with the dark outline. The ortho grid uses the Shell's
`orthoGrid.ts` rule (minor lines at least 8 px apart, 100 steps each side must cover the window); `[` / `]` and `grid.set` change the grid size (actions). The
entity definitions of the game profile (`entities.ent`) are loaded by the main window and also give the picker its boxes (point entities are now hit by their real,
turned box). Deviations: the overlay options (origins, directions, links, gizmos) are the OR of the viewports' settings and are re-read on a snapshot / selection
change only (no per-viewport lists yet, no settings UI yet); icons and `misc_model` boxes are not drawn (T5); a visual check was made on demo.map.json plus a map
with a light, a spot light and an info entity in all four views (labels, boxes, spot cone, ortho grid), not side by side with the Shell.

**As built (T4.1).** The gizmo is a list of `LineVertex` (`tools/gizmo_overlay`: move = axis shaft + pyramid head, rotate = three rings of 48 segments, scale = shaft +
cube; x red, y green, z blue, the hovered / dragged handle yellow; the origin square of a brush entity amber, white while selected) that the main window builds per
viewport (constant apparent size: `gizmoLength(distance)`) and gives to `RenderThread::setGizmoLines`; `mesh_pass` draws it in the `topLines_` pipeline (no depth test,
after the scene, before the screen-space lines) so handles are never hidden by the level. Plain mouse moves (no button) now reach the input hook (hover on handles, the
modal operators follow the pointer).

## 7. Frame timing

- Target: the display rate with up to 4 viewports on lvl2 (09_TESTING_AND_PERF.md §3).
- GPU timestamps per pass; CPU timing per frame stage; both in the Stats panel.
- The render thread never calls `vkQueueWaitIdle` / `vkDeviceWaitIdle` except at shutdown and device recreation.
