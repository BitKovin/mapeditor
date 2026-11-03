# Loading an exported level (OBJ + JSON + lightvol + lightmaps)

How to read what the editor's **OBJ exporter** writes (game profile `"exporter": "obj"`, or `bsp_export_tool <assets> maps/x.map.json maps/x.obj`),
and how to turn it into lit pixels. The geometry is trivial to load; the hard part is the light. Everything below is demonstrated, with real data,
by [`level_loader.cpp`](level_loader.cpp): one file, C++17, no dependencies (it even carries its own zlib inflate for the EXR pages), no 3D view; it
loads a level and prints the numbers a renderer would use.

```
g++ -std=c++17 -O2 level_loader.cpp -o level_loader            (MSVC: cl /std:c++17 /EHsc /O2 level_loader.cpp)
level_loader data/maps/demo_lit.obj ../../core/assets          (the 2nd argument, the game folder, only checks that the textures exist)
```

`data/maps/demo_lit.*` is a small level exported with the editor, so the example runs as it is.

## 1. What an export contains

For a map `maps/demo_lit.map.json` the exporter writes next to it:

| File | What | Read it with |
|---|---|---|
| `demo_lit.obj` | all geometry, **one object per model** | §3 |
| `demo_lit.json` | the entities (array of flat objects, every value a string) | §6 |
| `demo_lit.lightvol` | light grid for things that move (binary, absent without a bake) | §5 |
| `demo_lit/lm_0000.exr`, `lm_0001.exr`, ... | lightmap pages, **in a folder named like the map** | §4 |
| textures | NOT exported: they stay in the game folder, named by the editor | §7 |

Conventions, for all of it: **metres, Y up, right handed, triangles counter-clockwise** seen from the front. Entity origins use the same space as the vertices.

## 2. The idea in one paragraph

Static surfaces (`worldspawn`, `func_group`, `func_detail`) are lit by **lightmaps**: the `vl` coordinate of every vertex says where in a page the light of that
point is stored. Things that move (doors, lifts, anything that cannot own lightmap area) have **no lightmap**; they are lit from the **light volume**, a grid of
tiny directional light probes. Final colour of a lightmapped pixel is `albedo x light x exposure + brightness` (§4.3); of a moving thing
`albedo x (ambient + directed x max(0, N.L)) x exposure + brightness`. All light values are **linear HDR** and may exceed 1.

## 3. The OBJ (`loadObj`)

```
o world                        one object per model: "world", then "model_1", "model_2", ...
g dev/wall@lm_0000             one group per (texture, lightmap page): <texture>@lm_<page>
usemtl dev/wall                the texture name again (there is no .mtl)
v  x y z                       position
vt u v                         texture coordinate, V up (the usual OBJ way; flip V if your texture origin is top left)
vn x y z                       normal (smoothing included)
vl u v                         NON STANDARD: lightmap coordinate in the page, see §4.1. Other OBJ readers skip the line.
f a/a/a b/b/b c/c/c           triangle, counter-clockwise
```

Things that are different from a general OBJ and that make loading simple:

* `v`, `vt`, `vn` and `vl` are written **together, one each per vertex**, so `f a/a/a` is one vertex index. No index remapping is needed; the example
  reads the four lists into one `Vertex` array and refuses a file where the three numbers of a corner differ.
* Faces are already triangles.
* A group name without `@lm_...` (`g dev/metal`) is a surface **with no lightmap**: light it from the light volume (§5).
* The object `world` is the static level. `model_N` is the geometry of the brush entity with `"model": "*N"` (§6). It is stored **in world space, at the
  position the map has it**, and its entity's `origin` is the pivot to move / turn it about.

## 4. Lightmaps

### 4.1 Pages and the `vl` coordinate

