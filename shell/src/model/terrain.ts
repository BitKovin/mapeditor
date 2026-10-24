// Terrain (REQUIREMENTS §39, DESIGN §8.k). A terrain is ONE func_detail entity holding a grid of closed "slab" brushes, so the Core's validation, lightmapping and rBSP
// export see ordinary brushes and need nothing new. The entity carries a small record `terrain = { cols, rows, cell, tile }`; every tile brush carries
// `terrainTile = [tx, ty]`. There is no second copy of the heights: they ARE the y of the top vertices of the tiles, read back by `terrainOf` (cached per entity).
//
// A tile covers up to `tile` x `tile` cells and is a closed manifold made only of planar faces:
//   vertices  the (w+1)(d+1) top grid points (row-major along x), then the 2(w+d) points of the bottom ring, all at `base` (the bottom plane)
//   faces     2 triangles per cell (the diagonal alternates like a chessboard, so no direction shows), one quad wall per cell edge on the tile border
//             (planar: it lies in a vertical plane), and one bottom polygon. Walls and bottom are 'common/caulk' (nodraw), the top keeps the painted texture.
//   smooth    every edge between two top triangles of the tile (face id pairs, DESIGN §5.1a), so the surface lights as one. Smoothing never crosses brushes,
//             so the shading has a faint seam where two tiles meet (known limit).
// Face ids and the triangulation never change when sculpting, so sculpting only replaces a tile's `vertices` array (and texture painting only some `texture`
// strings): untouched tiles stay the very same objects (cheap undo, GPU caches keyed by brush, and Core's per-face change detection sees only what moved).
//
// Everything here is pure; the Shell's viewport and the Terrain panel only call it.
import type { Brush, Entity, Face, MapDocument } from './document'
import { Vec3 } from './math'

export interface TerrainSpec { cols: number; rows: number; cell: number; tile: number }
export type TerrainTool = 'raise' | 'lower' | 'smooth' | 'flatten' | 'paint'
export const TERRAIN_TOOLS: TerrainTool[] = ['raise', 'lower', 'smooth', 'flatten', 'paint']

/** Walls and the underside: not drawn by the game (REQUIREMENTS: the importer's slabs use the same texture). */
export const TERRAIN_SIDE_TEXTURE = 'common/caulk'
/** The top never goes closer than this to the bottom plane (the walls would collapse). */
export const MIN_THICKNESS = 0.25
export const MAX_CELLS_PER_SIDE = 512
export const MAX_CELLS = 65536
/** Raise / Lower move the ground by this many metres per stamp at strength 1. */
export const RAISE_PER_STRENGTH = 0.5

/** Heights are kept to 0.1 mm, as the shortest decimal (so the document does not fill up with 0.30000000000000004). */
const q4 = (v: number): number => Math.round(v * 1e4) / 1e4
const clamp = (v: number, lo: number, hi: number): number => (v < lo ? lo : v > hi ? hi : v)

/** The heights of a terrain, plus where it sits: the grid point (gx, gz) is at (ox + gx cell, h[gz (cols + 1) + gx], oz + gz cell); the bottom plane is y = base. */
export interface Heightfield { spec: TerrainSpec; ox: number; oz: number; base: number; h: Float64Array }

export const tilesAcross = (s: TerrainSpec): number => Math.ceil(s.cols / s.tile)
export const tilesDown = (s: TerrainSpec): number => Math.ceil(s.rows / s.tile)
const stride = (s: TerrainSpec): number => s.cols + 1

/** Cells covered by tile (tx, ty): first cell and size. */
export function tileCells(s: TerrainSpec, tx: number, ty: number): { x0: number; z0: number; w: number; d: number } {
  const x0 = tx * s.tile, z0 = ty * s.tile
  return { x0, z0, w: Math.min(s.tile, s.cols - x0), d: Math.min(s.tile, s.rows - z0) }
}

export function specOf(e: Entity): TerrainSpec | null {
  const t = e.terrain as Partial<TerrainSpec> | undefined
  if (!t || typeof t !== 'object') return null
  const { cols, rows, cell, tile } = t
  if (!Number.isInteger(cols) || !Number.isInteger(rows) || !Number.isInteger(tile) || typeof cell !== 'number' || !Number.isFinite(cell)) return null
  if (cols! < 1 || rows! < 1 || cols! > MAX_CELLS_PER_SIDE || rows! > MAX_CELLS_PER_SIDE || tile! < 1 || cell <= 0) return null
  return { cols: cols!, rows: rows!, cell, tile: tile! }
}

export function tileOf(b: Brush): [number, number] | null {
  const t = b.terrainTile
  return Array.isArray(t) && t.length === 2 && Number.isInteger(t[0]) && Number.isInteger(t[1]) && t[0] >= 0 && t[1] >= 0 ? [t[0] as number, t[1] as number] : null
}

