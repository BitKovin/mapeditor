// Fast Texture Tool (REQUIREMENTS §6 B5, Scythe's "Fast Texture Tool"): a UV editor for the selected faces of ONE brush. Everything here is pure; the window
// (ui/fastTextureTool.ts) only draws and turns pointer movement into the transforms below.
//
// A face's UV is one affine map of the plane (its Projection, model/uv.ts), so every edit is a 2D transform of UV space applied to the whole selection as ONE
// island (the faces keep their positions relative to each other, so a texture that continues over an edge keeps continuing). The result is written back as a
// Projection of kind 'free' with a frozen frame (`projectionFromAffine`), exactly like Flow, so the Surface Inspector keeps working on the face afterwards.
//
// UV space as drawn: u to the right, v DOWN (image convention, like the stored UVs). Angles are therefore clockwise on screen.
import type { Brush, Face } from './document'
import { faceNormal } from './geometry'
import { flowFaces } from './flow'
import { projectOrtho, projectAxial } from './surface'
import { Projection, projectFace, projectionFromAffine, projectionOf } from './uv'
import { Vec2, naturalSize, texEnv } from './texprops'
import { Aff, faceAffine } from './uvmesh'

const evalAff = (a: Aff, p: readonly number[]): number => a[0] * p[0] + a[1] * p[1] + a[2] * p[2] + a[3]

/** (u, v) -> (a u + b v + tx, c u + d v + ty) */
export type Xform = [number, number, number, number, number, number]

export const IDENTITY: Xform = [1, 0, 0, 1, 0, 0]
export const translate = (du: number, dv: number): Xform => [1, 0, 0, 1, du, dv]
export const scaleAbout = (sx: number, sy: number, p: Vec2): Xform => [sx, 0, 0, sy, p[0] - sx * p[0], p[1] - sy * p[1]]
export function rotateAbout(deg: number, p: Vec2): Xform {
  const r = (deg * Math.PI) / 180, c = Math.cos(r), s = Math.sin(r)
  return [c, -s, s, c, p[0] - c * p[0] + s * p[1], p[1] - s * p[0] - c * p[1]]
}
/** The mirror feature: flips the island left-right ('u') or top-bottom ('v') about `at` (a u or v value). */
export const mirrorAbout = (axis: 'u' | 'v', at: number): Xform => (axis === 'u' ? scaleAbout(-1, 1, [at, 0]) : scaleAbout(1, -1, [0, at]))
/** `second` after `first`. */
export function compose(second: Xform, first: Xform): Xform {
  const [a2, b2, c2, d2, x2, y2] = second, [a1, b1, c1, d1, x1, y1] = first
  return [a2 * a1 + b2 * c1, a2 * b1 + b2 * d1, c2 * a1 + d2 * c1, c2 * b1 + d2 * d1, a2 * x1 + b2 * y1 + x2, c2 * x1 + d2 * y1 + y2]
}
export const applyXform = (t: Xform, p: Vec2): Vec2 => [t[0] * p[0] + t[1] * p[1] + t[4], t[2] * p[0] + t[3] * p[1] + t[5]]

// ---- reading an island ----------------------------------------------------------------------------------------------------------------------------
const faceById = (b: Brush, id: number): Face | undefined => b.faces.find((f) => f.id === id)

/** The UV corners of a face (what the texture does at each corner of its loop). */
export function faceUvPolygon(b: Brush, f: Face): Vec2[] {
  const m = faceAffine(b, f)
  return f.loop.map((i) => [evalAff(m.u, b.vertices[i]), evalAff(m.v, b.vertices[i])] as Vec2)
}

export interface UvBounds { min: Vec2; max: Vec2 }
export function polygonsBounds(polys: Vec2[][]): UvBounds | null {
  let u0 = Infinity, v0 = Infinity, u1 = -Infinity, v1 = -Infinity
  for (const p of polys) for (const [u, v] of p) { u0 = Math.min(u0, u); u1 = Math.max(u1, u); v0 = Math.min(v0, v); v1 = Math.max(v1, v) }
  return Number.isFinite(u0) ? { min: [u0, v0], max: [u1, v1] } : null
}
export const boundsCentre = (b: UvBounds): Vec2 => [(b.min[0] + b.max[0]) / 2, (b.min[1] + b.max[1]) / 2]

export function islandPolygons(b: Brush, ids: number[]): { id: number; poly: Vec2[] }[] {
  const out: { id: number; poly: Vec2[] }[] = []
  for (const id of ids) { const f = faceById(b, id); if (f) out.push({ id, poly: faceUvPolygon(b, f) }) }
  return out
}

/**
 * The shape a face would have in UV space at the texture's own pixels per meter: its extent in metres (along the face's current u and v directions) divided
 * by the natural size. Scythe draws this as the blue rectangle: when the island's polygon matches it, the texel density is as authored.
 */
