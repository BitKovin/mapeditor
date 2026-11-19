# 07 — Porting the editor: inventory

Every Shell file falls in one of four groups. **Behaviour is defined by DESIGN.md / REQUIREMENTS.md and by the Shell's
tests**; the Shell code is the reference implementation. A port is done when the Shell test file for it has been converted
to doctest (same inputs, same expected outputs) and passes in `native_tests`.

Conversion rule for tests: one `TEST_CASE` per Shell `it(...)`, same name, inputs copied literally; numbers compared with
the tolerance the Shell test used. Put them in `native/tests/port_<shellfile>.cpp`.

## A. Already in the Core — call it, do not port

| Shell file | Core equivalent |
|---|---|
| `model/uv.ts`, `uvmesh.ts`, `rng.ts`, `texgroup.ts`, `tileset.ts`, `layout.ts`, `layoutPlan.ts`, `texprops.ts`, `seamless.ts`, `relief.ts`, `reliefApply.ts` | `core/include/core/texplan.h` (`texplan_uvmesh.cpp`, `texplan_layout.cpp`, `texplan_props.cpp`, `texplan_relief.cpp`) |
| `model/derived.ts` (carved fragments, their texturing) | `core/src/derived.cpp`, `csg.cpp`, `texplan_relief.cpp` |
| `model/expand.ts`, `rotation.ts` | `core/src/prefab.cpp` |
| `model/decal.ts` | `core/src/decal.cpp` |
| `model/lights.ts` (reading light keys) | `core/include/core/lights.h` |
| `model/sky.ts` | `core/include/core/sky.h` |
| `model/smooth.ts` | `core/include/core/smoothing.h` |
| `model/geometry.ts` `validateBrush` | `core::Brush::checkStructure` / `checkConvexSolid` |
| `model/entityBox.ts` (class boxes from the .ent) | `core/include/core/entdef.h` |
| texture lookup (`state/textures.ts` path rules) | `core/include/core/textures.h` |

## B. Pure logic — port to C++ (`native/src/tools/` or `native/src/doc/`)

Ordered by the milestone that needs them (08_MILESTONES.md).

| Shell file | Native file | Spec tests | Notes |
|---|---|---|---|
| `render/camera.ts` | `render/camera.*` | — | perspective / ortho, Y up, metres; fly and orbit modes |
| `render/gizmo.ts`, `model/transform.ts`, `tools/modal.ts` | `tools/gizmo.*`, `tools/transform.*`, `tools/modal.*` | `transform.test.ts`, `modal.test.ts`, `originGizmo.test.ts` | G / R / S / E / I, typed numbers, axis locks (DESIGN §17) |
| `render/picking.ts`, `tools/meshPick.ts` | `render/picking.*` | `tools.test.ts` (picking parts) | |
| `tools/boxTool.ts`, `model/shapes.ts` | `tools/box_tool.*`, `tools/shapes.*` | `tools.test.ts`, `batch33.test.ts` | |
| `state/actions.ts` | `doc/actions.*` | `store.test.ts`, `instanceActions.test.ts`, `contextMenuActions.test.ts` | pure document operations used by tools and menus |
| `state/clipboard.ts` | `doc/clipboard.*` | `clipboard.test.ts` | same JSON format (03_DOCUMENT.md §10) |
| `model/placement.ts`, `pointBox.ts`, `entityHandles.ts`, `arrows.ts`, `annotations.ts`, `sizeLabels.ts`, `modelEntity.ts` | `tools/placement.*`, `render/overlays/*` | `pointBox.test.ts`, `arrows.test.ts`, `annotations.test.ts`, `entityBox.test.ts` | |
| `model/refs.ts` | `doc/refs.*` | `refs.test.ts`, `detailsRefs.test.ts` | DESIGN §18.1 |
| `model/meshTopology.ts` | `tools/mesh/topology.*` | `meshOps.test.ts` (topology parts), `bridge.test.ts` | half-edge structure built from the stored mesh (DESIGN §5.1) |
| `model/meshOps.ts`, `meshCut.ts`, `meshSelect.ts` | `tools/mesh/ops.*`, `cut.*`, `select.*` | `meshOps.test.ts`, `meshCut.test.ts`, `meshSelect.test.ts`, `meshUvLock.test.ts` | keep face ids stable (lighting identity) |
| `model/brushOps.ts`, `faceDrag.ts`, `resize2d.ts`, `selectVolume.ts`, `tools/clipperPoints.ts` | `tools/brush_ops.*`, `face_drag.*`, `resize2d.*`, `select_volume.*`, `clipper.*` | `brushOps.test.ts`, `faceDrag.test.ts`, `resize2d.test.ts`, `selectVolume.test.ts`, `clipperPoints.test.ts` | |
| `model/parametric.ts` | `tools/parametric.*` | `parametric.test.ts` | |
| `model/surface.ts`, `flow.ts`, `ftt.ts`, `hotspots.ts`, `texelDensity.ts`, `lightmapView.ts`, `texPreview.ts`, `groupTools.ts` | `tools/texturing/*` | `surface.test.ts`, `surfaceInspector.test.ts`, `flow.test.ts`, `ftt.test.ts`, `fastTextureToolUi.test.ts`, `hotspots.test.ts`, `texelDensity.test.ts`, `lightmapView.test.ts`, `texPreview.test.ts`, `groupTools.test.ts`, `copySurface.test.ts`, `uvContinuity.test.ts` | |
| `model/reliefSession.ts` | `tools/relief_session.*` | `relief.test.ts` (session parts) | relief editor state |
| `model/terrain.ts` | `tools/terrain.*` | `terrain.test.ts`, `terrainStore.test.ts` | DESIGN §8.k |
| `model/visibility.ts` | `doc/hide_rules.*` | `hideRules.test.ts`, `hideMenu.test.ts`, `outlinerEye.test.ts` | view state, not saved |
| `model/levelSettings.ts` | `doc/level_settings.*` | `levelSettings.test.ts` | worldspawn keys |
| `state/prefabize.ts`, `prefabPreview.ts` | `doc/prefabize.*`, `ui/prefab_preview.*` | `prefabize.test.ts`, `prefabPreview.test.ts`, `prefabNames.test.ts` | |
| `model/gltf.ts`, `tga.ts` | `scene/gltf.*` (use `cgltf`, MIT, if the user agrees — ask), textures via Core | `gltf.test.ts` | model previews |