// ---- the shape of one tile ---------------------------------------------------------------------------------------------------------------------------
/** The point k of the bottom ring (k = 0 .. 2 (w + d) - 1) as a grid point of the tile; the ring runs +x along z = 0, +z along x = w, -x along z = d, -z along x = 0. */
function ringPoint(w: number, d: number, k: number): [number, number] {
  if (k < w) return [k, 0]
  if (k < w + d) return [w, k - w]
  if (k < 2 * w + d) return [w - (k - w - d), d]
  return [0, d - (k - 2 * w - d)]
}

/** Vertices of tile (tx, ty): the top grid at the heights `height(gx, gz)` gives, then the bottom ring. */
function tileVerticesWith(s: TerrainSpec, ox: number, oz: number, base: number, tx: number, ty: number, height: (gx: number, gz: number) => number): Vec3[] {
  const { x0, z0, w, d } = tileCells(s, tx, ty)
  const out: Vec3[] = []
  for (let j = 0; j <= d; j++)
    for (let i = 0; i <= w; i++) out.push([ox + (x0 + i) * s.cell, height(x0 + i, z0 + j), oz + (z0 + j) * s.cell])
  for (let k = 0; k < 2 * (w + d); k++) {
    const [i, j] = ringPoint(w, d, k)
    out.push([ox + (x0 + i) * s.cell, base, oz + (z0 + j) * s.cell])
  }
  return out
}

/** Vertices of tile (tx, ty) from the heights. */
export function tileVertices(hf: Heightfield, tx: number, ty: number): Vec3[] {
  const st = stride(hf.spec)
  return tileVerticesWith(hf.spec, hf.ox, hf.oz, hf.base, tx, ty, (gx, gz) => hf.h[gz * st + gx])
}

const faceCount = (w: number, d: number): number => 2 * w * d + 2 * (w + d) + 1

/** Faces of a tile (w x d cells at global cell offset x0, z0): the triangulation is fixed by the cell's chessboard parity. Also the smooth pairs between the top triangles. */
function tileFaces(x0: number, z0: number, w: number, d: number, top: string): { faces: Face[]; smooth: [number, number][] } {
  const g = (i: number, j: number): number => j * (w + 1) + i
  const T0 = (w + 1) * (d + 1), P = 2 * (w + d)
  const faces: Face[] = []
  const edgeFace = new Map<string, number>(), smooth: [number, number][] = []
  const addTop = (id: number, a: number, b: number, c: number): void => {
    faces.push({ id, loop: [a, b, c], texture: top })
    for (const [p, q] of [[a, b], [b, c], [c, a]]) {
      const k = p < q ? `${p}|${q}` : `${q}|${p}`, other = edgeFace.get(k)
      if (other === undefined) edgeFace.set(k, id); else smooth.push([other, id])
    }
  }
  for (let j = 0; j < d; j++)
    for (let i = 0; i < w; i++) {
      const c = j * w + i, p00 = g(i, j), p10 = g(i + 1, j), p01 = g(i, j + 1), p11 = g(i + 1, j + 1)
      if ((x0 + i + z0 + j) % 2 === 0) { addTop(2 * c, p00, p01, p11); addTop(2 * c + 1, p00, p11, p10) }
      else { addTop(2 * c, p00, p01, p10); addTop(2 * c + 1, p01, p11, p10) }
    }
  const W0 = 2 * w * d
  for (let k = 0; k < P; k++) {
    const [ai, aj] = ringPoint(w, d, k), [bi, bj] = ringPoint(w, d, (k + 1) % P)
    faces.push({ id: W0 + k, loop: [g(ai, aj), g(bi, bj), T0 + ((k + 1) % P), T0 + k], texture: TERRAIN_SIDE_TEXTURE })
  }
  faces.push({ id: W0 + P, loop: Array.from({ length: P }, (_, k) => T0 + k), texture: TERRAIN_SIDE_TEXTURE })
  smooth.sort((a, b) => a[0] - b[0] || a[1] - b[1])
  return { faces, smooth }
}

/** Brushes made by the functions in this file: reconcile does not need to re-check them. */
const TRUSTED = new WeakSet<Brush>()
const trust = (b: Brush): Brush => { TRUSTED.add(b); return b }

export function buildTile(hf: Heightfield, tx: number, ty: number, id: string, top: string): Brush {
  const { x0, z0, w, d } = tileCells(hf.spec, tx, ty)
  const { faces, smooth } = tileFaces(x0, z0, w, d, top)
  return trust({ id, op: 'add', vertices: tileVertices(hf, tx, ty), faces, ...(smooth.length ? { smooth } : {}), terrainTile: [tx, ty] })
}

// ---- reading a terrain back ----------------------------------------------------------------------------------------------------------------------------
const EPS = 1e-6
/** Does the brush hold exactly the tile (tx, ty) these vertices describe (positions, vertex and face count)? */
function matches(b: Brush, want: Vec3[], s: TerrainSpec, tx: number, ty: number): boolean {
  const { w, d } = tileCells(s, tx, ty)
  if (b.vertices.length !== want.length || b.faces.length !== faceCount(w, d)) return false
  for (let i = 0; i < want.length; i++) {
    const p = b.vertices[i], q = want[i]
    if (Math.abs(p[0] - q[0]) > EPS || Math.abs(p[1] - q[1]) > EPS || Math.abs(p[2] - q[2]) > EPS) return false
  }
  return true
}

