# tools/ -- the editing tools (pure logic, no Qt)

Everything here maps the current document version (and a mouse ray or pixel) to a new document version; the Qt side (`ui/`) owns the interaction, the history and the
drawing. Depends on `native_render` (rays, camera maths, line vertices) and `native_doc`.

| File | What |
|---|---|
| `transform` | `Affine` maps (rotate / scale about a pivot, translate), mapping of brushes (UV lock, mirror flip), entities (origin, angles, light radius, decal size, model scale), prefab instances; `selectionTargets`, `transformSelections`, `moveEntityOrigin`, `gizmoPivot` / `selectionPivot` (port of `model/transform.ts`) |
| `modal` | the formulas of the G / R / S modal operators: typed numbers (`typeInto`, `typedValue`), `grabDelta` (view plane or one axis), `rotateAngle`, `scaleFactor`, `describeModal` status text (port of `tools/modal.ts`) |
| `gizmo` | hit tests of the axis handles and rotation rings, ring angles, drag deltas (port of `render/gizmo.ts`) |
| `gizmo_overlay` | the gizmo and the origin handle as coloured lines |
| `box_tool` | the maths of the box-draw tool: planes, grid snapping along a surface, rectangle and height (Shift: square / cube), `buildBrush`, preview lines, `newId` (port of `tools/boxTool.ts`) |
| `edit_ops` | delete / duplicate / group / ungroup of a selection and the clipboard (build, serialize, parse, `pasteInto`) as pure `DocumentVersion` -> `EditResult` functions (ports of the parts of `state/actions.ts` and `state/clipboard.ts`) |
| `visibility` | hide keys (`e:` / `b:` / `i:`), hide-by-class rules, `hiddenObjects` for the renderer and picker (port of `model/visibility.ts`) |
| `shapes` | box, cylinder, wedge, arch, stairs and sphere filling the drawn box, quarter turns, smooth edges, edge preview (port of `model/shapes.ts`) |

**As built (T4.1).** `ui/main_window_transform.cpp` runs the operators: a `TransformSession` keeps the document `base` the operation began on; every pointer move
asks `transformSelections(base, ...)` for the new version and commits it into one open history interaction (`History::beginInteraction`), so the scene follows each move
and the whole operation is one undo step ("Move", "Rotate", "Scale", "Move origin"). A result the Core refuses (an invalid brush) keeps the last good version. Cancel
commits `base` back and ends the interaction, which then pushes nothing. Keys: `G` `R` `S` in a viewport; while an operator runs the `modal:transform` context is the
only viewport-side context (LMB / Enter confirm, RMB / Esc cancel, X / Y / Z constrain, digits / `.` / `-` / Backspace type a value). Not done: entity angle handle,
face-mode gizmo, pointer lock, mesh mode (M7), extrude / inset (T4.2).

**As built (T4.2).** `ui/main_window_box.cpp` keeps the tool state (`Tool::Select` / `Tool::Box`, the shape, its parameters, the active texture) and the drawing in progress
(`ui/box_drawing.h`). The left drag is the Hold action `box.draw` (context `tool:box`); phase 2 (the height) takes plain pointer moves and the second click straight from the
input hook, like the transform operators. The preview is drawn as gizmo lines (no depth test). The finished shape is added to the worldspawn (created when the
document has none) in one `commitEdit`, so a stairs shape of 8 brushes is one undo step; the new brushes are selected. Not done: the shape's parameter UI (T4.4),
the `shape` record for parametric regeneration, projection defaults of new faces (T5).

**As built (T4.3).** `ui/main_window_edit.cpp` applies an `EditResult` with one `commitEdit` and selects the result. Copy writes the Shell's `level-editor/clipboard` JSON to the OS clipboard
(`QClipboard`), paste parses it (empty or foreign text is refused with a status message). Duplicate and Paste then start a grab in the viewport under the pointer (the active one from
a menu). Hiding is a view setting: `hide_` (keys + rules) -> `refreshHidden()` computes `hiddenObjects(snapshot)` on every snapshot and hide change, hands it to the render thread
(`RenderThread::setHiddenObjects`, skipped in `rebuildDraws`) and to the picker / overlay, and prunes the selection (no undo step). Markers in the set: `{"", entityId, ""}` an entity
hidden as a whole (point entities), `{instanceId, "", ""}` a hidden prefab instance. Not done: the volume-class list and custom-rule entry (Outliner, T4.4).
