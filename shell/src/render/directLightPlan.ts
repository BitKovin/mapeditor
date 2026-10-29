// Predicting the direct light of an edit in the Shell (DESIGN 6.11): the pure half. What the lights are, what changed between two states of the scene, and which
// lit mesh triangles (lightmap receivers) can see a different amount of light because of it. No GL here: renderer-side work is in directLight.ts.
//
// The Core's lit mesh holds the geometry as the Core last baked it; `LightmapStore.plainMesh(world)` is that mesh with the brushes that changed since replaced by
// their current faces (litStale.ts), carrying the lightmap coordinates they had. Every vertex of it says where a lightmap texel is in the world, so the direct light
// of any texel can be evaluated with the document's current lights against the plain mesh as occluder. A change of the scene is found by comparing the brushes
// that are out of date (and their overlay) and the lights between the last predicted state and now.
import type { Entity } from '../model/document'
import type { EntityClassDef } from '../net/protocol'
import type { World } from '../model/derived'
import type { Vec3 } from '../model/math'
import { lightFrame, lightParams } from '../model/lights'
import type { LitFace } from '../net/protocol'
import type { LitAnalysis } from './litStale'
import { staleKey } from './litStale'

/** Floats per vertex of the lit mesh: position (3), normal (3), lightmap u, v, page, style mask (3). */
export const CORE_FLOATS = 12

/** A light the Shell predicts: what the Core evaluates analytically and traces shadows for (point, spot, directional). Sky and rect lights stay the Core's. */
export interface PredLight {
  id: string
  kind: 'point' | 'spot' | 'directional'
  position: Vec3
  /** The direction the light travels (spot axis, sun direction). */
  forward: Vec3
  /** colour x brightness x the level's general brightness (negative: a dark light) */
  color: Vec3
  radius: number
  cosInner: number
  cosOuter: number
  /** Source size as the live bake uses it: a sphere radius in metres (point, spot) or an angular diameter in degrees (directional); never 0 where the bake softens. */
  size: number
  castShadows: boolean
}

export interface LightDefaults { sourceSize: number; sunSize: number }
export interface PredScene { lights: PredLight[]; ambient: Vec3 }

const DEG = Math.PI / 180
const keyColor = (raw: string | undefined): Vec3 | null => {
  const p = (raw ?? '').trim().split(/\s+/).map(Number)
  return p.length === 3 && p.every(Number.isFinite) ? [Math.max(0, p[0]), Math.max(0, p[1]), Math.max(0, p[2])] : null
}

/** The base layer (style 0) lights of the world and its ambient, as the Core's collectLights + the service's softening make them. */
export function collectPredScene(world: World, defs: readonly EntityClassDef[], defaults: LightDefaults): PredScene {
  const entities: Entity[] = [...world.doc.entities, ...world.prefabs.map((p) => p.entity)]
  const ws = world.doc.entities.find((e) => e.classname === 'worldspawn')
  const sc = Number(ws?.keyvalues?._light_scale)
  const scale = ws?.keyvalues?._light_scale !== undefined && Number.isFinite(sc) ? Math.min(100, Math.max(0, sc)) : 1
  const ambient = keyColor(ws?.keyvalues?._ambient) ?? [0, 0, 0]
  const lights: PredLight[] = []
  for (const e of entities) {
    const p = lightParams(e, defs.find((d) => d.name === e.classname))
    if (!p || p.style !== 0 || (p.kind !== 'point' && p.kind !== 'spot' && p.kind !== 'directional')) continue
    const f = lightFrame(e)
    let size = p.sourceSize
    if (size <= 0) size = p.kind === 'directional' ? defaults.sunSize : defaults.sourceSize
    const b = p.brightness * scale
    lights.push({
      id: e.id, kind: p.kind, position: f.origin, forward: f.forward, color: [p.color[0] * b, p.color[1] * b, p.color[2] * b], radius: p.radius,
      cosInner: Math.cos(p.innerAngle * DEG), cosOuter: Math.cos(p.outerAngle * DEG), size, castShadows: p.castShadows,
    })
  }
  return { lights, ambient }
}

const same3 = (a: Vec3, b: Vec3): boolean => a[0] === b[0] && a[1] === b[1] && a[2] === b[2]
export function sameLight(a: PredLight, b: PredLight): boolean {
  return a.kind === b.kind && same3(a.position, b.position) && same3(a.forward, b.forward) && same3(a.color, b.color) && a.radius === b.radius && a.cosInner === b.cosInner &&
    a.cosOuter === b.cosOuter && a.size === b.size && a.castShadows === b.castShadows
}

