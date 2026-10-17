# MCP for AI level prototyping — design (not built yet)

**Status (2026-10-17):** design stage. Nothing of the MCP is implemented. This file is the source of truth for the MCP; `DESIGN.md` §18.8
points here, `REQUIREMENTS.md` §37 lists the user's requirements, `NEXT_STEPS.md` has the hand-over. The practical know-how for making
levels in this editor *today* (scripts, editor hooks, verification) is the project skill `.claude/skills/level-blockout/` — use it in a new
chat to continue prototyping before the MCP exists.

Read §2 (lessons) before designing or generating anything: every rule there came from a failed attempt and a correction by the user.

---

## 1. Goal and audience

- An MCP server that lets Claude (Claude Desktop, cowork mode; optionally with computer use) **prototype real levels** in this editor for
  almost any 3D genre: a playable critical path with gates, encounters and an end, in a believable setting. Not "a bunch of corridors or
  offices", not a pile of rooms.
- **Audience: level designers and artists**, to prototype general ideas fast and to help come up with them. **Not novices** (user's
  decision). The person brings the idea (words, sketch, reference image), gives directions and judges taste. The AI does fast, measured
  construction, proposes variants, keeps everything valid and reports what it can measure.
- **Credit efficiency matters**: good results without huge sessions (see §10).

## 2. Lessons from the 2026-10-17 session (rules)

| # | What happened | Rule |
|---|---|---|
| L1 | The user's earlier experience: models designing in text first give boring, sometimes absurd results — a model cannot imagine the view it describes. Our own text-only test map was a box house in a field. | Text is for **logic** only (what connects, locks/keys, order, encounters). **Space** comes from building types / precedents, set pieces, and views checked by rays and renders. |
| L2 | My own critique of a render called a cramped stair "a grand stair": the model's visual self-check matches labels, it does not judge form. | The AI reports **measurable facts** (sizes vs metrics, reachability, visibility); **the human judges taste**. Never claim quality from a render. |
| L3 | Forms were child-like: extruded boxes with holes, identical windows, flat roofs, no hierarchy, no in-between spaces (porch, vestibule, landing). Fixing it with designer-made module kits was rejected: "tedious work on the designer". | Form vocabulary must come from the **tool** (procedural architecture written once: massing hierarchy, roofs from footprints, plinth / string course / cornice, window depth and sills, entrances with steps, transitions) plus **real proportions**, never from the model improvising primitives and never from per-project kits. |
| L4 | The player of this game is fast, **jumps ~3× its height (~5.4 m), hovers, dashes**, and the **camera is far** (third person). | Player metrics drive every dimension: boundaries higher than the jump (≥ 7–8 m), anything 1–5 m tall is a platform, overhead structures clear the camera (a pergola had to go from 7.4 m to 11 m). |
| L5 | "Make everything 2–3× bigger" done as a uniform scale-up made the player feel **small**, not the manor feel big. | **Big = extent, not blown-up parts.** Keep elements human-scale (≈1.3–1.5× real for the far camera: storey 6.5 m, doors 3.5×4.5, windows 2.2×3.6, steps 0.25) and make the place big by length, floors, number of bays, wings, garden rooms. Only a few hero spaces are large. |
| L6 | The 300 m manor with every room open was not fun: "a lot of same without clearly defined path". | **A level is a path through a setting.** Most of the building is backdrop: closed facades, inaccessible wings and floors. Playable = the critical path + a few purposeful side pockets (reward, secret, shortcut). Block everything else. No two route spaces alike. |
| L7 | The entrance hall stacked every system — ramp-like stair into a mid-air bridge, two gallery rings, floating ledges, a chandelier disc: "this space is just way too absurd". | **Compose hero spaces from real typologies** (imperial staircase: one flight to a landing, two return flights along the walls; galleries on the walls) and make traversal **diegetic** (bookcases, scaffolding, a collapsed floor, balconies, statues on plinths). One idea per space. |
| L8 | Iteration worked when the user picked between variants and gave short directions. | Offer **2–3 distinct variants first**; then **change only what was asked**; report what changed and what did not. |
| L9 | Reachability per lock state caught real bugs: ground-floor window recesses next to playable ground were entrances; a missing top-floor stair landing cut off the attic. | Verification is a core tool (§8): flood fill per lock state, windows glazed and impassable, boundaries unjumpable. |
| L10 | Core accepted a 7,900-brush document (4.7 MB, ~7 s to open). Mass + carve (subtractive rooms) was fine at ~700 brushes. | For big buildings build **additively** (walls with openings, slabs); use carving for small things. Generate interiors only for playable spaces (L6) — it also keeps documents small. |