const cache = new WeakMap<Entity, Heightfield | null>()

/** The terrain an entity holds, or null when it has no (valid) terrain record or its tiles no longer are what the record says. Cached per entity object; do not modify the result. */
export function terrainOf(e: Entity): Heightfield | null {
  if (cache.has(e)) return cache.get(e)!
  const r = readTerrain(e)
  cache.set(e, r)
  return r
}

function readTerrain(e: Entity): Heightfield | null {
  const spec = specOf(e)
  if (!spec) return null
  const nx = tilesAcross(spec), ny = tilesDown(spec)
  const tiles = new Array<Brush | undefined>(nx * ny)
  for (const b of e.brushes ?? []) {
    const t = tileOf(b)
    if (!t || t[0] >= nx || t[1] >= ny || tiles[t[1] * nx + t[0]]) { if (t) return null; continue }
    tiles[t[1] * nx + t[0]] = b
  }
  if (tiles.some((b) => !b)) return null
  const first = tiles[0]!, c = tileCells(spec, 0, 0), T0 = (c.w + 1) * (c.d + 1)
  const v0 = first.vertices[0], vb = first.vertices[T0]
  if (!v0 || !vb) return null
  const hf: Heightfield = { spec, ox: v0[0], oz: v0[2], base: vb[1], h: new Float64Array((spec.cols + 1) * (spec.rows + 1)) }
  const seen = new Uint8Array(hf.h.length), st = stride(spec)
  for (let ty = 0; ty < ny; ty++)
    for (let tx = 0; tx < nx; tx++) {
      const { x0, z0, w, d } = tileCells(spec, tx, ty), b = tiles[ty * nx + tx]!
      if (b.vertices.length < (w + 1) * (d + 1)) return null
      for (let j = 0; j <= d; j++)
        for (let i = 0; i <= w; i++) {
          const y = b.vertices[j * (w + 1) + i][1], at = (z0 + j) * st + x0 + i
          if (seen[at] && Math.abs(hf.h[at] - y) > EPS) return null // a seam that does not close
          hf.h[at] = y; seen[at] = 1
        }
      if (!matches(b, tileVertices(hf, tx, ty), spec, tx, ty)) return null
    }
  return hf
}

/** The texture of the cell (gi, gj)'s top faces (the first top face of the tile that holds it). */
export function cellTexture(e: Entity, spec: TerrainSpec, gi: number, gj: number): string | undefined {
  const tx = Math.floor(gi / spec.tile), ty = Math.floor(gj / spec.tile)
  const b = (e.brushes ?? []).find((x) => { const t = tileOf(x); return t && t[0] === tx && t[1] === ty })
  if (!b) return undefined
  const { x0, z0, w } = tileCells(spec, tx, ty)
  return b.faces[2 * ((gj - z0) * w + (gi - x0))]?.texture
}

export function cloneHeightfield(hf: Heightfield): Heightfield { return { ...hf, h: hf.h.slice() } }

// ---- keeping the record honest -----------------------------------------------------------------------------------------------------------------------
/**
 * Run by the store after every edit (`prev` -> `next`). A terrain stays a terrain while every tile brush that changed still is exactly the tile the record
 * says (a move of the whole entity keeps it: the grid's place is read from tile (0, 0)); otherwise (a mesh edit, a clip, one tile moved...) the record and the tile
 * marks are dropped and the tiles are ordinary brushes. Tile marks without a record (a copied tile, a brush moved to another entity) are dropped too.
 * Returns `next` itself when nothing had to change.
 */
export function reconcileTerrain(prev: MapDocument | null, next: MapDocument): MapDocument {
  let changed = false
  const entities = next.entities.map((e) => {
    const marked = (e.brushes ?? []).some((b) => b.terrainTile !== undefined)
    if (e.terrain === undefined && !marked) return e
    if (prev && prev.entities.includes(e)) return e
    const strip = (): Entity => {
      const { terrain: _t, ...rest } = e
      void _t
      return { ...rest, ...(e.brushes ? { brushes: e.brushes.map((b) => { if (b.terrainTile === undefined) return b; const { terrainTile: _m, ...r } = b; void _m; return r as Brush }) } : {}) }
    }
    if (!valid(e, prev?.entities.find((p) => p.id === e.id))) { changed = true; return strip() }
    return e
  })
  return changed ? { ...next, entities } : next
}

/** y of the top vertex at the grid point (gx, gz) of tile (tx, ty)'s brush. */
function topAt(b: Brush, s: TerrainSpec, tx: number, ty: number, gx: number, gz: number): number {
  const { x0, z0, w } = tileCells(s, tx, ty)
  return b.vertices[(gz - z0) * (w + 1) + (gx - x0)][1]
}

