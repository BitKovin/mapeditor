# tools/ -- the editing tools (pure logic, no Qt)

Everything here maps the current document version (and a mouse ray or pixel) to a new document version; the Qt side (`ui/`) owns the interaction, the history and the
drawing. Depends on `native_render` (rays, camera maths, line vertices) and `native_doc`.

| File | What |
|---|---|
| `transform` | `Affine` maps (rotate / scale about a pivot, translate), mapping of brushes (UV lock, mirror flip), entities (origin, angles, light radius, decal size, model scale), prefab instances; `selectionTargets`, `transformSelections`, `moveEntityOrigin`, `gizmoPivot` / `selectionPivot` (port of `model/transform.ts`) |
| `modal` | the formulas of the G / R / S modal operators: typed numbers (`typeInto`, `typedValue`), `grabDelta` (view plane or one axis), `rotateAngle`, `scaleFactor`, `describeModal` status text (port of `tools/modal.ts`) |
| `gizmo` | hit tests of the axis handles and rotation rings, ring angles, drag deltas (port of `render/gizmo.ts`) |
| `gizmo_overlay` | the gizmo and the origin handle as coloured lines |

**As built (T4.1).** `ui/main_window_transform.cpp` runs the operators: a `TransformSession` keeps the document `base` the operation began on; every pointer move
asks `transformSelections(base, ...)` for the new version and commits it into one open history interaction (`History::beginInteraction`), so the scene follows each move
and the whole operation is one undo step ("Move", "Rotate", "Scale", "Move origin"). A result the Core refuses (an invalid brush) keeps the last good version. Cancel
commits `base` back and ends the interaction, which then pushes nothing. Keys: `G` `R` `S` in a viewport; while an operator runs the `modal:transform` context is the
only viewport-side context (LMB / Enter confirm, RMB / Esc cancel, X / Y / Z constrain, digits / `.` / `-` / Backspace type a value). Not done: entity angle handle,
face-mode gizmo, pointer lock, mesh mode (M7), extrude / inset (T4.2).
