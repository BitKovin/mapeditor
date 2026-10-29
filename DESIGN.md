# Level Editor — Design & Feature Requirements

**Status:** living document. This is the single source of truth for what this project is
supposed to become. When in doubt about whether something is in scope, how it should behave,
or whether a shortcut is acceptable, this file wins over improvisation. New requirements get
added here before (or as) they get built; nothing gets implemented in a way that quietly
forecloses an item below.

**Project name:** TBD — using "the editor" / "Core" / "Shell" throughout until named.

---

## 1. Vision

A modern, brush-based 3D level editor, spiritually a successor to id Software's
brush/BSP workflow but built with a Source-2-Hammer-grade modern toolset. Levels are authored
in the editor's own format; a Quake 3 BSP exporter is a later milestone, not a v1 concern.
Lighting is fully baked (lightmaps + light volumes, Quake-3-style) but computed with hardware
ray tracing and updated incrementally in real time as you edit, so the preview you see while
working *is* (a configurably-cheaper version of) the final lighting, not a stand-in for it.
Prefabs are just level files, includable at arbitrary depth, so a change to a prefab
propagates everywhere it's used. The overriding design goal is **iteration speed**: moving a
light, resizing a brush, or tweaking a prefab should feel instantaneous.

## 2. Guiding Principles

- **No throwaway v1 hacks.** Every subsystem is designed in its scalable, final-shape form
  from the start. If something is genuinely deferred (see §13), it's deferred *explicitly*,
  called out in code comments and in this doc's task list — never silently implemented in a
  way that makes the real version harder to build later.
- **The Shell edits; the Core does what a browser can't.** All interactive editing happens in
  the Shell, at zero latency, on its own copy of the document. The Core owns the filesystem,
  validation, derived geometry, lighting and export (§3). Nothing interactive ever waits on a
  round trip; anything that *may* be late (booleans, decals, lighting) is derived data that
  swaps in when ready (§5.6).
- **Easy to extend.** Adding a new tool, panel, or viewport render mode should be a small,
  local change, not a cross-cutting refactor. This is why there is exactly one edit message
  (§10) and per-viewport settings are open-ended structures.
- **Prefer proven prior art.** Where Source 2 Hammer, Blender, Quake-family tools, or
  established libraries (Boost.Beast, Manifold, etc.) already solved a problem well, start
  from their approach rather than reinventing it — see §7/§8/§9's tool inventories and §5.1's
  boolean library candidates.
- **Core treats the Shell as untrusted input.** Core can write files and listens on a socket
  that any web page in the browser could try to reach, so: strict parsing, semantic
  validation before anything is adopted, paths sandboxed to the assets root, and only local
  origins may connect (§10).
- **Every milestone ends with the full source zip** and an updated checklist (§15).

## 3. Architecture

| | **Shell** (TypeScript, browser) | **Core** (C++20, native process) |
|---|---|---|
| Owns | The working copy of the document, and **all interactive editing** | The authoritative *accepted* document, and everything a browser can't do |
| Does | Brush meshes (half-edge runtime structure), modeling tools (§8), UV/texture tools (§9), selection, gizmos, undo history, prefab-instance expansion, viewport rendering, docking UI | Filesystem (open/save/list/watch, atomic writes, sandbox); validation gatekeeper; prefab file loading + cache; **derived geometry** — booleans, decals (§5.6); lightmaps/light volumes with hardware RT + Embree fallback; streaming lighting chunks; BSP export |
| Latency | Zero: edits apply locally first | Allowed: everything Core produces is derived data that swaps in when ready |

- **One edit message.** The Shell mutates its own document and sends the *whole* updated
  document (`document_update`); Core validates it and either adopts it or rejects it (§10).
  Adding a tool never adds a wire message.
- **Ack, not echo.** The sender of an accepted update gets a small `update_ack`; every *other*
  session gets the new document. Echoing the document back to its sender would overwrite edits
  the Shell made while the update was in flight. The Shell keeps at most one update in flight
  and sends the latest state when the ack arrives (coalescing during drags).
- **Core keeps the raw JSON as the canonical document** and never rebuilds it from its typed
  structs, so editor-only data Core has never heard of (visgroups, UV projection params,
  selection sets…) survives every round trip (§5.5). Core's typed model is a read-only
  interpretation used for validation, resolution and (later) lighting.
- **Lighting is a separate, one-directional channel** (Core→Shell, future): lighting streams in
  spatial chunks — tiles of the per-face charts described in §5.6. The Shell never edits
  lighting; it only causes Core to recompute it by editing the document.
- **Multiple sessions** (other browser tabs, tools, a headless baker) are supported.
  **Verified in M2:** Dockview popout windows move the panel's DOM into a blank host page
  (`shell/public/popout.html`), so they share the main window's JavaScript context, Store and
  the single Core connection; WebGL renders in them. Up to 10 viewports need one session.
- **Primary platform: Windows 10/11.** Developed and shipped for Windows first: MSVC + Ninja via
  `build_core.bat`, `build_shell.bat`, `run.bat` (§16). Linux is kept building (the Core is
  portable C++20) but Windows decides every trade-off. Hardware ray tracing therefore targets
  D3D12/Vulkan on Windows GPUs first.
- **Electron is the packaging, not a new architecture** (§11, REQUIREMENTS §29). The desktop app starts the same
  Core and shows the same Shell build; development still runs Core + a plain browser tab (`run.bat`). No Shell or
  Core code changed for it: the window loads the Shell from `http://127.0.0.1:<port>`, which the origin policy (§10) admits.
- **The working format is not the export format.** Quake 3 BSP is produced by a separate
  compile stage that also does the unit/axis conversion (§4, §12).

## 4. Coordinate System & Units

- **Editor and Core: metric (meters), Y-up.** All internal math, all stored document data, all
  viewport cameras/grids/gizmos use meters with +Y as up.
- **Quake 3 BSP is Z-up, in Quake units (~1 unit ≈ 1 inch).** This conversion (axis swap +
  unit scale) happens **only** in the exporter (§12), as late as possible in the pipeline.
  Nothing upstream of the exporter should ever need to know Quake's conventions.
- **One exception, entity boxes:** a point entity's `box` in the `.ent` file is in Quake units and Quake axes.
  `shell/src/model/entityBox.ts` converts it for drawing and picking: `editor = (qx, qz, -qy) / 32`
  (32 Quake units = 1 m, Quake Z up becomes editor Y up, Quake +Y forward becomes editor -Z). This is a pure
  rotation, so handedness is kept. Classes without a usable box get 16 units (0.5 m) on a side. The box is
  axis-aligned and does not rotate with `angles`. The exporter (§12) uses the inverse, `editorToQuake`.
- Done in M1: the demo assets (`core/assets/`) are metric, Y-up (a 12 m room, 3 m walls, a
  2.5 m door frame), and Core contains no Z-up or Quake-unit assumption.

## 5. World / Document Data Model

### 5.1 Brushes — general geometry, not convex-only

Brushes are **arbitrary polygon meshes**, edited the way Blender edits meshes (§8). The classic
"convex set of half-space planes" model is not the representation.

- **Stored / transmitted format: an indexed polygon mesh** — `vertices` plus `faces`, each face
  `{ id, loop, uvs, texture }` (`loop` = vertex indices, CCW seen from outside). Deliberately
  plain: it is what Core validates, lights and exports, and what the Shell serializes.
- **The half-edge structure is a Shell runtime structure**, built from that mesh on load. It is
  what makes loop cut, edge slide, bridge and dissolve tractable (§8). Core never edits meshes,
  so it never needs edge adjacency.
- **Face ids are stable.** `face.id` is unique within its brush and modeling tools keep it
  whenever the face survives an operation. Lighting is keyed by `(brush id, face id)` (§5.6),
  so this is what lets baked lighting survive edits.
- **Faces must be planar** (within tolerance): lightmap charts (§5.6) and BSP export both need
  it. Core rejects non-planar faces; the Shell **splits** a face that stops being planar
  (decided, §14; implemented with the modeling tools in M3).
- **Textures are plain albedo paths**, exactly like a Quake map (`"dev/grey"`). No material
  system for now (§9).

**Convexity rule.** `worldspawn` and `func_group` brushes generate BSP vis portals at export, so
they must be **closed, outward-wound, planar, convex solids**. `func_detail` and every other
brush entity may be arbitrary and non-convex. Note this set is **smaller** than the "structural"
set used for the origin rule in §5.2 (`worldspawn`, `func_detail`, `func_group`): they are two
separate `EntityClassRegistry` predicates (`requiresConvexBrushes`, `isStructural`).
It is enforced twice: by the Shell while editing (so the tools simply won't produce an invalid
brush — good UX) and by Core when a document is submitted (the gatekeeper — the Shell is
untrusted). Both use the same rule; keep shared test vectors so they can't drift.

**Non-destructive booleans.** Each brush carries `op: add | subtract`. Resolving them into final
geometry is *derived* data, never stored — see §5.6 for who computes it and how it gets lit.
Candidate libraries for the resolver: [Manifold](https://github.com/elalish/manifold) (MIT,
guaranteed-manifold, parallel, real-time-capable, C++/WASM) and
[EMBER](https://github.com/zalo/ember-csg) (exact integer arithmetic, more robust on degenerate
input, unknown speed). Whichever is chosen must be able to report **which input face each
output surface came from** (§5.6 depends on it) — verify that in the spike.

### 5.1a Smooth edges (REQUIREMENTS §33 H6)
A brush may carry `smooth: [[faceA, faceB], ...]`: the edges where those two faces meet shade smoothly. Face ids, not vertex indices, because the
tools keep face ids (clipper, face drag, mesh tools all renumber vertices). Core `smoothing.cpp` turns the pairs into per-corner normals (at each vertex,
the angle-weighted average of the faces joined to it through smooth edges) and interpolates them across a face with mean value coordinates. Users:
the texel samples of the light bake (the normal a texel is lit with; the solid test still uses the flat normal), the lightmap packer (a smooth edge
joins two faces into one chart at any angle, so their light is continuous), the rBSP vertex normals (and so the OBJ export). Smoothing never crosses
brushes. Shapes: cylinder sides and the whole sphere are marked when their `smooth` parameter is on (the default).

### 5.2 Entities

- **Point entities**: classname + keyvalues + transform, no brushes.
- **Brush entities**: classname + keyvalues + brushes. `worldspawn` is the root static world.
- **Custom origin.** Every brush entity has an `origin`, shown as a gizmo in the viewport —
  **except** the structural set (`worldspawn`, `func_detail`, `func_group`). Working assumption,
  flagged for confirmation (§14): `origin` is an independent pivot, not baked into the brush
  coordinates (classic Quake/Source behavior, e.g. a door's hinge).
- **Entity-class registry.** Currently two hard-coded predicates; the natural future home of a
  full FGD-like definition system.

### 5.3 Lighting classification (data model only — no lighting yet)

- `worldspawn` / `func_detail` / `func_group` → **lightmapped** (static world).
- Every other brush entity → **lit from light volumes**, since it can move (Quake 3 light grid).
- Not designed in detail yet: when this milestone starts I'll ask for an example of how your
  engine applies light volumes, so the format matches your runtime.

### 5.4 Prefabs — embedded levels, arbitrary depth

A prefab **is** an ordinary map file. A `PrefabInstance` references another file (path relative
to the assets root + transform + sparse per-target overrides) and resolves recursively to any
depth (cycle-detected via normalized paths, depth-capped). Editing the referenced file changes
every map that includes it.

- **Loading is a Core job (filesystem); expansion is a Shell job (instant).** Core reads every
  prefab file reachable from the open document and sends the raw documents as `prefab_library`
  (cached by mtime; re-sent only when the reachable set or a file changes). The Shell expands
  instances locally from that library, so moving an instance or adding another instance of an
  already-loaded prefab has zero latency; only loading a *new* file needs a round trip. Core runs
  its own resolve for validation now and for lighting/export later.
- Deliberately collapses Source 2's two-tier Prefab/Instance system into one mechanism: every
  instance is a file reference, some files just happen to be used from one map.
- Not yet: authoring UI for placing/editing instances and overrides, live push when a prefab
  changes on disk without an edit (§13), rotation composition (translation-only so far).

### 5.5 File format

- **Schema `0.2`**, JSON. Readers reject any other `schemaVersion` rather than misparse.
- Core keeps the raw JSON as canonical (§3), validates it against a strict typed reader, and
  saves it exactly as received (atomic temp-file + rename; keys sorted, so diffs are stable).
- A git-friendlier, more compact text format (KeyValues3/VMF-style) is the longer-term goal —
  not urgent while the schema is still moving.

### 5.6 Derived geometry & lighting identity

The hard question: booleans (and decals) generate geometry, that geometry needs lightmaps, and
the editor must still edit with zero latency. The answer is a split, not a shared algorithm.

**Authored vs derived.** *Authored* data is what the user edits: brush meshes, entities, decal
*projectors* (position, direction, size, texture). It lives in the document and is edited only in
the Shell, instantly. *Derived* data is computed from it: boolean results, decal geometry (later
mirror/array, displacement). It is never stored in the map file and never edited directly.

**Rule 1 — the Shell never waits.** While derived geometry is pending, the Shell draws the raw
authored brushes (additive solid, subtractive as translucent volumes) at zero latency. When
Core's result arrives it swaps in. (If that latency is ever noticeable, a Shell-side preview
using the same library compiled to WASM is an optimization — never a requirement.)

**Rule 2 — Core is the single authority for derived geometry that is lit or exported.** The
lightmap layout has to correspond *exactly* to the geometry being lit. "Same boolean algorithm on
both sides" would need bit-identical tessellation and face splitting across native and WASM
(floating point, threading, library versions) — any mismatch shows up as misaligned lighting. So
Core computes the final geometry and sends it (a `derived_geometry` channel, not built yet). A
Shell-side preview may differ slightly; nothing depends on it matching.

**Rule 3 — lightmap charts belong to *authored faces*, not to derived fragments.**
The invariant that makes this work: **every derived surface lies on the plane of an authored
face** (CSG results lie on input planes; a decal lies on its target plane). So each authored face
(per side) owns a **planar chart on a world-aligned texel grid in its own plane**, stored sparsely
as tiles keyed `(instancePath, brushId, faceId, side, tileX, tileY)`. A derived fragment records
its parent `(brush, face, side)` and computes its lightmap UV from world position with a simple
plane formula. Consequences:

- Trimming or splitting a face with a boolean doesn't move its texels: lighting on surviving
  area stays valid, only newly exposed tiles are dirty — the "update only what changed" goal.
- **The preview can be lit too.** Because UV = f(position, plane) is a tiny deterministic
  contract, geometry the Shell shows while waiting for Core's result can sample the real
  lightmap. The contract that must match on both sides is that formula, not a boolean.
- **Decals cost no lightmap**: their fragments sample the parent face's chart, so they are lit
  exactly like the surface they sit on. On a dynamic (light-volume-lit) entity there is no chart
  at all — vertices sample the volume.
- **A tile is the lighting-streaming chunk** (§10): Core sends changed tiles, and only those.
- Requires stable ids — hence `face.id` (§5.1).

**Decals.** Authored = the projector. Derived = fragments clipped to the target surfaces, **owned
by the target's entity**, so they move with it in game and are appended to that entity's surfaces
at export. A decal spanning two entities yields fragments owned by each.

**Generators are pluggable**: boolean resolver, decal projector, later mirror/array — each turns
authored data into *surface fragments* `{ parent face, polygons, owner entity, kind }` over the
one derived-geometry channel.

Known limits: filtering seams between charts (standard lightmap problem, handled later), and
non-planar surfaces (displacement, curved patches) would need a different chart type. The two curved things the editor has, imported patches
(§12 Importer) and the terrain (§8.k), avoid that by being meshes of planar triangles with `smooth` edges, so they need no new chart type.

## 6. Rendering & Viewports

### 6.1 Multi-viewport model