function valid(e: Entity, before: Entity | undefined): boolean {
  const spec = specOf(e)
  if (!spec) return false
  const nx = tilesAcross(spec), ny = tilesDown(spec), seen = new Set<number>()
  const prevBrush = new Map((before?.brushes ?? []).map((b) => [b.id, b]))
  const tiles: [Brush, number, number][] = []
  for (const b of e.brushes ?? []) {
    const t = tileOf(b)
    if (b.terrainTile !== undefined && !t) return false
    if (!t) continue
    if (t[0] >= nx || t[1] >= ny || seen.has(t[1] * nx + t[0])) return false
    seen.add(t[1] * nx + t[0]); tiles.push([b, t[0], t[1]])
  }
  if (seen.size !== nx * ny) return false
  const first = tiles.find(([, tx, ty]) => tx === 0 && ty === 0)![0]
  const c = tileCells(spec, 0, 0), T0 = (c.w + 1) * (c.d + 1)
  const v0 = first.vertices[0], vb = first.vertices[T0]
  if (!v0 || !vb) return false
  const hf: Heightfield = { spec, ox: v0[0], oz: v0[2], base: vb[1], h: new Float64Array(0) }
  for (const [b, tx, ty] of tiles) {
    if (TRUSTED.has(b) || prevBrush.get(b.id) === b) continue
    // this tile changed: its own top heights define what it must look like
    const { w, d } = tileCells(spec, tx, ty), n = (w + 1) * (d + 1)
    if (b.vertices.length < n) return false
    const own = tileVerticesWith(spec, hf.ox, hf.oz, hf.base, tx, ty, (gx, gz) => b.vertices[(gz - ty * spec.tile) * (w + 1) + (gx - tx * spec.tile)][1])
    if (!matches(b, own, spec, tx, ty)) return false
    // the border it shares with a neighbour must still close
    for (const [nb, ntx, nty] of tiles) {
      if (Math.abs(ntx - tx) + Math.abs(nty - ty) !== 1) continue
      const a = tileCells(spec, tx, ty), c = tileCells(spec, ntx, nty)
      const gxs = ntx !== tx ? [(Math.max(tx, ntx)) * spec.tile] : null, gzs = nty !== ty ? [(Math.max(ty, nty)) * spec.tile] : null
      const from = gxs ? [gxs[0], Math.max(a.z0, c.z0)] : [Math.max(a.x0, c.x0), gzs![0]]
      const len = gxs ? Math.min(a.d, c.d) : Math.min(a.w, c.w)
      for (let k = 0; k <= len; k++) {
        const gx = from[0] + (gxs ? 0 : k), gz = from[1] + (gxs ? k : 0)
        if (Math.abs(topAt(b, spec, tx, ty, gx, gz) - topAt(nb, spec, ntx, nty, gx, gz)) > EPS) return false
      }
    }
  }
  return true
}

// ---- creating ----------------------------------------------------------------------------------------------------------------------------------------
export interface CreateParams {
  /** size of the terrain in metres */ sizeX: number; sizeZ: number
  /** edge of a cell in metres */ cell: number
  /** cells per tile side (a tile is one brush) */ tile: number
  /** the corner with the smallest x and z, at the height of the flat ground */ origin: Vec3
  /** depth of the slab below the ground in metres */ thickness: number
  texture: string
}
export const DEFAULT_CREATE: Omit<CreateParams, 'origin' | 'texture'> = { sizeX: 64, sizeZ: 64, cell: 1, tile: 16, thickness: 8 }

/** The grid a CreateParams describes, or why it cannot be made. */
export function gridFor(p: CreateParams): TerrainSpec | { error: string } {
  if (!(p.cell >= 0.05)) return { error: 'The cell size must be at least 0.05 m' }
  const cols = Math.round(p.sizeX / p.cell), rows = Math.round(p.sizeZ / p.cell)
  if (cols < 1 || rows < 1) return { error: 'The terrain must be at least one cell big' }
  if (cols > MAX_CELLS_PER_SIDE || rows > MAX_CELLS_PER_SIDE || cols * rows > MAX_CELLS) return { error: `Too many cells (${cols} x ${rows}): at most ${MAX_CELLS} cells and ${MAX_CELLS_PER_SIDE} per side; use a bigger cell` }
  const tile = Math.round(p.tile)
  if (!(tile >= 2 && tile <= 64)) return { error: 'A tile is 2 to 64 cells wide' }
  if (!(p.thickness >= 1)) return { error: 'The slab needs a thickness of at least 1 m' }
  return { cols, rows, cell: p.cell, tile }
}

