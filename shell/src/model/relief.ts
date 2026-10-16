// Texture relief: a small polygon mesh belonging to a texture (stored in `<texture file>.json` as `relief`). It describes how one repeat of the texture
// stands out of / sinks into the surface it is applied to (a window with an inward frame, a brick with a rim, a cornice). It is purely structural
// geometry: it makes no portals and is meant for parallax; applying it to world geometry is a separate step that reads this data.
//
// Stored form (normalised, independent of the texture's size in the world):
//   vertices: [x, y, z]  x, y = 0..1 across ONE REPEAT of the texture (x to the right, y UP, so (0,0) is the bottom-left corner of the picture),
//                        z = depth in metres along the surface normal: 0 = the surface, > 0 stands out, < 0 sinks in.
//   faces:    vertex index loops, counter-clockwise seen from the front (+z). Face ids are the array indices.
//
// The editor works in "edit space": the same mesh in metres (x * width, y * height, z), so every modeling tool of the Mesh editor (meshOps.ts) applies
// unchanged to it. A relief is a `Brush`-shaped mesh there (id 'relief'); it is an open surface, not a solid.
import type { Brush } from './document'
import { Vec3 } from './math'
import { buildTopology, finish, selFromVerts } from './meshTopology'
import { OpResult, extrudeFaces, insetFaces, isError, transformVerts } from './meshOps'
import { faceNormal, triangulateFace } from './geometry'

export interface Relief { vertices: Vec3[]; faces: number[][] }

export const RELIEF_MAX_VERTICES = 4096, RELIEF_MAX_FACES = 4096, RELIEF_MAX_LOOP = 64
/** Largest depth either way, metres. */
export const RELIEF_MAX_DEPTH = 8
const EPS = 1e-6

const fin = (v: unknown): v is number => typeof v === 'number' && Number.isFinite(v)
const clamp = (v: number, lo: number, hi: number): number => Math.min(hi, Math.max(lo, v))

/** A flat one-quad relief: the starting point of every new relief (extrude / inset it to model). */
export const flatRelief = (): Relief => ({ vertices: [[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0]], faces: [[0, 1, 2, 3]] })

/** Every vertex on the surface: it adds nothing to a flat face. */
export const isFlatRelief = (r: Relief): boolean => r.vertices.every((v) => Math.abs(v[2]) < EPS)

/** Lenient reader (hand-edited or damaged files must not break the editor): bad vertices / faces are dropped, coordinates clamped; null when nothing usable is left. */
export function parseRelief(j: unknown): Relief | null {
  if (!j || typeof j !== 'object') return null
  const o = j as Record<string, unknown>
  if (!Array.isArray(o.vertices) || !Array.isArray(o.faces)) return null
  const vertices: Vec3[] = []
  for (const v of o.vertices) {
    if (vertices.length >= RELIEF_MAX_VERTICES) break
    if (!Array.isArray(v) || v.length !== 3 || !v.every(fin)) { vertices.push([0, 0, 0]); continue } // keep indices stable
    vertices.push([clamp(v[0], 0, 1), clamp(v[1], 0, 1), clamp(v[2], -RELIEF_MAX_DEPTH, RELIEF_MAX_DEPTH)])
  }
  const faces: number[][] = []
  for (const f of o.faces) {
    if (faces.length >= RELIEF_MAX_FACES) break
    if (!Array.isArray(f) || f.length < 3 || f.length > RELIEF_MAX_LOOP) continue
    if (!f.every((i) => Number.isInteger(i) && i >= 0 && i < vertices.length)) continue
    if (new Set(f).size !== f.length) continue
    faces.push(f.map(Number))
  }
  if (faces.length === 0) return null
  return compactRelief({ vertices, faces })
}

/** Drops vertices no face uses. */
function compactRelief(r: Relief): Relief {
  const map = new Map<number, number>(), vertices: Vec3[] = []
  const faces = r.faces.map((f) => f.map((i) => {
    let n = map.get(i)
    if (n === undefined) { n = vertices.length; map.set(i, n); vertices.push([...r.vertices[i]] as Vec3) }
    return n
  }))
  return { vertices, faces }
}

const rnd = (v: number): number => Math.round(v * 1e6) / 1e6
export function reliefToJson(r: Relief): { vertices: number[][]; faces: number[][] } {
  return { vertices: r.vertices.map((v) => [rnd(v[0]), rnd(v[1]), rnd(v[2])]), faces: r.faces.map((f) => [...f]) }
}
export const reliefEqual = (a: Relief | null, b: Relief | null): boolean => JSON.stringify(a && reliefToJson(a)) === JSON.stringify(b && reliefToJson(b))

