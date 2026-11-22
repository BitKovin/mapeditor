# 14 — UI redesign: a calm, mouse-first editor

Status: **plan, waiting for the user's review.** Nothing here is built yet. Open questions for the user are in §11. When they are
answered, the tasks in §10 get rows in `08_MILESTONES.md` (milestone M14) and are worked through in order like every other task.

The user's request (2026-11-22): *"Right now interface looks very bad and unintuitive. Take inspiration from previous editor look
and NetRadiant-custom. Editor should be very easy to navigate, not visually overloaded and not require many clicks for simple
action."*

Three sources went into this plan:
- §2: the native editor as it is today (a screenshot on `demo_lit.map.json` at the default 1280×800 window, plus the code);
- §3: the previous editor, the Shell (`shell/src/app.ts`, `shell/src/style.css`, `shell/src/ui/viewport.ts`);
- §4: NetRadiant-custom (Garux): the repository README, `docs/mouse shortcuts.txt`, `docs/changelog-custom.txt`,
  `radiant/mainframe.cpp` and the features page at garux.github.io/NRC.

This document changes *where things are and how they are reached*. It adds no editing features, so DESIGN.md and
REQUIREMENTS.md stay authoritative for behaviour. Every action stays in the registry (10_UI_AND_INPUT.md §2): the toolbar,
headers, menus, palette and keys are all views of the same `ActionDef`s, so nothing gains a second code path.

---

## 1. Goals and measurable targets

| # | Goal | How it is checked |
|---|---|---|
| G1 | **Not overloaded.** At the default size, apart from the viewports, three panels are visible: Browser, Outliner, Properties. | Count in the default layout (GUI test) |
| G2 | **No truncated labels.** No tab or button text is cut off at 1280×800 (today: "Pl…", "St", "De", "Surf", "Le", "His", "Messa", "Ad", "Ligh"). | GUI test walks every visible `QTabBar` / button and checks that the elided text equals the full text |
| G3 | **State is visible without opening a menu.** Tool, edit mode, gizmo, grid size, snap, UV lock, camera and render mode of each viewport. | Screenshot review; GUI test reads the toolbar and header state |
| G4 | **A simple action takes at most 2 clicks or 1 key.** The task table in §7 is the contract. | §7 table; each row has a GUI test |
| G5 | **No modal box for a message that needs no decision.** Load errors, export done, validation refusals become notifications (U14). | grep for `QMessageBox::` outside real confirmations; GUI test opens a broken map |
| G6 | **The mouse does the common work in the viewport** (NetRadiant-custom style): pick or paste a texture, create an entity at the pointer, cycle through overlapping objects, maximise a view. | §8 bindings, each with a GUI test |
| G7 | **Everything stays rebindable and searchable.** New buttons and gestures are registry actions with keymap entries; the command palette finds them. | `test_keymap_edit`, `test_input_adapter` contexts |
| G8 | **An old saved layout does not bring the old clutter back.** The layout state version is bumped, so the new default appears once. | Unit test on `kStateVersion` handling |

---

## 2. The native editor today: findings

From the screenshot (default layout, 1280×800, `core/assets` with `maps/demo_lit.map.json`) and the code
(`ui/main_window.cpp` `buildViewports`, `ui/main_window_panels.cpp` `createToolDocks` / `layoutToolDocks`):

**Too many panels, too little room.**
- 13 tool docks: Stats, Place, Textures, Groups, Outliner, Details, Surface, Level, History, Messages, Adjust, Lighting,
  Tool options. They sit in three tab areas. The lower right area holds **nine tabs** in about 490 px, so every label is
  cut to 2–5 letters ("St", "De", "Surf", "Le", "His", "Messa", "Ad", "Ligh", "Tool"). The left area shows "Pl…", "Text…", "Gro…".
- Stats, a developer readout (GPU rows, DLSS state, Reflex latency), holds a default slot next to Details.
- Details, Surface, Adjust, Tool options and Level all describe "the thing I am working on" but are five separate tabs, so
  the user has to know which one to open. With nothing selected, Details fills its space with "Nothing selected."
- The Place dock takes the whole left column (≈215 px × full height) for six large shape buttons, with a tab strip that needs
  scroll arrows ("<", ">") to reach Entities and Decals.

**State is hidden.**
- There is **no toolbar**. Tool (Select / Box / Decal), edit mode (Object / Mesh), gizmo (move / rotate / scale), snap and
  grid size can only be seen in the status bar text or in menus.
- Camera mode, render mode, grid and DLSS for a viewport live under `View > … (active viewport)` submenus: three clicks, and
  the user first has to make the right viewport active. A viewport shows only its dock tab title ("Perspective").
- Visibility filters are in a dialog (`View > Hide`).

**Chrome without purpose.**
- Every dock area has a title bar with a tab-list menu, a float button and a close button (ADS defaults), even an area with a
  single panel. The viewport area repeats them.
- Splitters are 4 px light-grey bars, the brightest lines on screen, so the eye goes to the frame instead of the content.
- No icons anywhere. Every control is a text button, so groups do not read as groups.