/** A flat terrain as a func_detail entity (`entityId`), its tile brushes get ids from `newBrushId`. */
export function createTerrain(p: CreateParams, entityId: string, newBrushId: () => string): { entity: Entity } | { error: string } {
  const spec = gridFor(p)
  if ('error' in spec) return spec
  const hf: Heightfield = { spec, ox: p.origin[0], oz: p.origin[2], base: p.origin[1] - p.thickness, h: new Float64Array((spec.cols + 1) * (spec.rows + 1)).fill(p.origin[1]) }
  const brushes: Brush[] = []
  for (let ty = 0; ty < tilesDown(spec); ty++) for (let tx = 0; tx < tilesAcross(spec); tx++) brushes.push(buildTile(hf, tx, ty, newBrushId(), p.texture))
  const centre: Vec3 = [hf.ox + (spec.cols * spec.cell) / 2, (p.origin[1] + hf.base) / 2, hf.oz + (spec.rows * spec.cell) / 2]
  return { entity: { id: entityId, classname: 'func_detail', origin: centre, brushes, terrain: spec } }
}

// ---- sampling ------------------------------------------------------------------------------------------------------------------------------------------
/** Height of the surface at (x, z) exactly as the triangles of the tiles have it (outside the grid: the nearest edge). */
export function heightAt(hf: Heightfield, x: number, z: number): number {
  const s = hf.spec, st = stride(s)
  const gx = clamp((x - hf.ox) / s.cell, 0, s.cols), gz = clamp((z - hf.oz) / s.cell, 0, s.rows)
  const i = Math.min(Math.floor(gx), s.cols - 1), j = Math.min(Math.floor(gz), s.rows - 1), fx = gx - i, fz = gz - j
  const h00 = hf.h[j * st + i], h10 = hf.h[j * st + i + 1], h01 = hf.h[(j + 1) * st + i], h11 = hf.h[(j + 1) * st + i + 1]
  if ((i + j) % 2 === 0) return fz >= fx ? h00 + fx * (h11 - h01) + fz * (h01 - h00) : h00 + fx * (h10 - h00) + fz * (h11 - h10)
  return fx + fz <= 1 ? h00 + fx * (h10 - h00) + fz * (h01 - h00) : h11 + (1 - fx) * (h01 - h11) + (1 - fz) * (h10 - h11)
}

/** The two triangles of cell (i, j) as in the tiles (chessboard diagonal). */
function cellTriangles(hf: Heightfield, i: number, j: number): [Vec3, Vec3, Vec3][] {
  const s = hf.spec, st = stride(s)
  const P = (a: number, b: number): Vec3 => [hf.ox + (i + a) * s.cell, hf.h[(j + b) * st + i + a], hf.oz + (j + b) * s.cell]
  const p00 = P(0, 0), p10 = P(1, 0), p01 = P(0, 1), p11 = P(1, 1)
  return (i + j) % 2 === 0 ? [[p00, p01, p11], [p00, p11, p10]] : [[p00, p01, p10], [p01, p11, p10]]
}

function rayTri(o: Vec3, dir: Vec3, a: Vec3, b: Vec3, c: Vec3): number | null {
  const e1: Vec3 = [b[0] - a[0], b[1] - a[1], b[2] - a[2]], e2: Vec3 = [c[0] - a[0], c[1] - a[1], c[2] - a[2]]
  const p: Vec3 = [dir[1] * e2[2] - dir[2] * e2[1], dir[2] * e2[0] - dir[0] * e2[2], dir[0] * e2[1] - dir[1] * e2[0]]
  const det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2]
  if (Math.abs(det) < 1e-14) return null
  const inv = 1 / det, tv: Vec3 = [o[0] - a[0], o[1] - a[1], o[2] - a[2]]
  const u = (tv[0] * p[0] + tv[1] * p[1] + tv[2] * p[2]) * inv
  if (u < -1e-9 || u > 1 + 1e-9) return null
  const qv: Vec3 = [tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0]]
  const v = (dir[0] * qv[0] + dir[1] * qv[1] + dir[2] * qv[2]) * inv
  if (v < -1e-9 || u + v > 1 + 1e-9) return null
  const t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * inv
  return t > 0 ? t : null
}