export interface Box { mn: Vec3; mx: Vec3 }
export const emptyBox = (): Box => ({ mn: [Infinity, Infinity, Infinity], mx: [-Infinity, -Infinity, -Infinity] })
export function boxAdd(b: Box, x: number, y: number, z: number): void {
  if (x < b.mn[0]) b.mn[0] = x; if (y < b.mn[1]) b.mn[1] = y; if (z < b.mn[2]) b.mn[2] = z
  if (x > b.mx[0]) b.mx[0] = x; if (y > b.mx[1]) b.mx[1] = y; if (z > b.mx[2]) b.mx[2] = z
}
export const boxValid = (b: Box): boolean => b.mn[0] <= b.mx[0]
const sameBox = (a: Box, b: Box): boolean => a.mn.every((v, i) => v === b.mn[i]) && a.mx.every((v, i) => v === b.mx[i])

/** Triangle data of a lit mesh (12 floats per vertex): bounds, centre and radius per triangle, so changes can be tested without touching the vertices again. */
export interface TriInfo {
  count: number
  /** per triangle: centre xyz, radius */
  sphere: Float32Array
  page: Uint16Array
  /** the largest triangle radius */
  maxRadius: number
  /** small triangles by cell, and the big ones (floors, walls: tested one by one) */
  grid: Map<number, GridCell> | null
  big: number[]
  /** bounds of every triangle centre */
  bmin: Vec3
  bmax: Vec3
}
interface GridCell { tris: number[]; stamp: number }
export function triInfo(mesh: Float32Array): TriInfo {
  const count = Math.floor(mesh.length / (CORE_FLOATS * 3))
  const sphere = new Float32Array(count * 4), page = new Uint16Array(count)
  let maxR = 0
  for (let t = 0; t < count; t++) {
    const o = t * 3 * CORE_FLOATS
    let cx = 0, cy = 0, cz = 0
    for (let k = 0; k < 3; k++) { cx += mesh[o + k * CORE_FLOATS]; cy += mesh[o + k * CORE_FLOATS + 1]; cz += mesh[o + k * CORE_FLOATS + 2] }
    cx /= 3; cy /= 3; cz /= 3
    let r = 0
    for (let k = 0; k < 3; k++) r = Math.max(r, Math.hypot(mesh[o + k * CORE_FLOATS] - cx, mesh[o + k * CORE_FLOATS + 1] - cy, mesh[o + k * CORE_FLOATS + 2] - cz))
    sphere[t * 4] = cx; sphere[t * 4 + 1] = cy; sphere[t * 4 + 2] = cz; sphere[t * 4 + 3] = r
    page[t] = Math.max(0, Math.round(mesh[o + 8]))
    if (r > maxR) maxR = r
  }
  const bmin: Vec3 = [Infinity, Infinity, Infinity], bmax: Vec3 = [-Infinity, -Infinity, -Infinity]
  for (let t = 0; t < count; t++) for (let c = 0; c < 3; c++) { const v = sphere[t * 4 + c]; if (v < bmin[c]) bmin[c] = v; if (v > bmax[c]) bmax[c] = v }
  return { count, sphere, page, maxRadius: maxR, grid: null, big: [], bmin, bmax }
}

/** The triangles by 16 m cell of their centre (built on first use): a change is looked for along its shadow, not among every triangle of the level. */
export const GRID_CELL = 16
/** A triangle whose circumsphere is bigger than this is not put in the grid (one big floor would make every cell lookup wide). */
export const BIG_RADIUS = 6
const cellOf = (v: number): number => Math.floor(v / GRID_CELL) + 1024
const cellKeyN = (x: number, y: number, z: number): number => (x * 2048 + y) * 2048 + z
function gridOf(info: TriInfo): Map<number, GridCell> {
  if (info.grid) return info.grid
  const g = new Map<number, GridCell>()
  for (let t = 0; t < info.count; t++) {
    if (info.sphere[t * 4 + 3] > BIG_RADIUS) { info.big.push(t); continue }
    const k = cellKeyN(cellOf(info.sphere[t * 4]), cellOf(info.sphere[t * 4 + 1]), cellOf(info.sphere[t * 4 + 2]))
    const c = g.get(k)
    if (c) c.tris.push(t); else g.set(k, { tris: [t], stamp: 0 })
  }
  info.grid = g
  return g
}

