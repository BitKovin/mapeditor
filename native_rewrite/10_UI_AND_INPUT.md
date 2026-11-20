# 10 — UI and input: actions, rebindable keys, and other improvements

Status: **proposal** (2026-11-19). §1–§6 (actions and bindings) are requested by the user and part of the plan
(tasks in 08_MILESTONES.md: T1.6, T4.6, T4.7). §7 lists further UI improvements; each is marked **[plan]** (part of the
plan) or **[ask]** (the implementer asks the user before building it).

## 1. Why

In the Shell, keys are handled in ~69 separate `keydown` listeners (`shell/src/ui/viewport.ts`, panels, tools), the list in
`shell/src/ui/bindings.ts` is only documentation, and keys cannot be changed. The same key means different things by mode
(`X` = delete / clipper / dissolve, `F` = frame / fill, `Ctrl+M` = merge brushes / mirror), which works only because each
listener knows its mode. The native editor replaces this with **one action registry and one input router**: every thing
the user can do is an *action*; every key, mouse button or wheel input is a *binding* of an action in a *context*; users
can rebind almost anything.

## 2. Actions (`native/src/app/actions/registry.h`)

*Where it lives (T1.6):* the registry, contexts, keymap layers and the router are the Qt-free static library `native_actions`
(`native/src/app/actions/`, depends on `native_base` only; linked by `native_ui`, so the viewport glue and the tools can use
it). The Qt side is `ui/input_adapter` (Qt events -> `InputEvent`), `ui/app_actions` (the shell's own actions: file, edit,
view / layout, cameras) and `ui/registry_menus` (menus generated from the registry). `MainWindow` owns the registry, keymap and
router and filters every key event of the application; `ViewportWindow` forwards mouse / wheel events through a hook and keeps
only the pointer lock and the flight timer. A parameterised action lists `variants` (label + params) to get one menu entry each.

```cpp
enum class ActionKind {
    Trigger,     // runs once on press (Save, Delete, Extrude starts a modal operator ...)
    Toggle,      // on / off state shown in menus and toolbars (Proportional editing, Show grid, Lit / Unlit)
    Hold,        // active while the input is held (fly camera with RMB, temporary face drag with Alt)
    Axis,        // continuous: mouse move, wheel (orbit, dolly, fly speed, terrain brush radius)
    Modal,       // keys inside a running modal operator: confirm, cancel, constrain X / Y / Z, precision, snap toggle
};
struct ActionDef {
    std::string id;              // stable, dotted: "file.save", "mesh.extrude", "view.frame_selection", "camera.fly.forward"
    std::string label, category, description;   // shown in menus, tooltips, the binding editor, the command palette
    ActionKind kind;
    std::vector<std::string> contexts;          // where it is available (§3); empty = global
    nlohmann::json params;                      // fixed parameters of a parameterised action (§2.1)
    std::function<bool(const ActionEnv&)> enabled;   // greyed out when false
    std::function<void(ActionEnv&, const InputEvent&)> run;
    std::function<bool(const ActionEnv&)> checked;   // Toggle only
    std::string icon;
};
```

Rules:
- **Everything goes through the registry:** menus, toolbars, context menus, the command palette, tool panels (the Shell's
  Mesh Edit tab list), tooltips and the key help are *generated* from it, so they can never disagree with what is wired.
- Panels that need a button call `registry.run("mesh.bevel")`; no panel calls tool code directly.
- Actions are registered by the module that owns them (`tools/mesh/` registers `mesh.*`). Action ids never change once
  shipped (keymaps refer to them); a renamed action keeps its old id as an alias.
- Every action of the Shell's `bindings.ts` (MESH_TOOLS, BRUSH_TOOLS, SHAPES) and every key of DESIGN §17 and REQUIREMENTS
  (texture keys, nudge, selection keys, Tab, camera keys) becomes an action. 07_EDITOR_PORT.md tasks register theirs.

### 2.1 Parameterised actions

An action can have parameters, and a binding may fix them: `grid.set {size: 0.25}`, `selection.rotate90 {axis: "y"}`,
`texture.shift {du: 1, dv: 0}`, `camera.bookmark.recall {slot: 3}`, `place.entity {classname: "light_point"}`,
`texture.apply {path: "dev/grey"}`. The binding editor shows these as "Set grid size (0.25 m)". A user can bind any
parameter set, e.g. a key that places their most used prefab.

## 3. Contexts

A context is a named state that is either active or not. Active contexts form a stack, most specific last:

```
global
└─ window: main | relief_editor | texture_props | ...          (which top-level window has focus)
   └─ area: viewport | outliner | content_browser | details | uv_window | ...   (which panel has focus)
      └─ view: perspective | ortho                              (viewport only)
         └─ mode: object | face | mesh.vertex | mesh.edge | mesh.face | terrain
            └─ tool: select | box | clipper | knife | loopcut | ...
               └─ modal: grab | rotate | scale | extrude | bevel | ...   (a modal operator is running)
               └─ hold: camera.fly                              (RMB held in a perspective view)
```

**Resolution:** for an input event, the router walks the active stack from the most specific context outwards and runs
the first binding that matches the event and whose action is `enabled`. So `X` in `mode:mesh.*` is Dissolve, in
`tool:clipper` it is unused, and in `area:viewport` it is Delete. Text fields always get typed characters first (a focused
`QLineEdit` consumes printable keys; only chords with Ctrl / Alt reach the router).

## 4. Bindings and keymaps

### 4.1 Input chords

| Form | Examples |
|---|---|
| key press with modifiers | `Ctrl+Shift+S`, `Alt+N`, `Numpad .`, `F5`, `Delete` |
| key sequence (≤ 2 steps, 1 s timeout) | `Ctrl+K, Ctrl+S` |
| hold / release | `Hold Space`, `Release Alt` |
| double tap (300 ms) | `Double Shift` |
| mouse button, click or drag, with modifiers | `LMB`, `Shift+LMB`, `Alt+LMB`, `Ctrl+RMB drag`, `MMB drag`, `Mouse4` |
| wheel | `Wheel Up`, `Ctrl+Wheel` |
| axis | `Mouse X / Y` (while a Hold or Modal action is active) |
| 3D mouse (SpaceMouse) axes / buttons **[ask]** | `3D TX`, `3D Button 1` |

**Logical vs physical keys.** A binding stores the key either *logically* (the character, default: mnemonics like `G` for
Grab stay on G on AZERTY) or *physically* (the scan code position, prefix `phys:`, default for WASD-style movement so it
stays in the same place on any layout). **Non-Latin layouts:** when the active layout produces a non-Latin character (e.g.
Cyrillic), logical letter bindings are matched by the key's position on the US layout, so `Ctrl+Z`, `G`, `R`, `S` keep
working without switching the layout (Qt: `QKeyEvent::nativeScanCode` / `nativeVirtualKey` mapped through a US layout
table).

### 4.2 Keymap layers

```
built-in preset (read-only)  ←  user keymap (only the differences)  =  effective keymap
```
- **Presets** shipped as JSON in `native/assets/keymaps/`: `default` (this editor's current bindings: Blender keys in mesh
  mode, Radiant keys in brush mode, UE-style fly camera — DESIGN §17), plus **[ask]** `blender`, `hammer` (Source 2),
  `radiant` (NetRadiant), `unreal`.
- **User keymap** in `%APPDATA%\mapeditor_native\keymaps\<name>.json`, storing only additions and removals against its
  preset, so preset improvements still reach the user:
```json
{ "base": "default", "version": 1,
  "bind":   [ { "action": "mesh.bevel", "context": "mode:mesh.edge", "input": "Ctrl+B" },
              { "action": "grid.set", "params": { "size": 0.25 }, "context": "area:viewport", "input": "Alt+1" } ],
  "unbind": [ { "action": "mesh.merge", "context": "mode:mesh.vertex", "input": "M" } ] }
```
- Several user keymaps can exist; one is active. Import / export a keymap file (share with a team).
- An action can have any number of bindings; an input can be bound to one action per context.

### 4.3 What can be bound

Every registered action, including camera navigation (fly keys, orbit / pan / dolly buttons, speed wheel), selection
clicks (replace / add / toggle / subtract modifiers, box / lasso), modal operator keys (confirm, cancel, axis constraints,
numeric input start, precision, snap toggle, proportional size), viewport layout and view switching, panel commands, and
parameterised actions. **Not bindable:** typing inside text fields, the OS keys (Win, Alt+Tab), and `Esc` as the final
fallback "cancel / close" (it may get *additional* meanings, but always cancels a modal operator or closes a menu first).

## 5. The binding editor (Settings > Keymap)

- A searchable table of all actions: category, action, bindings (one chip per binding with its context), source
  (preset / user), a reset button per action. Filters: *changed*, *unbound*, *conflicts*, by context, by category.