## C. UI — rebuild in Qt (`native/src/ui/`)

Each Qt panel reproduces the panel the Shell has; the Shell file and its test (where one exists) are the spec, DESIGN §7
and REQUIREMENTS give the intent. Use Qt widgets, no QML.

| Shell UI | Qt |
|---|---|
| `app.ts`, `ui/menuBar.ts`, `ui/bindings.ts`, every `keydown` handler | `app/main_window.*`, the action registry and input router of 10_UI_AND_INPUT.md (menus, toolbars and key help generated from it; keys rebindable) |
| Dockview layout, layout presets (REQUIREMENTS §35) | Qt ADS dock manager; saved layouts in QSettings |
| `ui/viewport.ts` (2100 lines: input routing, tools, overlays) | `ui/viewport_widget.*` + `render/` + `tools/` — split by concern, not one file |
| `ui/panels.ts` (outliner, details) | `ui/outliner.*` (QTreeView + model), `ui/details.*` | 
| `ui/properties.ts` | `ui/property_editors.*` (typed editors from the .ent: colour, number with range, checkbox, reset) |
| `ui/contentBrowser.ts`, `textureThumbs.ts`, `texturePanel.ts`, `placePanel.ts` | `ui/content_browser.*` (QListView with lazy thumbnails), `ui/place_panel.*` |
| `ui/texturePropsWindow.ts`, `texGroupEditor.ts`, `texGroupsPanel.ts`, `tileSetEditor.ts`, `groupPreview.ts`, `layoutSection.ts` | `ui/texture_props.*`, `ui/texgroup_editor.*`, `ui/tileset_editor.*` |
| `ui/fastTextureTool.ts` | `ui/fast_texture_tool.*` |
| `ui/reliefEditor.ts`, `render/reliefView.ts` | `ui/relief_editor.*` with its own Vulkan viewport |
| `ui/levelSettingsPanel.ts`, `terrainPanel.ts`, `hideMenu.ts` | `ui/level_settings.*`, `ui/terrain_panel.*`, `ui/hide_menu.*` |
| `ui/settingsDialog.ts` | `ui/settings_dialog.*` (game profile, lighting config, editor prefs: Core's `profile.h`, `settings.h`, lighting.json) |
| `ui/fileDialog.ts`, `nativeDialog.ts` | `QFileDialog` restricted to the assets root |
| `ui/contextMenu.ts`, `dialog.ts` | `QMenu`, `QDialog` |
| status bar, `state/exportStatus.ts` | `app/status_bar.*` |

## D. Not ported (replaced by the architecture)

`net/connection.ts`, `net/protocol.ts` (no socket between halves), `render/lightmapStore.ts`, `lightvolStore.ts`,
`litStale.ts`, `directLight.ts`, `directLightPlan.ts`, `half.ts` (no tile transport, no prediction), the sync parts of
`state/store.ts` (acks, document_sync), `desktop/` (Electron). `render/renderer.ts`, `litTextured.ts`, `modelRenderer.ts`,
`lightmapDensity.ts`, `lightvolPreview.ts`, `orthoGrid.ts`, `icons.ts` are replaced by the Vulkan renderer, but remain the
**spec** for what it draws (05_RENDERER.md §5).

## E. Core features with no Shell file (UI hooks only)

Quake `.map` import (`core/map_import.h`, File > Import), exporters (`core/exporter.h`: BSP, OBJ), game profile and entity
definitions (`profile.h`, `entdef.h`), texture groups files (`texgroups.h`), launching the game (`settings.h` game exe).