## 3. Prior art (checked 2026-10-17)

**Editor MCPs** — all are remote controls: the model still places things by coordinates, and nothing says whether the result plays well.

| Tool | Notes |
|---|---|
| **Unreal 5.8, Epic's built-in MCP plugin** (tried hands-on over HTTP at `127.0.0.1:8000/mcp`) | Good: only **3 meta-tools** (`list_toolsets`, `describe_toolset`, `call_tool`) over ~20 toolsets → tiny context cost; a **programmatic toolset** = sandboxed Python (math/json/re only) that may only call registered tools — a whole blockout in one round trip; `CaptureViewport` with an **annotation overlay** (projected grid with coordinates, labelled actors, structured camera/actor data back); **AgentSkills stored as project assets**, created only with user permission; output schemas; "unset = keep" transforms; PIE control and logs. Bad: primitives + actor placement only; no spatial analysis except one `trace_world` ray; **screenshots returned as base64 inside JSON text** (456 KB for one capture); **units mixed** (API cm, overlay labels m); 90+ character tool names and object paths; `describe_toolset` costs 20–27 KB; untextured primitives render black; no booleans. |
| Unreal third-party (Flopperam, StraySpark, ChiR24) | 50–500+ tools; StraySpark groups tools into **presets** because big tool sets hurt tool selection; viewport capture. |
| Unity (official, AI Assistant) | Scenes, assets, scripts, console; external clients need approval. |
| Roblox Studio (official) | `screen_capture` from any camera, start/stop play, **simulated player input/navigation**, a **playtest sub-agent**, docs/skill lookup. |
| Blender (official, connected in this session) | Summaries first, screenshots, docs search, "run code only as a last resort". |

**Research**
- *Holodeck* (CVPR 2024): LLM-written coordinates went out of bounds and collided; LLM-written **relations + a constraint solver** fixed it.
- Xu & Verbrugge 2025, *Constraint Is All You Need* (FDG) and *A Database-Driven Framework for 3D Level Generation with LLMs* (AIIDE):
  LLM → constraint description → facility-layout optimisation (simulated annealing / GA / greedy) → navigability repair → agent simulation;
  reusable databases of rooms / facilities / mechanics; progression by topological order (key before lock).
- *Unexplored* (Dormans): levels as **cycles** (lock-and-key, shortcuts), not trees.
- Anthropic, *Writing effective tools for agents*: few consolidated tools, meaningful context, concise/detailed responses, errors that teach,
  evaluate with realistic tasks. *Code execution with MCP*: let the model batch work in code instead of many tool calls.
- MCP spec: Tasks (long-running, 2025-11-25), MCP Apps (interactive UI resources, official extension since 2026-01).

## 4. Architecture

```
Claude Desktop ─stdio─> level-mcp (Node/TypeScript; packaged as a Desktop extension)
                         ├─ reuses shell/src/model (pure TS: shapes, brushes, UV, texture groups, prefab expansion)
                         ├─ design engine: brief → beats/route → setting → playable spaces → population → checks
                         └─ WebSocket, as one more session ─> Core (validates, saves) ─> the open editor shows edits live
```

- **Why here:** Core already validates every document, keeps unknown fields (so a `design` key survives), and accepts several sessions.
  Shell model code has no browser dependencies. A C++ server would duplicate the shape/UV code.
- **Needed Core / Shell changes** (found in code):
  1. A `document_sync` from another session **resets the Shell's undo history** and is dropped while the user has an edit in flight
     (`shell/src/state/store.ts` `onMessage`). Remote edits must arrive labelled and land as **one undo step** ("AI: build east wing").
  2. Edits are last-writer-wins: add `baseRev` to `document_update`; on a stale base the MCP re-applies its (pure) operation on the newest document.
  3. **Player metrics** in `game_profile.json` (height, radius, eye, step, jump height/distance, crouch, speed, max slope, camera distance).
  4. A **role map** from the game's `.ent` classes to gameplay roles (drafted by the model from `entities.ent`, confirmed by the user).
  5. **Analysis in Core**: navmesh (Recast/Detour, zlib) from the surfaces Core already collects; BVH rays for visibility; light at player
     height from the light volume (stealth/horror).
  6. **Render requests**: a fixed-size render from a given camera, editor overlays off (subtractive volumes, grid, gizmos, **entity name
     labels** — the labels cannot be hidden today), returned as a real MCP image (not base64 text), small by default; from the open editor
     or a headless Shell (the e2e tests already drive one with Playwright).