/**
 * The occluder / receiver mesh of the prediction: the Core's mesh without the pieces of the brushes that changed, followed by their current faces (the overlay,
 * lightmap coordinates as they had them). The same as `buildPlainMesh`, but as typed array copies of whole runs: that function pushes every float of a 20 000
 * piece level into a JS array (0.4 s). Without changes it is the Core's mesh itself. `overlayTriangles` = how many triangles the overlay added.
 */
export function predictorMesh(mesh: Float32Array, faces: readonly LitFace[], a: LitAnalysis | null): { mesh: Float32Array; overlayTriangles: number } {
  if (!a) return { mesh, overlayTriangles: 0 }
  const total = Math.floor(mesh.length / CORE_FLOATS)
  const runs: [number, number][] = []   // [from, to) vertex ranges that stay
  let keepFrom = 0, at = 0, kept = 0
  for (const f of faces) {
    const verts = f.n * 3
    if (at + verts > total) break
    if (a.stale.has(staleKey(f.bi, f.b))) {
      if (at > keepFrom) { runs.push([keepFrom, at]); kept += at - keepFrom }
      keepFrom = at + verts
    }
    at += verts
  }
  if (at > keepFrom) { runs.push([keepFrom, at]); kept += at - keepFrom }
  let overlay = 0
  for (const o of a.overlay) overlay += o.vertexCount
  const out = new Float32Array((kept + overlay) * CORE_FLOATS)
  let w = 0
  for (const [from, to] of runs) { out.set(mesh.subarray(from * CORE_FLOATS, to * CORE_FLOATS), w); w += (to - from) * CORE_FLOATS }
  for (const o of a.overlay) { out.set(o.core, w); w += o.vertexCount * CORE_FLOATS }
  return { mesh: out, overlayTriangles: overlay / 3 }
}

/** One brush's geometry in a scene: where its pieces are (the Core's mesh: old) or where its current faces are (the overlay: new). */
export interface SceneBoxes { boxes: Map<string, { sig: string; boxes: Box[] }> }

/**
 * Per brush that is out of date: the boxes of its pieces in the Core's mesh and of its current faces. A brush whose pieces are all in the mesh as the Core
 * baked them is not listed (it is the same in every scene that shows the mesh as it is).
 */
export function staleBoxes(mesh: Float32Array, faces: readonly LitFace[], a: LitAnalysis | null): SceneBoxes {
  const out: SceneBoxes = { boxes: new Map() }
  if (!a) return out
  const total = Math.floor(mesh.length / CORE_FLOATS)
  let at = 0
  const old = new Map<string, Box[]>()
  for (const f of faces) {
    const verts = f.n * 3
    if (at + verts > total) break
    const key = staleKey(f.bi, f.b)
    if (a.stale.has(key)) {
      const b = emptyBox()
      for (let k = 0; k < verts; k++) { const o = (at + k) * CORE_FLOATS; boxAdd(b, mesh[o], mesh[o + 1], mesh[o + 2]) }
      const list = old.get(key) ?? []; old.set(key, list); list.push(b)
    }
    at += verts
  }
  const now = new Map<string, Box[]>()
  const sigs = new Map<string, number>()
  for (const o of a.overlay) {
    const b = emptyBox()
    let h = 2166136261
    for (let k = 0; k < o.vertexCount; k++) {
      const x = o.core[k * CORE_FLOATS], y = o.core[k * CORE_FLOATS + 1], z = o.core[k * CORE_FLOATS + 2]
      boxAdd(b, x, y, z)
      h = Math.imul(h ^ Math.round(x * 1000), 16777619); h = Math.imul(h ^ Math.round(y * 1000), 16777619); h = Math.imul(h ^ Math.round(z * 1000), 16777619)
    }
    const list = now.get(o.brush) ?? []; now.set(o.brush, list); list.push(b)
    sigs.set(o.brush, ((sigs.get(o.brush) ?? 0) ^ h) >>> 0)
  }
  // one box where the Core's pieces of the brush are and one where its current faces are (a wall's faces together are the wall)
  const union = (list: Box[] | undefined): Box => {
    const u = emptyBox()
    for (const x of list ?? []) { boxAdd(u, x.mn[0], x.mn[1], x.mn[2]); boxAdd(u, x.mx[0], x.mx[1], x.mx[2]) }
    return u
  }
  for (const key of a.stale) out.boxes.set(key, { sig: `${sigs.get(key) ?? 'gone'}`, boxes: [union(old.get(key)), union(now.get(key))] })
  return out
}

/**
 * The boxes whose occupancy differs between the scene `prev` and the scene `next` (both from staleBoxes): a brush that moved again, one that went back to where
 * the Core has it (its overlay is gone), a new one that moved away from the mesh. Both the old and the new place of a brush count, the light changes in both.
 */