- **Record a binding:** click "+" on an action, press the input (keys, mouse button inside the record box, wheel), choose
  the context (defaults to the action's narrowest context). Mouse drags are recorded by holding the button and moving.
- **Conflicts:** if the input is already bound in the same context or in a context that would shadow it (same stack
  path), the editor shows "Already used by *Dissolve* in *Mesh mode*" with buttons *Replace*, *Keep both (more specific
  wins)*, *Cancel*. A conflicts filter lists every input bound twice in one context.
- **Reverse lookup:** a field "Press a key to see what it does": shows the action for that input in every context.
- Reset all to preset; switch preset; import / export.
- Changes apply at once (no restart) and are undoable inside the dialog.

## 6. Discoverability

- **Command palette** (`F3` and `Ctrl+Shift+P` in `default`): fuzzy search over all enabled actions in the current
  contexts, shows their bindings, runs the chosen one; recently used first; parameterised actions can be typed with
  arguments ("grid 0.25", "texture dev/grey").
- Every menu entry, toolbar button and tooltip shows the action's current binding (from the effective keymap).
- **Status bar hints:** while a tool or modal operator is active, the status bar lists its keys ("LMB confirm · RMB / Esc
  cancel · X Y Z constrain · Shift precise · Ctrl snap"), generated from the modal context's bindings.
- **Key help overlay** (hold `F1` **[plan]**): a translucent sheet over the viewport listing the bindings of the active
  contexts.

## 7. Other UI improvements

| # | Improvement | Status |
|---|---|---|
| U1 | **Adjust last operation** panel (Blender's "redo panel"): after an operator with parameters (bevel, inset, hollow, array, shape), its parameters stay editable; changing one re-runs the command on the state before it and replaces the last undo step. Fits the version-based history (03 §5). | [plan] |
| U2 | **Tool options** panel: shows the active tool's settings (snap, thickness, segments, terrain brush) instead of separate tabs. | [plan] |
| U3 | **Undo history** panel: list of commands, click to jump to any point. | [plan] |
| U4 | **Workspaces**: named tabs (Layout, Modeling, Texturing, Lighting) each with its own dock layout and active viewport modes; a keymap context `workspace:<name>` lets bindings differ per workspace. | [ask] |
| U5 | **Snapping menu** in the viewport header: grid / vertex / edge / face / increment, per tool; snap target highlighted while dragging. | [plan] |
| U6 | **View cube / axis widget** in perspective views: click a face to look along an axis, drag to orbit. | [plan] |
| U7 | **Camera bookmarks**: save / recall 9 views per map (stored in user settings per map path, not in the map). Actions `camera.bookmark.save/recall {slot}` (unbound by default: number keys are mesh select modes). | [plan] |
| U8 | **Multi-object Details editing**: select many entities, edit a key once; mixed values shown as "—". | [plan] |
| U9 | **Selection tools**: select similar (same texture, same class, same prefab), lasso select, named selection sets (view state). | [plan] |
| U10 | **Visgroups / layers** stored in the map as editor-only keys (the file keeps unknown keys, so the Shell would preserve them; the format change needs the user's approval). | [ask] |
| U11 | **Texture browser**: favourites, recent, tags, filter as you type, eyedropper action (pick a face's texture), drag a texture onto a face. | [plan] |
| U12 | **Lighting panel**: list of all lights with solo / mute (editor-only, affects only the live bake), brightness and colour sliders that bake live while dragged, a "false colour" luminance view, the stale-chunks overlay (06 §14). | [plan] |
| U13 | **Measure tool**: click two points, shows distance and per-axis deltas. | [plan] |
| U14 | **Notifications** instead of modal dialogs for non-blocking messages (export done, validation rejected an edit), with a history in the status bar. | [plan] |
| U15 | **Theme and scale**: dark / light theme, UI scale independent of Windows DPI, viewport font size. | [plan] |
| U16 | **Pie menus** (radial quick menus bound to a hold key, user-editable contents) and a **Quick favourites** menu built from any actions. | [ask] |
| U17 | **Macros**: record a sequence of actions and bind it. | [ask] |

## As built: input that cannot get stuck (after T4.3)

A hold (fly key, look / pan / select drag) must never outlive the key or button that started it. Layers, from the event path outwards:
- **Router**: a hold's dependent holds end with it (the fly keys end with the look button); a key *release* is never filtered by the text-field rule; `Router::release(trigger)`
  ends a hold whose release never arrived; a `MouseMove` that carries the system's button state (`InputEvent::buttons` / `buttonsKnown`) ends a mouse hold whose button is not down.
- **ViewportWindow**: the pointer lock is dropped when a move reports its button up (`setVerifyButtons`), `resetInput()` clears lock, cursor, fly keys and boost.
- **MainWindow**: key releases still reach the router while a menu or dialog is open (a key *press* for the popup cancels the holds); `cancelInput()` (application deactivated, viewports
  rebuilt) also resets every viewport; a click in a viewport takes the keyboard from a text field; while something is held a 100 ms timer (`Services::watchInput`, on in the app) asks the
  system (`ui/input_probe`: GetAsyncKeyState) whether each held key / button is still down and releases the ones that are not (`reconcileInput(probe)`, testable with a fake probe).
Tests that inject events without real buttons set `Services::verifyButtons = false` (gui_support does), since the real moves that follow report no button.

## As built: the binding editor (T4.6)

Settings > Keymap (`ui/keymap_page`) is a thin widget over `app/actions/keymap_edit` (Qt-free, tested alone). The router, the menus and the editor share one `Keymap`, so an edit applies at the next input event; `MainWindow::keymapEdited()` then writes the active user keymap (`<keymap folder>/<name>.json`, only the differences from the preset) and refreshes the menu key texts. The keymap folder is the folder of `Services::userKeymapPath`; the active name is the QSettings key `keymap/active` (default: that file's name). Contexts are ordered global < window < area < view < mode < tool < modal / hold; a new binding clashes with the same input in the same context (`Same`), in a more specific overlapping context (the new one wins there) or a less specific one (it would never run). Recording: `ChordCapture` turns the next key, click, drag or wheel into the canonical chord text; the recorder picks the context (default: the action's narrowest) and the parameters. Replace unbinds what the input clashed with; Keep both is offered only where both can stay. Undo and redo cover the edits of the open page; switching or importing a keymap starts a new history. Not done: the other presets (`blender`, `hammer`, `radiant`, `unreal`: the user chose later), rebinding the typed-value keys of modal operators, recording key sequences (they are typed).


## As built: discoverability (T4.7)

`app/actions/palette` (Qt-free, tested alone) holds everything the three discoverability features decide; the widgets only show it.

- **Command palette** (`ui/command_palette`, F3 / Ctrl+Shift+P, also Help > Command palette...): lists every enabled trigger and toggle action, one entry per menu variant ("Camera mode: Top", "Tool: Box"), each with its keys and category; hold, axis and modal actions need an input stream and are not offered, nor is the palette itself, nor anything disabled now (Undo with nothing to undo). Words match in any order against label, category and id; consecutive letters and word starts score more. An empty query lists the recently used entries first (settings key `palette/recent`, 12 kept), then by category. A typed argument runs a parameterised action: `ActionDef::paletteKeyword` / `paletteParam` / `paletteNumeric` (set on `grid.set`: "grid 0.25"). Up / Down / PageUp / PageDown move in the list while the search box keeps the keys, Enter chooses, Esc closes; the dialog closes first and the action runs after it (`MainWindow::runAction`), so a dialog the action opens is not nested. The palette does not filter by the active contexts (an action that is enabled can run from anywhere).
- **Hints** (status bar, permanent field `status:hint` left of the tool field, so a running transform's message does not hide it): `hintFor(context)` lists the bindings of the mode that owns the keys, grouped by action: "LMB / Enter confirm  ·  RMB / Escape cancel  ·  X / Y / Z constrain" in `modal:transform`, "T turn shape  ·  LMB drag / Shift+LMB drag draw  ·  Escape cancel" in `tool:box`, the select drags in `tool:select`. The verb is `ActionDef::hint` (the label when empty); at most three inputs per action, "Numpad Enter" is dropped next to "Enter". `MainWindow::modeContext()` names the context; the field refreshes on `toolChanged`, `keymapChanged` and the new `transformChanged` signal, so a rebinding shows at once.
- **Key help** (`ui/key_help_overlay`): hold F1 (`help.keys`, a Hold action) or Help > Key help (`help.keys_toggle`). A translucent top-level window over the active viewport (the viewports are native windows, so a child widget cannot draw over them) that takes no input; its columns list `keyHelpSections(active contexts)`: the most specific context first, "Everywhere" (global) last, each action once with its inputs joined ("Delete / Backspace"). It is made again when the keymap changes while it is shown.
- **Tooltips**: the Place panel's shape buttons and the orientation box name their key from the keymap ("Cylinder (C): ...") and follow a rebinding; menu entries have shown their key since T1.6.