**Interaction gaps.**
- The viewport context menu (`tools/context_menu.cpp` `objectContextMenu`) is one flat list of 25 entries. It is the same
  whatever is selected, and it cannot create anything at the pointer (no "Add entity here").
- To texture a face: select the face, switch the left area to the Textures tab (it shares the area with Place), find the
  texture, click. There is no eyedropper or paste gesture in the viewport except `Shift+RMB` lift.
- To create an entity: Place > scroll the tabs > Entities > find the class > drag it into a viewport.
- A failed map open shows a modal `QMessageBox` (seen 2026-11-22 with a non-map JSON). The game folder (`assets`) is shown
  in the status bar, but nothing on screen says how to change it.
- Entity name labels are drawn for every entity at every distance, so a small room is already cluttered ("light_point",
  "light_sky" overlapping in the screenshot).

**What is already good and stays:** the registry and rebindable keymap; the command palette (F3 / Ctrl+Shift+P); the status
bar hints generated from the keymap; hold-F1 key help; the Blender-style mesh keys; G / R / S modal transforms; camera
bookmarks and selection sets; the Outliner with eye toggles and search; dockable, floatable panels for those who want them.

---

## 3. The previous editor (Shell): what to bring back

The Shell (`shell/src/app.ts`, `style.css`, `ui/viewport.ts`) looked calmer for four concrete reasons:

1. **One slim toolbar under the menu bar**, in labelled groups (`app.ts` line 481):
   `Save Undo Redo | ▶ Play | TOOL Select Shape Decal Terrain | MODE Brush Mesh | UV lock | Delete | + Viewport`, plus an
   export progress button on the right. The active tool and mode were highlighted (`button.on`: accent background and border).
   The rule in the comment: *"only the frequent actions, grouped; everything is also in the menus"*.
2. **A header row inside each viewport** (`viewport.ts` line 2091): two translucent pill groups over the image. On the left:
   view name, camera, fly speed slider, render mode, lighting progress, `View ▾`, `Hide ▾`, Frame, pop-out. On the right:
   move / rotate / scale gizmo icons, `⊞ Snap`, grid size, rotation snap, scale snap. Everything about a viewport was on the
   viewport, one click away.
3. **A design-token theme** (`style.css` `:root`): five surface steps `--bg0 #141416 … --bg4 #3a3d44`, a near-black line
   colour `--line #0c0c0e` (splitters dark, not bright), one blue accent `#3b8eea` / `#1d4f86`, 12 px Segoe UI, 24 px
   controls, 28 px tabs, small uppercase section headers with a hairline (`.section`). The active tab had a 2 px accent top
   line, inactive groups a grey one.
4. **A fixed, explainable default layout**: Place / Mesh Edit / Terrain over Texture on the left, the viewport over Content
   Browser / Texture Groups / History, Outliner over Details / Level on the right. Sizes were relative (Place 16 % of the
   width, clamped to 240–360 px; Outliner 21 %, clamped to 260–440 px).

What the Shell did **not** solve, and this plan must: it also had many tabbed panels (ten); Details and the Texture panel were
separate; there was no viewport context "Add" menu; and the bottom Content Browser took a third of the height permanently.

---

## 4. NetRadiant-custom: what to learn

NetRadiant-custom (GtkRadiant 1.4 → 1.5 → NetRadiant → NRC, now on Qt) is built for speed by people who map all day. Its README
says development *"is focused on smoothing and tweaking editing process"*. What matters for us:

**Very few panels; the viewports own the screen.**
- Window layouts: Regular / Regular Left (camera + texture browser in one column, 2D view + small console in the other),
  Floating, Split / 4-way. The View menu opens the rest on demand: Entity Inspector, Surface Inspector, Layers, Entity List,
  Model Browser. They are not docked by default.
- The changelog made the console smaller and the texture browser bigger in the regular layouts, and removed panel buttons
  that only duplicated the View menu.
- F12 maximises the view under the cursor; Tab focuses the camera on the selection; Ctrl+Tab centres the 2D views on it.

**One compact icon toolbar, grouped by purpose** (`mainframe.cpp` `create_main_toolbar`):
`Open Save | Undo Redo | Mirror ×2, Rotate ×2 | Select touching, Select inside | CSG subtract, Wrap merge, Room, CSG tool |
Vertices, Edges, Faces (component modes) | Next view | Camera | Drag, Clipper, Translate, Rotate, Scale, Transform, UV
(manipulators) | Patch | Texture lock, Vertex texture lock | Entity inspector, Console, Textures | Refresh`.
The toolbar can be switched off, buttons have a context menu to configure them, and a separate **filter toolbar** toggles
visibility classes (caulk, clip, hint, triggers, liquids, areaportals, detail, structural), with extra options on right-click.

**The mouse does the work** (`docs/mouse shortcuts.txt`; m1 = left, m2 = right, m3 = middle button):
- `m1` repeated on the same spot = **tunnel selector** (cycles through what is under the pointer); `Shift+m1` add to the
  selection; `Shift+m1 drag` = **paint selection** (sweep over objects to add them).
- `m1 drag` on empty space creates a brush, outside a selected brush resizes it, on it moves it; `+Shift` gives a square brush,
  `+Ctrl` a cube; `m1 + Alt` in 3D adjusts the height of the brush being created.