/** The nearest point where a ray meets the surface (top triangles only), walking the cells the ray crosses; null when it misses. Cost: the cells along the ray. */
export function rayTerrain(hf: Heightfield, origin: Vec3, dir: Vec3): { t: number; point: Vec3 } | null {
  const s = hf.spec, W = s.cols * s.cell, D = s.rows * s.cell
  let lo = 0, hi = Infinity
  const slab = (o: number, dv: number, a: number, b: number): boolean => {
    if (Math.abs(dv) < 1e-12) return o >= a && o <= b
    let t0 = (a - o) / dv, t1 = (b - o) / dv
    if (t0 > t1) [t0, t1] = [t1, t0]
    lo = Math.max(lo, t0); hi = Math.min(hi, t1)
    return lo <= hi
  }
  if (!slab(origin[0], dir[0], hf.ox, hf.ox + W) || !slab(origin[2], dir[2], hf.oz, hf.oz + D) || !slab(origin[1], dir[1], hf.base, 1e9)) return null
  // walk the cells of the column the ray crosses between lo and hi (Amanatides-Woo)
  const px = origin[0] + dir[0] * lo, pz = origin[2] + dir[2] * lo
  let i = clamp(Math.floor((px - hf.ox) / s.cell), 0, s.cols - 1), j = clamp(Math.floor((pz - hf.oz) / s.cell), 0, s.rows - 1)
  const sx = dir[0] > 0 ? 1 : -1, sz = dir[2] > 0 ? 1 : -1
  const nextX = (hf.ox + (i + (sx > 0 ? 1 : 0)) * s.cell - origin[0]) / dir[0], nextZ = (hf.oz + (j + (sz > 0 ? 1 : 0)) * s.cell - origin[2]) / dir[2]
  let tMaxX = Math.abs(dir[0]) < 1e-12 ? Infinity : nextX, tMaxZ = Math.abs(dir[2]) < 1e-12 ? Infinity : nextZ
  const tDX = Math.abs(dir[0]) < 1e-12 ? Infinity : Math.abs(s.cell / dir[0]), tDZ = Math.abs(dir[2]) < 1e-12 ? Infinity : Math.abs(s.cell / dir[2])
  for (let guard = 0; guard < s.cols + s.rows + 4; guard++) {
    let best: number | null = null
    for (const [a, b, c] of cellTriangles(hf, i, j)) {
      const t = rayTri(origin, dir, a, b, c)
      if (t !== null && (best === null || t < best)) best = t
    }
    if (best !== null) return { t: best, point: [origin[0] + dir[0] * best, origin[1] + dir[1] * best, origin[2] + dir[2] * best] }
    if (tMaxX < tMaxZ) { if (tMaxX > hi) break; i += sx; tMaxX += tDX } else { if (tMaxZ > hi) break; j += sz; tMaxZ += tDZ }
    if (i < 0 || i >= s.cols || j < 0 || j >= s.rows) break
  }
  return null
}

// ---- brushes ------------------------------------------------------------------------------------------------------------------------------------------
export interface BrushParams { radius: number; strength: number; falloff: number }
export const DEFAULT_BRUSH: BrushParams = { radius: 6, strength: 0.3, falloff: 0.7 }

/** Weight of a stamp at distance `d` from its centre: 1 up to (1 - falloff) of the radius, then a smooth drop to 0 at the radius. */
export function brushWeight(d: number, radius: number, falloff: number): number {
  if (d >= radius) return 0
  const inner = radius * (1 - clamp(falloff, 0, 1))
  if (d <= inner) return 1
  const t = (d - inner) / (radius - inner)
  return 1 - t * t * (3 - 2 * t)
}

/** Inclusive range of grid points. */
export interface PointRect { gx0: number; gx1: number; gz0: number; gz1: number }

function rectAround(hf: Heightfield, cx: number, cz: number, r: number): PointRect | null {
  const s = hf.spec
  const gx0 = Math.max(0, Math.ceil((cx - r - hf.ox) / s.cell)), gx1 = Math.min(s.cols, Math.floor((cx + r - hf.ox) / s.cell))
  const gz0 = Math.max(0, Math.ceil((cz - r - hf.oz) / s.cell)), gz1 = Math.min(s.rows, Math.floor((cz + r - hf.oz) / s.cell))
  return gx0 > gx1 || gz0 > gz1 ? null : { gx0, gx1, gz0, gz1 }
}

export const unionRect = (a: PointRect | null, b: PointRect | null): PointRect | null =>
  !a ? b : !b ? a : { gx0: Math.min(a.gx0, b.gx0), gx1: Math.max(a.gx1, b.gx1), gz0: Math.min(a.gz0, b.gz0), gz1: Math.max(a.gz1, b.gz1) }

/**
 * One stamp of a height brush at (cx, cz): Raise / Lower add or remove `strength x RAISE_PER_STRENGTH` metres at the centre, Smooth blends the ground toward the mean of its
 * neighbours by `strength`, Flatten blends it toward `target` by `strength`; all scaled by the falloff weight. `invert` swaps Raise and Lower. Changes `hf.h` in place and
 * returns the grid points that were touched (null when none).
 */