* Page `K` is the file `<mapname>/lm_KKKK.exr` (4 digits). The group name carries `K`. All pages are square (1024 x 1024 here).
* `vl` is a **0..1 coordinate over the whole page**. `(0, 0)` is the **first row of the file**, the top-left corner of the picture. (Not flipped like `vt`:
  if you upload the page to a GPU texture in file order, use `vl` as it is; if your API puts row 0 at the bottom, flip V.)
* Texel `i` has its centre at `(i + 0.5) / size`, as always.

### 4.2 The EXR file (`loadExr`)

OpenEXR, scanline, **RGB half floats**, **ZIP** compression (16 rows per block), linear, no alpha. Use tinyexr / OpenEXR / Unreal / any engine's loader if you
have one. If you do not, the example shows the whole format in ~60 lines:

1. header attributes until an empty name: `channels`, `compression`, `dataWindow` are all that is needed;
2. an offset table, one 64 bit offset per block of 16 rows;
3. a block = `int32 firstRow`, `int32 size`, zlib data. Inflate it, then undo EXR's byte filter (running sum of bytes - 128, then interleave the first half
   with the second half), then the data is: for each row, for each channel in alphabetical order (**B, G, R**), a full row of halfs.

Pages may also arrive as `.jpg` from the rBSP exporter, **never** from the OBJ exporter (it switches that off: it writes plain `.exr`).

### 4.3 Sampling and shading

```cpp
Vec3 light = sampleLightmap(pages[page], vl.u, vl.v);        // linear HDR, may be > 1
Vec3 colour = albedo * light * exposure + brightness;        // albedo = texture pixel
```

* **Multiply albedo and light as they are**. There is **no sRGB <-> linear conversion** anywhere in the pipeline: the editor multiplies in the texture's own
  space and so should you. Only convert once at the very end if your display needs it.
* A texel value is irradiance with pi absorbed: you do **not** divide by pi, and no extra brightness constant is needed.
* Sample with a **cubic B-spline** (4 x 4 texels, `sampleLightmap`; on a GPU it is four bilinear fetches). The editor and the game do. Plain bilinear works but
  shows creases in soft shadows. The baker leaves a 2 texel gutter around every chart for exactly this reach, so neighbouring charts do not bleed.
* `exposure` / `brightness` are the worldspawn keys `_cc_exposure` (default 1) and `_cc_brightness` (default 0): display colour correction chosen by the level
  designer (§6). Apply them once, after multiplying.
* Interpolate `vl` across the triangle like any texture coordinate and sample **per pixel**. Do not sample per vertex: charts are rectangles in a
  page, a lightmap is not vertex colour.
* Set the page's wrap mode to **clamp**; empty page area is black.

### 4.4 Light styles (flickering lights): what the OBJ does and does not carry

A light with a `style` (1..63) is baked into its own lightmap layer. In an rBSP export a surface lists up to 8 layers; **the OBJ only has the base layer**:
the `vl` and `@lm_` of a group always mean style 0. The extra pages (`lm_0001.exr` in the demo: the style 6 lamp) are written for completeness but nothing in the
OBJ tells where a chart lies in them. So:

* static lightmaps: style 0 only (a styled light's contribution is missing from walls, in the OBJ);
* the **light volume does carry every style** as its own layer, so moving things do flicker correctly (§5).

If you need animated static light, use the rBSP exporter (it has the slots) instead.

## 5. The light volume (`LightVolume`)

Binary, little endian:

```
char[4] "LVOL"   uint32 version = 1
float origin[3]  float cell               world position (m) of probe (0,0,0), grid spacing (m)
int32 nx, ny, nz, styleCount
styleCount x { int32 style;  nx*ny*nz probes of 9 bytes }      probe (x,y,z) at origin + (x,y,z)*cell, x fastest, then y, then z
probe = ambient rgb, directed rgb, lat, lng, flags             (9 bytes)
```

* colour byte -> linear value: `byte / 255 * 2` (range 0..2, same linear space as the lightmaps);
* direction **towards** the light: `lat = byte/255*pi`, `lng = byte/255*2pi - pi`, `dir = (sin lat cos lng, cos lat, sin lat sin lng)`, Y up;
* `flags` bit 0 = valid. An invalid probe lies inside a wall; its numbers are only a fill, so skip it when interpolating (`sample` does);
* style layers share the grid. Style 0 is the base light.

Light on a surface with normal `N` at point `p`:

```
for each style layer:  k = styleValue(style, time)       (style 0: 1)
                       light += k * (ambient + directed * max(0, dot(N, dir)))     // after trilinear interpolation of the 8 probes around p
colour = albedo * light * exposure + brightness
```

Styles are looping patterns, one letter per 0.1 s: `a` = 0, `m` = 1, `z` = 25/12 (`styleValue`). The default table (style 1 = `mmnmmommommnonmmonqnmmo`, 2 = `abcdefghij...`
and so on, 0..11) is the engine's; a game can override entries in `game_profile.json` under `lighting.lightStyles` (`{ "12": "mmmaaa" }`).

Use it for: every `model_N` group without a lightmap, and every surface without `@lm_`. Pick a point on or near the thing (its bounds' centre is a good
cheap choice; per pixel or per vertex with the real normal is better). The grid only covers the level's play area: outside it the example clamps to the edge.

## 6. Entities (`loadEntities`)

`demo_lit.json`: an array, **worldspawn first**, every key and every value a string.

```json
{ "classname": "info_player_start", "origin": "3.55534 0.75 2.5", "angles": "0 -135 0" }
{ "classname": "func_door", "model": "*2", "origin": "3.28206 1.5 -0.21794" }
```

* Numbers are text: parse them (`"x y z"`, `"pitch yaw roll"` in degrees) yourself.
* `origin` / `angles` are in the geometry's own space (metres, Y up), no unit conversion.
* **`"model": "*N"` links an entity to the object `model_N`.** Its vertices are in world space; to move it by `newOrigin` add `newOrigin - origin`; to rotate,
  rotate about `origin`. `worldspawn` is object `world`.
* `target` / `targetname` are plain strings; the entity with the matching `targetname` is the target (several can share a name).
* Lights are entities too (`light_point` ... with `color`, `brightness`, `radius`, `style`): **they are already baked**; a game that wants dynamic lights adds its own on top.
* `gridsize` is left out (the `.lightvol` has the grid). Game specific keys pass through as the designer wrote them.

## 7. Textures

Not exported. A group's texture is the editor's name, **relative to the game's texture root** (`game_profile.json` `"textureRoot"`, here `textures`):
`dev/wall` -> `<game folder>/textures/dev/wall.<ext>`, with the first extension of `"textureExtensions"` that exists (`.tga`, `.jpg`, `.png`). `none` = no
texture. Names without a file (sky shaders, `common/...`) are legitimate: draw them as the engine draws them. A texture may have a sidecar `<file>.json`
(transparency mode, ...: see DESIGN §9 / the editor's Texture panel); it is not needed to display a level.

## 8. Checklist of surprises

* Lightmap `vl` is top-left origin, texture `vt` is bottom-left origin. Only `vt` is flipped relative to the file.
* Lightmaps hold **HDR, linear** values: keep the page in a float (RGBA16F) texture; an 8 bit upload clips and bands the dark areas.
* Do not multiply light by anything else (no pi, no 2, no sRGB curve). The light volume's x2 range is handled when decoding bytes (`/255*2`).
* Surfaces without a lightmap exist inside `world` too (small / vertex lit charts, misc models): the exporter has no vertex colour, so the light volume
  is the only light information for them.
* Pages above the highest `@lm_` number are style layers, not extra base pages (§4.4).
* Vertices are not shared between surfaces (hard edges and chart borders need their own normals / `vl`): do not weld by position.
* Everything read back is exactly what the game gets: texture layouts, reliefs, decals, buried faces already dropped. Do not rebuild anything.
