# Level Editor

Brush-based level editor: a **Shell** (TypeScript, browser) that does all interactive
editing, and a **Core** (C++20, native) that does what a browser can't -- filesystem,
validation, derived geometry and lighting. `DESIGN.md` is the source of truth for what this
project is and what's left to do; read it first.

## Quick start (Windows)

Needs Visual Studio 2019/2022 (or Build Tools) with the **Desktop development with C++** workload (it
ships CMake and Ninja) and **Node.js 20+**. Nothing else: third-party C++ code is vendored in `core/vendored/`.

    build_core.bat       :: Core: CMake + Ninja, runs the unit tests
    build_shell.bat      :: Shell: npm install, typecheck, tests, build
    run.bat              :: starts Core + the Shell dev server and opens the editor

`NEXT_STEPS.md` says what to do next.

In the editor: the **fly camera** is the default (hold RMB and use W A S D Q E; switch to Orbit in the viewport header),
**⚙** opens Settings (assets folder), Open / Save As use a file browser, and subtractive brushes carve non-destructively.

## Core

Needs only a C++20 compiler, CMake >= 3.20 and Ninja (no system libraries):

    cmake -S core -B core/build -G Ninja && cmake --build core/build
    core/build/core_tests

Run (from `core/`; args: `[port] [assetsRoot] [initialMap]`, all optional: without them Core uses the assets folder chosen in
the editor's Settings, stored in `settings.json` next to the executable, else `assets`):

    ./build/mapeditor_core 8787 assets maps/demo.map.json

End-to-end smoke test against a running server (needs `pip install websockets`):

    python3 smoke_test.py

Core only accepts WebSocket connections from `localhost`/`127.0.0.1` origins (or no Origin
at all, for tools), binds to 127.0.0.1, and sandboxes every path to the assets root.

## Importing Quake maps

File > Import Quake Map... converts a Quake 3 `.map` from the game folder (brushes in every syntax, curved patches, entities, lights) to `<name>.map.json` beside it and opens it; `core/build/map_import_tool <gameFolder> <in.map> [out.map.json]` does the same without the editor. What is converted and how: DESIGN.md, "Importer: Quake 3 .map".

## Demo assets

`core/assets/gen_demo.py` regenerates `maps/` and `prefabs/` (meters, Y-up, schema 0.2); `gen_textures.py` the demo textures; `game_profile.json` is the demo game profile and `entities.ent` a small stand-in for a Quake 3 entity definition file.

## Shell

    cd shell && npm install
    npm run dev          # http://localhost:5173  (start Core first; ?core=ws://host:port overrides its address)
    npm test             # unit tests (model, geometry + shared vectors, UV, TGA, drawing math, prefabs, store, connection)
    npm run typecheck

Controls: `B` box tool (drag on a surface, release, move for height, click; **Shift** = 1:1:1; Esc cancels) ·
`Q` select (drag the gizmo arrows) · `Tab` face mode · double-click a texture in the Content Browser to apply it ·
RMB orbit · MMB pan · wheel zoom · `F` frame · `Del` delete · Ctrl+Z / Ctrl+Y undo/redo · Ctrl+S save. Full list: DESIGN.md §17. "+ Viewport" adds a viewport (max 10);
drag a tab out of the browser window (or press the ⧉ button in a group's header) to open it in its own window for another monitor. Prefabs and entities in the Place tab can be clicked or dragged into a viewport; a half-transparent preview shows where they will land.

Browser end-to-end test (real Chromium, real Core; needs `pip install playwright websockets pillow`
and `playwright install chromium`): with Core and `npm run dev` running, `python3 e2e/e2e.py`.
