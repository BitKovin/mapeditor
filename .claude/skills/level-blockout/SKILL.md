---
name: level-blockout
description: Prototype a playable level blockout in this map editor from an idea, sketch or reference image (any 3D genre) - write the map as .map.json with the bundled Python kit, open it through Core in the Shell, take camera shots, and verify the critical path with a reachability check per lock state. Use when asked to block out, prototype, generate or iterate on a level or map, or to continue the AI-level-design / MCP work (MCP_DESIGN.md).
---

# Level blockout (prototyping levels with AI in this editor)

The MCP described in `MCP_DESIGN.md` is not built yet. This skill is how to make and iterate levels **today**: generate the
document with Python, let Core validate and serve it, look at it in the Shell, verify it, and iterate on the user's directions.

**Read `MCP_DESIGN.md` §2 (lessons L1–L10) first.** They are the user's corrections from the first session; ignoring them repeats the
same failures. The short version:

1. **Path first, setting second.** A level is a route through a setting. Most of a building is backdrop (closed facades, inaccessible
   floors and wings). Only the critical path + 1–3 purposeful pockets are playable. Never make every room explorable.
2. **Every route space is distinct** (typology, size, height, light, view out). No long identical corridors, no rows of same rooms.
3. **Text for logic only** (route, gates, encounters); space from building types / precedents / typologies; never trust a self-critique
   of a render ("reads as grand") — report measurable facts, let the user judge taste.
4. **Human-scale elements, big by extent.** Never scale everything up. Sizes follow the player's metrics and camera (table below).
5. **Compose hero spaces from real typologies** (imperial staircase, galleries on the walls) and make traversal **diegetic**
   (bookcases, scaffolding, collapsed floors, balconies, plinths). One idea per space. No mid-air bridges + rings + floating ledges in one hall.
6. **Variants first (2–3, different), then change only what the user asks.** Small, cheap shots; tell what changed.
7. **Verify** with `reach.py` for every lock state before showing anything as "playable".

## Workflow

1. **Collect**: the idea / reference image, genre, player metrics (speed, jump height, dash/hover, camera distance), the setting's
   building type, how big. Ask only what is missing.
2. **Write the route** (text): beats in order, gates (key → lock), encounters, pockets, end, one loop or shortcut back.
3. **Generate 2–3 variants** cheaply (massing + route only), take one aerial + one key view each, let the user pick.
4. **Build the chosen one**: setting massing (closed shell), then only the route spaces with openings and interiors, then entities.
5. **Verify** (`reach.py` per lock state), fix leaks (sequence breaks) and dead ends, snap entities to floors.
6. **Shoot** 3–5 small views (eye height at key beats + one overview), send them with `SendUserFile`, list measured facts and known
   problems, ask for directions. Repeat 4–6 for each direction, changing only what was asked.

## Running the editor for a test (Windows)

- **Assets**: copy `core/assets` to a scratch folder and generate maps into `<scratch assets>/maps/<folder>` (the editor only opens maps
  inside its assets root; never write test maps into the repo's `core/assets`).
- **Core**: a worktree has no `core/build`; use the main checkout's build (or run `build_core.bat`):
  `F:/Git/mapeditor/core/build/mapeditor_core.exe 8787 "<scratch assets>"` — run it with `run_in_background`. Port 8787 must be free.
- **Shell**: `preview_start` with the `shell` configuration from `.claude/launch.json` (Vite on 5173). It needs `shell/node_modules`:
  `npm --prefix shell install`, or in a worktree a junction to the main checkout
  (`cmd //c "mklink /J node_modules F:\Git\mapeditor\shell\node_modules"` from `shell/`; remove it afterwards with
  `cmd //c rmdir node_modules` — rmdir without /S removes only the link).
- **Debug hook**: open `http://localhost:5173/?debug`. `window.__editor` has `open(path)` (Core-relative map path, returns
  `{ok, error}`), `store`, `viewport()`. Per viewport: `setCameraMode('fly')`, `camera.lookAt(eye, target)`, `invalidate()` (needed after
  moving the camera), `settings` (`showSubtractive`, `showGrid`, `showOrigins`, `showDirections`, `showLinks`, `showLightGizmos`,
  `showSizes` → set false for clean shots), `element`, `canvas`.
- **Full-viewport shots**: `v.element.style.cssText = 'position:fixed;left:0;top:0;width:100vw;height:100vh;z-index:9999'`, dispatch a
  `resize` event, then for each shot: `lookAt` → wait ~300 ms → `invalidate()` → wait ~1 s → `computer screenshot` (scale 0.45–0.55 is
  enough, ~150–250 tokens). Reload the page afterwards to restore the editor layout.
- **Pitfalls**: a **hidden Browser pane does not draw** (screenshots time out) → `mcp__ccd_view__show_pane` with `preview`. Capture
  resolution is the pane's real size; `resize_window` emulation does not add pixels. Point-entity **name labels cannot be hidden** (they
  clutter interior shots). Inside carved volumes the camera is tinted red unless `showSubtractive` is off. A 7,900-brush map opens in ~7 s.
- **Clean up** when done: stop Core (`TaskStop`) and the Vite server (`preview_stop`), remove a node_modules junction.

## The kit (`scripts/`)

All Python, standard library only. Set `LEVEL_OUT=<scratch assets>/maps/<folder>` before running.