- `Ctrl+m1` in 3D selects a face, `Ctrl+m1 drag` paints a face selection; `Ctrl+Alt+m1` extrudes faces.
- `m3` copies the pointed face's texture, alignment, colour and light power, and focuses it in the texture browser;
  `Alt+m3` pastes the texture name, `Shift+m3` texture + alignment, `Ctrl+m3` pastes **seamlessly** across faces and patches,
  `Ctrl+Shift+m3` projects from the copied face. All paste modes also work as a drag over many faces.
- `m2` in a 2D view, or a double `m2` in 3D, opens the **entity creation menu** at the pointer. In that menu `m1` creates and
  `m2` converts the selection; Ctrl keeps the menu open for several creations.
- `m2` in 3D toggles mouselook, `m2 drag` strafes, `Alt+m2` orbits around the clicked point; zoom goes to the mouse pointer.
- Light power is adjusted by dragging, and the transform origin can be dragged.

**Texture browser.** `m1` selects a texture *and applies it to the selection*; `m3` selects without applying; double-click
opens the folder of the current texture; search covers folders, tags and the textures shown; transparency is shown; thumbnail
size has min / max preferences that follow the UI scale.

**Small things that remove clicks.** The Entity Inspector has select-by-key / value / key+value buttons and a colour picker;
Tab moves between the key and value fields and Del deletes a key. The Surface Inspector's fit buttons take a right-click
variant ("fit width, keep height"). Ctrl+Shift+P opens a shortcuts list. There are dark themes (Blender / Adwaita Dark).

**Not copied:** the 2D/3D split as the main paradigm (our editor is 3D-first with optional ortho views, DESIGN.md), and
GTK-era density (tiny icons, no labels anywhere). We take the *workflow* ideas: mouse gestures, the filter toolbar, one icon
toolbar, few default panels, the texture browser that applies on click, and the entity menu at the pointer.

---

## 5. Design principles for the native editor

1. **The viewport is the product.** At least 70 % of the window area at 1280×800 goes to viewports. Panels exist to answer
   "what is this / what is there"; they are not a second way to edit what a gesture can do.
2. **One place per question.** *What am I working on?* → Properties (one contextual panel). *What is in the map?* →
   Outliner. *What can I put in?* → Browser. *What happened?* → the bottom drawer (messages, history).
3. **Show state where it applies.** Global state (tool, mode, snap) goes on the toolbar; per-viewport state (camera, render,
   filters) goes on that viewport's header; per-object state goes in Properties.