Up to **10 concurrent viewport windows**, each independently poppable into its own OS window
(via Dockview) so they can be spread across multiple monitors. Popouts share one
JavaScript context and one Core session (§3, verified in M2). In-page floating panels are enabled
(Dockview default): holding Shift while dragging a tab makes a panel float inside the window; it cannot leave the browser tab.
Real windows come from `popOut()` in `app.ts`: the ⧉ button in every group header, the ⧉ button of a viewport,
or dragging a tab (or a group header) out of the browser window and releasing it (the window opens under the
cursor; the drag source is read from the DOM because Dockview's will-drag events are an optional module). Each viewport has **its own render settings**, independent of every other viewport —
this needs to be a per-viewport settings object from the start, not a single global toggle.

### 6.2 Render modes

Each viewport picks one render mode:

| Mode | Description | Status |
|---|---|---|
| **Unlit** | Flat-shaded/textured geometry, no lighting at all | **Implemented target for this milestone** |
| **Lit** | Final baked lighting (lightmaps + light volumes) applied | Not implemented — depends on §5.3/§6.3 |
| **Wireframe** | Edges only, no fill | Not implemented |
| **Lighting only** | Just the lighting contribution (no albedo/material) — useful for judging bake quality in isolation | Not implemented — depends on §6.3 |

Only Unlit needs to actually work right now. The render-mode enum and per-viewport settings
struct must exist and be plumbed through now, so adding the other three later is additive
(new enum case + new shader path) rather than a re-architecture.

### 6.3 Real-time incremental lighting (future milestone, kept in mind, not started)

**Order of work (decided).** Nothing ray-traced gets written until the plumbing is proven:

1. **Lightmap transport + visualization with fake data.** Core generates *test* lightmaps
   (checkerboards, per-face gradients, a texel-index colour code) for every face's chart (§5.6),
   streams the tiles to the Shell, and the **Lit** and **Lighting only** viewport modes display
   them. This proves chart layout, tile addressing, streaming, partial updates, seam handling and
   UV lookup end to end — and gives a permanent debug view. A bug here must be visible as a wrong
   pattern, not hidden inside plausible-looking lighting.
2. **Ray tracing without texture sampling.** Every surface has the same white albedo; only the
   light transport is being tested. Hardware RT first (D3D12/Vulkan ray query), Embree CPU
   fallback behind the same interface.
3. **Ray tracing with texture sampling.** Albedo comes from the face's texture (the game
   profile's texture lookup, §14), so coloured bounce light works.

**What gets baked: diffuse lighting only, including global illumination (multi-bounce).** No
specular and no reflections — they depend on the camera position, so they cannot be baked.
Direct + indirect diffuse into lightmaps (static world) and light volumes (dynamic entities, §5.3).

Other rules:
- Preview and final share one estimator, differing only by a quality tier (rays/texel, bounces,
  texel density, probe density) — preview converges toward the same answer as final.
- Only affected surfaces are recomputed: a spatial index maps lights to the chart tiles (§5.6)
  and probes their influence volume touches, so moving a light dirties exactly that set.
- Ask for the engine's light-volume format before building §5.3's representation.

### 6.4 Lightmap UV generation (Core, `lightmap_uv.h`)

Input: the resolved surfaces of the document (`collectSurfaces`, the same carving the `derived_geometry` channel uses), restricted to
entities whose class is lightmapped (`lighting_classes.json`, default = structural classes). Output: a `LightmapLayout`.

**Identity.** One *face chart entry* per authored face side, key `(instance, brush, face, side)`; `side` is `front` when the fragment normal
equals the authored face normal and `back` for a cut wall (a subtractor's face seen from inside the carved room). A fragment's lightmap UV
is a pure affine function of its world position (`LightmapLayout::uv`), so carved fragments, decal fragments and the Shell's preview all use
the same contract, never the boolean itself.

**Per-face frame (world-aligned grid).** For a face with unit normal `n`: if `|n.y| > 0.7071` the `u` axis is the projection of world +X
onto the plane, otherwise `u = normalize(Y x n)` (horizontal); `v = n x u`. Texel `(s, t) = ((p - P0) . u, (p - P0) . v) * texelsPerMeter`
where `P0` is the projection of the world origin onto the face plane. The lattice therefore depends only on the plane, never on the face's
extent: resizing or re-cutting a face leaves every surviving texel where it was (the "update only what changed" goal of §6.3).

**Islands (connected edges stay connected).** Faces of one `(instance, entity)` are adjacent when an edge of one overlaps a collinear edge of
the other by more than `edgeEps`. Starting from the largest face (ties by key), neighbours are unfolded into the root frame by rotating
about the shared edge (identity for coplanar faces), provided the dihedral angle is `<= mergeAngleDeg` (default 15; 0 merges only coplanar faces). Faces of an island that are not joined by a shared edge must keep `2 * padding + 1` texels apart, so bilinear filtering never reads an unrelated face, the
unfolded polygon does not overlap any polygon already in the island (SAT on the triangulated polygons) and the island stays within
`maxChartTexels`. Texel alignment is guaranteed for the root face's plane; unfolded neighbours share the root lattice. Every face map is
still affine in world space.

**Chart rectangle.** Bounding box of the fragments' footprint in island texel space, rounded out to integers, plus `padding` empty texels on
each side (gutter for dilation / bilinear filtering). A chart larger than a page is scaled down uniformly (density drops, a warning is
reported). Charts are not rotated (rotation by 90 degrees would keep the lattice but is deferred). A face map is `s = p.su + so`, `t = p.sv + to` in page texels, composed from the unfold rotations, so it is exactly isometric (texel distance = metres x density) and tested as such.

**Sampling (REQUIREMENTS §30).** The editor's views and the game (`fs_bsp.sc`, rBSP v2) read lightmaps with a bicubic B-spline built from 4 bilinear fetches: bilinear's slope jumps at every texel border and shows soft shadows as creases on the texel grid. The B-spline reads up to 2 texels from a chart's edge, so `padding` is 2 (a `lighting.json` value below 2 is raised).

**Edge leaks (rules that keep corners clean).** Bilinear filtering reads the texel ring around a sample, so (1) the default merge angle is small (corners are separate charts, each with its own gutter), (2) unrelated faces unfolded into one island keep a gap of `2 * padding + 1` texels, (3) the baker uses conservative rasterisation: every texel overlapping a fragment is baked, texels that only partly overlap at the nearest point inside the fragment (`kEdgeInset`), centre-inside texels win.

**Packing (batch-friendly).** Charts are bucketed by `(instance, entity, dominant texture)` and the buckets are ordered by key. Within a
bucket charts are sorted along a Morton curve of their 3D centre, then placed with MaxRects (best short side). A bucket that does not fit the
current page entirely starts a new page when that helps; buckets larger than a page spill chart by chart. All pages share one square
power-of-two `atlasSize`, so the engine can use a texture array. This trades some packing efficiency for coherence; the fill ratio per page
is reported.

**Not done here:** light-style-aware chart arrangement (8 styles per surface), user-editable lightmap UVs, density view, 90 degree rotation,
cross-entity merging. They need the Shell side first.

### 6.5 Light baking (Core, `lightbake`)

**Order (decided by the user):** UV generation, then direct light without RT (point), then every light type without shadows, then ray-traced
shadows, then bounces. Each step is a working, tested bake.

**Pipeline.** (1) `buildTexelSamples`: rasterise every lightmapped fragment triangle into its atlas page; each covered texel centre gets a
world position and a normal (floats); texels in the gutter are filled by dilation after the bake. (2) Evaluate lights per texel. (3) Write
pages. The evaluator exists twice with identical formulas: a CPU reference (tests, fallback) and a **Vulkan compute shader**
(`shaders/lightmap_direct.comp`). The GPU path uses vendored Vulkan headers and loads the loader dynamically (`vulkan-1.dll` /
`libvulkan.so.1`), so building needs no SDK; if no Vulkan device is found the CPU path runs. GLSL sources live next to checked-in SPIR-V
headers (`core/src/shaders/*.spv.h`); `python3 tools/compile_shaders.py` regenerates them (needs glslangValidator), so a normal build needs no glslang.

**Units (no physical units, no magic numbers).** A lightmap value `V` is irradiance with the factor pi absorbed, so the displayed colour is
`albedo * V` with nothing else applied (engine and Lit view just multiply). A white surface facing a point light of brightness `B` at 1 m
(inside the window) stores `B`. Bounces follow from the same convention: `V_indirect(x) = mean over cosine-weighted rays of albedo(y) * V(y)`.

**Light entities** (`light_point`, `light_spot`, `light_directional`, `light_rect`, `light_sky`; keyvalues): `color` ("r g b", 0..1, default
white), `brightness` (default 1; negative = a dark light, below), `radius` (attenuation radius, m), `bounce` (bounce power, default 1), `cast_shadows` (0/1, default 1),
`style` (0..), `source_size` (m for point/spot sphere radius, degrees for directional angular diameter), spot: `inner_angle`, `outer_angle`
(degrees, full cone half-angles), rect: `width`, `height` (m). Direction = `R * (0,0,1)` with `R = Ry(yaw) Rx(pitch) Rz(roll)`, the same
forward axis as the Shell's direction arrows. Entities inside prefab instances are resolved like every other entity.

**Formulas (shadows excluded until step 4).** `window(d) = clamp(1 - (d/r)^4, 0, 1)^2`, `cos = max(0, n . l)`.
- point: `V = brightness * color * window(d) / max(d^2, dMin^2) * cos`, `dMin = 0.05 m`.
- spot: point times `cone = t*t`, `t = clamp((cosA - cos(outer)) / (cos(inner) - cos(outer)), 0, 1)`.
- directional: `V = brightness * color * cos`, `l = -forward`.
- rect: one-sided emitter of radiance `brightness`; `V = brightness * color * F * window(distance to centre)` where `F` is the form factor
  (fraction of the receiver's cosine-weighted hemisphere covered) from the exact polygon irradiance (Arvo) after clipping the polygon to the
  receiver's horizon. A rect larger than the receiver's whole sky gives `V = brightness`.
- sky: uniform dome of strength `brightness` above the horizon (+Y): unshadowed `V = brightness * color * (1 + n.y) / 2`.
- ambient: constant colour added to every texel (`_ambient` of worldspawn).

**Shadows (step 4).** Occluders = every surface piece of a class in `lighting_classes.json` `castShadows` (double-sided, opaque, the same
carved pieces the lightmaps use; lights and decals do not occlude). A shadow ray starts at `p + n * 0.01 m` (`kShadowBias`) and ends
`0.01 m` before its target; any hit means occluded. Visibility per light is the occluded fraction of its samples:
- point / spot: 1 ray to the centre, or `areaSamples` rays to points of a disc of radius `source_size` that faces the receiver (a sphere
  light, to the accuracy of a disc);
- directional: 1 ray, or `areaSamples` rays inside a cone of angular diameter `source_size` degrees;
- rect: `areaSamples` rays to uniformly sampled points of the rectangle (visibility scales the exact unshadowed form factor);
- sky: `skySamples` cosine-weighted rays around the normal; `V = brightness * color * (rays with direction.y > 0 that escape) / skySamples`.
  Unshadowed this converges to `(1 + n.y) / 2`; with `cast_shadows 0` or no occluders the analytic value is used instead.
`cast_shadows 0` skips the rays of that light. Sample positions come from a per-(texel, light, sample) hash with a rotated R2 sequence, the
same integer code on CPU and GPU, so both trace the same rays and differ only by float rounding at geometric edges.

**Ray tracing back ends.** GPU: one bottom-level acceleration structure of all occluder triangles in one TLAS instance, built on the device
(`vkCmdBuildAccelerationStructuresKHR`; host builds are not used), queried with `rayQueryEXT` from the same compute shader
(`lightmap_direct_rq`). Requires `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `VK_KHR_deferred_host_operations`, buffer device
address. A device without them runs the CPU back end (a median-split BVH with any-hit traversal in `raytrace.cpp`), which is also the
reference the GPU is tested against. Embree is not used (no dependency); the BVH is the stand-in until a real need appears.

**Bounces (step 5).** Diffuse only, albedo `a` (one constant until textures exist). With `Db` = the direct light with every light's contribution
multiplied by its `bounce` (ambient excluded) and the atlas holding `S` (rgb + valid flag per lightmap texel):
`S0 = Db`; `I(k+1)(p) = mean over giSamples cosine-weighted rays of a * S_k(hit)`; `S(k+1) = Db + I(k+1)`; result = `direct + I(bounces)`.
Each iteration is one dispatch of `lightmap_gather` reading atlas A and writing atlas B, then the roles swap. A hit is looked up through the hit
triangle's lightmap coordinates (`OccluderMesh::lightmap`, 2 x vec4 per triangle) with a bilinear fetch in which invalid texels weigh 0; a
shadow caster without a lightmap absorbs. Rays towards the sky are not gathered (the sky light already accounts for them). The directions of
a texel are the same in every bounce (sampler slot 100000): still unbiased because inner gathers happen at other texels, and the result is
monotonic in the bounce count. Limits: only surfaces in the acceleration structure (shadow-casting classes) are visible to gather rays; the
two atlas buffers must fit `maxStorageBufferRange` (else CPU fallback); no light styles per bounce yet.

**Dark lights (negative `brightness`, REQUIREMENTS §40).** A light whose `brightness` is negative takes light away, like a negative `light` in Quake 3 (the `.ent` range is -1000..1000).
Same formulas and shadows as a positive light, the sign rides on `brightness`; what changes is where the sign may survive:
- **Everything is summed signed.** `V = ambient + sum of the lights` per texel (`evalLight`, the shader's `shade`), so a dark light subtracts its irradiance from the other lights of its
  style layer and from the ambient. Shadow rays are traced for a light whose value is *not 0* (CPU `v != 0`, shader `!= 0.0`; it used to be `> 0`, which would have left a dark light
  unshadowed): a dark light is blocked by the same occluders as a bright one. `_light_scale` multiplies the sign too.
- **Bounces are linear in the signed light.** `Db` carries the dark light times its `bounce` (negative), the gather averages `albedo * S` of what it hits, so near a dark light the indirect
  light is negative and subtracts too; `bounce 0` makes a dark light direct-only. The bounce source `S`, the indirect `I` and `Db` are never clamped, so the light of `A` and a dark `B` is
  exactly the light of `A` minus the light of `B` (test `bounce cpu: dark lights are linear ...`); the live baker's moving average works the same way.
- **The only clamp is the finished texel.** `max(0, direct + indirect)` per channel, after the ambient occlusion factor (which is >= 0): `bakeLightmaps` before the pages are assembled
  and `LiveBaker::shownValue` (so `renderTile`, `snapshot`, the tiles sent to the Shell, the mirror and the EXR / OBJ / rBSP pages). A lightmap therefore never holds light below 0, and the
  RGBA16F transport and the EXR writer never see a negative number (`floatToHalf` also maps negatives to 0, kept as a guard). The raw evaluators (`evaluateDirectCpu`,
  `evaluateGiCpu`, `LiveBaker::value`) stay signed on purpose: the passes blend raw values and the tests check them.
- **Denoiser.** OIDN only knows values in [0, inf) (`filterRGB` zeroes the rest), so `denoiseSamples` filters the positive part and the negative part of the indirect light as two images and
  subtracts them again when any value is negative; levels without dark lights take the old single-image path at no cost.
- **Light volumes.** A probe fits the finished light: `V(n)` (and the direct-only copy that decides the direction) is clamped at 0 per normal before the sphere moments, so a probe agrees with
  the lightmaps next to it and `ambient`, `directed` stay >= 0 (the x2 byte range has no negative values).
- **Light styles.** Every style layer is its own bake, clamped on its own (§6.8), and the Shell adds the layers. A dark light therefore darkens only lights of ITS style (and the ambient when
  it is style 0); it cannot darken a light of another style, and a dark light of a style with no positive light just makes an all-black layer. Use style 0 for a dark light that should
  darken the static lighting. The style analysis ranks a light by `|brightness|`.
- **Change detection.** `sameLight` (live baker) and `sameLightLists` (light volumes) compare the signed brightness with `==`, so flipping the sign re-bakes the light's reach like any
  other brightness edit.
- **Shell.** `lightParams` no longer clamps `brightness`; the Details editor takes the `.ent` range (-1000..1000); a dark light's gizmo is drawn violet and crossed (see §6.9). The Lit and
  Lighting-only views need nothing: they only ever receive clamped light.

**Lighting classes and buried texels.** Which entities block light and which receive baked light is data in the game profile (see
REQUIREMENTS §18 step 1b); entities that do not receive baked light are to be lit from light volumes. Brushes of the same entity overlap
instead of being unioned, so a face can lie inside another brush (a floor top under a wall, a pillar bottom on the floor). Texels of such
faces are invisible, and rays starting inside geometry hit coplanar ties, so `buildTexelSamples` skips every texel whose point 2 cm above
the surface lies inside a light-blocking additive brush and outside every subtractive one (`SolidSet`).

**Super sampling of the direct light.** `buildTexelSamples` also stores, per texel, its centre on the chart plane, the world size of one texel in
u and v and a mask of which sub-texel positions lie inside the fragment (`TexelSamples::sub`). With `BakeOptions::superSamples = N` (default 8, 1 = off)
the direct light of a texel is evaluated at N of the eight D3D-8x-MSAA offsets (`kSuperOffsets`, the same table is `SUB[]` in the shader) and averaged;
positions outside the fragment (or within 5 mm of its border) are skipped, a texel with none keeps its single sample. Every position draws its own
shadow rays (`seed * 8 + k`), so `areaSamples` / `skySamples` are per position and the direct pass costs N times the rays. Only while shadows are
traced. CPU (`directCpu`) and GPU (binding 8 of the ray-query direct shader, `pc.superSamples`) use the same formulas.

**Global illumination on a finer grid, denoised.** The bounce light is the noisy part. `buildGiSamples(samples, scale)` splits every texel into
`scale x scale` cells (scale 2 or 4, `BakeOptions::giScale`, default 2); each valid cell is a gather sample with its own position (cells outside the
fragment are skipped). Bounces 1..B-1 run on the texel grid as before; the **last** bounce runs on the cells (the gather shader's `atlas` is the fine
atlas, `sAtlas` the bounce source's, flag bit 0 = do not write the source). The raw grid goes to **Intel Open Image Denoise** (`core/denoise.h`, filter
`RTLightmap`, HDR, one call per atlas page; empty atlas space is filled by growing the baked texels outwards first), and `resolveGi` averages the cells
back to the texel: the output resolution is unchanged. Only the indirect light is filtered; the direct light is added back untouched (sharp shadows).
OIDN is not linked: `denoise.cpp` opens `OpenImageDenoise.dll` / `libOpenImageDenoise.so.2` / `.dylib` at run time (order: `MAPEDITOR_OIDN`, the
executable's folder, the system path) against the vendored C header; it needs its sibling libraries (tbb, device libraries) next to it. Without it
the noisy estimate is kept and `BakeStats::warnings` says why. Measured on a closed room with a pillar (CPU, 16 gather rays, error vs 1024 rays, RMS):
texel grid raw 0.093, denoised 0.082; 2x grid averaged 0.053, denoised 0.051; 4x grid averaged 0.031, denoised 0.031. Most of the gain is the extra
rays (4 / 16 times as many); the denoiser mainly removes the remaining blotchy low-frequency noise. 4x is therefore a quality option (16x the rays
and 16x the pixels for OIDN), not a default.

### 6.6 Real-time progressive lighting (Core, `live_bake.h`)

`LiveBaker` keeps the lighting of the whole level up to date while the designer edits (REQUIREMENTS §18.1).
- **Chunks.** The atlas is cut into tiles of `chunkTexels` (16); a chunk owns the samples inside it, its world bounds and a pass counter
  (0 = stale). Results live per sample: direct light `D`, bounce-weighted direct `Db`, indirect `I`; the bounce source `S = Db + I` is
  kept in atlas layout for the gather.
- **Progressive passes.** One pass draws NEW random numbers (`sampleOffset = pass * samplesPerPass`) and merges them: `D`, `Db` as a running
  mean, `I` as a moving average over `indirectWindow` passes. Because each gather reads the current `S`, every pass adds one more bounce of
  depth, so indirect light converges to many bounces without an explicit bounce count. The first pass of a stale chunk replaces its old value
  (the old value stays visible until then: no flicker, no black frames).
- **Scheduling.** `step(camera, texelBudget)` refines stale chunks first, then the least refined, ties by distance to the camera. A
  global edit (directional / sky / ambient) makes everything stale, but the camera's neighbourhood is processed first.
- **Invalidation.** Lights are diffed by (instance, id). A point / spot / rect edit makes stale the chunks inside the old and new
  influence box (position +- radius); chunks within `indirectRadius` (40 m) of it only fall back to `indirectRefreshPasses` (bounce refresh).
  Every invalidated chunk (stale or bounce-refresh) immediately zeroes its bounce source `S` (CPU atlas and resident GPU atlas) and replaces its
  indirect estimate on its next pass instead of averaging into it: the estimate feeds on itself, so old light left in `S` or `I` would circulate
  between the surfaces that still hold it. The displayed value stays the old one until the new pass lands (no flicker).
  Geometry edits come as world boxes (`diffSurfaces` finds them); chunks within the largest light radius of a box go stale, a shadow-casting
  directional / sky light makes the change global.
- **Re-layout without losing work.** Texels are identified by (world position, normal), not by atlas position, so a repacked atlas keeps the
  values and pass counts of every unchanged texel.
- **Fine GI grid and denoising (live).** With `LiveSettings::giScale` > 1 the gather of a pass runs over the chunk's grid cells; their values are kept
  per cell (moving average), the texel value is the mean of its cells. A page whose chunks all reached `maxPasses` is denoised on its fine
  grid (`denoiseNext`, one page per service tick; refining a chunk makes its page raw again until the next denoise); the raw estimate keeps feeding the
  bounce source. Direct light is never filtered.
- **No flash to black.** A new lit mesh replaces the geometry the Shell shows, so `LightingService` holds it back (the Shell keeps the old lighting
  meanwhile). The very first mesh goes out when `firstSendFraction` (10 %) of the chunks have data (`LiveBaker::chunkHasData`); after that, when no
  chunk is without a result (`staleChunks() == 0`: the Shell keeps its pages, so every inherited chunk is already lit). `maxHoldSeconds` bounds the
  wait, convergence releases it. Edits that leave the geometry alone (moving a light) never resend the mesh, only refined tiles, and do not run the
  layout / sampling / chunking at all (the snapshot's surfaces, solids fingerprint and classes are compared with the last ones).
- **Live latency (edit to pixels).** What stands between a document change and visible light, in the order it happens:
  1. *Snapshot.* The socket thread takes a snapshot of the lighting inputs (`captureInputs`: surfaces, classes, solids, lights) once per message
     (`Router` calls `flushInputs()`), the ack never waits for a bake.
  2. *Bake thread* (`LightingService::startWorker`). Owns the `LiveBaker`. A newer snapshot or a switch on / off interrupts the bake in progress after its
     current step (latest wins, nothing queues), **but not before the snapshot applied last has had its first pass sent and a held mesh released**
     (`firstPassPending_`, capped at 500 ms): during a drag edits come faster than a rebuild takes, and without this guarantee every snapshot would be
     superseded before it showed anything. The synchronous `tick()` runs the same engine for tests and tools.
  3. *Geometry rebuild* (`LiveBaker::setGeometry`). `TexelSampleCache` keeps what each surface piece contributes (candidate texels with sub-texel data,
     keyed by an exact signature of geometry + chart placement) so only changed pieces are rasterised, in parallel (`parallelFor`); the assembly
     (first claim wins, buried texels dropped) is exactly the old result (`tests/texel_reference.h` is the oracle). Texels inherit values by atlas slot
     first (no hashing); a content-key lookup covers only old texels nobody took over. Chunks are indexed directly. A page whose texels are all
     unchanged keeps its denoised copy.
  4. *GPU session.* The resident arrays are filled in parallel from prefix-sum offsets; the Vulkan context keeps a `BufferPool` so a new session reuses
     the buffers of the last one (sizes are padded by 1/8, entries are matched by usage and memory kind).
  5. *Progressive delivery.* `LiveBaker::takeDirtyChunks()` lists the chunks whose pixels changed (refined, denoised, new / moved / still-unsent texels
     that survived a rebuild). After each step, at most every `flushIntervalMs` (25), their tiles are rendered (linear RGBA16F halves, the same bytes the Shell stores), compared with the
     **mirror** of what the clients hold and only the tiles that differ are sent; the last ones always go out when work stops. While two batches are
     undelivered (`delivered()` is called by the socket thread) the bake thread holds tiles back and merges them into the next flush.
  6. *Mirror.* `LightingService::Mirror` (RGBA16F pages of every style layer as the Shell has them, the last lit_mesh, light_styles, lightvol,
     entity_light and status messages) serves `fullState()` for windows that connect later; the mesh it sends always says `keepPages:false`.
     A style layer appears in the mirror with its first tile, like in the Shell. `keepPages` also requires that the set of style layers is the one
     the Shell holds (a layer that appeared or vanished starts every layer from empty pages), and `light_styles` is resent only when the table changed
     or the pages start over (the Shell rebuilds its texture array when it gets it).
  7. *Shell.* `lit_mesh.keepPages` = the atlas has the same shape as the one the Shell holds: it keeps pages, change log and the GPU texture and only
     re-uploads the vertex buffer (`LightmapStore.meshSerial`); otherwise pages start empty (`serial`).
  7b. *Style layers and light volumes on the bake thread.* Every style layer is its own `LiveBaker` (same engine, own dirty chunks, own tiles with
     `style`); all of them are owned by the bake thread. With more than one style among the lights, a light edit can change which charts a style
     reaches, so the layout and the layers are rebuilt for every snapshot (no lights-only shortcut); without styled lights the shortcut stays. The probe
     bake (`workLightvol`) runs after the lightmaps converged, works from the snapshot (classes, solids, surfaces; never from the Scene) and stops
     when a newer snapshot arrives. The bounce albedo loader is made on the socket thread (`captureInputs`) because it reads the game profile.
  8. *Denoise.* With a bake thread the filter of a converged page (of any layer) runs on its own thread (`beginDenoise` / `runDenoise` / `finishDenoise`); a result
     for a page that was refined or whose geometry was replaced meanwhile is dropped.
  `lighting_status.timingsMs` reports where the time of the last edit went (surfaces, layout, diff, geometry, lights, session, steps, tiles, denoise,
  firstTiles, converged). `bench_live` (CMake target, not in `all`) measures it with random noise instead of light.
- **GPU load.** A step adapts its texel budget so one submission takes about `gpuTargetStepMs` (default 100, at most 400), between a quarter of and
  `gpuMaxStepScale` (32) times `gpuTexelsPerStep` (262144); `tickBudgetMs` (120) lets one synchronous service tick run several steps back to back (with a bake thread steps follow each other without ticks). Supersampling
  and the fine GI grid multiply the GPU work per texel without more CPU work, which is what keeps the GPU busy while the CPU only accumulates results.
- **Config.** `lighting.json` next to `settings.json` (written with the defaults on first start): `texelsPerMeter, atlasSize, padding, mergeAngleDeg,
  exposure, directSupersample, giScale, areaSamplesPerPass, skySamplesPerPass, giSamplesPerPass, maxPasses, chunkTexels, bounces, albedo,
  indirectWindow, denoise, denoiseQuality (0 fast, 1 balanced, 2 high), useGpu, gpuTexelsPerStep, gpuTargetStepMs, gpuMaxStepScale, texelsPerStep,
  tickBudgetMs, flushIntervalMs, firstSendFraction, maxHoldSeconds`. Unknown keys are ignored, values are clamped; read once at start-up.
- **Backends.** This first version evaluates chunks with the CPU reference; the GPU back end needs a persistent context (device,
  acceleration structure updated in place, resident sample buffers) instead of one-shot bakes: next step.

### 6.7 Light volumes (Core, `lightvol.h`) and the Lit view

**What they light.** Brush entities whose class is not in `receiveBakedLight` (doors, lifts, ...): they move, so they cannot own lightmap area. Quake 3 style:
a grid of probes; an entity samples the grid once, at the centre of its brushes' bounds (not at its `origin`: that is a pivot, a door's hinge), and lights
every vertex with `ambient + directed * max(0, n . dir)`.

**One estimator, ray traced.** A probe is not a new light model. The lightmap value `V(n)` at a point (irradiance with pi absorbed, §6.5) is evaluated at the probe
position for `normals` (default 32, antipodal Fibonacci sphere) normals with the same direct light + shadow rays and the same cosine-weighted gather over the baker's
bounce source `S` that texels use (a probe sample is a TexelSamples entry that has no lightmap texel). On a ray query device the probes' samples stay **resident in a
`gpu::LiveSession`** (one device, one acceleration structure, the atlas S copied in once; the session's gather never writes the atlas, it only reads it) and every step
is one submission: ray traced direct light and ray traced one-bounce gather in the same shaders as the lightmaps. Without a device (or after any GPU failure) the CPU
evaluator runs the same maths on the BVH (`ProbeBaker`, `bakeProbesCpu`); a test compares both. Directional light and GI therefore both show in the probe.

**Fit to the ambient + directed model.** With `A = mean_k V(n_k)` and `G = mean_k V(n_k) n_k` (sphere moments), and `Gd` the same first moment of the
direct light alone (lights, sun, sky; no gathered bounce): `directed_c = 6 max(G_c . d, Gd_c . d)`, `ambient_c = A_c - directed_c / 4`, both clamped at 0 (and
`directed_c <= 4 A_c`), so every probe keeps its mean light. This reproduces a single unshadowed delta light (`V = E max(0, n . l)`: ambient 0, directed E, dir l) and
a uniform ambient exactly. The normal set is antipodal, so a constant `V` has no first moment.

**The direction is an artistic choice, made for shadows** (`fitProbeDirectFirst`). The engine shades entities with it AND casts their stencil shadows along it, so
direct light decides it before bounce light: `d = normalize(luminance(Gd) + w * luminance(G - Gd))`, `w = LightvolSettings::bounceDirectionWeight` (0.25). Where
direct light is (nearly) absent the light comes from straight above instead, blended by the direct share of the mean light (`luminance(mean direct) / luminance(A)`:
2% and below fully from above, 12% and above not at all, smoothstep between). Without this, bounce off a sunlit floor (light from below) sets the direction in every
shadow and indoors: shadows go onto ceilings, and neighbouring probes that disagree up / down average to a near-horizontal direction in the engine's interpolation
(huge stretched shadows). Light that does not come from `d` (the floor's bounce) ends up in ambient.

**Storage and the x2 range (REQUIREMENTS §9).** Per probe: ambient rgb, directed rgb (each `round(clamp(v / 2, 0, 1) * 255)`, read back `byte / 255 * 2`), direction as
lat / lng bytes (`lat = acos(y) / pi * 255`, `lng = (atan2(z, x) + pi) / (2 pi) * 255`) and a flag byte (bit 0 = valid). The byte layout of the shipping BSP lump is
decided in M6; this layout is the transport format and applies the x2 range compensation the engine requires. **Values above 2 saturate: that is the format** (a probe next to a
very bright light cannot exceed 2; its direction is kept). No other constant is applied.

**Grid.** Axis aligned, origin at the minimum of all surface bounds minus one cell, `gridSize` m (default 1) per cell, at most `maxProbes` when that is above 0 (default 0 = no limit; the cell
grows until it fits). A probe inside a light-blocking solid (`SolidSet`) is invalid; lookups interpolate trilinearly over
valid corners only and use the nearest valid probe when none of the 8 is valid. The rBSP light grid has no validity flag and the engine interpolates its 8 corners blindly, so the
export fills every invalid probe from its valid neighbours, ring by ring outwards (`fillInvalidProbes`): an in-wall probe never darkens or turns the light near a wall.

**Progressive.** `LightingService` bakes probes in steps (a GPU step is one submission of thousands of probes) after the lightmap converged (they read its final `S`),
restarts when geometry, lights or the lightmap change, and re-sends `lightvol` + `entity_light` when it finishes. The editor never waits for it: until the first result,
entities are shown with their unlit tint.

**Messages.** `lightvol` { rev, origin, cell, dims, probes: base64 of 9 bytes per probe: ambient rgb, directed rgb, lat, lng, flags } (for the preview);
`entity_light` { rev, entities: [{ i: instance, e: entityId, ambient, directed, dir }] } with decoded floats: one entry per brush entity that is lit from the grid.

**Lightmap transport is HDR.** `lightmap_tiles` carry **RGBA16F** (little endian half floats, linear, as baked: no exposure, no clamping, no encoding; alpha 0 = no value,
1.0 = value; saturates at 65504). The Shell keeps pages as half floats and uploads them to an RGBA16F texture array unchanged (filterable in core WebGL2).
`exposure` is a display setting (Level settings: colour correction): it travels in `lighting_status` and the Shell multiplies it in; changing it re-sends nothing.

**Relief in the bake (`relief_geometry`, Shell -> Core).** The Core lights flat surface pieces; texture relief (a Shell concept: projections, repeat modes, relief tiles)
is drawn over them reusing their lightmap coordinates. So the Shell sends, per authored face side with a relief, the displaced triangles it draws:
`{type:"relief_geometry", faces:[{i,e,bi,b,f,s,n,d,v}]}` (key = instance, entity, brush instance, brush, face, side 0 front / 1 cut wall; plane `n`,`d`; `v` base64 float32,
9 per triangle). Each message replaces the previous set. The Core attaches a mesh to `LightSurface::relief` only when the face's plane still matches (a message one edit
behind is ignored, never wrong). In the bake the relief replaces the flat pieces of its face as occluder, and every lightmap texel of the face is placed on it (displaced
position, relief normal) after the buried-in-solid test, which stays on the flat point. Relief and its hash/bounds take part in change detection, so editing a relief rebakes
the region. Editors, picking and outlines keep working on the source geometry only.

**Lit view.** `lit_mesh` carries a face table (`faces`: per surface piece the authored face it belongs to + its triangle count) so the Shell can compute the albedo UV
exactly like for carved fragments (`planarCoordsProjected` with the parent face's projection). Per fragment: `colour = albedo(texture) * L * exposure`, in linear light,
`L` = the lightmap texel as stored (HDR, may be well above 1) or the probe term of the entity; the **only** clamp is the display's, in the final sRGB encode. There is
**no tone mapping and no brightness constant** (REQUIREMENTS §9: lightmaps as stored, lightvols x2 only). Lighting-only shows `L * exposure` the same way. Decals are drawn in Lit (not in Lighting-only: a decal has no light of its own). Verified in a real WebGL2 context by `shell/e2e/lit.py`.

### 6.11 Fast light iteration: the direct layer, the Shell's prediction and what the live bake leaves out

Lit mode is for iterating on light, so an edit must show its direct light at once and the Core must start baking without waiting.
- **Direct layer (Core).** Next to the lightmap every base-layer tile carries `d`: RGBA16F of the *predictable* direct light (point, spot, directional and the ambient: the direct light minus the sky and rect share, which the GPU shader reports in `resX`, `lightmap_direct.comp` push bit 4, `LiveSession::JobResult::dx`; the CPU path in `evaluateDirectWithBounceCpu`), raw and signed, alpha = the ambient occlusion factor (`LiveBaker::renderDirectTile`). `lighting_status` says `directLayer` and the softening defaults (`defaultSourceSize`, `defaultSunSize`).
- **Prediction (Shell, `render/directLight.ts`, `directLightPlan.ts`).** When the document differs from the Core's mesh (a brush moved, a light edited) the Shell finds the lit-mesh triangles whose direct light may differ (the shadow of the changed boxes from every light, the reach of a changed light, the moved brush's own faces: a 16 m grid, the sun's ray clipped to the level), and renders their direct light into the atlas with shadow maps (512 cube per point light, 1024 orthographic map per 64 m cell for the sun, PCSS-like filter from the source size), in batches nearest to the camera first, a few ms per frame. What is shown there is `max(0, core total + ao * (predicted direct - core direct layer))`: the Core's noise, its staleness and everything the Shell does not model (sky, rect lights, bounces, AO) cancel out, nothing of the Core is held back. The prediction is dropped when the Core has converged after it saw the edit. On lvl2 the whole level predicts in ~170 ms; 97.5 % of its texels are within 15 % of the Core's direct layer.
- **No waiting.** The relief of textures is no longer part of the live bake (it arrived as a second snapshot right behind the edit's own, and the bake did the first one's layout and geometry for nothing); the export still gets it (`Router::lastRelief_`). The light volume is baked only after `lightvolIdleSeconds` (2) without an edit, `stickyVisibility` keeps the playable area's visibility answer of every face that did not change and lies more than `stickyVisibilityRadius` (16 m) from a changed region (the layout was 18-26 s on lvl2, now ~0.7 s; switching the lighting off and on starts afresh; the export always asks everything). `lighting_status.timingsMs.queued` says how long a snapshot waited for the bake thread.
- **Lit mode only previews light.** `skipVisibility` (editor default) never builds the playable area's visibility (every face counts as seen, so no unseen-face culling or density falloff in the live view; the export is unchanged). `firstSendFraction` is 0: the mesh goes out as soon as the layout exists, and the Shell, finding a mesh the Core has not baked, predicts the direct light of every triangle (empty Core pages, AO reads as 1) until the Core's tiles arrive.
- **Shell cost per edit.** `buildLitDraw` keeps the projected pieces of unchanged brushes and the relief message is cached per face (0.5 s each before), the prediction uses a typed-array mesh instead of the relief mesh.

### 6.8 Light styles (`light_styles.h`, `light_style_analysis.h`)

**Model (from the reference engine).** A light has a style id 0..63 (the engine's pattern table has 64 entries). Every surface has up to `kMaxStylesPerSurface` = 8 slots (the base
style 0 included), each slot a lightmap LAYER baked with only the lights of that style. The renderer shows `light = sum over slots of layer_slot * styleValue(style_slot, t)`.
Style 0 is always on (value 1). A style plays a pattern at 10 Hz: one letter per frame, `'a'` = 0, `'m'` = 1, `'z'` = 25/12; a style without a pattern is constant 1. Light volume
probes have the same slots (one probe per style layer; the entity sums them). The new BSP carries none of the legacy quirks (no ambient / directional factor rescaling per style).
Patterns: the engine's built-in table (styles 0..11), overridable per game in `game_profile.json` `lighting.lightStyles` { "12": "mmmaaa" } (letters a..z only, ids 1..63);
the Core sends the table to the Shell (`light_styles`), the Shell has no table of its own.

**Dark lights (negative brightness, §6.5) and layers:** each layer is clamped at 0 on its own, so a dark light only darkens the lights of its own style.

**Layers are independent bakes (linearity).** Light transport is linear, so layer `s` is exactly the bake of the lights with style `s` alone, GI bounces included: it gets its own
`LiveBaker` (own bounce source, own refinement, own GPU session) over the charts that carry the style, with the layout filtered by `layoutForStyle` (same page and rectangle as in the
full layout, so every layer's tiles address the same pixels). `LightingService` keeps the base layer as `baker_` and one more baker per style that has lights reaching some chart.
Test: a lamp of style 5 alone gives a layer equal (pixel by pixel, Monte Carlo noise only) to the base layer of the same scene with the lamp unstyled.
Bounce light of a style onto a chart that carries no layer of that style (not touched directly) is dropped, as in the engine.

**Which styles touch which face (chart arrangement, the REQUIREMENTS rule).** `analyseLightStyles` decides per authored face, before the layout is built: a style touches a face when from
some sample point (<= 64 per face, deterministic, area weighted) a light of that style is in range (attenuation radius, spot cone, rect side), the point faces it and a ray to the light
(to several points of its source) is not blocked by a shadow caster. That is the difference from q3map2, which assigns a style to a face whether the light reaches it or not.
`LightmapSettings::faceStyles` then makes the layout builder never merge faces with different style sets into one chart, so a chart carries as few layers as possible. If more than
7 styles besides the base touch a face, the ones with the smallest influence are discarded (influence = mean unshadowed irradiance estimate over the face's sample points) and a warning
is reported in `lighting_status.styleWarnings`.

**Transport and display.** `lightmap_tiles` carry `style` (0 = base). The lit mesh carries per vertex the style layers of its chart as a bit mask (styles 1..63 = bit style-1, three floats of
21 bits). The Shell stores all layers in ONE texture array (slice = slot * pageCount + page, slots compact: only styles that exist), samples the base and the layers in the vertex's mask
(<= 8 per fragment) and multiplies each by `styleValue(patterns, style, time)` (uniform array of 64 weights). The viewport redraws at 20 Hz while an animated layer is shown in a lit view, and
`Viewport.setStyleTime(t)` freezes the clock (inspecting a frame, tests). `entity_light` has one probe per style layer per entity; the entity shader sums the style-weighted probes.

**Decals in Lit.** The lit mesh's face table carries each lightmapped face's lightmap mapping once (`m`: affine u / v maps, page, style mask). A decal fragment lies on an authored face
(entity, brush, face id), so the Shell gives it that face's lightmap coordinates and style mask and its own albedo coordinates: `decal texture x light of the face under it`, alpha blended.
Fragments on faces of entities that are not lightmapped use that entity's light volume probes.

### 6.9 Light sources (entity definitions, Details editor, gizmos)

**Classes.** `light_point`, `light_spot`, `light_directional`, `light_rect`, `light_sky` (Unreal's PointLight / SpotLight / DirectionalLight / RectLight / SkyLight), defined in the game's
`entities.ent` like any entity, so the same definitions drive the Place tab, the Details editor and the baker's documentation. Keys (what each class has): `color`, `brightness`,
`bounce`, `cast_shadows`, `style` (all); `radius` (point, spot, rect: the attenuation radius, separate from brightness); `source_size` (point, spot: sphere radius in m; directional: angular
diameter in degrees); `inner_angle` / `outer_angle` (spot); `width` / `height` (rect). There are no physical units: brightness is "how much light a white surface receives".

**One source for the defaults.** The .ent key elements carry typed-editor hints: `default`, `min`, `max`, `step` (parsed by `entdef.cpp`, sent to the Shell in `entity_classes`). The Core test
`entities.ent: the modern light classes ... defaults are exactly what the baker uses` builds an entity of every class without keyvalues and compares the baker's parsed light to the .ent's
defaults, and checks the .ent's minimum is never clamped by the baker. The Shell reads defaults from the definitions (a built-in fallback equal to the baker's exists only for a missing .ent).

**Details editor.** A class whose keys have defaults gets a typed "Light" section: every key, its value (the default when the key is unset), a widget by type (`color1` colour picker, `real` /
`integer` number field with the .ent range, `boolean` checkbox, `list` select), a changed marker and a reset button that removes the key. Values are clamped to the range, integers rounded.
Classic Quake 3 classes (no defaults) keep the plain key / value list. The panel rebuilds when focus leaves it and whenever the selection changes (a focused input never keeps a stale entity).

**Spawn.** Place > Lights lists the five lights; a click or drag spawns the entity with no keyvalues (the defaults apply). Lights with a direction (spot, sun, rect) start pointing straight down
(pitch 90: forward = R * (0,0,1) = -Y), like a lamp on a ceiling (`defaultLightAngles`, used by every spawn path).

**Dark lights.** `brightness` has the range -1000..1000 in the `.ent` (the Details editor and the baker accept it as it is; the Core test checks the minimum is not clamped). A negative value is a
dark light that subtracts light (DESIGN §6.5). Its gizmo gets a cross (the four body diagonals of a small cube) over the normal icon and a violet-leaning colour (`isDarkLight`,
`lightGizmoColor`), so it cannot be mistaken for a normal light of the same colour.

**Preview gizmos (`model/lights.ts`, View > Light gizmos).** Pure line geometry in the light's colour (lifted so black lights stay readable), drawn as coloured overlays for the document's and
prefab content's lights. Every light shows an icon with its real shape: point = diamond; spot = cone along the arrow (outer circle and spokes, inner circle) up to 1.2 m; directional = ring of
parallel rays and an arrow; rect = the real width x height rectangle with diagonals and an arrow out of its emitting side; sky = a dome with rays coming down. A **selected** light also shows its
reach: point = attenuation sphere (+ source sphere), spot = the cone out to the attenuation radius with the outer and inner circles and the radius arcs, directional = long arrow and the cone of the
angular diameter, rect = the front half of the attenuation sphere in two planes and rays from the corners, sky = a large dome. The gizmos follow every edit (they read the entity's keys each frame).

### 6.10 Playable area and lightmap budget (`playable.h`)

Big maps make huge lightmaps because every surface gets the full density. The level says where the player can be, and lighting spends its budget there.

- **Playable area** = the union of the brushes of `editor_playable_area` entities (any number, in the map or in prefabs; convex pieces of each brush, subtractive brushes ignored). It is an *editor-only volume*: the class prefix `editor_` (`EntityClassRegistry::isEditorOnly`) means no surfaces, no carving, no solids, no decals, no vis, and nothing in the compiled map (`flattenForCompile` drops it). The Shell draws it as a texture-less tinted volume with outlines (View > Editor volumes). No volume in the level = everywhere is playable and nothing below applies.
- **Quality falloff (cascades).** The distance `d` of a face from the area (smallest over sample points of its pieces, about 2 m apart; the distance to a convex piece is `max(box distance, largest violated plane)`, a lower bound) picks a cascade `k = ceil(log_range(d / radius))`, clamped to `levels`; `k = 0` for `d <= radius`. The face's texel density is multiplied by `density^k` (`LightmapSettings::playable/lod`, applied to `Face::scale` before the islands are built, so faces of different cascades never merge into one chart). One scale per face: a big floor that reaches into the area keeps full density as a whole.
- **Unseen surfaces.** With `_lod_cull_unseen` (default 1) a face beyond the full-detail radius that no viewpoint of the area can see (`PlayableVisibility`: up to 96 viewpoints spread through the volumes, rays to the centre and vertices of the face's pieces against the light-blocking geometry, either side of the face) goes straight to the last cascade; it is never removed. On lvl2 (a box around the gameplay entities, 8 texels/m) the layout drops from 43 pages / 38 M chart texels to 20 pages / 17 M with the cascades and 19 pages / 15.6 M with the cull; the layout takes about 0.8 s.
- **Relief and vertex welding at export.** `_relief_distance` (default 48 m, 0 = off; `BspSettings::reliefDistance`): surfaces further than that from the area are compiled with the texture info stripped of its relief (`flatInfo`), so they are laid out flat; `BspStats::reliefSkipped` counts them. Nothing is skipped without a playable area. Separately, identical vertices (all 128 bytes equal) of one triangle-soup surface are written once and shared (`BspSettings::weldVertices`, on): lvl2's draw vertices went from 97.5 MB to 60.7 MB, the file from 111 MB to 71 MB; the relief of lvl2 lies entirely inside its playable volume, so the distance changes nothing there.
- **World settings** (worldspawn keys, `LevelSettings::lod`, Shell: World settings > Quality falloff): `_lod_radius` (16 m), `_lod_levels` (4; 0 = off), `_lod_range` (2), `_lod_density` (0.5), `_lod_cull_unseen` (1), `_lightvol_margin` (2 m), `_relief_distance` (48 m).
- **Light volume.** `buildLightvolGrid` spans the area grown by the margin (clipped to the surfaces' bounds); `solidProbes` leaves out the probes further than the margin from the area like probes in solids, and the export fills them from their neighbours. The light array header already carries the explicit bounds, so the engine reads the smaller grid unchanged.
- The live bake and the export use the same layout: `LightingService` re-lays out when the area or the settings change.

## 7. Editor Tools & UX — Hammer 5 (Source 2) feature inventory

Researched from Valve Developer Community's Hammer (Source 2) documentation. This is the
reference tool list to build toward; the Status column is the actual task tracker for this
section (see also §15's condensed checklist). Vertex/Face-level editing and texturing tools
are **not** listed here even though Hammer 5 has versions of them — see §8 and §9, which
supersede and substantially expand on Hammer's versions to reach real modern-DCC-grade
modeling and texturing.

**Selection & transform**

| Tool | What it does | Status |
|---|---|---|
| Selection Tool | Default tool; selects vertices/edges/faces/meshes/objects/groups | Object-level selection done (M2); sub-object modes in M3 |
| Translate Tool | Axis-constrained move gizmo (arrows), planar move (colored squares) | Axis arrows + snapping done (M2); planar handles not yet |
| Rotate Tool | Axis-constrained rotate gizmo (rings) + free rotate (center sphere) | Not started |
| Scale Tool | Per-axis + uniform scale gizmo | Not started |
| Pivot Tool | Temporarily relocate a selection's pivot; "Set Origin to Pivot" to persist | Not started |

**Geometry creation**

| Tool | What it does | Status |
|---|---|---|
| Block Tool | Create primitives (box, cylinder, arch, wedge, spike/torus, etc.) with pre-creation shape preview | **Box: done (M2.5)** — drag on a surface or the void, height by mouse, Shift = 1:1:1, grid snapping. Other primitives in M3 |
| Clipping Tool | Slice geometry along a 2D-viewport line; 3 modes (keep front/back/both); 3-point clip for arbitrary angles | Not started |
| Displacement Tool | Sculpt terrain-like detail on top of a base surface | **Done as the Terrain tool** (§8.k): a terrain is a grid of slab brushes sculpted with raise / lower / smooth / flatten / paint brushes; not a displacement on an arbitrary base surface |
| Boolean / non-destructive CSG | Add/Subtract brush resolution (see §5.1, §8) | **Beyond what Hammer 5 offers** (its own "Convex Tool" is explicitly unsupported) — this is one of our differentiators |

**Organization**

| Tool | What it does | Status |
|---|---|---|
| Outliner | Tree view of every object in the map, searchable/filterable | Not started |
| Selection Sets | Named, toggleable groups of objects (beyond the automatic "everything" set) | Not started |
| Visgroups / Cordons | Visibility grouping; cordon = bounded region for partial compiles/testing | Not started |
| Hide / QuickHide | Temporarily hide selected/unselected without a visgroup | Not started |
| Groups | Group objects to move/select as one, without the full prefab machinery | Not started |
| Undo/Command History | Full history list, not just linear undo — jump to any past state | Not started |

**Entities & content**

| Tool | What it does | Status |
|---|---|---|
| Entity Tool | Place any point/brush entity classname | Not started |
| Object Properties panel | Keyvalue editor; later an Input/Output tab for logic wiring | Not started (keyvalue editing exists at the protocol level, no UI yet) |
| Prefab placement + edit-in-place | Drag in a prefab; double-click to edit its source file directly | Prefab *resolution* implemented (§5.4); placement/edit-in-place UI not started |
| Asset browsers | Model / Sound / Prefab browser tabs (Material Browser moved to §9) | Not started |

**Not planned / not relevant**

- **Tile Editor** — Hammer's tile-based gameplay-space tool is specific to MOBA-style games
  (Dota 2); out of scope here.
- **Asset Spray Tool** — even Hammer 5 lists this as WIP/unsupported; low priority if ever.

## 8. Modeling Tools — Blender-inspired mesh editing

Since brushes are general polygon meshes (§5.1), brush editing needs genuine modeling-tool
parity with a DCC-style mesh editor, not just Hammer's basic Vertex/Face Edit. This is where
§5.1's "modeled like Blender" requirement actually lives. **All of it runs in the Shell**, on a
half-edge structure built from the stored mesh, at zero latency (§3); tools must preserve face
ids where a face survives (§5.1) and must not produce an invalid brush for classes that require
convex solids.

**Scope note:** deliberately not a full DCC. Sculpting, particle/hair systems, cloth/soft-body
simulation, and a full node-based modifier stack are out of scope — this is a level-geometry
authoring toolset, not a general-purpose modeling suite. Where Blender has both a manual tool
and a non-destructive modifier for the same idea (e.g. manual mirror vs. its Mirror modifier),
we lean toward keeping the non-destructive version live where it's cheap to (mirror, array),
and the direct/manual version where non-destructive tracking would be overkill (bevel amount
committed once you confirm the operation).

**Selection**

| Tool | Description | Status |
|---|---|---|
| Vertex / Edge / Face / Mesh selection modes | Baseline for every operation below | Not started |
| Box / lasso / circle select | Standard selection marquees | Not started |
| Select linked / select by trait (coplanar, similar) | Bulk selection helpers | Not started |
| Grow / shrink selection | Expand or contract current selection by one ring | Not started |
| Proportional editing (soft selection) | Falloff-weighted transforms around a selection | Not started, lower priority |

**Direct mesh editing**

| Tool | Description | Status |
|---|---|---|
| Extrude (region / individual / along normals) | Pull new geometry out from a selection | Not started |
| Inset faces | Create an inset ring of faces within a selection | Not started |
| Bevel (edge & vertex) | Round/chamfer edges or corners, with segment count | Not started |
| Loop Cut & Slide | Insert an edge loop and slide it along the surface | Not started — needs edge adjacency, see §5.1 |
| Knife tool | Freehand cut new edges into existing faces | Not started |
| Bridge Edge Loops | Connect two open edge loops with new faces | Not started |
| Merge Vertices (by distance / at center / at last) | Weld vertices together | Not started |
| Dissolve (vertex / edge / face) | Remove an element while healing surrounding topology | Not started |
| Subdivide | Split selected edges/faces into smaller ones | Not started |
| Fill / Grid Fill | Close an open edge loop with new faces | Not started |
| Spin | Sweep a selection around an axis to build lathed geometry | Not started, lower priority |

**Whole-brush operations**

| Tool | Description | Status |
|---|---|---|
| Non-destructive Boolean (Add/Subtract) | Our CSG-driven equivalent of Blender's Boolean modifier — see §5.1 | Not started (library spike pending) |
| Mirror (live, non-destructive) | Mirror geometry across a local axis, kept live while editing the source side | Not started |
| Array (live, non-destructive) | Repeat geometry N times along an offset/axis — railings, fences, columns | Not started, lower priority |
| Symmetrize | One-shot: force one half of a mesh to match the mirrored other half | Not started, lower priority |

**Normals & shading**

| Tool | Description | Status |
|---|---|---|
| Recalculate normals (outside/inside) | Fix winding/normal direction automatically | Not started |
| Flip normals (manual) | Per-face normal flip | Not started |
| Shading: flat vs. smooth | Per-face or per-object shading mode for the preview renderer | Not started |

**Precision & workflow aids**

| Tool | Description | Status |
|---|---|---|
| Snapping (vertex/edge/face/grid/increment) | Standard modeling snap targets | Not started |
| Numeric transform input | Type exact move/rotate/scale values rather than dragging | Not started |
| Transform orientation (global/local/normal/view) | Which axes the gizmo uses | Not started |

### 8.x Selection, cutting, mirror, proportional (batch 2026-10-14, REQUIREMENTS §21)
- **Selection rules are one pure function**, `model/meshSelect.ts clickSelect(brush, mode, pickAt, mods, sel, active, loop)`. The viewport only gathers what is under the cursor (`MeshPickAt`: vertex, edge, face, and the face's edge nearest to the cursor) and keeps the last clicked element (`meshActive`, used as the start of a Ctrl+click path only while it is still selected). Edge loop = Blender's rule (through vertices with exactly four edges, take the edge sharing no face with the one we came on; along an open border, the other border edge). Face loop = the quads crossed by the edge ring of the face's edge nearest the cursor (`edgeRing` of the loop cut). Paths: Dijkstra by edge length for vertices, the best vertex chain between the nearest ends for edges, BFS across shared edges for faces.
- **Cutting never moves anything** (`model/meshCut.ts`). Every cut is "split a planar convex face along a chord between two of its corners" (`splitFace`), which keeps planarity and convexity, so a valid brush stays valid without a re-check. Subdivide and Knife first insert the new edge vertices into EVERY face on that edge (neighbours keep a collinear extra corner, as the loop cut does), then make chords. `rebuild` gives the first piece the face's id and records `splitFrom` (new id -> face id); `finish(b, sel, before, splitFrom)` evaluates the parent face's UV map at the pieces' corners, so a cut never shifts a texture (the plain `finish` would re-project faces with stored UVs).
- **Mirror**: all vertices selected -> `mapBrushPoints(map, flip = true)` (the brush-mode Flip: loops and UVs reversed, so normals stay outward and the texture is mirrored with the geometry). Partial -> `transformVerts`; refused when a proper solid would stop being one or its volume would turn negative (the per-entity checks of `applyMeshOp` let func_detail be concave, but never inside out).
- **Proportional editing**: weights from the straight-line distance to the nearest selected vertex BEFORE the move, smoothstep falloff; the move is `p + w (map(p) - p)`, so it works for any point map (grab, rotate, scale, gizmo). Off by default; state `proportional` / `propRadius` in the store.
- **Shift+drag extrude** reuses the region extrude with an offset vector (`extrudeFaces(b, faces, 0, along)`): the gizmo's snapped axis delta is the vector, the drag stays one undo step like any gizmo drag.

### 8.y Parametric shapes (REQUIREMENTS §22 P1)
- **The generator lives on the brushes**: each part has `shape = { group, kind, part, parts, params, frame, turn }` (`model/parametric.ts`). `frame` is the drawn box (origin at the min corner, unit axes u / v / n, sizes du / dv / h) before the orientation; `turn` = quarter turns about n (`shapes.ts turned`: the same box, the shape's x axis along v, -u or -v). Building is deterministic and part i is always the same piece (`buildInFrame`), so a part can be checked against its record by comparing vertices.
- **No special cases in the tools: one reconcile pass** (`reconcileShapes(prev, next)`), run by the store on every `edit` and `dragUpdate`. A group stays a shape only if it sits whole in one entity (all part indices once, same kind / part count / box / params / turn) and every part that changed in this edit is still exactly what its record builds. Otherwise the record is dropped from the whole group. A duplicate (same group, brushes that were not members before) gets a fresh group id. So mesh edits, clipper, face drags, resize handles, deleting a part or moving one part alone all turn the shape into brushes without those tools knowing about shapes.
- **Transforms keep it**: `mapBrushPoints` maps the frame with the same point map (`mapFrame`: origin and the three edge vectors). Affine maps commute with the linear generator, so the moved parts still equal what the mapped record builds and reconcile keeps them.
- **Editing** (`regenerateShape`): rebuild every part from the patched record; part i keeps the id of old part i (extra parts new ids, surplus removed), faces keep their look by face id (stored `uvs` dropped so the world projection applies; `alignBrush` for new faces). One undo step. The Core needs nothing: it keeps the raw document, the `shape` key included.

### 8.z Level settings (REQUIREMENTS §22 P2)
- **Stored as worldspawn keys** (Quake convention, like `_ambient`), so they are saved with the map, go through undo, and reach the Core on every edit without a new message. Core `levelSettingsOf` (lights.h) parses and clamps them into `LightScene::level`; `collectLights` multiplies every light's brightness by `_light_scale`, so the CPU / GPU bake, the bounce and the light volumes all follow and only the influenced chunks re-bake.
- **Baked AO** is a per-sample factor in `LiveBaker`: the occluded fraction (32 cosine-weighted rays on the CPU BVH, weighted 1 - hit distance / radius) is computed when the sample's chunk is next refined, inherited by unchanged texels, forgotten within the radius of an edit, and applied in `shownValue` as `1 - intensity (1 - (1 - occ)^power)`. Being applied at display time it works the same for the CPU and GPU paths and every style layer, and power / intensity changes only re-send tiles; a radius change recomputes it. The bounce source keeps the un-occluded light.
- **Quality overrides** (`LightingService::applyLevel`): pass target = `_rays_per_texel` / gather rays per pass, `_supersample` (a change restarts the direct light: `LiveBaker::setQuality`), lightvol grid and probe rays (the probes are rebuilt). An absent key keeps the lighting.json value. `_bake_quality` is stored for the final bake (M6); the live view uses x1.
- **Colour correction** is display only: `_cc_exposure` overrides lighting.json's exposure, `_cc_brightness` is added after it; both travel in `lighting_status`, and the Lit shaders do `linearToSrgb(light * uExposure + uBrightness)`.

### 8.t Transparency in light baking (REQUIREMENTS §23)
- **A texture property**, `transparency` in `<texture>.json` (`mode` auto | opaque | cutout | translucent, `cutoff`, `opacity`, `tint`), sanitized by the Core like the other props. `AlbedoLibrary` reads the sidecar with the image (both stats count for its cache) and resolves `auto` per image by `classifyAlpha` (clear < 16, solid >= 240, glass when a quarter of the non-solid texels are in between): glass -> translucent with opacity 1 (the painted alpha is the opacity), on / off -> cutout, none -> opaque (`resolveTransparency`). The Shell classifies decoded images the same way for the views.
- **One packed buffer, read the same way on both sides.** `attachAlbedo` writes `OccluderMesh::alphaWords`: 8 words per albedo id (data offset, size, mode, cutoff / opacity / tint as float bits) and, for the non-opaque textures only, one RGBA8 word per texel. The grid is the alpha mask (kept at up to 1024 px, nearest-sampled so cutouts stay crisp) with the colour from the bounce texture's mip 0. `surfaceAt(record, words, b1, b2)` (raytrace.cpp) and `surfaceAt()` (both shaders) do the same nearest lookup (fract, float) and return hole / solid / translucent with its transmission `(1 - opacity a) mix(1, colour, tint)`.
- **CPU tracer**: `TriangleBvh` copies the records when the mesh has transparency. `closestHit` / `nearestHit` skip holes; `transmittance()` multiplies translucent layers and stops at the first solid hit; `occluded()` = transmittance below 1e-3. Translucent counts only where the ray enters a surface (against the triangle's winding normal), so a closed glass brush dims light once. `visibility` / `skyVisibility` in lightbake.cpp return per-channel light.
- **GPU**: with transparency the BLAS is built non-opaque with `NO_DUPLICATE_ANY_HIT_INVOCATION` (each triangle once per ray), and the shaders handle the candidates of their ray queries (direct: multiply / confirm + terminate; gather: confirm unless a hole). Without transparency the geometry stays opaque and the shaders take the old fast path (push constant bit). The direct shader gets two storage buffers (binding 9: per-triangle record + winding normal, 10: the words); the gather gets the words at binding 11, so its variable-count texture array moved to 12 (it must be the last binding).
- **Re-bake on a property edit**: `write_texture_props` calls `LightingService::textureChanged(path)`; the next input flush rebuilds the occluders and marks the boxes of the faces that use the texture as changed.
- **Views**: decoded textures carry `hasAlpha`; `drawTransparency(info)` gives the shaders `uTransp` (mode, cutoff, opacity). Cutout discards below the cutoff; translucent groups are drawn after all opaque geometry, blended, without depth writes (Unlit and the textured Lit view).

### 8.k Terrain (REQUIREMENTS §39)
A terrain editor that adds nothing to the Core: the terrain is made of ordinary brushes, so validation, lightmapping and rBSP / OBJ export already work.
- **Storage: one `func_detail` entity of closed slab brushes ("tiles")**, entity record `terrain = { cols, rows, cell, tile }` and `terrainTile = [tx, ty]` on each tile brush
  (`model/terrain.ts`; Core keeps both unknown keys as it keeps `shape`). A concave solid cannot live in worldspawn / func_group (§5.1), hence func_detail. A tile covers up to
  `tile` x `tile` cells and is a closed manifold of planar faces: the top grid points then the bottom ring (at the slab's bottom plane) as vertices; 2 triangles per cell (the
  diagonal alternates like a chessboard); one quad wall per cell edge on the tile border (vertical, so planar); one bottom polygon. Walls and bottom are `common/caulk`, the top
  keeps the painted texture. **Every edge between two top triangles is `smooth`** (§5.1a), so the surface shades and lightmaps as one; smoothing never crosses brushes, so there
  is a faint seam where two tiles meet (known). Why tiles, not one brush: a stroke rebuilds only the tiles it touches, and the lighting, keyed by (brush id, face id) (§5.6), then
  only has to look at those; one brush for 65 536 cells would be rebuilt and re-diffed whole on every stroke. Why triangles: any heightfield is planar per triangle, so Core's
  planarity rule holds with no tolerance games.
- **No second copy of the heights.** The heights are the y of the top vertices; `terrainOf(entity)` reads them back (and checks that every tile still is exactly what the record says,
  seams included), cached per entity object. The grid's place is read from tile (0, 0), so moving the whole entity, undo, copy / paste and save / load need no bookkeeping.
- **The record stays honest** (`reconcileTerrain`, run by the store after every edit next to `reconcileShapes`): a tile that changed must still be what the record builds
  from its own top heights (moving the entity, or lifting an inner vertex in Mesh mode, is fine; a vertex moved sideways, one tile moved alone, a clip, a deleted tile is not).
  Otherwise the record and the tile marks are dropped and the tiles are ordinary brushes (nothing is lost). Brushes built by the terrain code are remembered in a `WeakSet` and not re-checked.
- **Sculpting is replacing `vertices` arrays.** Face ids, the triangulation and the smooth pairs never change, so a stroke makes new brush objects only for the tiles it touched
  (a point on a tile border belongs to both tiles); every other tile stays the same object (cheap undo history, GPU caches keyed by brush). Painting replaces `texture` strings
  of the top faces of a cell. Heights are rounded to 0.1 mm and never go below the slab bottom + 0.25 m.
- **Brushes** (`stamp`): weight = 1 up to `(1 - falloff)` of the radius, then a smoothstep to 0 at the radius. *Raise / Lower*: +/- `strength` x 0.5 m at the centre; *Smooth*: blend
  toward the mean of the 3x3 neighbours by `strength` x weight (reads old, writes new); *Flatten*: blend toward a target (the height under the first stamp, or a fixed height) by
  `strength` x weight; *Paint*: the texture of the Content Browser on the cells whose centre is within the radius. Shift swaps Raise and Lower.
- **A stroke** (`TerrainStroke`) keeps a working copy of the heights and places a stamp every `max(0.2 radius, 0.25 cell)` along the path, so the result does not depend on how many
  pointer events arrived; holding still stamps again every 120 ms. `document(start)` returns the document with the tiles changed so far. The viewport drives it through the usual
  `beginDrag / dragUpdate / endDrag`, so one stroke is one undo step; Esc cancels. The pointer ray hits the heightfield directly (`rayTerrain`, a cell walk, no triangle soup);
  the brush circle (radius and falloff) is laid on the ground.
- **Responsive on large grids** (measured: 256 x 256 cells = 147 712 faces, 256 brushes): a stroke update is about 1 ms; creating takes about 1.4 s; the whole document is 11 MB, so
  `dragUpdate(fn, submit = false)` shows an update without sending it, and the viewport sends the document to Core at most every 250 ms (6 x what the last send took), plus once at
  the end of the stroke. A selected terrain makes the Texture / Details panels walk all its faces on each update, so a stroke deselects it. The Outliner shows one row per terrain.
- **Limits:** at most 512 cells per side and 65 536 cells, tiles of 2..64 cells, a 1 m minimum thickness. Textures use the axial world projection (no stored UVs), so a slope
  steeper than 45 degrees shows the side projection. The editor does not resize, rotate or scale a terrain (a moved terrain is still one; a rotated one becomes plain brushes).

## 9. UV & Texturing Tools

Modern per-face and per-brush texturing: fast auto-UV in the style of Hammer's face-fit
projection, plus genuine UV-editing capability closer to a modern DCC. Projection and UV math
live in the Shell; Core only stores and validates the resulting per-corner UVs (§5.1). This
supersedes and expands the "Texture Projection Tool" / "Paint Tool" / "Material Browser"
entries that used to live in §7's Hammer table.

**Projection / auto-UV**

| Tool | Description | Status |
|---|---|---|
| Planar / box / cylindrical / spherical projection | Fast auto-UV without manual unwrapping — box mapping picks the best of 6 axis projections per face automatically | Not started |
| Face-aligned (Hammer-style) projection | UV axes derived from the face's own plane, adjustable scale/offset/rotation per face | Not started — direct analog of Hammer's Texture Projection Tool |
| Texture Lock | Keep world-space texture alignment stable while the underlying brush geometry is moved/rotated/scaled (classic Hammer toggle) | Not started |
| Smart UV Project / Unwrap (seam-based) | Blender-style automatic unwrap from marked seams, for props/complex brushes that don't texture well with simple projection | Not started, lower priority |

**UV editing**

| Tool | Description | Status |
|---|---|---|
| UV editor viewport | Dedicated 2D view of UV islands, editable like geometry (move/rotate/scale islands and individual UV vertices) | Not started |
| Seam marking / sharp edges | Mark edges as UV seams to control how Smart UV Project splits islands | Not started |
| Island packing | Auto-pack UV islands into 0–1 space with configurable padding | Not started |
| Texel density tools | Measure and normalize texel density across faces/islands so texture resolution reads consistently across a level | Not started |
| Checker/grid overlay | Toggleable UV test-pattern overlay in the 3D viewport to spot stretching/misalignment at a glance | Not started |

**Textures** (plain albedo paths, like any Quake map — no material system for now)

| Tool | Description | Status |
|---|---|---|
| Per-face texture path + per-corner UVs | In the schema, validated by Core | **Done** |
| Per-face / per-brush texture assignment | Double-click in the browser; Face mode applies to one face, Object mode to every face of the brush | **Done (M2.5)** |
| Texture Browser (Content Browser) | Folder list, search, lazy thumbnails, PNG/JPG/**TGA** (own decoder) | **Done (M2.5)** |
| World-aligned planar projection | Upright, unmirrored on all walls; scale (m/repeat), shift, rotate | **Done (M2.5)** |
| Modify Texture panel | Scale ×2 ÷2, shift, rotate ±15/±90, numeric fields, Fit, Reset | **Done (M2.5)** |
| Texture lock, align-to-edge, treat-as-one (multi-face) | Blender/UE-style face texturing extras | Not started |
| Decals | Authored projector + derived fragments owned by the target entity (§5.6) | Not started, depends on §5.6 |
| PBR materials | Normal/roughness/metalness/AO slots | **Deferred** (§13) |

**Projection storage.** A face keeps `projection` (`scale`, `shift`, `rotate` — the parameters, so
the panel can show and adjust them) **and** `uvs` (the result, so Core, lightmapping and export never
need to know about projections). Faces with no `uvs` are projected with the default (2 m per repeat)
at draw time.

**Texture lookup.** Paths are Quake-style (no extension). Core resolves them through the game
profile (`game_profile.json`: `textureRoot` and `textureExtensions` in priority order) and serves the
bytes; browsers can't decode TGA, so the Shell decodes it itself.

**UV editing still to come:** UV editor viewport, seams, smart unwrap, island packing, texel-density
tools, checker overlay, box/cylindrical/spherical projections.

**Explicitly out of scope for now:** texture baking (AO/curvature/normal-from-highpoly) and a
full node-based shader/material graph — level texturing needs good projection + UV tools, not
a Substance-style authoring pipeline. Revisit only if a concrete need shows up.

## 10. Networking / Protocol

Local WebSocket, JSON text frames, multiple sessions. Constants live in
`core/include/core/protocol.h` (the Shell will mirror them in `protocol.ts`).

**Shell → Core**

| Message | Payload | Purpose |
|---|---|---|
| `document_update` | `{ doc }` | **Every edit, of every kind.** The whole new document. |
| `open_document` | `{ requestId, path }` | Load a map from disk (filesystem work). |
| `save_document` | `{ requestId, path? }` | Save (atomic); no `path` = current; a path = save-as. |
| `list_textures` | `{ requestId }` | Every texture under the profile's texture root → `response { textures }`. |
| `read_texture` | `{ requestId, path }` | Texture bytes → `response { ext, data (base64) }`. Path is sandboxed. |

**Core → Shell**

| Message | Payload | Sent to |
|---|---|---|
| `document_sync` | `{ rev, path, doc }` | New sessions on connect; **other** sessions after an accepted update; everyone after `open_document`; other sessions after a save-as |
| `update_ack` | `{ rev }` | The updating session only |
| `profile` | `{ profile }` | New sessions: the game profile (texture root, extension order). |
| `prefab_library` | `{ prefabs: { path: doc \| {error} } }` | New sessions; everyone when the reachable prefab set or a prefab file changes |
| `response` | `{ requestId, ok, path?, rev?, error? }` | The requester |
| `error` | `{ message }` | The sender only |

**Rules**

- **Accept/reject.** Core parses the document strictly, validates it (unique ids, brush
  structure, convexity for `worldspawn`/`func_group`, prefab references exist and don't cycle)
  and only then adopts it. A rejected update changes nothing, produces no broadcast, and the
  sender gets `error` with the reason.
- **Ack, not echo; one in flight.** See §3. `rev` increments on every accepted update and open.
- **Delivery order:** state first, the requester's reply last, so a Shell awaiting the reply to
  `open_document` finds the new document already loaded (`RouterOutput::inDeliveryOrder`).
- **Concurrent editors are last-writer-wins.** Two windows editing at once will overwrite each
  other; the expected setup is one editing window plus viewers.
- **Security.** Binds to 127.0.0.1; refuses any browser Origin that isn't `localhost`,
  `127.0.0.1` or `[::1]` (cross-site WebSocket hijacking would otherwise let any web page write
  files); every path is relative to the assets root, with absolute paths and `..` rejected.
  Symlink escapes are not handled yet.
- Whole-document resends per edit are intentional simplicity. Revisit when documents are big
  enough that the resend is the bottleneck.

**Coming**

- `list_directory` (texture/prefab/map browsers), file watching → push `prefab_library` when a
  prefab changes on disk.
- `derived_geometry` (Core→Shell, §5.6) and a one-directional lighting channel of chart tiles
  (§6.3), with a binary data plane once those exist.

## 11. Packaging & Distribution

- **Development:** `run.bat` (Core + Vite dev server + a browser tab).
- **Publishing:** `build_app.bat [notests]` builds everything into ONE folder, `release\LevelEditor\`, that runs on a
  Windows 10/11 x64 PC with nothing installed (zip it and hand it over). Steps: `build_core.bat Release` into
  `core\build_release` (a separate build dir, so a Core running from `core\build` does not lock it), `build_shell.bat`
  (`shell\dist`), then `desktop\package.mjs` (@electron/packager) which stages and packages:

  | In `release\LevelEditor\` | What |
  |---|---|
  | `LevelEditor.exe` + Electron runtime | `desktop/main.cjs` (in `resources\app.asar`) |
  | `resources\core\` | `mapeditor_core.exe`, `lightbake_tool.exe`, `bsp_export_tool.exe`, the OIDN / TBB DLLs, the MSVC runtime DLLs (app-local, from the VS `VC\Redist`), `assets\` (copy of `core\assets` without the generator scripts) |
  | `resources\shell\` | the built Shell (`shell\dist`) |
  | `README.txt` | where settings and logs live |

- **The app at run time** (`desktop/main.cjs`, helpers in `desktop/paths.cjs`, tests `desktop/tests`):
  - one instance (a second start focuses the window); arguments `[assetsFolder] [initialMap]` like `run.bat`, passed to the Core;
  - the Core runs hidden in `resources\core` (so `./assets` is the bundled folder) on a free port; its stdout / stderr go to
    `%APPDATA%\LevelEditor\logs\core.log`; if it cannot start or exits, a message shows the last lines and offers Restart (same
    port, the Shell reconnects by itself), Open the log, Quit; it is killed when the app quits;
  - the Shell is served by a tiny static server on `127.0.0.1:<free port>` (only files inside `resources\shell`), opened as
    `index.html?core=ws://127.0.0.1:<corePort>`; the Core's origin check admits it unchanged;
  - no Electron menu (the Shell has its own; no default accelerators); F12 / Ctrl+Shift+I = DevTools;
  - Dockview popouts: same-origin `window.open` is allowed (they share the page's JavaScript, as in a browser); other links
    open in the system browser; navigation away from the Shell (a dropped file) is blocked;
  - the Shell's `beforeunload` (unsaved changes) becomes a Close without saving / Cancel question (`will-prevent-unload`);
  - window size, position and maximised state in `%APPDATA%\LevelEditor\window.json`.
- **Unchanged:** settings and `lighting.json` stay next to the Core exe (`resources\core\`), so the folder must be writable
  (not Program Files). Native file dialogs and Play map are the Core's and work as before. An app icon is used when
  `desktop\icon.ico` exists (none yet). No installer, no code signing, no auto-update; Windows only.
- `cd desktop && npm start` runs the app from the working tree (`core\build`, `shell\dist`) without packaging.

## 12. Export: the `rBSP` format and the compiler (M6, REQUIREMENTS §25)

**Pipeline** (Core `bsp_export.h`; runs on its own thread with its own `Scene` holding a copy of the document, so the editor stays live):
flatten prefabs (`flattenForCompile`) -> derived surfaces (`collectSurfaces`, the same carving the editor shows) -> final bake (a headless
`LightingService` run until every style layer and the light volume converged, pass target x `_bake_quality`) -> cell tree -> sampled PVS ->
lumps -> `<name>.bsp` and `<name>/lm_NNNN.exr`.

**Units and axes.** `quake = (x, -z, y) * 32` (a rotation: handedness is kept). An entity's yaw about editor +Y is Quake's yaw about +Z.
Polygon windings are reversed to Quake's clockwise-from-the-front order.

**File.** Little endian. Header: `char ident[4] = "rBSP"`, `int32 version = 2`, then 18 `{int32 offset, int32 length}` lumps, every lump 4-byte
aligned. Struct sizes are asserted in Core and in the engine:

| # | Lump | Element | Bytes |
|---|---|---|---|
| 0 | entities | text, NUL terminated | |
| 1 | shaders | `char name[64]; int32 surfaceFlags, contentFlags` (name = `<textureRoot>/<texture path>`, no extension; contents 1 = solid) | 72 |
| 2 | planes | `float normal[3], dist`; pairs: plane `i ^ 1` is plane `i` flipped | 16 |
| 3 | nodes | `int32 plane, children[2]` (`< 0`: leaf `-(c + 1)`)`, mins[3], maxs[3]` | 36 |
| 4 | leafs | `int32 cluster` (-1 = no cluster)`, area, mins[3], maxs[3], firstLeafSurface, numLeafSurfaces, firstLeafBrush, numLeafBrushes` | 48 |
| 5 | leafsurfaces | `int32` | 4 |
| 6 | leafbrushes | `int32` | 4 |
| 7 | models | `float mins[3], maxs[3]; int32 firstSurface, numSurfaces, firstBrush, numBrushes` (model 0 = world) | 40 |
| 8 | brushes | `int32 firstSide, numSides, shader` | 12 |
| 9 | brushsides | `int32 plane, shader` (Quake 3's 8 bytes, not Raven's 12) | 8 |
| 10 | drawverts | `float xyz[3], st[2], lightmap[8][2], normal[3]; uint8 color[8][4]` | 128 |
| 11 | drawindexes | `int32`, relative to the surface's first vertex | 4 |
| 12 | fogs | (empty) | 72 |
| 13 | surfaces | `int32 shader, fog, type, firstVert, numVerts, firstIndex, numIndexes; uint8 lightmapStyles[8], vertexStyles[8]; int32 lightmapNum[8], lightmapCorner[8][2], lightmapSize[2]; float lightmapOrigin[3], lightmapVecs[2][3], normal[3]; int32 patchSize[2]` | 204 |
| 14 | lightmaps | (empty: external EXR files) | |
| 15 | lightgrid | palette: `uint8 ambient[8][3], directed[8][3], styles[8], latLong[2]` | 58 |
| 16 | visibility | `int32 numClusters, bytesPerCluster; uint8 bits[numClusters * bytesPerCluster]` (bit `c` of row `r` = cluster `r` may see `c`) | |
| 17 | lightarray | header `float origin[3], cellSize[3]; int32 dims[3], reserved` (40 bytes, Quake units / axes: the grid's own bounds, so a later playable-area volume only changes these numbers), then `uint16` per grid point (x fastest, then y, then z): index into the lightgrid palette | |

**Surfaces.** `type` 1 (planar polygon, fan triangulated) or 3 (triangle soup). Slot `k < 8`: `lightmapStyles[k]` = style id or 255 (unused),
`lightmapNum[k]` = page (file `lm_%04d.exr`), `lightmapCorner[k]` = corner of the chart rectangle in that page, `drawvert.lightmap[k]` = 0..1 page
coordinates. Slot 0 is style 0. A surface without a lightmap (an entity lit from the light volume) has every slot 255 and `lightmapNum` -1.
`vertexStyles` are all 255 (no vertex lighting); `color` is white. `lightmapVecs` are the chart's world axes in texels per Quake unit and
`lightmapOrigin` is the world point of texel (0, 0) of the page.

**8 lightmaps, 4 UV sets (the user, 2026-10-16: "Don't change vertex format. Keep 4 unique UVs, but have 8 lightmaps").** Slot k of a surface is the k-th
style layer of its chart. Slot 0 keeps the base placement (UV set 0); slots 1 and 2 are each packed densely on their own (UV sets 1, 2); slots 3..7 share
ONE placement (UV set 3, packed for the charts with a 4th layer), each slot in its own pages, so `drawvert.lightmap[3..7]` are equal and the engine
samples slots 4..7 with its 4th UV set.

**Lightmaps.** Page size = the layout's atlas (1024). RGB half float, ZIP, linear: the value is irradiance with pi absorbed (§6.5); displayed
**Light scale (HDR and dark light from 8 bit pages; `core/light_scale.h`, `BspSettings::lightmapScale`, worldspawn `_lightmap_scale`, default 1).** A page holds 0..1 in 8 bits, so bright light does not fit and a flat 0.02 uses 5 of 255 steps. Every chart is therefore stored NORMALISED: its texels (all style layers) are divided by one colour `C` = the per-channel peak over all its layers and texels, and the drawvert colour of every vertex of the chart carries `C`: rgb (linear) is the mantissa, A the shared exponent, `multiplier(A) = 2^(5 (255 - A) / 255)` = 1..32. The fragment shader computes `light = sum(lightmap_slot * styleColor_slot) * rgb * multiplier(A)` (`fs_bsp.sc`, isRBSP v2 only; opaque white = A 255 = no scale, so every older file reads the same). `C` is quantised BEFORE dividing, so the quantisation costs nothing; a peak above 32 clips and warns. Rules the GPU forces: (1) **charts joined by a smooth edge of a brush share one colour** (union-find over smooth corners, `bsp_compile.cpp`), as the shared corners are interpolated; the group's colour is the peak of the whole group. (2) **Vertex lit surfaces** (an entity with `_vertex_lit` 1 (Details > "Vertex lit only (no lightmap)" on any brush entity, func_detail included), or a smooth group of charts whose total area is at most `_vertex_lit_area`) keep no texels: the engine binds a white lightmap and the light is in the vertex colour alone, each vertex its own rgb and ONE exponent per surface (rgb and A are interpolated separately and decoded after, so one A per surface makes the interpolation exact). A smooth vertex lit group shares one exponent over all its surfaces (from the group's brightest style 0 texel) and is vertex lit as a whole. Only style 0 survives vertex lighting. (3) The OBJ exporter turns both the scale and .jpg pages off (`BspExportRequest::rbspLightmapEncoding`). Tests: `test_bsp.cpp` "light scale: ...", "bsp lightmaps: ..." (incl. the smooth cylinder check: one colour at every corner the sides share, lightmapped and vertex lit).

colour = albedo x value, exactly what the Lit view multiplies. The base layer is packed again by size at export (`lightmap_pack.h` MaxRects over all charts, biggest first; the live layout keeps buckets of one entity / texture together, which leaves pages 20-60% full) whenever that takes fewer pages (`BspSettings::repackLightmaps`); a texel moves with its chart, gutter included, and the vertices' lightmap coordinates follow. At export every chart is also **fitted** to fewer texels (`lightmap_fit.h`, `BspSettings::lightmapTolerance` = 2.5 sRGB levels, 0 = off): all style layers of a chart together, per axis halve / take a quarter off while what the engine would show (the bicubic B-spline of the stored texels, after the level's exposure, as 8-bit sRGB) stays within the tolerance for 99% of the baked texels and 2.4x it for all of them; a flat chart ends at 1x1 interior texels, a plain gradient at a few. The smaller chart is the area weighted mean of the texels each new texel covers, its gutter continues the gradient linearly (so a ramp stays a ramp up to the edge), the vertices' coordinates are scaled about the chart's interior corner and `lightmapSize` / `lightmapVecs` / `lightmapOrigin` follow. Charts with identical stored light (same size, same texels rounded to 1/16384, same layer count) share one rectangle. On the first lvl2 export (before the playable-area falloff) this saved 15% of the texels, almost all in the 5000 small charts: 91% of that level's texels are in 728 charts larger than 128 texels (merged floors), and a chart is judged as a whole, so those only shrink when everything in them is smooth. **8 bit pages** (`BspSettings::lightmapJpeg`, worldspawn `_lightmap_jpg`, default 1; needs the fit): a chart whose stored light is nothing above 1 and survives 8 bits within the same tolerance (linear bytes: byte / 255 is the light; the engine reads no sRGB lightmaps, so there is no sRGB option) is packed on pages of its own, at 8x8 block positions and sizes so no JPEG block mixes two charts (the rectangle is filled up to the block edge with the continued gradient, empty page area with the page mean). Such a page is encoded as a baseline 4:4:4 JPEG (`jpeg.h`, quality `_lightmap_jpg_quality`, 98) and read back; every chart is judged on its own (`_lightmap_jpg_tolerance`, default 8 sRGB levels, 99% of the texels within it and all within twice it: looser than the fit's 2.5, because one byte of linear light is already 12 levels next to black), the charts that fail are laid out again on the .exr pages, and a page that still does not hold up stays an .exr. On a 60 s partial bake of lvl2: 4 of 11 pages (~650 KB instead of ~5 MB as .exr), the dark charts stay .exr. Files: `lm_NNNN.jpg` or `lm_NNNN.exr` per page index, the writer removes the other extension and the pages of an older export. On the first lvl2 export 72% of the texels fit linear bytes by the fit's measure (the darks are what does not: 0.02 is 5 steps of 255). The engine's rBSP loader (`Quake3Bsp.cpp` `LoadExrLightmap`) only asks for `.exr` as of 2026-10-25, and the engine now tries `.jpg`, then `.exr`, then `.tga` for every page index. Base pages first; each further style layer is re-packed densely (only the charts
that carry it, same sizes) into its own pages after them. Gutters dilated as in the editor.

**Light grid.** Its bounds are explicit (the lightarray header: origin, cell, counts), NOT derived from the world's bounds as in Quake 3: the user
(2026-10-16) plans separate light volume bounds that only cover the playable area. The exporter writes the editor's grid as baked (no
resampling), axes converted; worldspawn also gets `gridsize "g g g"` for tools that look for it. Per style slot:
ambient and directed bytes `round(clamp(v / 2, 0, 1) * 255)`, read as `byte / 255 * 2` (REQUIREMENTS §9); unused slots have style 255. `latLong`
in Quake axes: `lat = acos(dir.z) / pi * 255`, `lng = atan2(dir.y, dir.x) / (2 pi) * 255` (mod 256), as the engine's `computeLightDirection`
reads it. Light at a normal: `ambient + directed * max(0, n . dir)` per slot, times the style value, summed. Identical palette entries are shared.

**Visibility: sampled cells, no portals (the user, 2026-10-16: "modern system that supports open levels. So no leaks to worry about").**
No sealing, no leak, no `.prt`. The level's bounds are cut into cubic CELLS (`_vis_cell`, default 4 m; grown until at most `kMaxClusters`
cells are not entirely solid). The tree is an axis-aligned kd tree whose leafs are exactly the cells (median splits on cell boundaries), so the
engine's point -> leaf -> cluster lookup works unchanged; a cell entirely inside solid brushes has cluster -1. Cell A may see cell B when a ray
from a sample point of A (points inside solids are skipped) reaches B: every cell the ray passes before it hits an OCCLUDER is visible, and so is
the cell of the hit. Occluders are the opaque surfaces of static, light-blocking classes (glass, cutout and translucent textures and moving brush
entities never occlude). Each cell casts `samples x directions` rays (3D DDA through the grid on the CPU BVH); the result is made symmetric and
dilated by one cell (a camera at a cell border), which keeps it conservative in practice. Surfaces and brushes go into every leaf their bounds
overlap.

**Collision brushes.** Every additive brush of the exported entities becomes convex pieces: a convex brush as is, a concave one split by its
own face planes; then each subtractor in its carve scope is subtracted (piece minus convex subtractor = at most one piece per subtractor plane).
Pieces go into the leafs they overlap.

**Entities.** In the ENGINE's coordinate space, which is the editor's (REQUIREMENTS §26 F5): `origin` in metres, Y up; `angles` = pitch yaw roll in
degrees, R = Ry(yaw) Rx(pitch) Rz(roll), forward +Z (MathHelper.hpp). The engine reads them without conversion for rBSP v2 (`EntityData::DirectSpace`)
and adds the legacy `angle` key (yaw - 90) for game code that reads `angle + 90`. Geometry lumps stay in Quake units / axes. The flattened map's entities (resolved names); func_group and func_detail brushes are merged into
model 0; every other brush entity gets `model "*N"` and its surfaces and brushes become submodel N (world space, no origin offset). The worldspawn
keeps all its keys (level settings included) and gets `gridsize`.

**Engine side** (`SdlWasmEngine/source/Engine/BSP`): ident 'rBSP' v2, 8-slot structs, EXR lightmaps (tinyexr) into RGBA16F textures, gridsize
from the worldspawn, none of the RBSP brightness tweaks for this format.

**Contents (REQUIREMENTS §26 F4).** `game_profile.json` `"bsp"`: `contentFlags` / `surfaceFlags` (name -> value, Quake 3's by default), `textures` and
`classes` rules (`{"match": glob, "contents": [names], "surface": [names]}`), the config's rules before the built-in ones, first match wins. Shader lump:
one entry per (texture, contents, surface flags); brush shader = the class rule's contents, else the union of its sides'.

**Decals (REQUIREMENTS §26 F8).** `collectDecals` (derived.h): each fragment becomes a triangle surface of its owner's model with the decal texture,
lifted `decalOffset` (0.25 Quake units) along its normal, lit through the chart of the face it lies on, written before the model's other surfaces,
shader surface flag `decal` = 0x100000 (contents 0). The engine draws flagged faces first in its transparent pass.

**Files.** `core/bsp_format.h` (structs), `core/sky.h` (skybox mapping, sky light tables), `core/exr.h` (EXR half + ZIP, miniz), `core/bsp_compile.h` (`compileBsp`: pure, plus `convexPieces` /
`subtractConvex` in `bsp_convex.cpp`), `core/bsp_export.h` (`exportBsp`: own Scene from a copy of the document, headless `LightingService` with
`Options::qualityScale`, files; `BspExportJob`: one export thread, polled by `Router::onTick`). Messages: protocol.h `export_bsp` / `export_cancel` /
`export_progress` / `export_result`. Shell: toolbar `Export BSP`, `state/exportStatus.ts`.

**Sky (REQUIREMENTS §28): an enclosed, automatically generated sky; the engine's skybox convention, no engine change.**
*Level settings* (worldspawn keys; `skySettingsOf` in `bsp_compile.h`, Shell `model/sky.ts`): `_sky_auto` = `box` | `areas` (absent = off), `_sky_margin`
(m, default 8, 0..1000), `_sky_texture`. The level's sky texture is `_sky_texture`, else (Auto sky on) the first skybox of `env/`, else none.
*Auto sky* (`autoSkyBoxes`, `compileBsp`): the bounds of every brush whose faces are not sky; `box` = one box around all of them, `areas` = brushes whose
bounds touch or overlap are one group (union-find), then group boxes that touch once grown by the margin are merged until none do; every box is
grown by the margin and becomes 6 quads of model 0 facing inwards (wound like every face: clockwise seen from the front = from inside), texture coordinates 0.
The floor is sky too. Only the export has them; no collision brushes. A box larger across than `BspSettings::engineFarPlane` (3000 m, the engine's
`Camera::FarPlane`) is reported: its far corners would be clipped in the game. Suggested engine fix (not made): a `vs_bsp_cube` that
sets `gl_Position.z = gl_Position.w` (the sky at the far plane, drawn with depth test LEQUAL) or keeps the sky vertex at `cameraPosition + dir * 100`.
*Sky faces* (`BspContentRules::isSky`: a skybox name, or a texture whose rule gives the `sky` surface flag; built-in rules `*cube` and `*sky*` have no
contents and `sky` + `nolightmap`; `forFace` forces that for every sky face): no lightmap slot (all 255, `lightmapNum` -1), contents 0, never dropped as
buried, not occluders of the visibility rays, put into the leaf list of EVERY leaf that has a cluster (seen from everywhere); the cell grid covers only
the non-sky geometry, so the sky does not add clusters. `Scene::lightSurfaces()` leaves sky faces out (the bake neither lights them nor lets them block
light: sun and sky light pass as through open sky); `lightSurfaces(true)` (the export) keeps them. A sky texture that is not a skybox is drawn flat by
the engine: the export warns.
*How the engine loads and draws a skybox* (SdlWasmEngine, read, not changed): `Quake3Bsp.cpp ResolveFaceTextureData`: a shader name ending in `cube`
(`nameL > 5`) is a cubemap loaded from `GameData/env/<last path part>` + `.png`, `.jpg`, `.jpeg` in that order; `CubemapTexture::DecodeSmart` takes the
six files `<base>_lf _rt _up _dn _ft _bk` when all exist (bgfx faces +X -X +Y -Y +Z -Z, `_up` turned 90 degrees clockwise and `_dn` counter-clockwise
on load), else converts `<base>` as an equirectangular panorama into those files (written next to it) and loads them. No mip maps. The face is drawn with
`bsp/vs_bsp` + `bsp/fs_bsp_cube`: `dir = normalize(v_world - cameraPosition)` in the engine's world space (vertices are loaded as `(qx, qz, -qy)` and the
model matrix scales by 1/32, so world = the editor's space: metres, Y up), `dir = (worldOrientation * dir) * (-1, 1, 1)`, `textureCube(dir)`; no light,
no fog. `worldOrientation` is the inverse of the rotation from +Y to the world's up vector (`WorldOrientationManager`), identity unless a game turns gravity.
The engine's collision puts faces whose texture contains `_cube` into a separate `WorldSkybox` body that only collides with the character capsule, so a
skybox name should end in `_cube`. Only the direction matters: the box's size and position do not change the picture.
*The resulting mapping* (editor directions; north = -Z = Quake +Y, east = +X), Core `core/sky.h` `cubemapLookup` / `panoramaLookup` and Shell
`model/sky.ts` (identical code, tested for the same directions): `_lf` looks west, `_rt` east, `_ft` south (+Z, the engine's forward), `_bk` north, each
upright and not mirrored as seen from inside; `_up` has west at its top and south at its right; `_dn` has east at its top and south at its right.
Standard cube selection on `s = (-x, y, z)`: +X: `(u, v) = ((-s.z / |s.x| + 1) / 2, (-s.y / |s.x| + 1) / 2)`, -X: `(s.z, -s.y)`, +Y: `(s.x, s.z)`,
-Y: `(s.x, -s.z)`, +Z: `(s.x, -s.y)`, -Z: `(-s.x, -s.y)` (each over the major axis); then `_up` file = `(v, 1 - u)`, `_dn` file = `(1 - v, u)`.
A panorama: `u = atan2(x, -z) / 2 pi + 0.5` (north in the middle, east at three quarters), `v = 0.5 - asin(y) / pi`; tests replay the engine's
panorama-to-faces conversion (`bgfxFaceDir`, its write order `_lf _rt _up _dn _bk _ft` and turns) and land on the same point.
*Editor* (Shell): Core `listTextures` adds `listSkyboxes` (`env/<base>`); `read_texture` of a skybox answers `{sky: {panorama, images}}` with the
engine's files; `TextureCache` keeps the faces (`SkyImages`) and a preview (the faces as a cross, or the panorama); the browser has a "Skyboxes" entry and
a SKY badge; the views draw sky faces with `SKY_VS` / `SKY_FS` (a WebGL cube map uploaded with the engine's face order and turns, sampled with
`(-x, y, z)`; a panorama with the formula above), never lit; with Auto sky on and `_sky_texture` set the perspective views draw the sky behind everything.
*Sky light from the skybox*: a `light_sky` takes its light per direction from the level's sky texture (`Scene::lightScene` attaches `Light::sky`, cached
while the files are unchanged): `SkyRadiance` = the skybox averaged (16 x 16 samples per cell, sRGB to linear) into 32 x 8 longitude / elevation cells over
the upper hemisphere, plus an irradiance table for 16 x 9 normals (cosine weighted / pi, bilinear). Only directions that point up are sky, as for the
uniform sky; a white skybox gives the uniform sky's `(1 + n.y) / 2`. Unshadowed: colour x brightness x irradiance(n); shadowed: each upward cosine ray's
transmittance x the cell's radiance. On the GPU the tables follow the light records in the lights buffer (record 7 = where they start, 1), the same
lookups in `lightmap_direct.comp`. No sky texture (or an unreadable one): the uniform sky as before.

**Not yet:** texture repetition modes (mirror / anchor) are exported as plain repeat; decals and reliefs; vertex colours; classic IBSP export;
reliefs; the engine decides collision from texture names and does not read the contents flags yet; only SlasherGame's BSP shader has the 8 slots.

### Exporters (REQUIREMENTS §31)

- **Interface** (`core/exporter.h`): `Exporter { id, name, extension, run(request, progress, cancelled) }`; `exporters()` lists them (`rbsp`, `obj`). Every exporter starts from
  `prepareExport` (`bsp_export.h`): the document in its own Scene, the final bake, the rBSP compiled in memory; it then writes what it needs. A new format = one
  function + one entry in `exporters()`.
- **Which one:** the game's `game_profile.json` `"exporter"` (default `rbsp`; unknown = rBSP with a warning). The `profile` message carries `exporter` and
  `exporters`; the Shell names the format in File > Export, asks for its extension, and Play map exports `<map><ext>`. `export_map` (alias `export_bsp`)
  replaces the path's extension by the format's. `bsp_export_tool` picks the format from the output's extension, else the profile's.
- **OBJ** (`exportObj`), for `maps/<name>.obj`; everything in the editor's space (metres, Y up). The geometry is read back from the compiled rBSP
  (BspView), so it is exactly what the game gets (texture layouts, reliefs, decals, auto sky, buried faces dropped):
  - `<name>.obj`: one object per model (`world`, `model_N` = the brush entity whose `"model"` is `*N`), one group per texture and lightmap page
    (`g <texture>@lm_NNNN`, `usemtl <texture>`), `v` / `vt` (texture, V up: `1 - t`) / `vn`, and `vl u v` = the base light style's lightmap coordinate in its
    page ((0,0) = the page's first row; not standard OBJ, readers skip unknown lines). Faces counter-clockwise; v, vt, vn and vl share one index.
  - `<name>.json`: the entity lump as an array of objects (worldspawn first), every value a string; point entities in the editor's space (as in the rBSP);
    `gridsize` (Quake units) left out.
  - `<name>.lightvol` (little endian): `char[4] "LVOL"`, `uint32 version = 1`, `float origin[3]` (m, position of probe 0,0,0), `float cell` (m),
    `int32 nx, ny, nz, styleCount`; then per style `int32 style` + nx*ny*nz probes of 9 bytes (`lightvol.h` PackedProbe: ambient rgb, directed rgb,
    lat, lng, flags; values / 2 * 255, dir from lat / lng as in the editor), x fastest, then y, then z. Absent without a bake.
  - `<name>/lm_NNNN.exr`: the pages, as for rBSP (all light styles; the OBJ refers to the base style's).

### Importer: Quake 3 `.map` (File > Import Quake Map…, `core/map_import.h`)

`import_map { path, overwrite? }` (a `.map` inside the game folder) converts the file to `<name>.map.json` beside it and opens it; the response carries `stats` and `warnings`.
`map_import_tool <assetsRoot> <in.map> [out.map.json]` does the same on the command line. The converted document is validated before it is returned (one bad brush would make the
Core refuse the whole file), so an import either opens or fails with a reason.

- **Syntax read:** classic planes (Quake 1 and Quake 3, with or without `contents surface value`), Valve 220 (`[ ux uy uz off ] [ vx vy vz off ]`), Quake 3 brush primitives
  (`brushDef`), `patchDef2` / `patchDef3`, `//` and `/* */` comments, any mix in one file. `brushDef3` / `patchDef5` / `terrainDef` (Doom 3 and later) are refused with a line number.
- **Space:** Quake units, Z up -> metres, Y up: `editor = (qx, qz, -qy) / 32`. A pure rotation, so winding is kept.
- **Brushes:** the planes are intersected (every triple, welded at 0.002 units) into a closed convex mesh whose faces list all their vertices (no T-junctions), CCW from outside;
  redundant planes drop out. Every result passes `checkConvexSolid`, also in entities that would allow concave brushes. A brush that cannot be built is skipped with a warning.
  `common/origin` brushes set the entity's `origin` and are not geometry. A brush with the `detail` content flag (0x8000000) in worldspawn / `func_group` goes to a `func_detail`
  entity (`e_detail`). Non-zero contents / surface / value flags are kept on the face as `q3`.
- **UVs:** the editor stores repeats, Quake pixels / units, so the texture's pixel size is read from the image header (`imageSize`; fallback 64x64 with a warning; shader-only names
  like `common/*` and `*cube` are silent). Classic = q3map2's `QuakeTextureVecs` (six base axes, rotation, scale, shift), Valve = `dot(p, axis) / scale + offset`, brush primitives =
  Radiant's `ComputeAxisBase` matrix. Checked against q3map2-compiled BSPs of the game's own maps (every shared planar vertex, mod 1): classic 351/351 and 333/333, brush primitives
  48/48. Each face also gets a `projection` of kind `free` that reproduces the uvs, so the Surface Inspector can edit them.
- **Patches ("displacements"):** the editor has no curved surfaces, so a patch is tessellated (quadratic Bezier, adaptive: chord error <= `patchTolerance` = 2 units, at most 16 segments
  per span) and becomes ONE closed triangle slab, `patchThickness` = 4 units BEHIND the front side (front = columns x rows, as q3map2), in `func_detail` (`e_patches`) when it came from
  worldspawn / `func_group` (patches do not seal visibility and are not convex). The top keeps the patch texture and its `s t`; back and sides take `common/caulk` (a Quake patch is
  one-sided). Edges between top triangles are in the brush's `smooth` list, so the lightmap and normals treat the curve as one surface. Collapsed edges (cones, spheres) are welded.
- **Entities:** every key survives, except those converted: `origin` -> the entity's `origin`; `angles` (pitch yaw roll in Quake's axes) -> the entity's `angles`; `angle` stays a key
  (same convention in both). Brush entities keep world-space brushes; `origin` = the origin brush or key, else the centre of the geometry (the editor's default pivot).
  `target` / `killtarget` become `refs` to the entity with that `targetname`, or to ALL of them when the name is shared (several targets, see §18.1); unknown names stay text (a warning names them). TrenchBroom `func_group` layers and
  groups without brushes are dropped (their `_tb_*` keys stay on the members). `_lightmapscale` N -> faces' `lightmapScale` 1/N. worldspawn `_ambient N` (0..255, tinted by `_color`)
  -> `_ambient "r g b"`.
- **Lights** (Quake 3 / q3map2 meaning; documented approximations): `light` + `_sun 1` -> `light_directional` looking from the light to its `target` entity (brightness = light / 255);
  `light` with a `target` -> `light_spot` (outer angle = atan(radius / distance), inner 80 %); `light` -> `light_point`. Reach = `light / fade` units (spawnflags 1, linear) or
  sqrt(light * 7500) (default falloff), radius = reach / 32 m; brightness is chosen so the editor's irradiance equals the Quake value at half the reach. `_color` -> `color`, `style`
  kept, `_deviance` -> `source_size`. A negative `light` stays negative: a dark light (§6.5) whose reach and strength come from the absolute value (the sample maps have -8000 and -1500). The game's own `light_point` (`light_color`, `intensity`, `radius`) is renamed
  (`color`, `brightness`, `radius`, same numbers: its radius is already metres). Other keys (`fogLight`, `transformPoint`, ...) are the game's and pass through unchanged.
- **Not converted:** keys that hold Quake-unit lengths for the game (`targetLocation`, `radius` of non-light entities, ...) are passed as written; `patchDef3`'s explicit subdivision
  numbers (tessellation is adaptive); `terrainDef`.

## 13. Explicitly Deferred / Not Yet Decided

- The entire real-time lighting pipeline (§6.3) and the light-volume format (§5.3) — waiting on
  your engine's convention.
- Derived geometry (§5.6): boolean library choice (Manifold vs EMBER, spike needed), decals,
  the `derived_geometry` channel. Shell-side WASM boolean preview only if latency demands it.
- Rotation composition for nested prefab transforms (translation-only today).
- Live push of prefab changes made on disk (currently picked up on the next update/connect).
- BSP export (§12): in progress (M6).
- Full parametric modifier stack (§8) — only Mirror/Array are planned as live.
- Texture baking, node-based shader graphs, and **PBR materials** (§9) — plain albedo paths only.
- Symlink-escape hardening of the path sandbox.
- Binary data plane / diff sync (§10).

## 14. Decisions & Open Questions

**Decided**

- **Carve scope** (§5.6): a subtractive brush has three checkboxes — *subtract self*, *subtract
  structural*, *subtract other non-structural*. Defaults ✔ ✔ ✘. Stored per brush as
  `carve: { self, structural, nonStructural }`; already in the Shell inspector and Core parser.
- **Non-planar faces** (§5.1): the Shell **splits** them (with the modeling tools, M3); Core rejects them.
- **Assets root is a setting** (M2.6, not built): today a command-line argument to Core; becomes a
  persisted setting the Shell can change.
- **Game profile**: one config file per game (`game_profile.json` in the assets root today —
  texture root and extension order). Grows to model formats and other per-game parameters (M2.6).
- **Entity definitions**: Quake 3 `.ent` files supported (M2.6); until then `bindings.ts` holds a small
  hard-coded list of placeable entities and lights.
- **Draw tool**: start on the surface under the cursor, or — over the void — on the ground plane, or a
  horizontal plane 10 m out along the ray if the ground is out of reach.
- **Windows is the primary platform** (§3, §16). **Bake diffuse + GI only** (§6.3).
- **Grid sizes** (m): 0.1, 0.25, 0.5, 1, 2, 4, 5, 10, 15, 20, 50, 100, 250, 500; snapping optional, per viewport.

**Open**

1. **Light-volume format** (§5.3/§6.3): what does your engine expect? Needed before lighting.
2. **Custom-origin semantics** (§5.2): independent pivot marker, or brush coordinates relative to it?
3. **Prefab rotation convention** (Euler order / handedness), once rotation support is built.
4. Project name — cosmetic.

## 15. Milestones & Task Checklist

Every milestone ends with the full source zip. `NEXT_STEPS.md` says what to do next.

| # | Milestone | Status |
|---|---|---|
| M1 | Core foundation — document model, validation, prefab library, save/open, sync protocol, hardened server | **Done** |
| M2 | Shell foundation — model, protocol client, WebGL2 Unlit viewport, Dockview, selection/translate, undo | **Done** |
| **M2.5** | **Drawing, texturing, UI, Windows build** — box tool, grid snap, textures (PNG/JPG/TGA), Content Browser, Modify Texture, UE5-style layout, vendored CMake, `.bat` scripts | **Done** |
| **M2.6** | **Settings, entity definitions, non-destructive booleans, fly camera** — persisted assets root, richer game profile, `.ent` parser, file browser, live prefab push, subtractive-brush carving (BSP CSG in Core, `derived_geometry`), free-fly camera as default | **Done** |
| M3 | Modeling tools (§8) with Blender bindings (§17): half-edge runtime, more primitives, plane-splitting | |
| M4 | Derived geometry, remainder: decals; booleans inside prefabs; carving-aware picking/drawing; swap BSP CSG for Manifold if robustness or speed demands it (§5.6) | |
| M5a | **Lightmap transport + test visualization** (§6.3 step 1) | |
| M5b | Ray tracing without textures → with textures; diffuse + GI (§6.3 steps 2–3) | |
| M6 | UV editor & advanced texturing (§9) | |
| M7 | Quake 3 BSP export (§12) | |

### M2.5 — done
- [x] Box tool: drag on a surface (or the void) → height → click; Shift = 1:1:1 in both phases; Esc cancels;
      works on tilted surfaces; live wire preview
- [x] Grid snapping: optional, per viewport, 14 sizes (0.1 … 500 m); applies to drawing and to the gizmo
- [x] Textures: Core `list_textures` / `read_texture` + `game_profile.json`; Shell cache, PNG/JPG + own
      TGA decoder, per-texture draw batches, per-corner UVs
- [x] Face selection mode (Tab); texture assignment to face / brush; Modify Texture panel; Content Browser
- [x] UI reworked toward UE5: tool bar, viewport header overlay, **Place** tab (Shapes / Entities / Lights),
      **Mesh Edit** tab (Blender tools + bindings listed, disabled until M3), Outliner, Details, Texture,
      Content Browser, History, status bar
- [x] Core builds with **no system dependencies** (`core/vendored/`): verified with system Boost/nlohmann/doctest hidden
- [x] `build_core.bat`, `build_shell.bat`, `run.bat` (Windows; **not yet run on Windows** — see NEXT_STEPS.md)
- [x] Tests: Core 44 cases · Shell 79 tests · real-browser e2e (drawing, Shift, grid, texturing, face mode, popout)

### M2.6 — done
- [x] **Camera:** free-fly is the default (RMB look, RMB+W/A/S/D/Q/E fly, Shift = 3x, RMB+wheel = speed, wheel = dolly,
      MMB pan); Orbit stays available per viewport (header dropdown); switching keeps the view
- [x] **Assets root** is a persisted setting (`settings.json` next to the exe; `set_assets_root`; File > Settings → the system's folder dialog (Core: native_dialog)
      via `browse_filesystem`, folders only). Reloads profile, entity definitions, prefab cache, re-opens the current map
- [x] **Game profile** gained `entityDefinitions`, `mapRoot`, `prefabRoot` (all confined to the assets folder)
- [x] **`.ent` parser** (`core/src/entdef.cpp`, tolerant tag scanner) → `entity_defs` message. Place tab lists the classes
      (filterable, colour swatches); marker colours; Details: description, spawnflag checkboxes, list keys, class/key suggestions
- [x] **File browser** for Open / Save As (`list_directory`), Ctrl+O / Ctrl+Shift+S, unsaved-changes indicator (`•`), confirm on discard
- [x] **Live push:** Core polls once a second; edited prefab files re-send `prefab_library`, edited `game_profile.json` /
      `.ent` re-send `profile` + `entity_defs`
- [x] **Non-destructive subtractive brushes:** Core resolves them (BSP CSG, `core/src/csg.cpp`) into surface fragments that each
      name the authored face they lie on; `derived_geometry` message; the Shell draws the carved result, falls back to the
      authored brushes for exactly the brushes an unconfirmed edit touched (Rule 1). Carve scope (self/structural/non-structural) honoured
- [x] Tests: Core 73 cases · Shell 111 · browser e2e 52 checks

**Booleans: what was decided.** The design listed Manifold vs EMBER as a spike. Neither is vendored yet; a small BSP-tree CSG
(Naylor / csg.js) was written instead because it needs no dependencies, works on the non-convex `func_detail` brushes, and tags every
output polygon with its source face natively, which is the one hard requirement of §5.6. Known limits: floating-point epsilon
(1e-6 m), a face with a hole is several convex fragments (outline seams are hidden Shell-side), picking and the box tool still use the
authored brushes, prefab contents are not carved, and there is no fragment merging. If robustness matters more later, replace
`csgSubtract` behind the same `CsgFragment` interface.

### M3 / M3.1 — done
- [x] `model/meshTopology.ts`, `meshOps.ts` (pure, 38 tests): topology via directed-edge map, selection conversion, planarity splitting, extrude, inset, bevel (chamfer), loop cut, merge, dissolve, delete+fill, normals. Vertex indices stable inside an op; face ids stable (lighting keys on them).
- [x] Modal operators (`tools/modal.ts`, `ViewportPanel.startModal`): the whole modal is one `beginDrag`/`dragUpdate`/`endDrag` transaction, re-applied from the start document on every mouse move, so parameter 0 is the identity and Esc restores exactly.
- [x] Convex rule enforced in `Store.brushProblem` for every result; refused results keep the last good state and show the reason.
- [x] Shapes (`model/shapes.ts`), decals (Core `decal.cpp`, `derived.cpp`), carving in prefab instances (instance-partitioned), picking on carved surfaces (`PickTri`), View menu, speed slider, pointer lock, grid-anchored start snap.

### M3.5 (part 1) — done
- [x] Flying is integrated per rendered frame with the real frame time (`ViewportPanel.stepFly`), with a 25 ms fallback timer that only steps when frames are throttled and shares the timestamp.
- [x] `ViewportSettings`: `showOutlines` false, `backfaceCulling` true (renderer enables `CULL_FACE` for brush/decal passes, disables it for overlays).
- [x] UV lock: `Store.state.uvLock` (toolbar toggle); `mapBrushPoints(..., lockUv)` bakes per-corner UVs (lock) or re-projects (unlocked); used by object-mode G/R/S and gizmo moves.
- [x] Selection: Shift+LMB = face, Ctrl+LMB = toggle multi (`toggleSelect` compares brush *and* face), double-click = all brushes of the entity; `faceTargets` accepts a list; `applyTexture` applies to all selected faces/brushes and to decals.
- [x] ⧉ popout via `dock.addPopoutGroup(panel, { position })` (real OS window); `restoreViewport` re-creates the viewport when the window closes.

### M3.5 (part 2): placing prefabs and entities
- [x] Point-entity boxes come from the `.ent` definition (`model/entityBox.ts`, §4); `Store.entityBox(classname)`; used by the renderer, picking and the drop placement.
- [x] Entities are spawned like prefabs: click (at the camera target, resting on the ground) or drag from the Place tab into a viewport. Shared drag state is in `ui/dragTypes.ts`; the landing position is `model/placement.ts` (snap the point under the cursor, then push the box out along the surface normal), used by both the preview and the drop so they cannot disagree.
- [x] While dragging, the viewport draws a half-transparent ghost at the final position (`Frame.ghost`: cached content plus an offset folded into the view-projection matrix, so GPU buffers are reused). Prefab ghosts use real content; entity ghosts a filled box in the class colour.
- [x] Prefab content for previews: Core message `read_prefab` (one file, sandboxed) and `state/prefabPreview.ts` (loads a file and what it nests, expands it at the origin like a real instance).
- [x] Preview icons in the Place tab (`render/icons.ts`): entities = isometric box of their real proportions in the class colour; prefabs = drawn with the real renderer into an offscreen canvas.
- [x] Popout of any group, see §6.1.

### Later
- [ ] Modeling tools (§8); rotate/scale gizmos; multi-select; duplicate; more primitives
- [ ] Derived geometry (§5.6); lighting (§6.3); UV & texturing (§9); BSP export (§12); Electron (§11, done 2026-10-17)
- [ ] Prefab placement + edit-in-place UI, override authoring (§5.4)
- [ ] Outliner extras: selection sets, visgroups/cordons, hide, groups (§7)

## 16. Build & Platform Notes

- **Windows (primary):** `build_core.bat` (finds MSVC via `vswhere`, CMake + Ninja, runs the tests),
  `build_shell.bat` (npm install, typecheck, tests, build), `run.bat` (starts Core and the dev server,
  opens the browser), `build_app.bat` (the publishable desktop app, §11). Needs Visual Studio's C++ workload (bundles CMake and Ninja) and Node 20+.
- **Vendored dependencies:** `core/vendored/` holds Boost (Beast/Asio/System subset), nlohmann/json and
  doctest. No package manager. See `core/vendored/README.md` for versions and how to update.
- **Linux/macOS:** `cmake -S core -B core/build -G Ninja && cmake --build core/build`. The Core is portable;
  only Windows is a first-class target.
- **Line endings:** `.gitattributes` forces CRLF for `.bat`.

## 17. Key Bindings (Blender / UE conventions)

**Implemented (M2.6)**

| Key | Action |
|---|---|
| `Q` / `B` / `T` | Select tool / Box tool / Terrain tool (§8.k; `[` `]` change the brush radius) |
| `Tab` | Toggle Object ↔ Face mode |
| `Ctrl+Z`, `Ctrl+Y` / `Ctrl+Shift+Z` | Undo / Redo |
| `Ctrl+S` | Save |
| `Del`, `X` | Delete selection |
| `F` | Frame everything |
| `Shift` (while drawing) | Keep 1:1 (base) and 1:1:1 (height) proportions |
| `Esc` | Cancel drawing |
| RMB drag (Fly camera, default) | Look around |
| RMB held + `W` `A` `S` `D` / `E` `Q` | Fly forward, left, back, right / up, down (Shift = 3x) |
| RMB held + wheel | Change fly speed |
| Wheel / MMB drag | Dolly forward-back / pan |
| RMB drag / wheel (Orbit camera) | Orbit / zoom |
| `Ctrl+O` / `Ctrl+Shift+S` | Open / Save As |

**Planned (M3+, shown greyed in the Mesh Edit tab)** — Blender bindings

| Key | Action |
|---|---|
| `Tab` | Enter/leave **mesh edit mode** (Object mode ↔ edit mode; sub-modes below) |
| `1` / `2` / `3` | Vertex / Edge / Face select (in edit mode) |
| `G` / `R` / `S` (+ `X` `Y` `Z`, numeric input) | Grab / Rotate / Scale, axis-constrained |
| `E` | Extrude |
| `I` | Inset |
| `Ctrl+B` | Bevel (edges); `Ctrl+Shift+B` bevel vertices |
| `Ctrl+R` | Loop cut and slide |
| `K` | Knife |
| `M` | Merge |
| `X` / `Del` | Dissolve / delete menu (in edit mode) |
| `F` | Fill (make face); `Ctrl+E` edge menu |
| `A` / `Alt+A` / `Ctrl+I` | Select all / none / invert |
| `Alt+LMB` | Select edge/face loop |
| `H` / `Alt+H` | Hide / reveal |
| `Ctrl+D` | Duplicate |
| `Shift+A` | Add (shape menu) |
| `Numpad .` / `F` | Frame selection |
| `Shift+RMB` / `Ctrl+…` | UE/Blender-style context menus |

Notes: `F` means *frame* in Object mode and *fill* in edit mode (as in Blender, where the two live in
different contexts); bindings are collected in `shell/src/ui/bindings.ts` so the Mesh Edit tab and the
help stay in sync with what is actually wired.

## 18. Planned designs (decided in conversation, not built yet) — read before implementing
Each block says *what* was decided so it is not re-invented. Status lives in REQUIREMENTS.md.

### 18.1 Prefab identity, references, overrides (M3.3, priority #1)
- **Instance** = `{ id (reserved, never shown), name (unique among siblings in its scope; default = file basename + incrementing number: door_prefab, door_prefab2…), file, origin, angles, scale?, overrides }`. Renaming changes `name` only.
- **Entity names:** the entity's own name is its `targetname` keyvalue (key name configurable in the game profile: `nameKey`). **References are never typed strings.** An entity stores `refs: { "<key>": "<scope-relative path>" }` for every reference key (`referenceKeys` in the profile, default `target`, `killtarget`; additionally any key whose `.ent` type is `target`). A path is `entityId`, `childInstanceId/entityId`, `childA/childB/entityId`…; it is relative to the **scope in which it is declared**: for a ref written in a prefab file the scope is that prefab instance; for a ref written in an *override* the scope is the instance's **parent** scope (this is how two sibling child prefabs reference each other, or a child references something in its parent).
- **Resolved name** (shown in the UI and written at compile) = instance names from the root joined with `|`, then the entity's `targetname`: `button_prefab3|button` → `door_prefab2|door`; nested: `outer|inner|door`. Root-document entities have no prefix. Because refs use ids, **renaming an entity or an instance rewrites nothing** — resolved names are recomputed. Several entities may share one `targetname` (by design: a Quake `target` triggers every entity with that name), so a duplicate name is no problem.
- **Several targets (2026-10-24).** `refs["<key>"]` is one path (the usual form, unchanged) or an array of paths: `{ "target": ["door1", "door2"] }`; Core's `RefTargets`, the Shell's `RefValue` (`refPaths`, `refValueOf`). The key still holds ONE name in the compiled entity, so the targets must share it: `flattenForCompile` writes the first named target's name; an unnamed target joins that name (a problem line says so); a target with another name is reported (`different names`) and not reachable through that key; unresolved paths are reported and skipped, the others still count. An override replaces the whole list. The Details editor shows one picker per target, a trailing one to add (the pipette adds too), `×` removes one, and when the names differ a **Share first name** button gives the document's targets the first one's name (`shareTargetName`, one undo step). Links are drawn to every target; a plain-text `target` name links to every entity with that name.
- **Overrides:** `instance.overrides = { "<entityPath within the instance>": { keyvalues?, refs?, origin?, angles?, scale?, deleted? } }`; only differences are stored; each overridden field has a **revert** button; the sub-tree (instance → child instances → entities) is shown in the Outliner and Details.
- **Compile = flatten:** a Core function walks the instance tree applying overrides, transforms and name resolution and emits one flat entity list with resolved `targetname`/`target` strings (used by export, M6, and by tests now).
- **UI:** Place tab lists prefab files (click or drag into the viewport → new instance with generated name); instances are pickable (their expanded content picks the *instance*), selectable in the Outliner, movable/rotatable/scalable with gizmo and G/R/S, deletable, renameable; the reference picker is a list of all entities (grouped by prefab) plus a **pipette** (click an entity in the viewport).

#### 18.1a Schema of N1 step (a) — additive, old maps still load (schema stays 0.2)
```
prefabInstances[]: { id, name?, file, origin?, angles?, scale?, overrides? }
entities[]:        { ..., refs?: { "<key>": "<scope-relative path>" } }
overrides:         { "<entityPath>": { keyvalues?: {k:v}, refs?: {k:path}, origin?, angles?, scale?, deleted? } }
```
- `name` is optional. A missing name is *derived*: `<basename of file without .map.json>`, then `…2`, `…3` in document order, skipping every
  explicit name and every name already handed out (`effectiveInstanceNames` in Core, `instanceNames` in the Shell; both must give the same
  result - tested on shared cases). Explicit names must be unique among the instances of one document and may not contain `|`, `/` or be empty.
- `entityPath` (override key) is relative to the instance that owns the override: `entityId`, or `childInstanceId/entityId`, or deeper.
  Ids, not names - renaming never breaks an override.
- **Legacy overrides** (`{ "<entityId>": { "<key>": "<value>" } }`, every value a string) are read as `keyvalues` overrides. A structured
  override is recognised by any non-string value; both forms may coexist in one file. The Shell writes the structured form only.
- `refs` paths are stored, validated for shape (non-empty segments, no `|`) and **resolved later** by step (b) (`flattenForCompile`).
- Core keeps raw JSON as the canonical copy (Scene), so all of this survives a save from either side.

#### 18.1b `flattenForCompile` (N1 step b) — semantics
One walker in `core/src/prefab.cpp` serves both `resolvePrefabs` (derived geometry) and `flattenForCompile` (names + refs), so the viewport, the
carver and the compiler always agree. It now honours instance `angles`, `scale` and `overrides` (before: origin only).
- **Transform of an instance:** `p_parent = R·(S·p) + origin`, `R = Ry(yaw)·Rx(pitch)·Rz(roll)` (same convention as decals, DESIGN 18.3 - this
  also answers open question 3), `S = diag(scale)`. Nested instances compose the affine maps. A negative-determinant map (mirror) reverses face
  loops and uvs so brushes stay outward-wound.
- **Entity angles** in root space = decompose(`R_chain · R_entity`) (rotation parts only; gimbal lock at pitch ±90 sets roll = 0). Brush entities
  get composed `angles` as well (it is their direction metadata, e.g. door move angle); their brush vertices are transformed.
- **Scale:** positions and brush vertices use the full affine map. For non-brush entities the accumulated instance scale multiplies the
  `modelscale_vec` keyvalue (default `1 1 1`) when it is not identity. (Decal width/height/depth scaling is part of M3.5 item 3.)
- **Overrides** are looked up per entity by path `childInstanceId/.../entityId` relative to each instance in the chain, applied inner to outer so
  the **outer instance wins**. `origin`/`angles`/`scale` replace the entity's local value *before* the instance transform; `keyvalues` set
  keys; `refs` replace refs; `deleted` drops the entity.
- **Names:** the entity's own name = `keyvalues[nameKey]` (default `targetname`, after overrides). Resolved name = effective instance names from
  the root joined with `|`, then the entity name (`outer|inner|door`). Root-document entities keep their name. Two entities with the same resolved
  name -> a problem (warning), never an error.
- **Refs:** each ref remembers its *scope* = the chain of instance ids it is relative to: a ref written in a prefab file -> the entity's own chain;
  a ref written in an override of instance *I* -> the chain of *I*'s parent. Resolution is a lookup of `chain + path` in an index of all flattened
  entities by full id path, so it works inside a prefab, into a child, between siblings and across any depth, and renaming anything changes only
  the *output* names. The resolved name is written into `keyvalues[key]` and `refs` is cleared. Unresolvable / deleted target -> problem + key
  omitted; a referenced entity without a name gets a generated `<prefix>|<entityId>` (+ problem).
- Output: `FlattenResult { entities: [{ entity, resolvedName, instanceNames, instancePath, sourceEntityId }], problems }`; inclusion cycles and
  excess depth still throw `PrefabResolveError`.

### 18.2 Parametric shapes
A placed shape is stored as a **generator record** on the brush(es): `{ kind, params, frame }` (frame = oriented box: origin, axes, size). Details edits params and regenerates all brushes of the shape in place (ids of the shape's brushes are stable by index). Orientation is chosen at placement (plane/normal, rotation about it) and editable. Entering mesh mode drops the record: the brushes become ordinary; if their entity is not structural (convex-only classes are `worldspawn`/`func_group`), they move into a `func_detail` entity (or stay if already structural).

### 18.3 Rotation and scale of entities
`angles = [pitch, yaw, roll]` degrees, composed as `Ry(yaw)·Rx(pitch)·Rz(roll)` (the decal convention in `core/src/decal.cpp` and `model/decal.ts`); rotating an entity composes the new rotation into this matrix and decomposes back. `modelscale_vec = "x y z"` stores scale. Decals: orientation = `angles`, size = the keyvalues `width` (local X), `height` (local Z), `depth` (local Y, projection axis; default 0.5) — this is what Core already reads. A prefab instance's scale stretches each of them by the length of the transformed axis (`resolvePrefabs`/`expandPrefabs`); `modelscale_vec` is for models only. Direction arrows: forward = `R*(0,0,1)`. The decal projector is drawn as an oriented box. Snap values: position (grid), rotation (degrees), scale (step) are separate settings.

### 18.4 UV lock
A global toolbar toggle (default ON). Brush-mode transforms: when ON the face UVs travel with the geometry (translation: compensate the projection shift; rotation/scale: bake per-corner UVs); when OFF the world-aligned projection is re-applied. Mesh-mode vertex moves currently re-project (documented limitation until the affine UV model of M3.4).

### 18.5 Bevel with segments
Profile arcs are quadratic Béziers with control point at the original edge vertex: `B(t) = (1-t)²S + 2t(1-t)V + t²T`, N segments. Both strips meeting at a corner share the same arc points, so the surface stays closed; remaining holes (3+ beveled edges meeting, or a single edge ending at a corner) are filled by `closeOpenLoops`. N is changed with the mouse wheel in the modal.

### 18.6 Subtractive brushes and the convex rule
The convex requirement applies to **solids** (additive brushes in `worldspawn`/`func_group`). **Subtractive brushes may be concave anywhere** (they are never compiled as solids). Mesh edits that make an additive brush concave convert it to `func_detail` (setting "Auto-convert concave brushes to func_detail", default ON; OFF refuses the edit).

### 18.7 Lighting configuration files (M5)
`lighting_classes.json`: `{ "lightmapped": [classnames], "castShadows": [classnames] }` — Core loads and sends it; no classname is hard-coded. Decals own no lightmap area: their fragments sample the **lightmap UV space of the parent face**. Lit view = engine rendering of the new BSP: lightmaps as stored (EXR floats), lightvols ×2 compensation only, **no magic brightness constants**. Light entities (point, spot, directional, rect, sky …): brightness (no physical units), colour, attenuation radius, source radius/length, bounce power, cast shadows, style (0–7).


### 18.8 MCP for AI level prototyping (design only, REQUIREMENTS §37)
**`MCP_DESIGN.md` is the source of truth** (architecture, pipeline, tools, prior art, lessons). Decided so far: a Node/TypeScript MCP server
that joins Core as one more WebSocket session and reuses `shell/src/model`; the model writes logic (route, gates, encounters) and never
places geometry by improvised coordinates; space comes from tool-side building types and typologies sized by the player's metrics;
levels are a path through a setting (most of a building is closed backdrop); every result is verified by a reachability check per lock
state; the user judges taste. Needed here: labelled remote edits as one undo step (today a `document_sync` from another session resets
the Shell's history), `baseRev` on `document_update`, player metrics in the game profile, a render request with overlays and entity labels
off. Until it exists, levels are prototyped with the project skill `.claude/skills/level-blockout/`.

### Convex rule and auto func_detail (M3.5 item 4)
- The convex rule applies to **solids** only. A subtractive brush (`op: "subtract"`) is never compiled as a solid, so it may be concave in any entity (Core `validateDocument`, Shell `Store.brushProblem`; CSG tests prove the exact removed volume for an L-shaped cutter).
- **Auto-convert** (Shell setting "Concave → func_detail", default on): when a mesh edit - instant or a modal drag (G/R/S/E/I) or the gizmo - makes a brush in a convex-only entity concave but otherwise valid, the brush moves into a new `func_detail` entity instead of the edit being refused. One undo step; the selection (and the mesh selection) follow the brush when the drag ends. With the setting off the edit is refused as before.
- Right-click (without dragging) opens a context menu: Make entity > class, Move to world, Clipper, Make hollow, Merge, Duplicate, Delete. RMB-drag still looks around; the pointer is locked only once the drag starts.

## Texture properties and repetition behaviour (M3.4 part A, REQUIREMENTS §19)
- **Storage.** Core stores `<texture file incl. extension>.json` next to the texture (`textures.cpp`: sanitise, merge, atomic replace; unknown keys kept). `read_texture` returns it as `props`; `write_texture_props` writes it. The Shell parses leniently (`model/texprops.ts`). `pixelsPerMeter` default comes from the game profile (64).
- **Texture environment.** `texEnv` (model/texprops.ts) is what the pure UV functions read: `info(path)` (size, effective ppm, props; undefined while loading) and `rev`, bumped on any load, property edit or profile change. `TextureCache` is the provider. Caches of built meshes (renderer per brush, carved meshes, lit draw, stale overlay) compare `rev` and then `textureSignature` so only meshes whose textures changed are rebuilt.
- **Repetition as UV data (`model/uvmesh.ts`).** The sampler is never touched. A face has an affine map p -> (u, v) (stored uvs when affine, else its projection). Per axis the repeat setting becomes a piecewise linear function t -> t' with segments (`axisSegments`); anchoring uses the AUTHORED face's extent. A triangle is clipped against the cells (pairs of segments); each piece is a convex polygon with exact UVs, given as barycentric weights of the source triangle so any per-vertex attribute (lightmap uv) can be interpolated. The cut points are the invisible vertices: same plane, no outline, no brush change. Pieces may use another texture (alternate), so they go to that texture's draw group. More than `MAX_CELLS` cells per face = plain fallback.
- **Natural size.** `projectionOf(face)` without parameters uses `defaultProjectionFor(texture)` = pixels / ppm (2 m while not loaded). Explicit `projection.scale` is untouched.
- **Heat map** (`model/texelDensity.ts`): octaves of (worst axis pixels per metre) / authored ppm; viewport pass `drawTexelHeat` buckets faces into 17 colours.
- **Window** (`ui/texturePropsWindow.ts`, preview maths in `model/texPreview.ts`): the preview is drawn from the same plan as the viewports.
- **Origin and edge bleed (2026-10-10).** `uv.ts` `bottomLeft` / `justify` place a texture relative to the authored face's extent along its rotated axes; `withBottomLeftDefault` bakes that into a face at creation / first texturing. Edge bleed: `AxisRepeat.edge`; `TexInfo.seamless` comes from `analyseSeams` (textures.ts, strips + `model/seamless.ts`); `makePlan` also runs for plain-props textures when an axis clamps; `axisSegments(..., tiles)` splits segments at integer tiles (`Seg.tile`) and `resolve` applies `inset` per tile.
- **Projection kinds, Flow, hotspots (2026-10-10).** `Projection` gained optional `shear`, `kind` (axial | ortho | cam | free) and a frozen `basis`; `planeCoords` / `placedCoords` (uv.ts) are the single place that turns a world point into plane coordinates, `projectionFromAffine` makes any plane affine editable ('free'). `model/flow.ts` unfolds a neighbour face across the shared edge into the solved face's plane (isometry) and evaluates the solved face's affine map there. Hotspots live in `TextureProps.hotspots` (image-space rectangles); `model/hotspots.ts` chooses by distortion then size and builds the exact projection. UV lock for mesh vertex moves: `finish(b, sel, before)` / `refreshUvs(b, before, parentOf)` evaluate the face's OLD affine map at the new corner positions.

## Texture groups (REQUIREMENTS §20)
**What it is.** A rule set that lays textures out over a face. The face keeps `layout: { group, seed?, params?, rotate?, flipU? }` (only differences from the defaults are stored; `model/texgroup.ts` `parseFaceLayout` / `faceLayoutToJson`). The group is a file `<assets>/<texGroupRoot>/<name>.texgroup.json` (`texGroupRoot` in `game_profile.json`, default `texgroups`). Core only stores it (`core/src/texgroups.cpp`: list / read / write / delete, name sandbox, 1 MB cap, atomic write; protocol `list_texgroups`, `read_texgroup`, `write_texgroup`, `delete_texgroup`); the Shell interprets it.

**Format.** Documented at the top of `shell/src/model/texgroup.ts` (one place, next to the reader). Summary: `params` (number / int / bool / choice), `slots` (name -> texture), `fallback`, `rules` (named nodes), `root`. Nodes: leaf `tex` (fit tile / stretch / width / height, origin face / cell, align, flip, rotate), `split` (axis u or v; parts of fixed / param / fraction / natural / flex size; optional parts dropped by priority; `repeat` to fill with whole equal copies, named with `as`; `leftover` and `fill`), `choice` (first match / random by weight / cycle by an index), `ref`. Conditions are structured JSON (`all` / `any` / `not` / comparisons / a variable name), never code, so a group file cannot run anything. The reader is lenient and reports what it dropped (`parseGroup` -> `{ group, errors, warnings }`).

**Pipeline (one code path).**
1. `model/layout.ts` `solveLayout` (pure, deterministic): group + face size + parameters + seed -> `Cell[]` (rectangles in metres, bottom-left origin, y up). Randomness is `rand01(seed, path)` of a hash of the place in the rule tree, so a result never changes unless the group, the parameters or the seed do. Caps: 2048 cells, 40000 steps, nesting 40; exceeding a cap gives no cells (the face is drawn plainly) and a warning.
2. `model/layoutPlan.ts` `layoutFrame`: the face's own frame (x right as seen from the front, y up; for floors and ceilings world -Z up, like Project Ortho), independent of the texture projection, so it does not depend on the face's UVs. `cellMap`: affine UV of a cell from fit / align / flip / rotate. `buildLayoutPlan`: a `FacePlan` whose `pieces(tri)` clips the triangle to every cell and gives each piece the cell's texture and UV, with barycentric weights against the source triangle (so lightmap UVs, carved fragments and relief placement work unchanged).
3. A cell whose texture needs cuts (mirror, anchor, alternate texture, edge clamp) is planned by the existing `makePlan` as a small face of its own (own world affine map, own UV extent; the whole face's extent for `origin: face`, so anchoring means what it means on the face). Its pieces are re-expressed against the source triangle (weights composed), which is how those behaviours keep working inside a layout.
4. `uvmesh.planForFace` returns the layout plan when the face has a usable `layout`; while the group or any slot texture is not loaded it returns the plain plan. Signature: `brushTextureSignature` includes the group's file version and its slot textures, so caches rebuild when a group is edited or a slot's relief changes.

**Relief.** Not special-cased anywhere: relief is placed by the UV of the piece that samples the texture (§19), and every cell is such a piece. Verified on the plain, carved, Lit and bake paths (`tests/layoutRelief.test.ts`). Consequences authors should know: a `stretch` cell shows exactly one relief tile that fills the cell (depth stays in metres); a `tile` cell repeats it at the texture's natural size; neighbouring cells join because relief keeps its border at depth 0.

**State.** `state/texgroups.ts` `TexGroupCache` (load on first request, `groupEnv` provider, bumps the texture environment through `TextureCache.notify()` so every viewport rebuilds). Actions in `state/actions.ts`: `applyTexGroup` (each face takes the rule branch that fits its orientation, so a whole brush can be done at once), `setFaceLayout`, `rerollLayoutSeed`, `clearFaceLayout`, `selectedLayout`.

**Not built yet / known gaps.** See REQUIREMENTS §20 T8 and T9.

## Fast Texture Tool (REQUIREMENTS §6 B5)
**Model (`model/ftt.ts`, pure, `tests/ftt.test.ts`).** A face's UV is one affine map of its plane (`faceAffine`, `Projection`). Every edit of the tool is a 2D transform `Xform` of UV space (`translate`, `scaleAbout`, `rotateAbout`, `mirrorAbout`, `compose`) applied to the whole island: `transformFaces(brush, ids, t)` composes it onto each face's gradients (`gu' = a gu + b gv`, `du' = a du + b dv + tx`, likewise v) and writes the result back with `projectionFromAffine` (kind 'free', frozen frame) plus `projectFace` for the stored `uvs`. A face whose result is degenerate is left alone. The tests check, corner by corner, that the new UVs equal the transform applied to the old ones, and that an island flowed over a shared edge still joins there after a rotate + scale. UV space is drawn as stored: u right, v DOWN, so positive angles are clockwise on screen.
`dragXform(kind, start, cur, bounds, opts)` turns a pointer drag into an `Xform` (move with snap of the island corner; scale handles keep the opposite side or corner fixed, Shift = aspect, past the opposite side = mirror, never collapsing; rotate about the centre, Shift = 15 degrees). `hitTest`, `pointInPolygon`, `nearestEdge`, `alignEdgeXform` (rotate the nearest axis onto an edge about its midpoint), `idealSize` (extent in metres over natural size), `unwrapFaces` (follow = `flowFaces`, ortho / axial = the Surface Inspector projections).
**State (`state/actions.ts`).** `fttTarget` (selected faces of the first brush), `fttApply` (one `store.edit`), `fttDrag` (`store.dragUpdate` relative to the document at `beginDrag`, so the viewports update live and history gets one entry at `endDrag`), `fttUnwrap`.
**Window (`ui/fastTextureTool.ts`).** Floating non modal `div` (not `openDialog`, which is modal), singleton, Shift+Q toggles (`app.ts`), button in the Surface Inspector. Follows the selection through `store.subscribe`. Drawing uses a canvas pattern of the texture with a DOMMatrix of one repeat per `view.s` pixels; the handles' geometry and picking come from the model so they cannot disagree.

## Which UV is authoritative: stored `uvs` before `projection` (2026-10-11)
`Face.uvs` (one pair per loop corner) and `Face.projection` are written together by every texturing action, but UV lock (the default for moves) rewrites only `uvs`: the texture travels with the face and the projection becomes stale. The drawn face uses the stored uvs (`uvsForDraw`, `faceAffine`), so every consumer that needs the UV of a point of the face must use `faceUvAt(brush, face)` (model/uvmesh.ts) and never `projectionOf(face)` + `planarCoordsProjected`. `faceUvAt` returns the affine map of the stored uvs when they are affine, a per-triangle interpolation of them when they are not, and the projection when there are none. Consumers: `buildCarvedMesh`, `reliefBake.pieceTriangles`, `litTextured`, `litStale`. The projection stays the data the Surface Inspector edits.

## Simple texture sets (REQUIREMENTS §20b)
**Data.** `TexGroup.kit: Kit | null` (`model/texgroup.ts` reads the file key `kit` with `parseKit`; `slots` is filled with the set's textures as `s0..sN` so loading, signatures and the relief border check work unchanged; `root` is only a placeholder). `Kit` = `WallKit` (`top` / `middle` / `bottom` lanes of `KitPiece { cells: (string|null)[][] rows top to bottom, weight, place }`, `tile`, `avoidRepeat`) or `FrameKit` (nine slots). A lane's pieces are all as tall as the lane (`rows`); a piece with a null cell is "not chosen yet" and is not used.
**Solver (`solveKit`, `model/tileset.ts`).** `solveLayout` dispatches to it when `group.kit` is set; it returns the same `Cell[]` (fit `stretch`, origin `cell`, one cell per tile), so `layoutPlan.ts` and everything after it are shared with the advanced groups. Wall: columns `nC = round(W / tileW)`, rows `R = bottom + nM * unit + top` with `nM = round((H - (bottom + top) * tileH) / (unit * tileH))` (the middle lane is dropped when it would not get half a unit), equal row and column sizes so tiles line up exactly. `placeRow` fills one lane row: first the fixed places (one of the left / right / middle candidates, every `once` piece at a random free spot), then each free run left to right with weighted ordinary pieces that fit the run, skipping the previous piece when there is a choice; a run nothing fits is returned as `gaps` and drawn with the fallback. Randomness is `rand01(seed, "<lane>#<n>")` (`model/rng.ts`), so a wall never changes unless the set, the size or the seed does. Frame: corners `min(tile, side / 2)`, edges and middle in `round(inner / tile)` equal copies per axis.
**Editing operations** (`fitPieceRows`, `fitPieceCols`, `setLaneRows`, `addPiece`, `movePiece`, `duplicatePiece`, `removePiece`, `addTextures`, `suggestWallKit`) are pure and tested; the editor only calls them. A taller piece repeats its last row, a wider one its last column, so a resized piece is usable at once.
**UI.** `ui/tileSetEditor.ts` (editor), `ui/texGroupsPanel.ts` (Simple / Advanced tabs; a group's tab is decided by whether its file has `kit`), `app.ts` `openGroup` (routes to the editor by file content).
**Match names (`KitPiece.match`).** `solveKit` keeps `below`, the Match name of the piece under each column, and passes it to `placeRow`, which treats `matches(piece, col)` as a filter on every choice (the one candidate of left / right / middle, the spots of a `once` piece, the ordinary fill). A filter that would leave nothing is relaxed (the placement is made and one warning is added), so a set can never dead-end. The names of a row are returned as `tags` and become `below` for the next row, including between repeated Middle units.
**Stretch limit (`WallKit.maxStretch`, `FrameKit.maxStretch`; fraction, file value in percent).** `stretchOf(scale) = max(scale, 1/scale) - 1`. `axisTiles(len, tile, limit, natural)` splits a length: scaled (the whole count with the least scaling, `bestCount`) when within the limit, else natural size with the last tile cut (`Seg.crop`). A cut tile is a `Cell` with `cropU` / `cropV` (fraction of the texture shown from its left / bottom); `cellMap` (`layoutPlan.ts`) divides the stretch size by it, so UV, relief and the editor preview follow with no other change. Wall rows: candidates in the order [bottom+middle+top], [b+m], [m+t], [m], [b+t], [b], [t]; the first whose scaled rows are within the limit wins; otherwise, with a Middle row, the first candidate whose fixed rows fit gets natural size rows with the top Middle unit cut. `maxStretch = 0` keeps the previous behaviour exactly.
**Saving a simple set.** Core's `write_texgroup` accepts a group with `root` (rule group) or `kit` (simple set). Earlier it required `root`, so saving a set failed with "a group needs a root node" (fixed 2026-10-12; regression test in `core/tests/test_texgroups.cpp`). The set editor still writes `"root": {}` next to `kit` so a Core built before this fix accepts the file too; the reader looks at `kit` first and ignores it.