export function changedBoxes(prev: SceneBoxes, next: SceneBoxes): Box[] {
  const out: Box[] = []
  const keys = new Set([...prev.boxes.keys(), ...next.boxes.keys()])
  for (const k of keys) {
    const a = prev.boxes.get(k), b = next.boxes.get(k)
    if (a && b && a.sig === b.sig) continue
    for (const s of [a, b]) for (const x of s?.boxes ?? []) if (boxValid(x) && !out.some((y) => sameBox(x, y))) out.push(x)
  }
  return out
}

/** Can geometry inside the sphere (c, rb) change the light a receiver sphere (t, rt) gets from `l`? Conservative: the segment light - receiver passes the box's sphere. */
export function mayShadow(l: PredLight, cx: number, cy: number, cz: number, rb: number, tx: number, ty: number, tz: number, rt: number): boolean {
  if (l.kind === 'directional') {
    // the receiver looks along -forward to the light: the ray t + s * toLight, s >= 0
    const dx = -l.forward[0], dy = -l.forward[1], dz = -l.forward[2]
    const px = cx - tx, py = cy - ty, pz = cz - tz
    const s = Math.max(0, px * dx + py * dy + pz * dz)
    return Math.hypot(px - dx * s, py - dy * s, pz - dz * s) <= rb + rt
  }
  const lx = l.position[0], ly = l.position[1], lz = l.position[2]
  if (Math.hypot(tx - lx, ty - ly, tz - lz) - rt > l.radius) return false // out of the light's reach
  const dx = tx - lx, dy = ty - ly, dz = tz - lz
  const len2 = dx * dx + dy * dy + dz * dz
  const s = len2 < 1e-12 ? 0 : Math.min(1, Math.max(0, ((cx - lx) * dx + (cy - ly) * dy + (cz - lz) * dz) / len2))
  return Math.hypot(lx + dx * s - cx, ly + dy * s - cy, lz + dz * s - cz) <= rb + rt
}

/** Does a light's light change reach the receiver sphere at all (it moved, changed colour, ...)? Point / spot: within the radius; a sun: everywhere. */
export function lightReaches(l: PredLight, tx: number, ty: number, tz: number, rt: number): boolean {
  if (l.kind === 'directional') return true
  return Math.hypot(tx - l.position[0], ty - l.position[1], tz - l.position[2]) - rt <= l.radius
}

export interface PredChange {
  boxes: Box[]
  /** lights that exist now or existed before and differ (added, removed, modified): both versions in this list */
  lights: PredLight[]
}

export function changedLights(prev: readonly PredLight[], next: readonly PredLight[]): PredLight[] {
  const out: PredLight[] = []
  const was = new Map(prev.map((l) => [l.id, l]))
  for (const l of next) {
    const p = was.get(l.id)
    if (!p || !sameLight(p, l)) { out.push(l); if (p) out.push(p) }
    was.delete(l.id)
  }
  for (const p of was.values()) out.push(p)
  return out
}

/**
 * The triangles whose direct light may differ because of `change`: those inside the reach of a changed light, those a changed box can shadow from any light
 * of `lights` (the lights of the new scene and of the old one, `all`), and every triangle from `forced` (the overlay: it moved, its texels are new).
 */
