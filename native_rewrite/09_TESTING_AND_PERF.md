# 09 — Testing, parity and performance gates

## 1. Test maps

| Map | Where | Use |
|---|---|---|
| `demo.map.json`, `demo_lit.map.json`, `engine_test.map.json`, `sky_test.map.json` | `core/assets/maps/` | unit and parity tests, screenshots |
| prefabs `door_frame`, `hinge` | `core/assets/prefabs/` | prefab tests |
| generated rooms | `core/bench/bench_bake.cpp --rooms N` builds them | lighting parity, perf without external files |
| **lvl2** (~512 × 76 × 437 m, 2 082 brushes, 25 MB, auto sky box, `_vis_cell 1.5 2.5 1.5`) | `F:\Git\SdlWasmEngine\source\Games\ShooterGame\GameData\maps\lvl2.map.json`, assets root `...\GameData`, lighting config `...\GameData\lighting.json` | performance gates, large-map behaviour. **Read only.** |

Outputs (exports, screenshots, dumps) go to a scratch folder, never into the repo or the game folder; delete them when done.

## 2. Export parity (for every task that changes Core code)

1. Before the change: build `bsp_export_tool` and run, into a scratch folder,
   `bsp_export_tool <assets> maps/demo_lit.map.json <scratch>/before/demo_lit.bsp --no-bake` and the same for lvl2
   (`--lighting <GameData>/lighting.json`).
2. After the change: the same into `<scratch>/after/`.
3. The `.bsp` files and every other written file must be **byte-identical**.
4. When the change touches baking code (`lightbake`, `live_bake`, `lightmap_uv`, shaders): also
   `bench_bake <assets> --rooms 6 --dump <scratch>/x.bin` before and after; the dumps must be identical (the bench's
   documented contract).

## 3. Performance gates

Measured by the built-in benchmark mode (task T6.5 adds it):
`mapeditor_native --bench <scenario> --map <map> --assets <root> --bench-out <file.json>` replays a scripted edit with a
fixed camera and writes timings: per frame CPU / GPU ms, time until the visible chunks' direct light was updated, time until
visible chunks reached `maxPasses`, peak VRAM.

| Gate | Scenario | Pass condition (lvl2, RTX 4070 Ti, 1440p, 1 Lit viewport + 3 Unlit) |
|---|---|---|
| G-open | open lvl2 | first Unlit frame < 10 s; first Lit frame with direct light on visible chunks < 20 s |
| G-frame | orbit for 10 s, converged | ≥ 60 fps (p99 frame ≤ 20 ms) with 4 viewports |
| **G-light** | drag a point light (radius 10 m) across a room for 3 s, then stop | every frame during the drag ≤ 20 ms; visible direct light updated within **2 frames** of each move; after stopping, visible chunks at `maxPasses` (bounces included, after M7) within **2 s** |
| G-sun | change the directional light's angles by 10° | visible direct light updated ≤ 100 ms; visible converged ≤ 3 s |
| **G-brush** | move one wall brush 1 m in Lit (one step) and drag it for 3 s | geometry follows the mouse every frame; its own and neighbours' visible direct light updated ≤ 100 ms after the last move; visible converged ≤ 2 s; nothing outside the invalidation region changed (06 §7) |
| G-undo | undo the brush move | as G-brush; cache hits for the derived data (no CSG recompute, counted) |
| G-vram | lvl2 fully converged | ≤ 8 GB device memory used by the app (12 GB card) |

If a gate fails after the task that should meet it, profile first (Stats panel, `--trace`), report the numbers, and ask
the user before adding anything from M12.

## 4. Lighting parity tests (`native_tests [gpu][lighting]`)

- Harness `native/tests/light_parity.cpp`: builds a scene from a map, runs `core::LiveBaker` (GPU session of the Core)
  for N passes with a fixed schedule, and the native bake for the same N passes; compares per texel by world position
  (placement differs) with the bounds of 06 §1.
- Scenes: demo_lit; rooms(6); a scene per light type (point, spot, rect, directional, sky with and without skybox, dark
  light); a cutout + translucent scene (`test_transparency.cpp` cases as maps); a smooth-edge cylinder; a relief wall; a
  styled light.
- Invalidation tests (T6.6, T7.6) compare buffers bit-exactly outside the region.

## 5. What not to run

The full `core_tests.exe` (the CPU lighting tests take very long). Always pass `-tc` with the cases a task names.