4. **Frequent = visible, rare = searchable.** The toolbar holds roughly the 20 most used actions. Everything else is in the
   menus and the palette, and the palette is advertised (a search box on the toolbar's right end opens it).
5. **Gestures before panels** (NetRadiant-custom). Texture pick / paste, entity creation, tunnel select and viewport
   maximise happen in the viewport with one click.
6. **Calm visuals.** Dark neutral surfaces, one accent colour for "active / selected", dark 1 px separators, monochrome icons,
   no chrome on areas that have one panel, labels never truncated.
7. **Nothing is lost.** Every panel can still be opened, docked, floated and saved in a layout preset; the change is the
   *default* and the grouping.

---

## 6. The new layout

### 6.1 Default arrangement (1280×800 and up)

```
┌──────────────────────────────────────────────────────────────────────────────────────────────┐
│ File Edit View Tools Mesh Brush Texture Help                                                  │ menu bar 24 px
├──────────────────────────────────────────────────────────────────────────────────────────────┤
│ [💾][↶][↷] │ [▢Select][⬛Shape▾][✂Clip][◩Decal][📏] │ [Object|Mesh] (·Vert·Edge·Face) │          │ toolbar 32 px
│ [✥][⟳][⤢] │ Grid [0.5 m▾] [⊞Snap] Rot [15°▾] │ [UV🔒] │ [▣1|▥2|▦4] │ ……… [🔍 Search actions  F3] │
├──────────────┬──────────────────────────────────────────────────────┬────────────────────────┤
│ BROWSER      │ Perspective ▾  Lit ▾  Show ▾      (12.4 m/s)    ⤢ ⧉ │ OUTLINER  [🔍 filter ]  │ header 22 px
│ [Textures▾]  │                                                      │ ▸ worldspawn   8       │
│ [🔍 search ] │                                                      │ ▸ func_detail  1       │
│ Recent: ▪▪▪▪ │                                                      │   light_point  l1      │
│ ▪▪▪▪▪▪▪▪▪▪▪▪ │                 viewport(s)                          │   …                    │
│ ▪▪▪▪▪▪▪▪▪▪▪▪ │                                                      ├────────────────────────┤
│ ▪▪▪▪▪▪▪▪▪▪▪▪ │                                                      │ PROPERTIES             │
│ ▪▪▪▪▪▪▪▪▪▪▪▪ │                                                      │ light_point  l1    [⋯] │
│              │                                                      │ ── Transform ───────── │
│              │                                                      │ ── Light ───────────── │
│              │                                                      │ ── Keys ────────────── │
│              │                                                      │ ── Last operation ──── │
├──────────────┴──────────────────────────────────────────────────────┴────────────────────────┤
│ ▲ Messages (2)  History  Lighting  Stats │ hints: LMB select · Ctrl+LMB toggle · …  │ Grid 0.5 │ status 22 px
└──────────────────────────────────────────────────────────────────────────────────────────────┘
```

- **Left, Browser** (≈ 20 % of the width, 240–360 px): one panel for everything that can be put into the map. A combo
  or segmented switch at the top (not a tab strip, so it never truncates): **Textures** (default), **Shapes**, **Entities**,
  **Prefabs**, **Decals**, **Models**, **Texture groups**. One search field filters whichever is shown.
- **Centre, viewports** with a header strip each (§6.3). The layout presets 1 / 2 / 4 stay (Ctrl+1/2/4 and the toolbar).
- **Right, Outliner over Properties** (≈ 22 %, 260–440 px; the Outliner gets 40 % of the height).
- **Bottom drawer**, closed by default: Messages, History, Lighting, Stats as ADS **auto-hide** tabs (the ADS version in
  `native/third_party/qtads` has `CAutoHideSideBar`). A click on a tab slides the panel up over the viewport and it closes
  when focus leaves; the pin keeps it docked. A new warning or error shows a count on the Messages tab and a notification
  (§6.6). It does not open the drawer by itself.
- **Status bar**: the hint text (kept), then clickable fields: grid ("Grid 0.5 m": a click opens the grid menu), snap, the
  selection count ("3 brushes": a click frames them), the map path (a click opens the containing folder), the game folder (a
  click changes it, §6.7).

### 6.2 The toolbar

A `QToolBar` built from a list of action ids (`ui/toolbar_def`, data like the menus): no hand-made buttons, so the binding
appears in every tooltip and the checked state follows `ActionDef::checked`. 20 px monochrome icons, text beside the icon
only for Shape / Clip / Decal and the mode switch. Groups are separated by a 1 px divider, not a label.

| Group | Contents (action ids) | Notes |
|---|---|---|
| File | `file.save`, `edit.undo`, `edit.redo` | Save shows a dot while the map is dirty |
| Tools | `tool.set select`, `tool.set box` (split button: the arrow lists the shapes, the last one chosen is remembered), `brush.clipper`, `tool.set decal`, `view.measure` | Exclusive group; the active tool is accented |
| Edit mode | `mesh.toggle` as a two-way segmented `Object | Mesh`; in Mesh mode, `mesh.element vertex/edge/face` appear next to it | NRC's component modes, Blender's 1/2/3 |
| Gizmo | `gizmo.mode move/rotate/scale` | Exclusive group |
| Snap | grid size combo (`grid.set`), `snap.toggle`, rotation step combo | `[` / `]` update the combo |
| Texture | UV lock toggle (new action `texture.uv_lock`; today `tools::mapBrushPoints` always locks UVs, the Shell had a toggle) | Accented when on; see §11 |
| Layout | `view.layout 1/2/4` | Three small icons |
| Right end | export / lighting progress (when running), `file.play`, a search field that opens the palette | The palette becomes discoverable |

Customisation (later, T14.11): right-click on the toolbar → "Customise toolbar…" (add or remove registry actions; stored in
the user settings like the keymap). NRC does the same through the button context menu.

### 6.3 The viewport header

Viewports are native windows (`QWidget::createWindowContainer`), so Qt widgets cannot be drawn over the image. The header is
therefore a **22 px strip above each viewport container**, inside the same dock widget, styled as part of the viewport
(the same dark surface as the clear colour). It replaces the dock title bar of viewport docks (ADS: no title bar for
viewport areas; the dock tab is hidden while the area has one dock).

Left: **camera** menu button showing the current mode ("Perspective ▾": Perspective / Orbit / Top / Front / Side,
bookmarks 1–9, frame selection, frame all), **render** menu button ("Lit ▾": Unlit / Lit / Lighting only, DLSS submenu, show
lightmap density), **Show ▾** (§6.5). Right: fly speed (only while flying or after a speed change; it fades after 2 s),
**maximise** (⤢, F12 / Ctrl+Space toggles), pop-out (⧉, floats the viewport).

Clicking a header makes that viewport the active one, so the "(active viewport)" items in the View menu are no longer the
only way.

### 6.4 Properties: one contextual panel

Properties replaces **Details, Surface, Level, Adjust and Tool options**. It shows sections for what is selected, in this
order, each collapsible (the collapsed state is remembered per section type):

| Selection | Header | Sections |
|---|---|---|
| Nothing | "Map: demo_lit" | **Level** (worldspawn keys, sky, bake settings: today's Level panel), **Tool** (options of the active tool when it has any) |
| One or more entities | class + name, `[⋯]` menu (select by key / value, copy keys) | **Transform** (origin / angles; multi-edit shows "—" for mixed values, U8), class-typed **Properties** (light colour, intensity…), raw **Keys** (Tab moves key ↔ value, Del removes, as in NRC) |
| Brushes | "3 brushes (func_detail)" | **Transform**, **Entity** (when the brushes belong to one), **Surface** (texture of all faces) |
| Faces | "4 faces" | **Surface** (texture thumbnail + name, shift / scale / rotate, fit, justify; NRC: a right-click on Fit width keeps the height) |
| Mesh elements (Mesh mode) | "12 vertices" | **Mesh** (element counts, smooth / sharp), **Surface** for faces |
| Always, after an operator with parameters | — | **Last operation** (today's Adjust panel, U1) at the bottom, open, until the next edit |

The **Tool** section also appears at the top of Properties while the Shape, Decal or Clip tool is active, so their options
(segments, thickness, decal size, the T13.1 Decals tab) are in one predictable place.

### 6.5 Show menu (NetRadiant-custom's filter toolbar)

The `Show ▾` header button opens a popup with checkboxes, two clicks for any filter, per viewport or for all (a toggle at the
top): brushes, detail, entities, point entities, lights, triggers, clip / hint / caulk (the game profile's `hideGroups` and
today's hide rules from `View > Hide`), decals, models, names, light radii, grid, size labels. The checked rules of the game
profile stay shared as today. A right-click on an entry gives "Show only this" (NRC's right-click extras). The full hide-rule
editor (patterns) stays as "Edit rules…" at the bottom.

### 6.6 Notifications instead of boxes (U14)

A small stacked toast in the bottom right of the main window (above the status bar; a top-level, input-transparent window
like the key help overlay, because viewports are native windows). It shows for 4 s for info and 8 s for warnings, stays until
dismissed for errors, and every notification also goes to the Messages drawer. It is used for load / save / export results,
validation refusals and "texture not found". `QMessageBox` stays only where a decision is needed (unsaved changes, overwrite).

### 6.7 Game folder (the start page was dropped by the user, 2026-11-22)

With no map open, the central area shows a start page instead of an empty grid: **New map**, **Open map…**, the recent maps
(thumbnails when a screenshot exists next to the map), and the **game folder** with a "Change…" button. Opening a map that lies
outside the current game folder offers to switch to the folder that contains `maps/` (this would have avoided the
2026-11-22 confusion between `samples/integration/data` and `core/assets`). The start page is a normal widget in the
viewport dock area; it is replaced by the viewports when a map opens.

### 6.8 Context menu at the pointer

A right-click without drag in a viewport (RMB drag stays camera look / orbit / pan) opens a **short, selection-aware** menu
built from sections, instead of today's flat 25 entries:

- **Add here ▸**: the five most recent classes / shapes / prefabs, then *Entity…*, *Shape…*, *Prefab…*. Each "…" opens the
  Add search popup (§8.4) at the pointer. The new object is placed on the surface under the click (`placementPoint` with the
  ray, as in T13.4).
- **Selection** (only with a selection): the 4–6 actions that fit the selection kind (brushes: Hollow, Merge, Clip, Make
  entity ▸; entities: Select same class, Move to world; faces: Copy surface, Paste surface, Fit, Select same texture).
- **Edit**: Cut, Copy, Paste here, Duplicate, Delete, Hide, Unhide all (n).
- **Properties** (focuses the panel).

NRC's "convert the selected entities" (right button inside the entity menu) becomes "Change class ▸" in the entity section.

---

## 7. Click budget: common tasks before and after

"Click" = mouse click; a key press counts as one. Today's counts are from the current build.

| Task | Today | After | How |
|---|---|---|---|
| Change a viewport's render mode | make it active, View › Render mode › Lit: **4** | header "Unlit ▾" › Lit: **2** | §6.3 |
| Change the grid size | View › Grid › 0.25: **3** (or `[` `]`) | toolbar combo: **2**; `[` `]` unchanged; status field: **2** | §6.2 |
| See the current tool / mode / gizmo | read the status bar | always visible on the toolbar: **0** | §6.2 |
| Texture one face | Shift+click the face, Textures tab, find, click: **3+** | Shift+click the face, click the texture (Browser shows Textures by default): **2**; or `MMB` on a source face and `Shift+MMB` on the target: **2** | §6.1, §8.1 |
| Texture many faces like another | lift, select faces, apply: **4+** | `MMB` source, `Shift+MMB drag` over the targets: **1 drag** | §8.1 |
| Create a light at a spot | Place, scroll the tabs, Entities, find, drag: **4+** | RMB › Add here › light_point (recent): **3**, or `Shift+A`, type "li", Enter: **1 key + type** | §6.8, §8.4 |
| Edit a light's colour | select, find "De" among 9 tabs, scroll: **3** | select, Properties is visible, colour swatch: **2** | §6.4 |
| Select the object behind the one in front | rotate the camera or use the Outliner: **many** | click again on the same spot: **2** | §8.2 |
| Look at one viewport full screen | Ctrl+1, then Ctrl+4 to go back (re-lays out the docks) | F12 / Ctrl+Space or header ⤢: **1** | §8.3 |
| Hide all triggers | View › Hide › dialog › check › close: **5** | header Show › Triggers: **2** | §6.5 |
| Open a recent map | File › Open Recent › map: **3** | start page: **1**; File › Recent: 3 | §6.7 |
| Read an error | dismiss the modal box: **1**, blocks everything | toast, also in Messages: **0** | §6.6 |
| Find any command | F3 (not visible anywhere) | toolbar search field or F3: **1** | §6.2 |
| Hollow a brush with a thickness | select, Ctrl+H, find "Ad" tab, edit: **3+** | select, Ctrl+H, the Last operation section is open in Properties: **2** | §6.4 |

---

## 8. Mouse-first workflows (from NetRadiant-custom)

All of them are registry actions with keymap entries (contexts `tool:select` or `area:viewport`), so the user can rebind them.
The defaults avoid today's bindings; the clashes that need a decision are in §11.

### 8.1 Texture eyedropper and paste (NRC `m3` family)

| Default input | Action | Effect |
|---|---|---|
| `MMB` click (no drag) on a face | `texture.pick_at` | copies texture + alignment of the face, selects it in the Browser (scrolls to it) and shows it in the status bar |
| `Shift+MMB` click / drag | `texture.paste_at {mode: full}` | pastes texture + alignment on the face under the pointer, or on every face swept by the drag |
| `Alt+MMB` click / drag | `texture.paste_at {mode: name}` | texture only, keeps the face's alignment |
| `Ctrl+MMB` click / drag | `texture.paste_at {mode: seamless}` | seamless continuation from the copied face (DESIGN.md "copy surface", NRC `Ctrl+m3`) |

`MMB drag` (no modifier) stays camera pan. The router already tells clicks from drags (`LMB` vs `LMB drag`). A paste drag is
one undo step ("Paste texture"). `Shift+RMB` lift stays as an alias.

### 8.2 Tunnel select (NRC `m1` repeated)

A second plain click within 3 px of the previous one, on the same set of hits, selects the **next** object along the ray
instead of the first (wraps around). The status bar says "2 of 4 under the pointer". `render::Picker::pick` returns only the
nearest hit today, so this adds a `pickAll(ray)` that returns every hit sorted by distance (same per-item test, no new
renderer work); no new binding.

### 8.3 Maximise the view under the cursor (NRC F12)

`F12` and `Ctrl+Space` (Blender) toggle `view.maximise`. The dock area of the viewport under the cursor takes the whole central
area; the other viewports are hidden, not destroyed (their render targets are kept), and the second press restores them. The
header ⤢ button does the same.

### 8.4 Add at the pointer (`Shift+A`, Blender; NRC entity menu)

`Shift+A` in a viewport opens a small search popup at the pointer: one list of shapes, entity classes, prefabs, decals and
models, recent ones first, with type-to-filter (the palette's matcher). Enter creates the object on the surface under the
pointer (T13.4 `placementPoint` with the pointer ray) and selects it, so the move gizmo is immediately in place. Ctrl+Enter
keeps the popup open (NRC Ctrl keeps the menu). The right-click menu's *Add here ▸* opens the same popup.

### 8.5 Browser applies on click (NRC texture browser)

In the Textures view a click selects the texture **and applies it to the selected faces / brushes** (one undo step "Apply
texture"); `Ctrl+click` only makes it the active texture. Double-click opens the texture properties window (today's
behaviour). `Ctrl+wheel` changes the thumbnail size (48–160 px, following the UI scale). A **Recent** row (last 12) sits above
the grid and **Favourites** (star on hover) is a filter (U11). Dragging a texture onto a face in a viewport applies it there
(the drop uses the T13.4 mouse-tracking path, since viewports cannot take Qt drops).

### 8.6 Smaller gestures

- `Alt+RMB drag` in a perspective view orbits around the clicked point (NRC `Alt+m2`), alongside the orbit camera mode.
- Zoom (wheel dolly) moves toward the mouse pointer, not the view centre (NRC option, default on). This is a camera rig change
  inside `render/`; it needs the user's yes (§11).
- `Tab` stays Object / Mesh (Blender convention, already in the keymap). NRC's "focus on selection" stays `F`.
- Paint selection (NRC `Shift+m1 drag`): today `Shift+LMB drag` is face pick, so this is offered as an option (§11), not a
  default.

---

## 9. Visual style

### 9.1 Theme tokens (`ui/theme.cpp`, one QSS generated from tokens)

Port the Shell's palette so the two editors look related, and drive the whole style sheet from named tokens (as
`style.css :root` did). `ui/theme.cpp` already exists; it gets a token table and a QSS template:

| Token | Dark (default) | Use |
|---|---|---|
| `bg0` | `#141416` | menu bar, tab strip background, status bar |
| `bg1` | `#1c1d20` | panel background |
| `bg2` | `#24252a` | toolbar, viewport header, popups |
| `bg3` | `#2e3036` | buttons, hovered rows |
| `bg4` | `#3a3d44` | pressed / hovered buttons |
| `line` | `#0c0c0e` | splitters (1 px), panel borders |
| `line2` | `#393c43` | section hairlines, input borders |
| `text` / `textStrong` / `dim` | `#d2d4d8` / `#f2f3f5` / `#868a93` | normal / emphasised / secondary text |
| `accent` / `accentBg` | `#3b8eea` / `#1d4f86` | active tool, selection, focus ring, active tab top line |
| `ok` / `warn` / `bad` | `#4ec46a` / `#f0a030` / `#e5645e` | status, notifications |

Sizes: 9 pt Segoe UI (12 px at 100 %), controls 24 px, tabs 26 px, toolbar 32 px, headers 22 px, 4 / 8 / 12 px spacing.
The light theme and UI scale (U15) keep working by swapping the token table. Viewport overlay colours (selection orange,
gizmo axes) are renderer settings and do not change.

### 9.2 Docking chrome (ADS configuration flags)

- `DockAreaHasCloseButton`, `DockAreaHasUndockButton`, `DockAreaHasTabsMenuButton` off; `DockAreaHideDisabledButtons` on.
  Close and float stay available in the tab context menu and the View menu.
- `AlwaysShowTabs` off: an area with one panel shows a 22 px caption, not a tab bar. `EqualSplitOnInsertion` on.
- No title bar on viewport areas (the header of §6.3 replaces it).
- Splitter handle 1 px `line` (hit area 5 px through the handle's margin), so the frame recedes.
- Active tab: 2 px accent top line (the Shell rule); inactive tabs: `dim` text, no box.
- Auto-hide sidebar for the bottom drawer (§6.1).

### 9.3 Icons

One consistent monochrome set, 16 / 20 px SVG, coloured by the theme (`text`, `accent` when checked). Use the **Lucide** set
(ISC licence, permissive; download allowed per the standing rule) for the generic ones (save, undo, search, eye, lock,
maximise) and draw the editor-specific ones (box tool, clipper, decal, vertex / edge / face, gizmo modes) in the same 1.5 px
stroke style, stored in `native/assets/icons/*.svg` and compiled into a Qt resource. Every icon button has a tooltip:
"Clipper (X): split brushes with a plane".

### 9.4 Viewport text

**Approved by the user.** Entity name labels draw only for the selected and hovered entity, and for the others within 8 m of the camera, fading out
between 6 and 8 m. A `Show ▾ › Names: All / Near / Selected` choice sets this. This touches the label overlay in `render/`, so
it needs the user's yes (§11). Until then the default of the existing names toggle is set to "selected only", which needs no
renderer change if the toggle already filters by selection; otherwise it waits.

---

## 10. Implementation plan (milestone M14)

Each task is one commit `native: T14.n …` with tests, a status row in `08_MILESTONES.md` and a memory note, as before.
Order: the visual foundation first (it touches every later screenshot), then the structure, then the gestures.

| Task | Scope | Main files | Tests / acceptance |
|---|---|---|---|
| **T14.1** Theme tokens + docking chrome | token table, QSS template, ADS flags of §9.2, 1 px splitters, section header style; no layout change yet | `ui/theme.cpp`, `ui/main_window.cpp` (flags) | GUI test: the flags are set, the splitter width is 1; screenshot reviewed |
| **T14.2** Icon set | Lucide subset + own icons, `.qrc`, `ActionDef::icon` field, icons in menus | `native/assets/icons/`, `CMakeLists.txt`, `app/actions/registry.h` | Unit test: every action listed in the toolbar def has an icon that loads |
| **T14.3** Toolbar | `ui/toolbar_def` (data), `ui/main_toolbar` (QToolBar from the registry), split button for shapes, grid / rotation combos, palette search field, progress area | new `ui/main_toolbar.{h,cpp}` | GUI: clicking each button runs its action; checked states follow the tool / mode / gizmo / snap / UV lock; tooltips contain the binding; G3 |
| **T14.4** Viewport header | header strip per viewport, camera / render / Show menus, maximise, pop-out, the speed readout; viewport docks lose their ADS title bar | `ui/viewport_header.{h,cpp}`, `main_window.cpp` `buildViewports` | GUI: the header changes that viewport only; a click on the header activates it; §7 rows 1 and 10 |
| **T14.5** Properties panel | one panel with sections (Level, Tool, Transform, Entity props, Keys, Surface, Mesh, Last operation) built from today's `DetailsPanel`, `SurfacePanel`, `LevelSettingsPanel`, `AdjustPanel`, `ToolOptionsPanel` as section widgets; collapse state kept | `ui/properties_panel.{h,cpp}`; the old panels become sections | GUI: the sections for each selection kind (§6.4 table); edits unchanged (existing details / surface / adjust tests still pass against the sections) |
| **T14.6** Browser panel | one panel with a view switch (Textures, Shapes, Entities, Prefabs, Decals, Models, Groups), one search field; click applies (§8.5); Recent row; Ctrl+wheel size | `ui/browser_panel.{h,cpp}` from `PlacePanel`, `TextureBrowserPanel`, `GroupsPanel` | GUI: click applies to selected faces as one undo step; Ctrl+click does not apply; the search filters each view; drag-spawn (T13.4) still works |
| **T14.7** Default layout + drawer + status fields | the §6.1 arrangement with relative sizes; Messages / History / Lighting / Stats in the bottom auto-hide bar; clickable status fields; `kStateVersion` bump; View › Reset layout | `main_window.cpp`, `main_window_panels.cpp`, `status_bar.cpp` | GUI: G1 and G2 at 1280×800 and 1920×1080; an old saved state is ignored once (G8) |
| **T14.8** Notifications | toast overlay + Messages; replace non-decision `QMessageBox` calls | `ui/notifications.{h,cpp}`, callers | GUI: opening a non-map JSON shows a toast and no modal (G5) |
| **T14.9** Game folder | clickable game-folder status field, the "map outside game folder" offer, a notification instead of a modal when a file is not a map | `status_bar.cpp`, `main_window_document.cpp` | GUI: opening a map from another `maps/` folder offers the switch |
| **T14.10** Context menu + Add popup | sectioned, selection-aware RMB menu (§6.8); `Shift+A` Add popup (§8.4) with recent items | `tools/context_menu.cpp`, `ui/add_popup.{h,cpp}` | Unit: menu sections per selection kind; GUI: Add creates on the surface under the pointer and selects it |
| **T14.11** Viewport gestures | MMB texture pick / paste family (§8.1), tunnel select (§8.2), maximise (§8.3), `Alt+RMB` orbit around point | `main_window_texture.cpp`, select code, `main_window.cpp`, keymap | GUI per gesture; a paste drag is one undo step; keymap tests updated |
| **T14.12** Toolbar customisation + polish | right-click "Customise toolbar…", saved per user; final screenshot pass of every panel for truncation and spacing | `ui/main_toolbar.cpp`, settings | GUI: a customised toolbar survives a restart |
| T14.13 Label decluttering (approved) | §9.4 distance / selection rule for entity names | `render/` overlay | unit test of the label rule |
| T14.14 Zoom to pointer (approved) | §8.6 wheel dolly toward the cursor | `render/camera` rig | unit test: the point under the cursor stays under it |

The existing 49 GUI cases keep passing after each task (tests that address a panel by its old dock name get a helper that
finds the section inside Properties / Browser). `13_CUTOVER_CHECKLIST.md` gets a line per task where it changes how a
REQUIREMENTS item is reached.

---

## 11. Decisions

**Answered 2026-11-22: all proposed defaults accepted ("all yes"), except the start page, which is dropped.** So: box select stays
(NRC drag-to-draw as a setting), MMB texture gestures yes, Shift+LMB stays face select, all four drawer panels auto-hide, both
renderer items (labels, zoom to pointer) yes, Lucide + own icons, no start page (§6.7 is cancelled, T14.9 shrinks to the
game-folder status field and the "map outside the game folder" offer), the UV lock button is added, the look comes first.
The questions are kept below for the record.

1. **Empty-space LMB drag in the Select tool**: box select (today, UE / Blender) or *draw a brush* (NetRadiant-custom)?
   Proposal: keep box select as the default and offer the NRC behaviour as a preference.
2. **MMB texture gestures** (§8.1): OK to give `MMB` click / `Shift` / `Alt` / `Ctrl+MMB` to texture pick and paste, with `MMB
   drag` staying pan?
3. **Paint selection** (`Shift+LMB drag` sweeps to add): today that input is face pick. Keep face pick (proposal), or switch to
   NRC paint select and move face pick to `Ctrl+Shift+LMB`?
4. **Bottom drawer**: Messages / History / Lighting / Stats hidden behind auto-hide tabs. Is the Lighting panel used often
   enough to stay docked on the right instead?
5. **Renderer-side items**: entity-name decluttering (§9.4) and zoom to the pointer (§8.6) change `render/`. Yes or no?
6. **Icons**: Lucide (ISC) plus our own drawn icons. OK, or do you prefer text-only buttons?
7. **Start page** (§6.7): wanted, or open straight into an empty map as today?
8. **UV lock toggle**: UV lock is always on in the native editor. Add the Shell's on/off toggle to the toolbar, or leave it
   always on and drop the button?
9. **Order**: the plan starts with the theme (T14.1–T14.2) because it changes how everything else looks. Start with the
   toolbar and the panel merge (T14.3–T14.7) instead if the structure matters more to you.

---

## 12. Sources

- Native editor: screenshot of `mapeditor_native.exe --assets core\assets --map core\assets\maps\demo_lit.map.json` at the
  default window size (2026-11-22); `native/src/ui/main_window.cpp`, `main_window_panels.cpp`, `tools/context_menu.cpp`,
  `native/assets/keymaps/default.json`; `native/third_party/qtads/src/DockManager.h` (config and auto-hide flags).
- Shell: `shell/src/app.ts` (menu bar, toolbar, default layout), `shell/src/style.css` (tokens), `shell/src/ui/viewport.ts`
  (viewport header).
- NetRadiant-custom: github.com/Garux/netradiant-custom — `README.md`, `docs/mouse shortcuts.txt`, `docs/changelog-custom.txt`,
  `radiant/mainframe.cpp` (`create_main_toolbar`, `create_view_menu`, `MainFrame::Create`); garux.github.io/NRC (features page).
  The developer.valvesoftware.com wiki page could not be read (HTTP 403). The camera and patch toolbar contents and the layout
  picker icons were not in the part of `mainframe.cpp` that was read.
- Earlier UI decisions: `10_UI_AND_INPUT.md` §6–§7 (U1–U17); U4 workspaces and U16 pie menus stay [ask] and are not part of this plan.