export function idealSize(b: Brush, f: Face): Vec2 {
  const poly = faceUvPolygon(b, f)
  const info = texEnv.info(f.texture ?? 'dev/grey')
  const nat = info ? naturalSize(info) : ([1, 1] as Vec2)
  // metres along u / v: the world length of the polygon's u and v extents (world distance per unit u or v from the face's own map)
  const m = faceAffine(b, f)
  const gu = Math.hypot(m.u[0], m.u[1], m.u[2]), gv = Math.hypot(m.v[0], m.v[1], m.v[2])
  const b2 = polygonsBounds([poly])
  if (!b2 || gu < 1e-12 || gv < 1e-12) return [1, 1]
  // extent in repeats = extent in metres / natural size, and extent in metres = (uv extent) / (repeats per metre)
  return [((b2.max[0] - b2.min[0]) / gu) / nat[0], ((b2.max[1] - b2.min[1]) / gv) / nat[1]]
}

// ---- applying a transform ---------------------------------------------------------------------------------------------------------------------------
/**
 * Applies `t` to the UV map of every face in `ids` (all of one brush). A face whose result would be degenerate (a collapsed axis) is left as it was.
 * Returns the new brush and how many faces changed; the same brush object when nothing did.
 */
export function transformFaces(b: Brush, ids: number[], t: Xform): { brush: Brush; changed: number } {
  const set = new Set(ids)
  let changed = 0
  const faces = b.faces.map((f) => {
    if (!set.has(f.id)) return f
    const m = faceAffine(b, f)
    const [a, bb, c, d, tx, ty] = t
    const gu: [number, number, number] = [a * m.u[0] + bb * m.v[0], a * m.u[1] + bb * m.v[1], a * m.u[2] + bb * m.v[2]]
    const gv: [number, number, number] = [c * m.u[0] + d * m.v[0], c * m.u[1] + d * m.v[1], c * m.u[2] + d * m.v[2]]
    const du = a * m.u[3] + bb * m.v[3] + tx, dv = c * m.u[3] + d * m.v[3] + ty
    const p = projectionFromAffine(faceNormal(b, f), gu, gv, du, dv)
    if (!p) return f
    changed++
    return { ...f, projection: p, uvs: projectFace(b, f, p) }
  })
  return changed ? { brush: { ...b, faces }, changed } : { brush: b, changed: 0 }
}

/** Rotation that makes the UV edge a -> b horizontal or vertical (whichever is nearer), about its midpoint. Null for a zero-length edge. */
export function alignEdgeXform(a: Vec2, b: Vec2): Xform | null {
  const du = b[0] - a[0], dv = b[1] - a[1]
  if (Math.hypot(du, dv) < 1e-12) return null
  const phi = (Math.atan2(dv, du) * 180) / Math.PI
  const delta = Math.round(phi / 90) * 90 - phi
  return Math.abs(delta) < 1e-9 ? IDENTITY : rotateAbout(delta, [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2])
}

// ---- unwrap modes ---------------------------------------------------------------------------------------------------------------------------------
export type UnwrapMode = 'follow' | 'ortho' | 'axial'
/**
 * Re-projects the faces. `follow` = Follow Quads (Flow): the first id's UVs continue over the shared edges onto the others, along the shortest chain of faces
 * (REQUIREMENTS §6 Flow); `ortho` / `axial` project each face on its own (Surface Inspector > Project).
 */
export function unwrapFaces(b: Brush, ids: number[], mode: UnwrapMode): { brush: Brush; changed: number } {
  if (mode === 'follow') {
    if (ids.length < 2) return { brush: b, changed: 0 }
    const r = flowFaces(b, ids[0], ids.slice(1))
    return r ? { brush: r.brush, changed: r.flowed.length } : { brush: b, changed: 0 }
  }
  const set = new Set(ids)
  let changed = 0
  const faces = b.faces.map((f) => {
    if (!set.has(f.id)) return f
    const cur = projectionOf(f)
    const p: Projection = mode === 'ortho' ? projectOrtho(b, f, cur) : projectAxial(cur)
    changed++
    return { ...f, projection: p, uvs: projectFace(b, f, p) }
  })
  return changed ? { brush: { ...b, faces }, changed } : { brush: b, changed: 0 }
}

// ---- direct manipulation ---------------------------------------------------------------------------------------------------------------------------
export type HandleId = 'nw' | 'n' | 'ne' | 'e' | 'se' | 's' | 'sw' | 'w' | 'rotate'
export const HANDLES: HandleId[] = ['nw', 'n', 'ne', 'e', 'se', 's', 'sw', 'w']

/** Where a handle sits in UV space (the rotate handle is above the top edge by `rotateGap`). */
export function handlePoint(bounds: UvBounds, h: HandleId, rotateGap = 0): Vec2 {
  const [u0, v0] = bounds.min, [u1, v1] = bounds.max, um = (u0 + u1) / 2, vm = (v0 + v1) / 2
  switch (h) {
    case 'nw': return [u0, v0]; case 'n': return [um, v0]; case 'ne': return [u1, v0]; case 'e': return [u1, vm]
    case 'se': return [u1, v1]; case 's': return [um, v1]; case 'sw': return [u0, v1]; case 'w': return [u0, vm]
    case 'rotate': return [um, v0 - rotateGap]
  }
}