export function stamp(hf: Heightfield, tool: Exclude<TerrainTool, 'paint'>, cx: number, cz: number, p: BrushParams, opts: { invert?: boolean; target?: number } = {}): PointRect | null {
  const s = hf.spec, st = stride(s), r = p.radius
  const rect = rectAround(hf, cx, cz, r)
  if (!rect || !(r > 0)) return null
  const lo = hf.base + MIN_THICKNESS, hi = hf.base + 100000
  const sign = (tool === 'lower' ? -1 : 1) * (opts.invert ? -1 : 1)
  const out: [number, number][] = []
  const weights: number[] = []
  for (let gz = rect.gz0; gz <= rect.gz1; gz++)
    for (let gx = rect.gx0; gx <= rect.gx1; gx++) {
      const w = brushWeight(Math.hypot(hf.ox + gx * s.cell - cx, hf.oz + gz * s.cell - cz), r, p.falloff)
      weights.push(w)
      if (w > 0) out.push([gx, gz])
    }
  if (!out.length) return null
  const wAt = (gx: number, gz: number): number => weights[(gz - rect.gz0) * (rect.gx1 - rect.gx0 + 1) + (gx - rect.gx0)]
  if (tool === 'smooth') {
    // read the old heights, write the new ones (so the result does not depend on the order of the points)
    const next = new Map<number, number>()
    for (const [gx, gz] of out) {
      let sum = 0, n = 0
      for (let dz = -1; dz <= 1; dz++)
        for (let dx = -1; dx <= 1; dx++) {
          const x = gx + dx, z = gz + dz
          if (x < 0 || z < 0 || x > s.cols || z > s.rows) continue
          sum += hf.h[z * st + x]; n++
        }
      const f = clamp(p.strength * wAt(gx, gz), 0, 1), cur = hf.h[gz * st + gx]
      next.set(gz * st + gx, cur + (sum / n - cur) * f)
    }
    for (const [at, y] of next) hf.h[at] = clamp(q4(y), lo, hi)
  } else {
    for (const [gx, gz] of out) {
      const at = gz * st + gx, w = wAt(gx, gz)
      let y = hf.h[at]
      if (tool === 'flatten') y += ((opts.target ?? y) - y) * clamp(p.strength * w, 0, 1)
      else y += sign * p.strength * RAISE_PER_STRENGTH * w
      hf.h[at] = clamp(q4(y), lo, hi)
    }
  }
  return { gx0: Math.min(...out.map((q) => q[0])), gx1: Math.max(...out.map((q) => q[0])), gz0: Math.min(...out.map((q) => q[1])), gz1: Math.max(...out.map((q) => q[1])) }
}

/** Tile indices (ty * tilesAcross + tx) whose brush holds any grid point of the rect (a point on a tile border belongs to both tiles). */
export function tilesOfRect(s: TerrainSpec, r: PointRect): number[] {
  const nx = tilesAcross(s), ny = tilesDown(s), T = s.tile
  const range = (a: number, b: number, n: number): [number, number] => [Math.max(0, Math.ceil(a / T) - 1), Math.min(n - 1, Math.floor(b / T))]
  const [tx0, tx1] = range(r.gx0, r.gx1, nx), [ty0, ty1] = range(r.gz0, r.gz1, ny)
  const out: number[] = []
  for (let ty = ty0; ty <= ty1; ty++) for (let tx = tx0; tx <= tx1; tx++) out.push(ty * nx + tx)
  return out
}

/** Cells (gj * cols + gi) whose centre is within `radius` of (cx, cz). */
export function cellsInRadius(hf: Heightfield, cx: number, cz: number, radius: number): number[] {
  const s = hf.spec
  const i0 = Math.max(0, Math.floor((cx - radius - hf.ox) / s.cell)), i1 = Math.min(s.cols - 1, Math.floor((cx + radius - hf.ox) / s.cell))
  const j0 = Math.max(0, Math.floor((cz - radius - hf.oz) / s.cell)), j1 = Math.min(s.rows - 1, Math.floor((cz + radius - hf.oz) / s.cell))
  const out: number[] = []
  for (let j = j0; j <= j1; j++)
    for (let i = i0; i <= i1; i++)
      if (Math.hypot(hf.ox + (i + 0.5) * s.cell - cx, hf.oz + (j + 0.5) * s.cell - cz) <= radius) out.push(j * s.cols + i)
  return out
}

// ---- a stroke -----------------------------------------------------------------------------------------------------------------------------------------
/**
 * One drag of a brush over a terrain. Holds a working copy of the heights; `moveTo` places stamps along the path at a fixed spacing (so the result does not depend on how
 * many pointer events the browser sent), `document(start)` returns the document with the changed tiles replaced. Only the tiles a stamp touched are rebuilt, once per call, and
 * kept for the next call, so a long stroke over a big terrain stays cheap.
 */
export class TerrainStroke {
  readonly entityId: string
  private hf: Heightfield
  private tiles: Map<number, Brush>
  private heightDirty = new Set<number>()
  private paintDirty = new Set<number>()
  private built = new Map<number, Brush>()
  private painted = new Map<number, string>()
  private target: number | undefined
  private last: [number, number] | null = null
  private carry = 0
  /** Raise <-> Lower (Shift held); may change during the stroke */
  invert = false

  constructor(start: Entity, hf: Heightfield, readonly tool: TerrainTool, readonly params: BrushParams, readonly opts: { invert?: boolean; flattenHeight?: number | null; texture?: string } = {}) {
    this.entityId = start.id
    this.invert = !!opts.invert
    this.hf = cloneHeightfield(hf)
    const nx = tilesAcross(hf.spec)
    this.tiles = new Map()
    for (const b of start.brushes ?? []) { const t = tileOf(b); if (t) this.tiles.set(t[1] * nx + t[0], b) }
  }