// ---- edit space ------------------------------------------------------------------------------------------------------------------------------------
/** The mesh in metres for a repeat `width` x `height` metres large, as a Brush the mesh tools accept. */
export function reliefToBrush(r: Relief, width: number, height: number): Brush {
  return { id: 'relief', vertices: r.vertices.map((v) => [v[0] * width, v[1] * height, v[2]] as Vec3), faces: r.faces.map((loop, id) => ({ id, loop: [...loop] })) }
}

/**
 * Back to the stored form. Positions are clamped into the repeat (x, y in 0..1, |z| <= RELIEF_MAX_DEPTH), snapped off numeric noise, unused vertices dropped
 * and faces renumbered in order.
 */
export function brushToRelief(b: Brush, width: number, height: number): Relief {
  const w = Math.max(width, 1e-6), h = Math.max(height, 1e-6)
  const vertices = b.vertices.map((v) => [clamp(v[0] / w, 0, 1), clamp(v[1] / h, 0, 1), clamp(v[2], -RELIEF_MAX_DEPTH, RELIEF_MAX_DEPTH)] as Vec3)
  return compactRelief({ vertices, faces: b.faces.map((f) => [...f.loop]) })
}

/** Keeps every vertex inside the repeat (the mesh must not leave its tile) and within the depth limit. Faces that stop being planar are split. */
export function confineBrush(b: Brush, width: number, height: number): Brush {
  const out = b.vertices.some((v) => v[0] < -EPS || v[0] > width + EPS || v[1] < -EPS || v[1] > height + EPS || Math.abs(v[2]) > RELIEF_MAX_DEPTH)
  if (!out) return b
  const r = transformVerts(b, b.vertices.map((_, i) => i), (p) => [clamp(p[0], 0, width), clamp(p[1], 0, height), clamp(p[2], -RELIEF_MAX_DEPTH, RELIEF_MAX_DEPTH)])
  return 'error' in r ? b : r.brush
}

/** A mesh left in a state the tools cannot continue from (no faces) is replaced by the flat quad. */
export const ensureBrush = (b: Brush, width: number, height: number): Brush => (b.faces.length > 0 ? b : reliefToBrush(flatRelief(), width, height))

// ---- checks ----------------------------------------------------------------------------------------------------------------------------------------
export interface ReliefReport {
  vertices: number; faces: number; triangles: number
  /** deepest sink (<= 0) and highest rise (>= 0), metres */
  minZ: number; maxZ: number
  warnings: string[]
}

/** Observations for the editor's status line. The mesh stays usable with warnings; they say why it may look wrong when applied. */
export function reliefReport(b: Brush, width: number, height: number): ReliefReport {
  const warnings: string[] = []
  let triangles = 0, minZ = 0, maxZ = 0
  for (const f of b.faces) triangles += Math.max(0, f.loop.length - 2)
  for (const v of b.vertices) { minZ = Math.min(minZ, v[2]); maxZ = Math.max(maxZ, v[2]) }
  const tol = 1e-4
  const onBorder = (v: Vec3): boolean => Math.abs(v[0]) < tol || Math.abs(v[0] - width) < tol || Math.abs(v[1]) < tol || Math.abs(v[1] - height) < tol
  const t = buildTopology(b)
  let openInside = 0, nonManifold = 0, raisedBorder = 0
  for (const e of t.edges.values()) {
    if (e.faces.length > 2) nonManifold++
    if (e.faces.length === 1) {
      const p = b.vertices[e.a], q = b.vertices[e.b]
      const sameSide = (Math.abs(p[0]) < tol && Math.abs(q[0]) < tol) || (Math.abs(p[0] - width) < tol && Math.abs(q[0] - width) < tol) ||
        (Math.abs(p[1]) < tol && Math.abs(q[1]) < tol) || (Math.abs(p[1] - height) < tol && Math.abs(q[1] - height) < tol)
      if (!sameSide) openInside++
    }
  }
  for (const v of b.vertices) if (onBorder(v) && Math.abs(v[2]) > tol) raisedBorder++
  if (openInside > 0) warnings.push(`${openInside} open edge(s) inside the tile: the surface behind would show through`)
  if (raisedBorder > 0) warnings.push(`${raisedBorder} vertex(es) on the tile border are not at depth 0: neighbouring repeats will not join`)
  if (nonManifold > 0) warnings.push(`${nonManifold} edge(s) shared by more than two faces`)
  let backFacing = 0
  for (const f of b.faces) { const n = faceNormal(b, f); if (n[2] < -0.5) backFacing++ }
  if (backFacing > 0) warnings.push(`${backFacing} face(s) look into the surface (flip them if they should face out)`)
  // the tile should be covered: the projected area of all faces must equal the tile
  let area = 0
  for (const f of b.faces) for (const [i, j, k] of triangulateFace(b, f)) {
    const a = b.vertices[i], c = b.vertices[j], d = b.vertices[k]
    area += Math.abs((c[0] - a[0]) * (d[1] - a[1]) - (d[0] - a[0]) * (c[1] - a[1])) / 2
  }
  const total = width * height
  if (total > 0 && area < total * 0.999) warnings.push(`The mesh covers ${Math.round((area / total) * 100)}% of the tile when seen from the front`)
  return { vertices: b.vertices.length, faces: b.faces.length, triangles, minZ, maxZ, warnings }
}