- **Computer use / remote control** is the most expensive path (a screenshot per step); keep it for optional real playtests in the game.

## 5. Pipeline: path first, setting second

1. **Brief** — genre playbook, player metrics, the reference (image / sketch / words), the setting's building type.
2. **Beats and route (text, logic only, L1)** — an ordered sequence of moments: what the player sees and does, gates (keys/locks),
   encounters, 1–3 optional pockets, the end. Loops and shortcuts back (L6, *Unexplored*).
3. **Setting** — the building type's massing as backdrop (e.g. château: long main block, taller central pavilion with spire, wings,
   end pavilions, mansard roofs; hôtel particulier: U-plan around a cour d'honneur behind a street wall). Closed facades, glazed windows.
4. **Playable footprint (L6)** — only spaces on the route (and the chosen pockets) get interiors and openings; everything else stays a closed
   shell. Each route space gets its own identity (typology, size, height, light, view out) so the path is legible and nothing repeats.
5. **Route spaces** — built from typologies / set pieces (§7) with diegetic platforms (L7), fitted to the player metrics (L4/L5).
6. **Population** — role map → entity classes; keys, locks (`trigger_key_once` → `func_door`), triggers, end; enemies snapped to floors.
7. **Checks (§8)** — reachability per lock state, beat views (rays), metric checks; a small contact sheet of shots for the human.
8. **Iterate on directions (L8)** — variants first, then only what is asked.

## 6. The design layer (stored in the map under `design`)

```json
{ "brief": { "genre": "slasher", "setting": "chateau", "reference": "re-zero mansion (image)",
             "metrics": { "jump": 5.4, "hover": true, "dash": true, "camera": "far" } },
  "route": [ { "id": "gate", "role": "start" }, { "id": "allee", "role": "fight" }, { "id": "round_point", "role": "arena" },
             { "id": "rose_garden", "role": "key", "gives": "red" }, { "id": "conservatory", "role": "arena", "lock": "red" },
             { "id": "stair_hall", "role": "hero" }, { "id": "library", "role": "climb", "gives": "blue" },
             { "id": "ballroom", "role": "boss", "lock": "blue" }, { "id": "end", "role": "end" } ],
  "pockets": [ { "id": "loggia", "gives": "secret", "via": "climb" } ],
  "beats": [ { "from": "gate", "see": "central_pavilion", "why": "landmark on the axis" } ],
  "setting": { "type": "chateau", "main": { "length": 300, "floors": 5 }, "wings": { "floors": 4 } },
  "playable": ["allee", "round_point", "rose_garden", "conservatory", "stair_hall", "library", "ballroom"],
  "provenance": "every generated brush/entity records its route space, generator and seed" }
```

Rules: deterministic by seed; editing one space leaves the others as they were; hand edits win (the same reconcile idea as parametric shapes:
an object edited by hand is detached and never regenerated over).

## 7. Generators

**Setting / massing** (tool-side procedural architecture, L3): building types with parameters — bays, floors per part, pavilions and
projections, roofs from footprints (mansard / hip / spire), plinth, string courses, cornice, glazed windows with sills, entrances with
terrace and steps. Windows next to playable ground must be impassable (glazed) — found by L9.

**Route-space typologies** (each with composition rules and diegetic traversal, L7): entrance hall with imperial staircase, enfilade of
salons, long gallery, library with bookcase tiers and ladders, ballroom with musicians' gallery and stage, chapel, kitchen, conservatory,
courtyard, parterre, bosquet (garden room), allée, round point with fountain, pergola walk, fight pocket. One idea per space; vary size,
height, light and view between consecutive spaces.

**Connectors**: door / double door / arch, stair tower (switchback), ladder, drop (one-way), gate, locked door (`func_door` + `trigger_key_once`).