/** Where the last affectedTriangles spent its time (ms), for profiling. */
export const affectTimings = { grid: 0, lights: 0, boxes: 0, visits: 0, tests: 0, cells: 0 }
let stampCounter = 0
export function affectedTriangles(info: TriInfo, change: PredChange, all: readonly PredLight[], forcedFrom: number): Uint32Array {
  const hit = new Uint8Array(info.count)
  const spheres = info.sphere
  const margin = 0.5
  for (let t = Math.max(0, forcedFrom); t < info.count; t++) hit[t] = 1
  if (change.lights.some((l) => l.kind === 'directional')) hit.fill(1) // a sun changed: everything
  else if (change.boxes.length > 0 || change.lights.length > 0) {
    const g0 = performance.now()
    const grid = gridOf(info)
    affectTimings.grid = performance.now() - g0
    affectTimings.visits = 0; affectTimings.tests = 0; affectTimings.cells = 0
    const reach = Math.min(info.maxRadius, BIG_RADIUS)
    const cellR = GRID_CELL * 0.8660254 + reach // a cell's circumsphere, plus what sticks out of it
    // the cells of the window of radius `d` round p that were not visited for this stamp, with their centre
    const around = (px: number, py: number, pz: number, d: number, stamp: number, fn: (cell: GridCell, mx: number, my: number, mz: number) => void): void => {
      const n = Math.ceil((d + reach) / GRID_CELL)
      const cx = cellOf(px), cy = cellOf(py), cz = cellOf(pz)
      for (let x = cx - n; x <= cx + n; x++) for (let y = cy - n; y <= cy + n; y++) for (let z = cz - n; z <= cz + n; z++) {
        const cell = grid.get(cellKeyN(x, y, z))
        if (!cell || cell.stamp === stamp) continue
        cell.stamp = stamp
        affectTimings.cells++
        fn(cell, (x - 1024 + 0.5) * GRID_CELL, (y - 1024 + 0.5) * GRID_CELL, (z - 1024 + 0.5) * GRID_CELL)
      }
    }
    for (const l of change.lights) {
      const stamp = ++stampCounter
      around(l.position[0], l.position[1], l.position[2], l.radius, stamp, (cell) => {
        for (const t of cell.tris) if (!hit[t] && lightReaches(l, spheres[t * 4], spheres[t * 4 + 1], spheres[t * 4 + 2], spheres[t * 4 + 3])) hit[t] = 1
      })
      for (const t of info.big) if (!hit[t] && lightReaches(l, spheres[t * 4], spheres[t * 4 + 1], spheres[t * 4 + 2], spheres[t * 4 + 3])) hit[t] = 1
    }
    const b0 = performance.now()
    for (const b of change.boxes) {
      const cx = (b.mn[0] + b.mx[0]) * 0.5, cy = (b.mn[1] + b.mx[1]) * 0.5, cz = (b.mn[2] + b.mx[2]) * 0.5
      const rb = Math.hypot(b.mx[0] - b.mn[0], b.mx[1] - b.mn[1], b.mx[2] - b.mn[2]) * 0.5 + margin
      for (const l of all) {
        if (!l.castShadows) continue
        // the shadow of the box: from it away from the light, as far as the light reaches (a sun: as far as the level goes)
        let dx: number, dy: number, dz: number, len: number
        if (l.kind === 'directional') { dx = l.forward[0]; dy = l.forward[1]; dz = l.forward[2]; len = 8192 }
        else {
          dx = cx - l.position[0]; dy = cy - l.position[1]; dz = cz - l.position[2]
          const dist = Math.hypot(dx, dy, dz) || 1
          dx /= dist; dy /= dist; dz /= dist
          len = l.radius - dist + rb
          if (dist - rb > l.radius) continue // the box lies beyond the light's reach: nothing it blocks is lit
        }
        if (len <= 0) continue
        for (const t of info.big) if (!hit[t] && mayShadow(l, cx, cy, cz, rb, spheres[t * 4], spheres[t * 4 + 1], spheres[t * 4 + 2], spheres[t * 4 + 3])) hit[t] = 1
        // clip the ray to where the level is: s from the box centre along (dx, dy, dz)
        let s0 = -rb, s1 = len
        for (let c = 0; c < 3; c++) {
          const o = c === 0 ? cx : c === 1 ? cy : cz, d = c === 0 ? dx : c === 1 ? dy : dz
          const lo = info.bmin[c] - GRID_CELL, hi = info.bmax[c] + GRID_CELL
          if (Math.abs(d) < 1e-9) { if (o < lo || o > hi) { s1 = -Infinity } continue }
          const a = (lo - o) / d, e = (hi - o) / d
          s0 = Math.max(s0, Math.min(a, e)); s1 = Math.min(s1, Math.max(a, e))
        }
        if (s1 < s0) continue
        const stamp = ++stampCounter
        const step = GRID_CELL * 0.75
        for (let s = s0; s <= s1 + step; s += step) {
          around(cx + dx * s, cy + dy * s, cz + dz * s, rb + step, stamp, (cell, mx, my, mz) => {
            affectTimings.visits++
            if (!mayShadow(l, cx, cy, cz, rb, mx, my, mz, cellR)) return // the whole cell is out of this box's shadow
            for (const t of cell.tris) {
              if (hit[t]) continue
              affectTimings.tests++
              if (mayShadow(l, cx, cy, cz, rb, spheres[t * 4], spheres[t * 4 + 1], spheres[t * 4 + 2], spheres[t * 4 + 3])) hit[t] = 1
            }
          })
        }
      }
    }
    affectTimings.boxes = performance.now() - b0
  }
  const out: number[] = []
  for (let t = 0; t < info.count; t++) if (hit[t]) out.push(t)
  return Uint32Array.from(out)
}
