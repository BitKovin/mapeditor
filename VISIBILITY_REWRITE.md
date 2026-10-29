# Visibility rewrite (BSP export PVS) — implementation spec

Status: **approved design, not started** (decided with the user 2026-10-29). This file is the complete brief for the implementer. Read all of it
before writing code. When the work is done, fold the final description into DESIGN.md §12 ("Visibility: …" paragraph, currently at about line 1040)
and NEXT_STEPS.md (the "Visibility:" line, about line 401), then delete this file or mark it historical.

Scope: replace the visibility part of `compileBsp` (core/src/bsp_compile.cpp, section "cells (the leafs of an axis aligned tree) and sampled
visibility", about lines 1216-1469, plus the leaf-surface assignment and the visibility lump writer about 1656-1666). **The game engine
(F:\Git\SdlWasmEngine) must NOT be changed** — the output keeps the rBSP v2 format exactly (nodes, planes, leafs, leafsurfaces, leafbrushes,
visibility lump). The engine repo has uncommitted work of the user's: do not touch it, do not commit there.

Out of scope unless the user asks: engine changes, door/area portals, StaticMesh's 8-corner test (user: "good enough"), `PlayableVisibility`
(lighting's unseen-face pass, core/src/playable_visibility.cpp) — the user said it *could* read the new data later; ask before doing it.

---------------------------------------------------------------------------------------------------------------------------------------------

## 1. Decisions (the user's answers — do not re-litigate)

| Topic | Decision |
|---|---|
| Method | Ray sampling (hardware ray tracing) between uniform cube cells, **made far less aggressive** by: cells only inside playable zones, big walls splitting cells, testing every pair of cells directly, aiming rays through narrow openings, one ring of dilation through open boundaries. |
| False negatives | Must be avoided ("the engine's rendering also relies on the PVS"). Sampling cannot prove it, so every design choice below errs toward "visible". |
| Occluders | Everything baked into the static world: worldspawn, func_group, func_detail (opaque textures only; glass/cutout/translucent, sky, and moving brush entities never occlude — as today). |
| Playable zones | Cells exist only inside playable zones (`editor_playable_area` volumes; there can be several, far apart — important). **No margin** around zones (the engine already samples bounding-box corners). |
| Zone contact | Zones usually overlap or lie less than 1 m apart. A cell is active when it touches ANY zone grown by 0.5 m (`kZoneBridge`), so connected zones form one continuous area with no gaps of cluster 0 between them. |
| Outside the zones | "Always visible". NPCs and dynamic objects stay inside zones 99% of the time. |
| Background geometry | Geometry fully outside every zone: still **blocks rays**, never **splits cells**, and is **drawn from everywhere** (like the sky). |
| No playable area in the map | Whole level is one zone (bigger cells, like today). |
| Splitter walls | Only faces inside a playable zone whose (merged, coplanar) shape has no dimension under 2 m. Small detail never splits. |
| Dilation | One ring: a cluster also gets the visibility of the clusters it touches **with nothing blocking between them**. Never across a wall. |
| Narrow openings | **Only** detect openings and aim rays through them. Do NOT close/ignore small cracks. |
| Visibility table size | Up to **50 MB** per map. |
| Compute device | Hardware ray queries first; **GPU compute (software BVH in a compute shader) as fallback**; CPU (TriangleBvh) only as the last resort (machines without Vulkan, unit tests). |
| Compile time | Optimize, but under 10 s for visibility on lvl2 is fine. |
| Old system | Remove it (no option to keep the old algorithm). |
| Debug view | Wanted, as a later phase (section 9). |
| Regression map | lvl2: "the player can sometimes see enemies through one of the walls". A dedicated regression test comes later (the user will point at the spot). |

## 2. Why the current system fails (measured 2026-10-29 on lvl2)

lvl2 = `F:\Git\SdlWasmEngine\source\Games\ShooterGame\GameData\maps\lvl2.map.json`: world about 512 x 76 x 437 m; 131 worldspawn + 183 func_group +
1755 func_detail brushes, 22.4k faces, 1 prefab instance; ONE playable-area brush of about 189 x 7.5 x 238 m (≈337k m³, ~2% of the world box).

Export result: `clusters 3397, leafs 3500, visCell 17.46 m, visSeconds 2.39` (whole export 53 s).

- The cells are cubes over the whole world box. `maxClusters = 4096` forces the cell edge from 4 m up to **17.5 m**, so nearly every cell
  contains walls with sample points on both sides of them → it "sees" both sides. Angled walls cross many cells → leaks everywhere.
- The cell that a ray hits is marked visible (it reaches across the wall), and the final one-cell dilation (26 neighbours, through walls) gives
  a cell everything its neighbour behind the wall sees.
- Rays are omnidirectional from 16 random points per cell: the chance to hit a far, small cell falls with distance² → possible false negatives.
- "Slow" = accuracy cannot be bought: smaller cells over the whole world box explode in count and in rays.

## 3. Engine contract (read the engine code; it defines what the output must satisfy)

All in `F:\Git\SdlWasmEngine\source\Engine\BSP\Quake3Bsp.cpp` unless noted.

- `FindClusterAtPosition(p)` (≈2184): walks `nodes` from node 0: `dist = dot(p, plane.normal) - plane.dist; child = dist >= 0 ? children[0] : children[1]`;
  a negative child is leaf `-(c+1)`; returns `leaf.cluster`. **Arbitrary (non-axial) planes are fine.** Planes are in Quake units/axes.
- `IsClusterVisible(src, dst)` (≈2221): **returns true if either cluster is < 0** or out of range ("fail open"), else bit `dst` of row `src`.
- `RenderBSP(... useClusterVis ...)` (≈2324): the WORLD model (model 0) is drawn with `useClusterVis = true`: it iterates **all `leafs`**
  (the array, not the tree), skips leafs with `cluster < 0`, skips leafs whose cluster is not visible, and draws the faces listed in visible
  leafs (merged meshes: a merged batch is drawn if any of its faces is listed in a visible leaf). ⇒ **Every world face must be listed in at
  least one leaf with cluster ≥ 0, or it is never drawn.**
- `LinetraceLightmapColor` (≈2067): same pattern (iterates `leafs`, skips `cluster < 0` and not-visible ones, uses their faces).
- `BSPModelRef::IsBspVisible` (≈2946, brush entities = models ≥ 1): camera cluster < 0 → visible. Samples a 3x3x3 grid over the entity's bounds;
  **target points with cluster < 0 are SKIPPED** (treated as "inside a wall"); visible if any known target cluster is visible; if all were < 0 → visible.
- `StaticMesh::PrecalculateCameraVisible` (StaticMesh.cpp ≈150) and SkeletalMesh: 8 bounds corners; any visible → visible.
- AI (`AiPerception/Observer.cpp:40`, `SlasherGame/AI/AIController.cpp:358`, `NpcHumanBase.cpp:106`), particles, SimGame spawning: `IsClusterVisible` as an early-out.
- No `.prt` file is produced and none is needed.

### Consequence: the OUTSIDE cluster `O` (required, not optional)

"Outside the zones" must NOT be cluster -1: `BSPModelRef` skips -1 points, so a brush entity (door, lift) that straddles a zone border would be
culled when its in-zone part is hidden but its outside part is visible — a false negative. Therefore:

- Create one real cluster **O = cluster 0**. Its PVS row is all ones, and bit O is set in every row (everyone sees O, O sees everyone).
- Every point outside the active cells (section 4.2) resolves to a leaf with cluster O.
- Cluster -1 is used **only** for regions inside active cells that are entirely inside opaque solid (walls).
- Always-visible world faces (sky, background geometry, faces that leave the active cells, faces not covered by any clustered leaf) are listed
  in ONE extra leaf with cluster O. That leaf may be unreferenced by the tree (the engine iterates the `leafs` array); append it as the last leaf.
  Other O leafs that the tree reaches carry empty lists.

## 4. Algorithm

Work in **editor space (metres, Y up)** for everything geometric (LightSurface verts, OccluderMesh/TriangleBvh, SolidSet, PlayableArea are all in
editor metres). Convert only when writing: points `toQuake(p)`, directions `toQuakeDir(n)`, plane `n·p = d` becomes `toQuakeDir(n)·q = d * 32`
(`rbsp::kUnitsPerMeter`), bounds via `toQuake` of the 8 corners (min/max, floor/ceil to int). The old code mixed spaces (cells in Quake units,
BVH in metres) — do not copy that.

### 4.1 Inputs

- `occ`: occluder surfaces = the current filter (bsp_compile.cpp ≈1369-1377): LightSurfaces of model 0, `in.opaque(texture)`, not sky.
  Build `OccluderMesh` + `TriangleBvh` as now (`buildOccluders` with castShadows = {"worldspawn"} after renaming classname).
- `visSolids`: a SolidSet of **only** brushes whose every face is an occluder (static class worldspawn/func_group/func_detail, every face
  texture opaque, not sky, contents include the solid bit per `BspContentRules::forTexture`). Add `const SolidSet* visSolids` to `BspInput`
  and build it in `bsp_export.cpp` next to `statics` (≈line 93; `collectSolids` with a predicate — check its signature in derived.h; you may
  need a variant that filters by brush textures). Fallback: `in.solids` if null (tests).
  WHY: sample points are rejected inside solids and regions inside solids get cluster -1. `in.solids` also contains glass/water brushes; an
  enemy swimming in a water brush must not land in a region whose visibility was sampled only from the air part (false negative).
- `zones`: `in.playable` pieces (`PlayableArea::Piece{mn, mx, planes}`; planes: outward unit normal, offset; inside when `n·x <= d`).
  `PlayableArea::pieces_` is private — add a public `const std::vector<Piece>& pieces() const`. Empty/null area → one zone = world bounds
  (the same bounds the old code computes from model-0 surfaces and brushes, ≈1219-1228, in metres) grown by 1 m.

### 4.2 Active cells (sparse uniform grid)

- One global grid of cubes with edge `c`, origin snapped to a multiple of `c` (so cells of different zones line up).
- A cell is **active** when its box overlaps some zone piece: AABB overlap with `piece.mn/mx` AND for every piece plane the box corner most
  inside (`min over corners of n·x`) is `<= d`. (Conservative: may include a few cells that only touch — fine.)
- Store active cells sparsely (sorted vector of packed (i,j,k) + hash map to index). Never allocate the dense world grid: zones can be far apart.
- Cell size: start at `visCell` (default **2 m**), build cells + splits + solid classification, count clusters (non-solid regions + 1 for O);
  if `clusters > maxClusters` grow `c *= 1.15` and redo. `maxClusters = floor(sqrt(maxVisBytes * 8))` with `maxVisBytes = 50 MiB` → 20480
  (row bytes are `(N + 7) / 8`, keep `8 + N * ((N+7)/8) <= maxVisBytes`). lvl2 estimate: ~337k m³ → about 2.6-3 m cells.
  Do NOT redo the expensive solid sampling per attempt for unchanged cells if avoidable; at least make each attempt parallel (`parallelFor`).

### 4.3 Splitter walls

1. Candidates: `occ` pieces (LightSurface: `verts` convex CCW, `normal`) that overlap any zone (polygon AABB vs piece test as in 4.2 is enough).
   Background geometry (fully outside zones) is therefore never a splitter.
2. Group coplanar candidates: key = (normal quantized to 1e-4, d = n·v0 quantized to 1 mm), same facing. Inside a group, union-find pieces
   whose 2D bounds (in the plane's 2D frame) touch within 1 cm → components.
3. A component qualifies when the **minimum width of the convex hull of all its vertices (2D, rotating calipers) is ≥ `visSplitMinWidth`
   (default 2 m)**. (Merging coplanar pieces first matters: walls built from modular 1 m brushes must still qualify.)
4. For every active cell, the qualifying components that intersect the cell box (any piece of it: polygon-vs-box SAT) are its splitters,
   ordered by the area of the component's pieces inside the cell, descending. Cap at `visMaxSplitsPerCell` (default 8; skipping planes is
   always safe).
5. Split the cell recursively: region = convex polytope (cell box 6 half-spaces + accumulated split half-spaces; use `HalfSpace`,
   `convexWindings`, `tidyConvex` from bsp_compile.h). Take the next splitter plane; skip it if it does not cut the region or if either side
   would be under 1% of the cell volume. Splitting a cell by a whole plane is ALWAYS valid (finer partition), even where the wall polygon
   ends inside the cell — the two parts then just see each other around the wall end. Both faces of a wall produce two planes; the slab
   between them is inside solid → cluster -1.
6. Region classification: test sample points (27 stratified interior points + the region's vertices pulled 1 cm toward the centroid) with
   `visSolids->inside`. All inside → solid (cluster -1, no samples, no faces). Otherwise a cluster.

### 4.4 Tree (nodes, planes, leafs)

- Top level: kd tree over the active cells. Recurse with (index range box, list of active cells in it). Empty list → leaf with cluster O.
  Split on a cell boundary along the axis of largest extent of the list's cell-index range (median of the list on that axis). When the list
  is one cell but the range is larger, keep splitting to carve the cell out (≤ 6 planes). The single active cell → its split subtree (or one
  leaf if unsplit).
- Inside a cell: one node per split plane. Engine convention: `children[0]` = side where `n_q·q - d_q >= 0`. For a HalfSpace (inside `n·p <= d`)
  that means children[0] = the OUTSIDE (`n·p >= d`) part. Get this right; test with `clusterAt` (test_bsp.cpp:94, same walk as the engine).
- Keep the existing special case: Quake 3 needs at least one node (≈1304).
- Use the existing plane set (`planes.add(n, d)` in Quake units, pairs `i ^ 1` flipped).
- Leaf `mins/maxs`: region bounds in Quake units (O leafs: their kd box clamped to the world box).

### 4.5 Leaf surfaces / leaf brushes (model 0 only, as today)

- Sky faces → the O face leaf (replaces "every clustered leaf").
- Other model-0 surface S with bounds B (Quake units, grown by 1 unit):
  - if every grid cell overlapped by B is active, list S in every **clustered** (cluster ≥ 0) leaf whose region bounds overlap B;
  - otherwise (it leaves the zones / is background), or if it ended up in no clustered leaf → the O face leaf.
- Leaf brushes: same rule with brush bounds (the engine does not use them for culling, but keep the lump valid).
- Invariant (assert in tests): every model-0 surface is listed in at least one leaf with cluster ≥ 0 (O counts).

### 4.6 Sample points per cluster

`P = visSamplePool` (default 32) points per cluster, deterministic (use `hash01(cluster, k, axis)`, bsp_compile.cpp:98):
- half on the region's boundary: pick a boundary face polygon (from `convexWindings`) weighted by area, a random point on it, pulled 1 cm toward the
  region centroid; include the region's vertices pulled 1 cm inward first (corners matter most for from-region visibility);
- half in the interior (rejection sampling in the region bounds against its planes);
- reject points inside `visSolids`; retry up to 8·P attempts. A cluster with fewer valid points keeps what it has (≥1 guaranteed, else it would
  be solid).

### 4.7 Pair pass (the main pass, GPU)

For every unordered pair of clusters (A, B), A < B, both ≠ O: trace `M = visPairSegments` (default 16) segments from a sample of A to a sample of B,
indices `i = hash(A,B,m) % P`, `j = hash(B,A,m) % P`, stop at the first segment that hits nothing → set bit (A,B). Any-hit, terminate on first
hit, opaque, no back-face culling; segment `origin = p, dir = q - p, tmin = 0, tmax = 1`.
- Skip pairs already set by the aperture pass (run that first, 4.8) — or just OR both results.
- Cost: N(N-1)/2 · M rays for invisible pairs (visible ones exit early). lvl2 ≈ 12-20k clusters → 0.7-2·10^8 pairs → 1-3·10^9 rays: seconds on RT hardware.
- Output: a bit matrix on the GPU (N² bits ≤ 50 MB), `atomicOr` per 32-bit word, read back once.
- Windows TDR: a single submission must stay well under 2 s. Dispatch in tiles (e.g. 2D dispatch of 8x8 threads = (A,B), threads with A >= B
  return; submit rows of tiles in batches sized so one submission is ≈0.2-0.3 s; measure and adapt like `runParts`).

### 4.8 Aperture pass (narrow openings: cracks, slits, small holes)

Purpose: openings too narrow for random segments to thread (from sub-mm cracks between brushes up to `visApertureMax`, default 0.5 m).
1. **Edge points.** For every edge of every `occ` piece inside (or touching) a zone, at a spacing of 5 cm along the edge: let `n` be the piece
   normal, `e` the edge direction, `w = e × n` or `n × e` — whichever points away from the piece (in its plane, outward of the edge).
   Candidate `C = edgePoint + 0.5 mm * w - 1 cm * n` (just beyond the edge, 1 cm into the wall's depth).
   - `visSolids->inside(C)` → reject (an internal edge between pieces of the same face, or a brush continuing the wall).
   - Gap width: trace from C along `+w` up to `visApertureMax`: no hit → wide opening, reject (random pairs handle it). Hit at distance h →
     gap `g = h + 0.5 mm`.
   - Aperture point `M = C + (h / 2) * w` (centre of the gap), axis `w`, through-direction `n`, length direction `e`.
   - Deduplicate: keep one aperture point per 10 cm along an edge and drop points within 2 cm of an already kept one (spatial hash).
2. **Lines through each aperture point.** Directions `d(θ) = cos θ · n + sin θ · e` for θ stratified in (-89°, 89°) (default 128 angles),
   plus 64 directions in a cone of half-angle 30° around `n` jittered with a `w` component within ±g/0.1 (non-parallel crack walls, round holes).
   For each direction trace both ways from M (`+d` and `-d`, closest hit, tmax = world diagonal): the unoccluded line is `[M - t2·d, M + t1·d]`.
   Lines through the mid-plane of a crack stay inside the crack — that is why this finds what random sampling misses.
3. **Marking.** Collect the clusters the line passes on each side of M (3D DDA through the active grid — the old code ≈1417-1439 shows a DDA; inside
   a split cell locate the sub-leaves at the cell entry, the cell exit and both sides of every split-plane crossing; points outside active cells
   are O, ignore). Set bits for every (cluster on the − side) × (cluster on the + side), plus pairs within each side (they see each other along the
   same line). Do the tracing on the GPU (return t1, t2 per line) and the cluster walk + bit setting on the CPU (`parallelFor`, atomic OR on
   uint64 words), unless profiling says otherwise.
4. Stats: aperture points, lines.

### 4.9 Dilation (one ring, open boundaries only)

- Open-adjacency: regions (clusters) A and B whose bounds touch (within 1 cm) and that share boundary area: take A's boundary sample points that lie
  within 2 cm of B's bounds; for each, `q = p + 2 cm * (outward normal of that boundary face)`; if `q` is inside B (its half-spaces), not in
  `visSolids`, and the segment p→q hits nothing → A and B are open-adjacent. Across a splitter wall the segment hits the wall → not adjacent.
  Sub-leaves of the same split cell around a wall's end ARE adjacent (correct). (If B has no qualifying points, test a few points of B toward A.)
- Order: symmetrize (V |= Vᵀ) → dilate once (row(A) |= row(B) for every open-adjacent B, reading the pre-dilation rows) → symmetrize again
  (dilation breaks symmetry; the old code did not re-symmetrize — fix that). Set the diagonal. Set row O and column O to all ones.
- Symmetrize with 64x64 bit-block transposes, not the old O(N²) getBit/setBit double loop (≈1447-1449; at 20k clusters that is 4·10^8 calls).

### 4.10 Visibility lump

Unchanged format: `int32 numClusters, int32 bytesPerCluster = (N+7)/8`, rows of bits (bit c of row r = r may see c). Write from the uint64 matrix
directly (rows are contiguous; convert word → bytes little-endian), not bit by bit.

## 5. GPU implementation notes

- Existing Vulkan code: `core/src/gpu_bake.cpp` / `gpu_bake.h` (namespace `core::gpu`). It loads the Vulkan loader at run time, picks a device,
  builds BLAS/TLAS from an `OccluderMesh` for `VK_KHR_ray_query`, and has `Context`, `contextFor(useRT, validation)`, `allocBuffer`, a buffer pool.
  Add the visibility entry points there (same file or a new `gpu_vis.cpp` that gets access to the needed internals — `Context` etc. are in an
  anonymous namespace; moving shared helpers into an internal header is fine). Proposed API:
  ```cpp
  // one session: occluders resident (AS or software BVH), then many calls
  std::unique_ptr<VisSession> createVisSession(const OccluderMesh&, const std::vector<float>& bvhNodes /* for the compute fallback */, std::string& error);
  bool VisSession::pairs(const std::vector<float>& samples /* N*P*4 */, int N, int P, int M, std::vector<uint64_t>& bits /* in/out, N rows of words */);
  bool VisSession::traceLines(const std::vector<float>& origins, const std::vector<float>& dirs, std::vector<float>& tPos, std::vector<float>& tNeg);
  bool VisSession::segments(...)  // generic any-hit segment test, for dilation adjacency if useful
  const std::string& device() const;  // for stats: "<gpu name> (ray query)" / "(compute)"
  ```
- Shaders: `core/shaders/`, listed in `core/shaders/manifest.txt` (`name | source | target-env | defines`). One source `vis.comp` compiled twice:
  `vis_rq` (vulkan1.2, `RAY_QUERY`) and `vis_bvh` (vulkan1.2, software BVH traversal). CMake compiles with glslangValidator when found; otherwise
  checked-in SPIR-V in `core/src/prebuilt/core_shaders` is used and must match the source hash — **after changing a shader run
  `python3 tools/compile_shaders.py`** (in core/) to refresh the prebuilt headers, or configure fails on machines without the SDK.
- Software BVH for the compute fallback: `TriangleBvh` (core/include/core/raytrace.h) has a flat node array (`Node{mn[3], mx[3], left, count}`,
  children `left, left+1`, leaf triangles `[left, left+count)` in internal order) but it is private. Add a public accessor that exports nodes +
  reordered triangles for upload, and traverse it in the shader with a small stack (any-hit, segment). Look at how `lightmap_direct.comp`
  without `RAY_QUERY` behaves first — the existing non-RQ path may not trace at all.
- Check `makeContext` (gpu_bake.cpp ≈328-500): device selection currently fails with "no Vulkan device supports hardware ray queries" — verify
  that a context with `useRT = false` can be created on a device without RT (needed for the compute fallback).
- Fallback chain: ray query → compute BVH → CPU `TriangleBvh::occluded` with `parallelFor` (unit tests, no Vulkan). Record which one ran in stats
  and as an export warning when it is not the ray-query path.
- Determinism: all randomness from `hash01`; the same input must give byte-identical .bsp on the same device path. GPU and CPU may differ only
  where a segment grazes geometry within float precision — never test on such configurations.

## 6. Settings, stats, protocol

`BspSettings` (core/include/core/bsp_compile.h ≈97-101): remove `maxClusters`, `visSamples`, `visRays`; keep `visibility` (false = every cluster sees
every cluster, still with the new tree/O); new fields with comments in the file's style:
`visCell = 2.0` (m, initial edge, grows to fit the budget), `maxVisBytes = 50 << 20`, `visSplitMinWidth = 2.0` (m), `visMaxSplitsPerCell = 8`,
`visSamplePool = 32`, `visPairSegments = 16`, `visApertureMax = 0.5` (m).

Worldspawn keys parsed in `core/src/bsp_export.cpp` (≈196, next to `_vis_cell`): keep `_vis_cell`; add `_vis_max_mb` (1..512) and
`_vis_split_min` (m, 0.5..64). Same try/catch + warning pattern as the neighbours.

`BspOutput::Stats`: keep `leafs`, `clusters`, `visCell`, `visSeconds` (the Shell reads them: shell/src/net/protocol.ts:85/179,
shell/src/state/exportStatus.ts:13 prints "N vis cells of X m" — keep the names, the text still fits). Add `visSplits`, `visApertures`,
`visPairs`, `visBytes`, and a `visDevice` string; add them to the stats JSON in bsp_export.cpp:59. Unknown stat keys are ignored by the Shell.

## 7. Tests (core/tests/test_bsp.cpp; helpers `inputFor`, `twoRooms`, `clusterAt`, `sees` at lines 64-114)

Build: `cmake --build core/build` inside a vcvars64 environment (not `build_core.bat`: it runs the whole suite, and the CPU lighting tests are very
slow). Run only what you touched: `core/build/core_tests.exe -tc="*bsp*"`. The tests must pass on the CPU path (no GPU assumed); add one GPU test that
`SKIP`s/returns when no Vulkan device is available and otherwise compares GPU and CPU bits on a simple map.

Tests to write (the map documents live in the test file; build small rooms with `Brush::makeBox`; add an `editor_playable_area` entity where the
case needs a zone — check `collectPlayableArea` / entity_class.h for the class):
1. Keep/adapt "closed rooms do not see each other, a doorway lets them" (≈276).
2. **Angled wall**: two closed rooms separated by a 0.3 m wall at 45°, cell 4 m: for a grid of points on each side (≥ 0.5 m from the wall) no
   cluster on one side sees a cluster on the other; with a 1 m opening they do.
3. **Thin axis wall off the grid**: 0.1 m wall at x = 7.3 with 2 m cells: no leak.
4. **Crack**: two wall brushes with a 1 cm vertical slit between them (otherwise sealed): the rooms see each other (aperture pass). Same with a
   5 cm square hole. Without the gap they do not.
5. **Brute-force false-negative check (the key property)**: on a cluttered test map, 20k random point pairs inside the zone, outside `visSolids`,
   with an unoccluded segment (CPU BVH) ⇒ `sees(clusterAt(p), clusterAt(q))` must hold. Zero misses.
6. **Outside / O**: a point outside the zone → cluster 0; `sees(0, x)` and `sees(x, 0)` for every x.
7. **Faces**: every model-0 surface is in at least one leaf with cluster ≥ 0; a background face (outside the zone) and the sky are in the O face
   leaf; a face crossing the zone border is in the O face leaf.
8. **Multiple zones** far apart: active cells only inside both; leaf count ≈ active regions + a few O leafs (no dense grid).
9. **Splitter rule**: a 3x3 m wall in the zone splits its cells; a 1x1 m box does not; a big wall outside the zone does not.
10. **Dilation never crosses a wall**: clusters on both sides of a splitter wall with nothing else connecting them stay invisible to each other.
11. **Budget**: with a tiny `maxVisBytes` the cell grows and the lump fits.
12. **Determinism**: two compiles of the same input → identical bytes.
13. Engine convention: `clusterAt` on points just in front of / behind a split plane lands in the right sub-leaf.

lvl2 measurement (the real target):
```
core/build_release/bsp_export_tool.exe <GameData> maps/lvl2.map.json maps/zz.bsp --no-bake
```
with `<GameData> = F:\Git\SdlWasmEngine\source\Games\ShooterGame\GameData`, run from that folder. `--no-bake` skips the 40 s light bake. Delete
`maps/zz.bsp` and `maps/zz/` afterwards (never overwrite lvl2.bsp). Read `visSeconds`, `clusters`, `visCell`, `visDevice` from the printed JSON.
Also add an EXCLUDE_FROM_ALL tool (pattern: `bench_bake` in core/CMakeLists.txt ≈200) `vis_check <assets> <map>` that compiles without baking and
runs test 5 with 1M random pairs inside the zones, printing misses (must be 0) and the timings per pass. Targets: lvl2 visibility < 10 s on
the ray-query path, 0 misses.

## 8. Pitfalls (read before coding)

- Never produce cluster -1 for space where a camera, NPC or object can be outside walls — only for regions entirely inside opaque solid.
- Never cull world faces by accident: the engine draws model-0 faces ONLY through clustered leaf lists.
- Engine plane side: `>= 0` → children[0].
- Editor vs Quake space (section 4 intro). `toQuake` is a rotation times 32; normals use `toQuakeDir` (no scale).
- Sample points must never lie on an occluder surface (pull them 1 cm into the region; region faces that are splitter planes coincide with wall faces).
- Skipping a split, dropping an occluder, or adding a visible bit is always safe; the opposite never is. When unsure, mark visible.
- `visSolids`, not `in.solids`, for sample rejection and solid regions.
- lvl2 is big: keep memory bounded (sparse cells, N ≤ 20480, bit matrix ≤ 50 MB); no N² vectors of anything bigger than bits.
- Don't kill processes by name (the user's editor runs a Core too); stop only PIDs you started.
- Only what is in this spec; ask the user before anything else (engine, PlayableVisibility, UI beyond section 9).

## 9. Phases (do them in order, verify each before the next)

1. CPU end-to-end: zones → active cells → splitters → tree → O cluster → face lists → pair pass on CPU → dilation/symmetry → lump. Tests 1-3, 6-13.
2. Aperture pass (CPU tracing first). Tests 4 and 5.
3. GPU: `vis.comp` ray-query + compute-BVH variants, fallback chain, TDR-safe batching, GPU-vs-CPU test. lvl2 timing via `vis_check`.
4. Tune on lvl2 (cell size, M, P, aperture spacing) for < 10 s and 0 misses; update DESIGN.md §12 / NEXT_STEPS.md; remove dead code of the old system.
5. Debug view (the user approved it). Minimal version: the Core answers a request with the clusters of the last export — for a picked point:
   its cluster, the region bounds (or polygons) of every cluster visible from it, solid regions, O — and the Shell draws them as translucent boxes in
   the viewport (look at how an existing debug overlay is drawn, e.g. the light volume probe overlay, and how `export_bsp` / `export_result` flow
   through `core/include/core/protocol.h` and `shell/src/net/protocol.ts`). Agree the UI entry point with the user before building it.
6. (Only if the user asks) `PlayableVisibility` reading the new data.