export interface DragOpts { /** snap step in repeats for move and scale; 0 = none */ snap: number; /** keep the aspect ratio of a scale; rotate in 15 degree steps */ shift: boolean }

const snapTo = (v: number, step: number): number => (step > 0 ? Math.round(v / step) * step : v)
const keepSign = (v: number, min = 0.01): number => (Math.abs(v) < min ? (v < 0 ? -min : min) : v)

/**
 * The transform a drag means. `start` = pointer in UV space when the drag began, `cur` = now, `bounds` = the island's bounds at the start.
 * 'move' = the island's body; the others are the handles. Scale handles keep the opposite edge / corner fixed.
 */
export function dragXform(kind: 'move' | HandleId, start: Vec2, cur: Vec2, bounds: UvBounds, o: DragOpts): Xform {
  if (kind === 'move') {
    // snap the island's top-left corner, not the pointer, so the island lands on the grid whatever was grabbed
    const du = snapTo(bounds.min[0] + (cur[0] - start[0]), o.snap) - bounds.min[0], dv = snapTo(bounds.min[1] + (cur[1] - start[1]), o.snap) - bounds.min[1]
    return translate(du, dv)
  }
  const c = boundsCentre(bounds)
  if (kind === 'rotate') {
    const a0 = Math.atan2(start[1] - c[1], start[0] - c[0]), a1 = Math.atan2(cur[1] - c[1], cur[0] - c[0])
    let deg = ((a1 - a0) * 180) / Math.PI
    if (o.shift) deg = Math.round(deg / 15) * 15
    return rotateAbout(deg, c)
  }
  const opposite: Record<Exclude<HandleId, 'rotate'>, HandleId> = { nw: 'se', n: 's', ne: 'sw', e: 'w', se: 'nw', s: 'n', sw: 'ne', w: 'e' }
  const pivot = handlePoint(bounds, opposite[kind])
  const h0 = handlePoint(bounds, kind)
  const moveU = kind !== 'n' && kind !== 's', moveV = kind !== 'e' && kind !== 'w'
  const cu = snapTo(h0[0] + (cur[0] - start[0]), o.snap), cv = snapTo(h0[1] + (cur[1] - start[1]), o.snap)
  let sx = moveU && Math.abs(h0[0] - pivot[0]) > 1e-12 ? (cu - pivot[0]) / (h0[0] - pivot[0]) : 1
  let sy = moveV && Math.abs(h0[1] - pivot[1]) > 1e-12 ? (cv - pivot[1]) / (h0[1] - pivot[1]) : 1
  sx = keepSign(sx); sy = keepSign(sy)
  if (o.shift && moveU && moveV) { const k = Math.abs(sx) > Math.abs(sy) ? sx : sy; sx = k; sy = k }
  return scaleAbout(sx, sy, pivot)
}

/** Which handle (or the body) is under a UV point; `tol` is the pick radius in UV units (the window passes pixels / scale). */
export function hitTest(bounds: UvBounds, polys: Vec2[][], p: Vec2, tol: number, rotateGap: number): 'move' | HandleId | null {
  for (const h of [...HANDLES, 'rotate' as HandleId]) {
    const q = handlePoint(bounds, h, rotateGap)
    if (Math.hypot(q[0] - p[0], q[1] - p[1]) <= tol) return h
  }
  for (const poly of polys) if (pointInPolygon(p, poly)) return 'move'
  return null
}

export function pointInPolygon(p: Vec2, poly: Vec2[]): boolean {
  let inside = false
  for (let i = 0, j = poly.length - 1; i < poly.length; j = i++) {
    const [xi, yi] = poly[i], [xj, yj] = poly[j]
    if (yi > p[1] !== yj > p[1] && p[0] < ((xj - xi) * (p[1] - yi)) / (yj - yi) + xi) inside = !inside
  }
  return inside
}

/** The polygon edge nearest to a UV point within `tol`, as its two UV endpoints (for Align edge). */
export function nearestEdge(polys: Vec2[][], p: Vec2, tol: number): [Vec2, Vec2] | null {
  let best: [Vec2, Vec2] | null = null, bd = tol
  for (const poly of polys) for (let i = 0; i < poly.length; i++) {
    const a = poly[i], b = poly[(i + 1) % poly.length]
    const dx = b[0] - a[0], dy = b[1] - a[1], l2 = dx * dx + dy * dy
    if (l2 < 1e-18) continue
    const t = Math.max(0, Math.min(1, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / l2))
    const d = Math.hypot(p[0] - (a[0] + t * dx), p[1] - (a[1] + t * dy))
    if (d <= bd) { bd = d; best = [a, b] }
  }
  return best
}