| File | What it gives you |
|---|---|
| `mapgen.py` | `Map`: `mass(min, max, tex)` convex worldspawn box; `cut(min, max, wall, floor, ceil)` subtractive brush (carves worldspawn only); `det(min, max, tex)` func_detail box; `stairs(x0, x1, z_from, z_dir, y0, y1, riser, tread)`; `point(class, origin, **keys)`; `doc()` → schema 0.2 document; `visible(a, b)` ray check through solids (beat checks). Also the side-by-side test maps (text-only A vs building type B). |
| `mansion.py` | Shape helpers: `hull(m, points, tex, detail)` (any convex brush from points, loops CCW from outside), `roof(m, x0, z0, x1, z1, y, h, inset)` (mansard / hip / ridge), `prism`, `cone` (round-ish columns, topiaries, spires), `block(...)` palace volume (carved windows); plus the small carved mansion variants A/B (Roswaal-style, slasher scale). |
| `mansion4.py` | `Level(Map)` with brush entities (`brush_ent`, `door` → `func_door`); `Bld` — additive building generator: exterior walls with glazed windows in bays and doors, floor slabs with holes, corridor + room slots, partitions, switchback stair towers, hero spaces (hall, library, ballroom, kitchen, chapel); gardens; gated gameplay (keys, locks, triggers, end), enemies snapped to floors; runs `reach.py` per lock state. **It builds every room — the approach rejected in L6.** Reuse `Bld` for the setting shell and for route spaces only. Its hall is the "absurd" one (L7). |
| `reach.py` | `Model(doc, open_doors, bounds)`: lazily computed solid columns (1 m cells, `reach.CELL` / `reach.TILE`), floors with ≥ 1.8 m head room; `reach(x, z, y)` flood fill (up ≤ `JUMP` 5.4 m, any drop); `at(seen, x, z, y)`; `floor_at(x, z, y)`. Hull brushes count as their bounding boxes (conservative). |

Verification pattern (from `mansion4.py`):

```python
for name, open_doors in [("no keys", ()), ("red used", ("conservatory_door",)), ("red + blue", (...))]:
    md = reach.Model(doc, open_doors=open_doors, bounds=BOUNDS); seen = md.reach(*START)
    print(name, {label: md.at(seen, x, z, y) for label, x, z, y in CHECKS})
```
Expected: each locked area unreachable until its key is used; the end reachable only at the last state; every pocket reachable.

## Document essentials (schema 0.2)

- Meters, **Y up, north = −Z, east = +X**. Faces are vertex loops **CCW seen from outside**; face ids are stable ints. Box loops for vertex
  order `(x0,y0,z0) (x1,y0,z0) (x1,y1,z0) (x0,y1,z0) (x0,y0,z1) (x1,y0,z1) (x1,y1,z1) (x0,y1,z1)`: `[0,3,2,1] [4,5,6,7] [0,4,7,3] [1,2,6,5] [0,1,5,4] [3,7,6,2]`.
- `worldspawn` / `func_group` brushes must be **closed convex solids**; `func_detail` and other brush entities may be concave.
  Subtractive brushes (`op: "subtract"`) may be concave; `carve: {self, structural, nonStructural}` decides what they cut (scope by "same
  entity", then structural or not).
- Brush entities (`func_door`, `trigger_*`, `triggerOnce`) carry `brushes`; point entities an `origin`; all keyvalues are strings.
- Visibility is per cell at export: **no sealing needed**; open-air levels are fine (`_sky_auto` on worldspawn makes the sky box).
- Textures (demo assets): `dev/grey` walls, `dev/metal` roofs, `dev/concrete` stone/paths, `dev/floor` (green) lawn/hedges, `dev/wood`,
  `dev/glass`, `dev/grate`, `dev/wall` (red brick). Faces without `uvs` are projected at 2 m per repeat.

## This game (demo profile) cheat sheet

- Locks: `trigger_key_once` (`requiredKeys` "red" / "red blue", `target`) → `func_door` (`targetname`, `targetLocation` real3 — engine
  semantics unknown, left "0 0 0", `time`). One-shot: `triggerOnce` (`target`). End: `logic_level_end` (`targetname`, `nextLevel`).
- Items: `item_key_red|blue|green|secret|special`, `item_weapon_twinswords|pistol|shotgun|smg|cannon|cane`, `item_ammo_*`.
- Enemies: `npc_human_axe` (sword), `npc_human_gun`, `npc_dog`, `npc_guard`, `npc_civilian` (+ `task_smoke|chair|chat`, `nav_point`
  patrols via `target`), `spawner`, `counter`. No boss class. Point-entity yaw `angle`: 90 = north (−Z), 270 = south.
- Player (user, 2026-10-17): slasher, fast, jumps ~5.4 m (3× height), hovers, dashes, far third-person camera.

## Sizes for this game (far camera, ≈1.3–1.5× real)

Storey 6.5 m · interior door 3.5 × 4.5 · main door 5 × 7 · window 2.2 × 3.6, bay 6, glazed (sill 1.0 / 1.2) · corridor 6 (only on the
route) · stairs riser 0.25, tread 0.4–0.6 · boundaries ≥ 7.5 m · climbing steps ≤ 4.5 m apart and adjacent · overhead structures on
paths ≥ 11 m · arenas ~40–70 m across, structured (fountain tiers, statues on plinths, low beds as platforms), never an empty plaza.

## Token budget habits

Generate and verify in Python (text output only); take few small screenshots; batch camera moves + screenshots in one
`browser_batch`; never paste whole documents; report tables of measured facts.