// ---- drawing data ----------------------------------------------------------------------------------------------------------------------------------
/** Interleaved triangles for the viewer: position (3), normal (3), picture uv (2). The uv is the position across the repeat, v down (image convention). */
export function reliefTriangles(b: Brush, width: number, height: number): Float32Array {
  const out: number[] = []
  const w = Math.max(width, 1e-6), h = Math.max(height, 1e-6)
  for (const f of b.faces) {
    const n = faceNormal(b, f)
    for (const [i, j, k] of triangulateFace(b, f))
      for (const vi of [i, j, k]) { const p = b.vertices[vi]; out.push(p[0], p[1], p[2], n[0], n[1], n[2], p[0] / w, 1 - p[1] / h) }
  }
  return new Float32Array(out)
}

/** Edge list for line drawing (each edge once). */
export function reliefEdgeLines(b: Brush): number[] {
  const out: number[] = []
  for (const e of buildTopology(b).edges.values()) out.push(...b.vertices[e.a], ...b.vertices[e.b])
  return out
}

/** Selected vertices keep their meaning when a tool returns a new mesh: used by tests and the editor to re-select by position. */
export const selectAllVerts = (b: Brush) => selFromVerts(b, b.vertices.map((_, i) => i))

/** Cleans a mesh after any edit: confined to the tile, split into planar faces. */
export function settle(b: Brush, width: number, height: number): Brush {
  const c = confineBrush(b, width, height)
  return finish(c, { verts: [], edges: [], faces: [] }).brush
}

// ---- starting points -------------------------------------------------------------------------------------------------------------------------------
export type PresetKind = 'flat' | 'recess' | 'raise' | 'window'
export const PRESETS: { id: PresetKind; label: string; hint: string }[] = [
  { id: 'flat', label: 'Flat', hint: 'One quad at depth 0' },
  { id: 'recess', label: 'Recessed panel', hint: 'The middle sinks into the surface' },
  { id: 'raise', label: 'Raised panel', hint: 'The middle stands out of the surface' },
  { id: 'window', label: 'Window', hint: 'A recessed frame with a pane set slightly forward' },
]

/** A preset relief for a repeat `width` x `height` metres large; depths scale with the smaller side so it looks right on any texture. */
export function buildPreset(kind: PresetKind, width: number, height: number): Relief {
  let b = reliefToBrush(flatRelief(), width, height)
  if (kind === 'flat') return brushToRelief(b, width, height)
  const s = Math.max(1e-3, Math.min(width, height)), depth = Math.min(0.25, s * 0.08)
  let sel: ReturnType<typeof selFromVerts> = { verts: [], edges: [], faces: [0] }
  const run = (fn: (b: Brush, faces: number[]) => OpResult): void => {
    const r = fn(b, sel.faces)
    if (!isError(r)) { b = r.brush; sel = r.sel }
  }
  const frame = kind === 'window' ? 0.1 : 0.12
  run((x, f) => insetFaces(x, f, s * frame))
  if (kind === 'recess') run((x, f) => extrudeFaces(x, f, -depth))
  else if (kind === 'raise') run((x, f) => extrudeFaces(x, f, depth))
  else {
    run((x, f) => extrudeFaces(x, f, -depth))
    run((x, f) => insetFaces(x, f, s * 0.07))
    run((x, f) => extrudeFaces(x, f, depth * 0.5))
  }
  return brushToRelief(b, width, height)
}