**Element sizes** (this game's far camera, ≈1.3–1.5× real, L5):

| Element | Size |
|---|---|
| Storey | 6.5 m (slab 0.5 m) |
| Interior door | 3.5 × 4.5 m; main doors 5 × 7 m |
| Window | 2.2 × 3.6 m (ground floor sill 1.0 m), bay 6 m, glazed |
| Corridor (only where on the route) | 6 m wide |
| Stair | riser 0.25 m, tread 0.4–0.6 m; switchback towers 12 × 14.5 m |
| Boundary (hedge / wall / fence) | ≥ 7.5 m (jump 5.4 m) |
| Platform steps for climbing | ≤ 4.5 m apart, horizontally adjacent |
| Overhead structures on paths | ≥ 11 m (camera) |

**Construction:** additive (walls with openings, slabs, roofs as convex hulls) for big buildings (L10); mass + carve (subtractive rooms with
`carve: {self: true, structural: false, nonStructural: false}` in worldspawn, interiors in `func_detail`) is fine for small ones.

## 8. Analysis and verification

- **Reachability per lock state** (prototype: `.claude/skills/level-blockout/scripts/reach.py`): solid columns on a 1 m grid (worldspawn
  adds − subtractors + `func_detail` + closed `func_door`s), floors where ≥ 1.8 m is free above, moves up ≤ jump height and down any height
  with head room. Run it with no keys, after each key, and report a table of checkpoints (start, each key, each locked area, end, pockets).
  Result for the v4 mansion: house closed without the red key; ballroom/end closed without the blue key; everything reachable with both.
- Snap enemies and items to the floor under them (same model).
- Beat checks: ray visibility from a named viewpoint to a target (prototype `Map.visible` in `mapgen.py`).
- Metric checks: boundaries vs jump, step heights, door sizes, camera clearance, open-area size (L6: no huge open spaces).
- Renders: few, small, annotated (meters, labels), as MCP images; contact sheets; the human judges (L2).

## 9. MCP surface (12 tools + resources + prompts)

| Tool | Does |
|---|---|
| `editor_status` | project, game profile, player metrics, role map, open level, missing setup (read-only) |
| `catalog` | search entity classes, textures, prefabs, building types, typologies, playbooks; optional contact sheet (read-only) |
| `level_read` | summaries at a chosen depth: whole level, route space, region, by class/tag; raw mesh only for one brush (read-only) |
| `design_plan` | set/patch brief, route, beats, gates, setting; validate logic (key before lock, loops); 2–3 variant thumbnails in one image |
| `design_build` | build setting + playable spaces + population for all or some route spaces; seed; returns a short build report |
| `edit` | batch of direct operations on a selection (shapes, walls, openings, transforms, textures, entities, prefabs, apply fixes); one transaction |
| `analyze` | reachability per lock state, beat views, metric checks; issues with fix ids (read-only) |
| `view` | small annotated renders (eye height or overview), several per image (read-only) |
| `history` | checkpoints, undo, diff, variants |
| `script` | sandboxed code that may only call these tools (Unreal's programmatic toolset idea), one transaction |
| `library_save` | save a space / building type / playbook for reuse (with the user's permission, like Unreal's AgentSkills) |
| `play` | save, export BSP (MCP Task with progress), launch the game |

Resources: playbooks, typology guides, element-size tables, the current design. Prompts: `new_level`, `variants`, `critique`, `from_reference`,
`setup_game`. Long tail of `edit` operations via `describe`-style progressive disclosure. Short readable ids, meters everywhere, compass words
(north = −Z, east = +X).

## 10. Credit efficiency

The server does the heavy work (building, solving, checking, simple repairs); the model spends tokens on decisions. Batch work in one call
(`design_build`, `edit` batches, `script`). Few tools with tight schemas. Text first; images small (~150–700 tokens each) and only when
useful. Diffs and issue lists, never whole documents. Generate interiors only for playable spaces (L6) — fewer brushes, fewer tokens.
Measured in this session: the 60k-token side-by-side test, the mansion iterations at roughly 5–15k tokens each.

## 11. This project's demo game (facts used by the prototypes)

- Classes in `core/assets/entities.ent`: `trigger_key_once` / `trigger_key_multi` (`requiredKeys` "red", target), `func_door` (`targetname`,
  `targetLocation` real3 — **semantics unknown, left "0 0 0"**, `time`), `triggerOnce` / `trigger` (target, onEnterAction), `logic_level_end`
  (`nextLevel`, `targetname`), `item_key_red|blue|green|secret|special`, `item_weapon_*`, `npc_human_axe` (sword), `npc_human_gun`,
  `npc_dog`, `npc_guard`, `npc_civilian` (+ `task_*`, `nav_point` patrols), `spawner`, `counter`, `trigger_ladder`, `destructible`,
  `area_restricted`, `snd_reverb_zone`, `music_player`, lights `light_point|spot|directional|rect|sky`. **No boss class.**
- Player (user, 2026-10-17): slasher; fast; jumps ~3× its height; hovers for a while; dashes; far third-person camera.

## 12. Next steps

1. **Validate L6/L7 before building the MCP**: rebuild the mansion path-first — one route of ~10–12 distinct spaces through a closed
   château (setting only), the hall as a real imperial staircase, diegetic climbing; verify with `reach.py`; let the user judge.
2. Decide where generators live (`shell/src/model/levelgen/` shared by the Shell and the MCP vs a separate `mcp/` package).
3. Spikes: Core render request with overlays and labels off; Recast navmesh in Core; labelled remote edits as one undo step.
4. Phases: (1) server + `level_read`/`edit`/`history`/`view` + Core `baseRev` and labelled undo; (2) design layer, building types,
   typologies, route-first build; (3) analysis in Core; (4) population + playbooks + role map; (5) script sandbox, variants UI (MCP Apps).
5. Build an evaluation set (~10–20 briefs across genres), scored by the checks and by token cost, reviewed by the user.

## 13. Session log (2026-10-17)

1. Survey of the editor → first design (graph → layout solver → generators → analysis; 12 tools). User: think in general directions.
2. Prior art: Unreal (Epic plugin tried hands-on), Unity, Roblox, Blender MCPs; Holodeck; Xu & Verbrugge; *Unexplored*.
3. User: text-first design gives boring/absurd results → revised to building types + beats + visual loop (L1).
4. Test 1 (in the editor, ~50k tokens): text-only map vs building type + beats + rays + shots. B read as a place; rays and shots caught
   three issues. User: general shape OK, interior/exterior child-like, scale wrong — "not about texturing" (L2, L3).
5. Designer-made kits proposed → rejected as tedious (L3). Novices dropped as a target (§1).
6. Artist workflow test with the user's reference (Roswaal mansion, Re:Zero) for a slasher: two variants (front approach / back approach),
   user picked A + back gardens; west walk turned into a garden path with pergola (raised to 11 m for the camera), alcoves, fight pocket;
   interior added (library tiers, ballroom gallery, loggia) (L4, L8).
7. "2–3× bigger, 4 floors, main building 5": uniform scale-up felt wrong → big by extent with human-scale parts (L5). v4: 300 m manor,
   every room built, gated path verified (L9, L10).
8. Final feedback: every room explorable is not fun, no clear path; the hall composition is absurd (L6, L7). Session stopped; this file written.

Sources: [Epic UE 5.8 MCP overview](https://explainx.ai/blog/unreal-engine-5-8-claude-codex-mcp-ai-integration-2026) ·
[Flopperam](https://www.flopperam.com/mcp) · [StraySpark guide](https://www.strayspark.studio/blog/mcp-unreal-engine-complete-guide-ai-level-design) ·
[ChiR24 Unreal_mcp](https://github.com/ChiR24/Unreal_mcp) · [Unity MCP](https://docs.unity3d.com/Packages/com.unity.ai.assistant@2.0/manual/unity-mcp-overview.html) ·
[Roblox Studio MCP](https://create.roblox.com/docs/en-us/studio/mcp.md) · [Holodeck](https://arxiv.org/abs/2312.09067) ·
[Constraint Is All You Need](https://www.pcgworkshop.com/archive/xu2025constraint.pdf) · [Database-driven 3D levels](https://ojs.aaai.org/index.php/AIIDE/article/download/36840/38978) ·
[Unexplored cyclic generation](https://www.gamedeveloper.com/design/unexplored-s-secret-cyclic-dungeon-generation-) ·
[Writing tools for agents](https://www.anthropic.com/engineering/writing-tools-for-agents) · [Code execution with MCP](https://www.anthropic.com/engineering/code-execution-with-mcp) ·
[MCP Tasks](https://modelcontextprotocol.net/specification/2025-11-25/basic/utilities/tasks) · [MCP Apps](https://www.thoughtworks.com/radar/platforms/mcp-apps)