  get heights(): Heightfield { return this.hf }
  get touched(): boolean { return this.heightDirty.size + this.paintDirty.size > 0 }
  private spacing(): number { return Math.max(this.params.radius * 0.2, this.hf.spec.cell * 0.25) }

  /** One stamp at (x, z). */
  stampAt(x: number, z: number): void {
    const { tool, params, hf } = this
    if (tool === 'paint') {
      const tex = this.opts.texture
      if (!tex) return
      const s = hf.spec
      for (const c of cellsInRadius(hf, x, z, params.radius)) {
        const gi = c % s.cols, gj = Math.floor(c / s.cols)
        const t = Math.floor(gj / s.tile) * tilesAcross(s) + Math.floor(gi / s.tile)
        if (this.painted.get(c) !== tex) { this.painted.set(c, tex); this.paintDirty.add(t) }
      }
      return
    }
    if (tool === 'flatten' && this.target === undefined) this.target = this.opts.flattenHeight ?? heightAt(hf, x, z)
    const r = stamp(hf, tool, x, z, params, { invert: this.invert, target: this.target })
    if (r) for (const t of tilesOfRect(hf.spec, r)) this.heightDirty.add(t)
  }

  /** The brush moved to (x, z): stamps along the way, every `spacing` metres; the first call stamps where it stands. */
  moveTo(x: number, z: number): void {
    if (!this.last) { this.stampAt(x, z); this.last = [x, z]; return }
    const [lx, lz] = this.last, len = Math.hypot(x - lx, z - lz), sp = this.spacing()
    let at = sp - this.carry
    while (at <= len) { const f = at / len; this.stampAt(lx + (x - lx) * f, lz + (z - lz) * f); at += sp }
    this.carry = len - (at - sp)
    this.last = [x, z]
  }

  /** Stamps again where the brush stands (holding still keeps working). */
  repeat(): void { if (this.last) this.stampAt(this.last[0], this.last[1]) }

  /** `start` with every tile the stroke has changed so far (also when the last moves added nothing). Returns `start` itself while nothing has changed. */
  document(start: MapDocument): MapDocument {
    if (!this.touched && !this.built.size) return start
    const dirty = new Set([...this.heightDirty, ...this.paintDirty])
    const s = this.hf.spec, nx = tilesAcross(s)
    for (const t of dirty) {
      const old = this.tiles.get(t)
      if (!old) continue
      const tx = t % nx, ty = Math.floor(t / nx)
      let b: Brush = old
      if (this.heightDirty.has(t)) b = { ...b, vertices: tileVertices(this.hf, tx, ty) }
      if (this.paintDirty.has(t)) {
        const { x0, z0, w } = tileCells(s, tx, ty), top = 2 * w * tileCells(s, tx, ty).d
        const faces = b.faces.map((f) => {
          if (f.id >= top) return f
          const cell = f.id >> 1, gi = x0 + (cell % w), gj = z0 + Math.floor(cell / w)
          const tex = this.painted.get(gj * s.cols + gi)
          return tex !== undefined && tex !== f.texture ? { ...f, texture: tex } : f
        })
        b = { ...b, faces }
      }
      this.built.set(t, trust(b))
    }
    this.heightDirty.clear(); this.paintDirty.clear()
    return {
      ...start,
      entities: start.entities.map((e) => {
        if (e.id !== this.entityId) return e
        return { ...e, brushes: (e.brushes ?? []).map((b) => { const t = tileOf(b); return t ? (this.built.get(t[1] * nx + t[0]) ?? b) : b }) }
      }),
    }
  }
}

// ---- the brush cursor ----------------------------------------------------------------------------------------------------------------------------------
/** Line segments (flat x y z x y z ...) of a circle of radius r laid on the ground at (cx, cz), `lift` above it. */
export function circleOnTerrain(hf: Heightfield, cx: number, cz: number, r: number, segments = 64, lift = 0.05): number[] {
  const out: number[] = []
  const pt = (k: number): Vec3 => { const a = (k / segments) * Math.PI * 2, x = cx + Math.cos(a) * r, z = cz + Math.sin(a) * r; return [x, heightAt(hf, x, z) + lift, z] }
  let prev = pt(0)
  for (let k = 1; k <= segments; k++) { const p = pt(k % segments); out.push(...prev, ...p); prev = p }
  return out
}

/** A short plain-language summary for the panel (from the record alone: cheap enough to ask on every edit). */
export function describeSpec(s: TerrainSpec): string {
  return `${+(s.cols * s.cell).toFixed(2)} x ${+(s.rows * s.cell).toFixed(2)} m, ${s.cols} x ${s.rows} cells of ${s.cell} m, ${tilesAcross(s) * tilesDown(s)} tiles (brushes), ${(s.cols * s.rows * 2).toLocaleString('en-US')} top triangles`
}
