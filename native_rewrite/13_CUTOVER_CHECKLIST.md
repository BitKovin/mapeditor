# 13 — Cut-over checklist (M13)

M13 asks: every ✅ item of `REQUIREMENTS.md` is either **working in native (with where)** or listed as **not ported** with the user's agreement.
This file is that list, grouped by the sections of REQUIREMENTS.md (255 ✅ lines; sub-items of one feature share a row). Status of this audit:
2026-11-22, by reading each section against the native code, `08_MILESTONES.md` rows and the test names. "Done" means the feature has code and tests in
`native/`; the user still has to look at it in the real editor (the cut-over decision is theirs, 08 M13).

Legend: **done** = working in native · **replaced** = the Shell feature exists for another reason or in another form (the Core does it, or the architecture
makes it unnecessary) · **gap** = not in native yet, needs a task or the user's decision · **not ported** = the user decided to leave it.

## Gaps that decide the cut-over (everything else is done or replaced)

| # | Gap | REQUIREMENTS | Size | Note |
|---|---|---|---|---|
| G1 | Drag from the Place tab into a viewport (entities, prefabs, shapes), with the half-transparent ghost | §3 #147, §5 (d), §12 | small (drop) + renderer (ghost) | Click-to-spawn works. The drop itself is UI only; the ghost needs an overlay pass, ask |
| G2 | Entity angle handle (the tip of the direction arrow drags the entity's `angles`) | §33 H2 | small | The arrow is drawn (`scene/arrows`); the drag is not wired |
| G3 | Terrain tool (sculpt, paint, panel) | §39 | large | **Not ported, user decision**: the user skipped the terrain tool ("terrain will be replaced"); the data model (`tools/terrain`, T11.8a) is kept, the WIP of the tool is in `git stash` |
| G4 | Packaging: `release/native/` with the Qt DLLs (`windeployqt`) | M13 | small | `build_native.bat` builds; nothing copies the Qt runtime yet |
| G5 | View cube / axis widget | 10 U6 (not in REQUIREMENTS) | medium | needs an overlay in the camera frame; ask |
| G6 | Named selection sets, lasso select | 10 U9 (not in REQUIREMENTS) | medium | extras of the native plan, not Shell parity |
| G7 | Electron-only items (§29 P1..P7: desktop app, one build script, single instance, Core restart) | §29 | — | **Replaced**: one process, no Core socket; single instance is not implemented (the editor can be started twice) |
| G9 (done, T13.1) | Decal tool: click a surface to lay a projector (Place > Decals tab: width, height, depth; texture = the active one) | §8 line 115 | small | UI only, port of `decalDown` in `viewport.ts` |
| G8 | Classic IBSP export | §10 (⬜), §26 F7 (💤) | — | Not planned in the Shell either |

Items the Shell lists as 🟡 / ⬜ (surface inspector Patch Natural, hold-and-drag path select, lightmap UV editing, Scythe selection modes, E10 known gaps of the
export) are not parity targets: native carries the same open points.

## Per section

### 1 Platform and architecture (§1, §1.1)
| Item | Status | Where in native |
|---|---|---|
| Shell does editing, Core does files, validation, derived geometry, lighting | **replaced** | one process: the editor links the Core as `core_lib` (01_ARCHITECTURE.md); same files and results (T2.2: lvl2 byte-identical load + save) |
| Assets root persisted, game profile, `.ent` definitions, live reload of prefabs / profile / `.ent` | done | `MainWindow::setAssetsRoot / reloadProfile`, Settings > Game, `entityDefinitionsChanged`; T4.5, T11.3 |
| Compile = flatten (`flattenForCompile`) | done | export calls the Core (`exporting/export_job`) |
| Process rules (zips, document first) | n/a | workflow, not a feature |

### 2 Camera and viewport (§2)
| Item | Status | Where |
|---|---|---|
| Fly default, orbit optional, RMB look, WASD/QE, Shift x3 | done | `render/camera_rig`, actions `camera.*` (T1.4, T1.6) |
| Fly speed control (slider + text), wheel x1.2 with RMB | done | `camera.fly.speed`, status bar speed field |
| Pointer lock while looking | done | Qt cursor capture in `ui/viewport_widget`, G/R/S modal keeps a virtual cursor (`tools/modal`) |
| View options (face outlines, subtractive shapes, grid, origins, lightvol samples, density) | done | `view.*` actions, per-viewport `ViewportSettings`; density and lightvol views T3.x / T8 |
| Grid-snapped start of drawn shapes | done | `tools/box_tool` (T4.2) |
| Up to 10 viewports, per viewport options, ortho views with adaptive grid, 2D resize handles | done | `view.layout`, `tools/resize2d` (T4.x, T11.3) |
| Any panel in its own window | done | Qt ADS floating docks (T1.3) |
| Smooth keyboard flight | done | per-frame integration in `CameraRig::advance` |
| Backface culling default on, wireframe render mode | done | `RenderMode::Wireframe`, view.render_mode |
| Layout default, kept, presets | done | `layout/preset`, `view.layout`, ADS state in QSettings; `--layout` |

### 3 Selection, editing, context menus (§3, §12 selection items)
| Item | Status | Where |
|---|---|---|
| Click / Shift face / Ctrl multi / box select / double-click entity | done | `ui/main_window_*`, `tools/select_ops` (T11.10f) |
| Duplicate, delete, G/R/S with axis constraint and numeric input, group move | done | `tools/transform`, `tools/modal`, `transform.*` actions (T4.3, T11.4) |
| Rotate / scale gizmo handles, snaps for rotation and scale | done | `tools/gizmo`, `snap.*` actions (T11.11i) |
| Right-click menus (Make entity, Move to world, Select inside/touching, Group, Hide, Properties, Copy / Paste UV) | done | `tools/context_menu` (T11.10c) |
| Prefab instances selectable, outliner | done | `ui/outliner_panel`, picking of expanded instances |
| Tab toggles brush / mesh mode, Blender keys in mesh mode | done | `mesh.toggle`, `mesh.*` actions (T4.x, T11.x) |
| Only-brush-of-entity selects the entity, double-click selects all brushes | done | `tools/select_ops` |
| Select all / invert / none, select similar | done | `edit.select_*` (T11.10f, T11.11d) |
| Entity spawn by dragging with ghost | **gap G1** | click spawn works (Place panel) |
| Entities drawn and picked as their `.ent` boxes, translucent with class name, direction arrow, reference arrows | done | `scene/point_box`, `scene/arrows`, `render/entity_overlay` |
| Entity origin handle of brush entities | done | `MainWindow::originHandle`, `tools/transform` (moves only the origin) |
| Entity angle handle | **gap G2** | |
| Size labels of the selection | done | `scene/size_labels`, drawn by `mesh_pass` |
| Hide classes / rules / unhide all | done | `ui/hide_dialog`, `tools/visibility` (T11.10e) |

### 4 Brush tools (§4, §12 M3.6-M4.4)
| Item | Status | Where |
|---|---|---|
| Box tool, subtractive brushes, carve scope | done | `tools/box_tool`, `tools/brush_edit` (T4.2) |
| Cylinder, wedge, arch, stairs, sphere; parametric shapes with Details parameters | done | `tools/shapes`, `tools/parametric` (T11.6a/b) |
| Face drag, Alt-drag face resize (with and without selecting first) | done | `tools/face_drag`, `ui/main_window_facedrag.cpp` |
| Clipper (X) with keep back / front / split | done | `brush.clip_*` |
| Hollow, merge, snap to grid, flip, rotate 90, nudge | done | `brush.*`, `edit.nudge` |
| Texture keys (shift / scale / rotate, Alt finer) | done | `texture.shift / scale / rotate` actions |
| Select inside / touching | done | `brush.select_inside / select_touching` |
| Concave brush becomes func_detail when needed | done | `tools/edit_ops` messages ("use func_detail"), T11.10 |
| Make brush entity, brush to worldspawn, origin at bounds centre | done | `edit.make_entity`, `tools/edit_ops` |

### 5 Prefabs (§5)
| Item | Status | Where |
|---|---|---|
| Instances: unique names, rename updates references, overrides with revert, sub-tree in Details | done | `tools/entity_edit`, `tools/details` (T4.4), `core` resolver |
| References picked, not typed (list + pipette), nested scope-relative references | done | `tools/ref_targets`, Details reference editor |
| Flatten at compile | done | Core |
| Spawn from the Place tab (click) | done | `ui/place_panel` |
| Spawn by drag with ghost | **gap G1** | |
| Turn selection into a prefab | done | `tools/prefabize` (T11.7a) |
| Prefab previews | done | `tools/prefab_preview` (T11.7b) |

### 6 Surfaces, UVs, textures (§6, §19, §20)
| Item | Status | Where |
|---|---|---|
| New UV model (shear, ortho / cam projections, repeat / mirror / wrap / clamp), flow, justify, copy / paste UV, lift material, UV lock | done | `tools/texturing/*`, `texture.*` actions, `ui/surface_panel` (T11.2, T11.3) |
| Surface inspector | done | `ui/surface_panel` (Patch Natural stays open as in the Shell) |
| Texture size from resolution, texture properties window, hotspots | done | `ui/texture_props_window`, `texture.hotspot` (T11.3) |
| Repetition behaviour per axis, anchor, alternate texture, UV origin bottom-left, no line artefact | done | `tools/texturing/plan*` (port of the Core's `texplan`), renderer samples the plan |
| Texture groups (`.texgroup.json`: layout, conditions, seeded variation, relief) and the Simple tile sets / frames | done | `ui/texgroup_editor`, `ui/tileset_editor` (T11.4) |
| Relief editor | done | `ui/relief_editor`, `tools/relief` (T11.5a/b) |
| Fast texture tool | done | `ui/ftt_window`, `texture.ftt` (T11.10) |
| Texture browser with folders, favourites, recent, search, skyboxes badge | done | `tools/texturing/browser`, `ui/texture_browser_panel` (T11.11j) |
| Transparency modes (auto / opaque / cutout / translucent) incl. coloured glass, re-bake on change | done | texture sidecar `<tex>.json` read by the GPU texture table (T11.11a), same CPU / GPU result |

### 7 Modeling (§7, §21, §36 W2, §33 H6)
| Item | Status | Where |
|---|---|---|
| Mesh mode: select, move, extrude, inset, bevel (segments with wheel), loop cut, merge, dissolve, delete + fill, flip | done | `tools/mesh/*`, `mesh.*` actions (T4.x) |
| Path / loop / ring select, connect, subdivide, mirror, proportional edit, knife, Shift-drag extrude | done | `mesh.pick`, `mesh.knife*`, `mesh.mirror`, `mesh.proportional`, `mesh.subdivide` |
| Bridge edge loops, mark edge smooth / sharp | done | `tools/mesh` (bridge, smooth) |

### 8 Derived geometry (§8) and decals
| Item | Status | Where |
|---|---|---|
| Carving (also inside prefabs and by prefab content), fragments name their authored face, picking on carved surfaces | done | Core `derived`; `scene` builds from it; `render/picking` |
| Hide subtractive shapes | done | view option |
| Decals (projector entity, clipped onto carved surfaces, own lightmap mapping, rendering with alpha) | done | Core + `scene/render_mesh` (T3.x) |
| Decal tool (click a surface, Place > Decals: width / height / depth) | done (T13.1) | `decal.place` in `tool:decal`, `MainWindow::placeDecalAt`, Place > Decals tab |

### 9 Lighting (§9, §18, §23, §40)
| Item | Status | Where |
|---|---|---|
| Lightmap UVs, charts follow smooth edges, padding 2 | done | Core, used unchanged |
| Light entities (point, spot, directional, rect, sky, ambient), styles, negative lights | done | Core `lights.h`; both live paths (`LightingService`, `RtLightsProducer`) |
| Live lighting in the viewport (Lit, Lighting only), density view, lightvol samples | done | M5 / M6 / R3 / R6 of 08; `view.render_mode`, view options |
| Bounces, light volumes, soft shadows | done | GPU bake (`gpu::LiveSession`), RT view with bounces (R6: 0.97-0.99 of the Core's bounce share) |
| Level settings panel | done | `ui/level_settings_panel`, `doc/level_settings` (T11.x) |
| Retexturing rebakes, transparency in bake | done | `LightingService::textureChanged` |
| Classnames that receive / block light in a JSON config | done | game profile via Core |
| Lighting page of Settings, Lighting dock (mute / solo) | done | `tools/lighting_config` (T11.10d), `ui/lighting_panel` (T11.11m) |
| Bicubic lightmap sampling in the viewport | **replaced** | the native Lit view is path traced / baked live, it does not display stored tiles (no `lightmapStore`) |

### 10 Export (§10, §25, §26, §27, §28, §31, §34)
| Item | Status | Where |
|---|---|---|
| rBSP v2 export: lightmaps (EXR), lightvols, visibility, collision brushes, entities lump, brush contents from game config, 8 styles, decals, relief, repeat modes | done | Core, called by `exporting/export_job` (File > Export, cancel, progress) |
| Auto sky box / box per area, sky faces, skyboxes in the browser, level sky picker | done | `doc/level_settings`, `tools/texturing/browser`; the export is Core |
| Exporter interface and OBJ exporter, format from the game profile | done | `exporting::exporterOf`, Core `exporter.h` |
| Game folder settings file | done | Core `settings.h`, Settings dialog (T4.5) |
| Play map (launch the game) | done | `file.play` |
| Import Quake `.map` incl. patches as slabs | done | `file.import_quake_map` (T9.3) |
| Classic IBSP | **not planned** | G8 |

### 11 Platform items of §29 (Electron app)
Replaced by the native process; packaging is G4, single instance G7.

### 12 Viewer extras the user added in later batches
| Item | Status | Where |
|---|---|---|
| Models (glb / obj, animated) drawn for `misc_model` / `model_path` | done | `scene/model*`, `render/model_scene` (T11.9) |
| Wireframe, density, lightvol views | done | see section 2 |
| MCP for AI level prototyping | n/a | design only (`MCP_DESIGN.md`); native has an automation socket (`automation/server`, T9.4) as a counterpart |
| Terrain | **not ported (user decision), G3** | |

## What remains for M13
1. G4 packaging script (`package_native.bat`).
2. G1 / G2: the two small gaps (drop spawn, angle handle) — unless the user prefers to list them as not ported.
3. The user opens the editor on their maps (lvl2, demo_lit) and signs off the list above; then the Shell is retired (their decision).
