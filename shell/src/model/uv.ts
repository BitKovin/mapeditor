// UV projection (DESIGN.md §9). World-aligned planar projection: pick the axis the face
// normal points along, project onto the other two, then apply rotate -> scale -> shift.
// Orientation is chosen so textures read upright and unmirrored when viewed from outside.
// Faces without stored `uvs` get the default projection at draw time; editing a face's
// projection writes `projection` (the parameters) AND `uvs` (the result, so it round-trips
// through Core and lightmap/export code never needs to know about projections).
import type { Brush, Face } from './document'
import { faceNormal } from './geometry'
import type { Vec3 } from './math'
import { Vec2, naturalSize, texEnv } from './texprops'

/**
 * How the face plane's two texture axes are chosen (REQUIREMENTS §6 B4):
 *   axial  along the dominant world axis (the classic Quake projection; the default)
 *   ortho  axes lie in the face: v follows the face's steepest slope (world up projected into the plane), u is across it
 *   cam    axes are the camera's right / up projected into the face, frozen at the moment the button was pressed
 *   free   axes are an arbitrary in-plane frame computed by an operation (Flow, copy along an edge): the stored affine map made editable
 * `ortho`, `cam` and `free` store their axes in `basis` (unit world vectors [u axis, v axis pointing down the image]), so the mapping never changes when the camera moves.
 */
export type ProjectionKind = 'axial' | 'ortho' | 'cam' | 'free'
export interface Projection { scale: [number, number]; shift: [number, number]; rotate: number; /** u += shear * v after the rotation (in the plane, before scale); 0 / absent = none */ shear?: number; kind?: ProjectionKind; basis?: [Vec3, Vec3] }
/** `scale` = metres per texture repeat. `shift` in repeats. `rotate` in degrees. Together with `shear` this is the 2x3 affine map of the plane coordinates. */
export const defaultProjection = (): Projection => ({ scale: [2, 2], shift: [0, 0], rotate: 0 })

/** The projection of a face without parameters: the texture at its natural size (pixels / pixels per meter), the fixed 2 m while the texture is not loaded. */
export function defaultProjectionFor(texture: string | undefined): Projection {
  const info = texEnv.info(texture ?? 'dev/grey')
  const d = defaultProjection()
  if (info && info.width > 0 && info.height > 0) { const [w, h] = naturalSize(info); d.scale = [w, h] }
  return d
}

export function projectionOf(f: Face): Projection {
  const p = f.projection as Partial<Projection> | undefined
  const d = defaultProjectionFor(f.texture)
  const pair = (v: unknown, dflt: [number, number]): [number, number] =>
    Array.isArray(v) && v.length === 2 && v.every((x) => typeof x === 'number' && Number.isFinite(x)) ? [v[0], v[1]] : dflt
  const scale = pair(p?.scale, d.scale)
  const out: Projection = {
    scale: [Math.abs(scale[0]) < 1e-6 ? d.scale[0] : scale[0], Math.abs(scale[1]) < 1e-6 ? d.scale[1] : scale[1]],
    shift: pair(p?.shift, d.shift),
    rotate: typeof p?.rotate === 'number' && Number.isFinite(p.rotate) ? p.rotate : 0,
  }
  if (typeof p?.shear === 'number' && Number.isFinite(p.shear) && p.shear !== 0) out.shear = p.shear
  const basis = validBasis(p?.basis)
  if ((p?.kind === 'ortho' || p?.kind === 'cam' || p?.kind === 'free') && basis) { out.kind = p.kind; out.basis = basis }
  return out
}

/** A stored basis: two finite, unit-ish, non-parallel vectors; anything else is ignored (the face falls back to axial). */
function validBasis(b: unknown): [Vec3, Vec3] | null {
  if (!Array.isArray(b) || b.length !== 2) return null
  const ok = (v: unknown): v is Vec3 => Array.isArray(v) && v.length === 3 && v.every((x) => typeof x === 'number' && Number.isFinite(x))
  if (!ok(b[0]) || !ok(b[1])) return null
  const l0 = Math.hypot(...b[0]), l1 = Math.hypot(...b[1])
  if (l0 < 1e-6 || l1 < 1e-6) return null
  const d = (b[0][0] * b[1][0] + b[0][1] * b[1][1] + b[0][2] * b[1][2]) / (l0 * l1)
  return Math.abs(d) > 0.999 ? null : [b[0], b[1]]
}

/** Unrotated in-plane coordinates (a right, b down the image) of a world point for a face normal. */
export function planarCoords(n: Vec3, p: Vec3): [number, number] {
  const [ax, ay, az] = [Math.abs(n[0]), Math.abs(n[1]), Math.abs(n[2])]
  if (ay >= ax && ay >= az) return [Math.sign(n[1] || 1) * p[0], p[2]] // floor/ceiling
  if (ax >= az) return [-Math.sign(n[0] || 1) * p[2], -p[1]]           // X-facing walls
  return [Math.sign(n[2] || 1) * p[0], -p[1]]                          // Z-facing walls
}

const rot = (a: number, b: number, deg: number): [number, number] => {
  const r = (deg * Math.PI) / 180, c = Math.cos(r), s = Math.sin(r)
  return [a * c - b * s, a * s + b * c]
}

/** In-plane coordinates (right, down) of a world point under the projection's kind. */
export function planeCoords(n: Vec3, q: Vec3, p: Projection): [number, number] {
  if (p.basis && (p.kind === 'ortho' || p.kind === 'cam' || p.kind === 'free')) {
    const [u, v] = p.basis
    return [q[0] * u[0] + q[1] * u[1] + q[2] * u[2], q[0] * v[0] + q[1] * v[1] + q[2] * v[2]]
  }
  return planarCoords(n, q)
}

/** The in-plane coordinates after rotation and shear (metres, before scale / shift): what the justify / fit bounds are measured in. */
export function placedCoords(n: Vec3, q: Vec3, p: Projection): [number, number] {
  const [a, c] = rot(...planeCoords(n, q, p), p.rotate)
  return [a + (p.shear ?? 0) * c, c]
}

export function projectFace(b: Brush, f: Face, p: Projection = projectionOf(f)): [number, number][] {
  const n = faceNormal(b, f)
  return f.loop.map((i) => {
    const [a, c] = placedCoords(n, b.vertices[i], p)
    return [a / p.scale[0] + p.shift[0], c / p.scale[1] + p.shift[1]] as [number, number]
  })
}

/** Scale/shift so the texture covers the face exactly once (rotation kept). */
export function fitProjection(b: Brush, f: Face, p: Projection = projectionOf(f)): Projection {
  const n = faceNormal(b, f)
  const pts = f.loop.map((i) => placedCoords(n, b.vertices[i], p))
  const [minA, maxA] = [Math.min(...pts.map((q) => q[0])), Math.max(...pts.map((q) => q[0]))]
  const [minB, maxB] = [Math.min(...pts.map((q) => q[1])), Math.max(...pts.map((q) => q[1]))]
  const sa = Math.max(maxA - minA, 1e-6), sb = Math.max(maxB - minB, 1e-6)
  return { ...p, scale: [sa, sb], shift: [-minA / sa, -minB / sb] }
}

// ---- face-relative placement (the origin of the texture is the face's BOTTOM-LEFT corner) ---------------------------------------------------------
/** Extent of the face along the projection's placed axes (rotation, shear, kind) in metres. */
export function faceBounds(b: Brush, f: Face, p: Projection): { min: Vec2; max: Vec2 } {
  const n = faceNormal(b, f)
  let min: Vec2 = [Infinity, Infinity], max: Vec2 = [-Infinity, -Infinity]
  for (const i of f.loop) {
    const q = placedCoords(n, b.vertices[i], p)
    min = [Math.min(min[0], q[0]), Math.min(min[1], q[1])]; max = [Math.max(max[0], q[0]), Math.max(max[1], q[1])]
  }
  return { min, max }
}

export type JustifyEdge = 'left' | 'right' | 'top' | 'bottom' | 'center'

/** Justify (REQUIREMENTS §6, B2): shift the texture so its left / right / top / bottom edge touches the same edge of the face (the face's extent along the rotated texture axes), or centre it on both axes. Stretch and rotation are kept; mirrored axes work. */
export function justify(b: Brush, f: Face, p: Projection, edge: JustifyEdge): Projection {
  const { min, max } = faceBounds(b, f, p)
  const shift: Vec2 = [p.shift[0], p.shift[1]]
  const apply = (axis: 0 | 1, where: 'start' | 'end' | 'center'): void => {
    const s = p.scale[axis], lo = Math.min(min[axis] / s, max[axis] / s), hi = Math.max(min[axis] / s, max[axis] / s)
    shift[axis] = where === 'start' ? -lo : where === 'end' ? 1 - hi : 0.5 - (lo + hi) / 2
  }
  if (edge === 'left') apply(0, 'start'); else if (edge === 'right') apply(0, 'end')
  else if (edge === 'top') apply(1, 'start'); else if (edge === 'bottom') apply(1, 'end')
  else { apply(0, 'center'); apply(1, 'center') }
  return { ...p, shift }
}

/** Moves the texture so its bottom-left corner touches the face's bottom-left corner (stretch and rotation kept). */
export const bottomLeft = (b: Brush, f: Face, p: Projection): Projection => justify(b, f, justify(b, f, p, 'left'), 'bottom')

export const uvsForDraw = (b: Brush, f: Face): [number, number][] =>
  f.uvs && f.uvs.length === f.loop.length ? f.uvs : projectFace(b, f)

/** UVs for arbitrary world points on a plane with outward normal `n` (derived-geometry fragments). */
export function projectPoints(n: Vec3, pts: Vec3[], p: Projection): [number, number][] {
  return pts.map((q) => {
    const [a, c] = placedCoords(n, q, p)
    return [a / p.scale[0] + p.shift[0], c / p.scale[1] + p.shift[1]] as [number, number]
  })
}

/** projectPoints with the default projection when the parent face is unknown. */
export const planarCoordsProjected = (n: Vec3, pts: Vec3[], p: Projection = defaultProjection()): [number, number][] => projectPoints(n, pts, p)

/** Lightmap texel density multiplier of a face (1 = the level's texels per metre). The Core clamps to the same range (core/brush.h). */
export const LIGHTMAP_SCALE_MIN = 0.0625, LIGHTMAP_SCALE_MAX = 16
export const clampLightmapScale = (v: number): number => Math.min(LIGHTMAP_SCALE_MAX, Math.max(LIGHTMAP_SCALE_MIN, v))
export const lightmapScaleOf = (f: Face): number => (typeof f.lightmapScale === 'number' && Number.isFinite(f.lightmapScale) && f.lightmapScale > 0 ? clampLightmapScale(f.lightmapScale) : 1)

/**
 * The projection parameters of an arbitrary (u, v) map on a face's plane: a frozen in-plane frame (kind 'free') plus scale / shift / rotate / shear, so
 * the Surface Inspector can edit a face whose UVs were produced by an operation (Flow). Exact: projectFace(b, f, result) reproduces the map on the plane.
 * `gu` / `gv` are the gradients of u and v (world vectors; only their in-plane part matters), `du` / `dv` their values at the world origin.
 * null when the map is degenerate (a collapsed axis).
 */
export function projectionFromAffine(n: Vec3, gu: Vec3, gv: Vec3, du: number, dv: number): Projection | null {
  const nl = Math.hypot(...n)
  if (nl < 1e-9) return null
  const nn: Vec3 = [n[0] / nl, n[1] / nl, n[2] / nl]
  // an in-plane frame (right, down) that reads like the axial one: world up projected into the plane gives "down"
  const up: Vec3 = Math.abs(nn[1]) > 0.999 ? [0, 0, -1] : [0, 1, 0]
  const cr = (a: Vec3, b: Vec3): Vec3 => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
  const nrm = (v: Vec3): Vec3 => { const l = Math.hypot(...v); return [v[0] / l, v[1] / l, v[2] / l] }
  const e1 = nrm(cr(up, nn)), upIn = nrm(cr(nn, e1)), e2: Vec3 = [-upIn[0], -upIn[1], -upIn[2]]
  const dt = (a: Vec3, b: Vec3): number => a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
  const m11 = dt(gu, e1), m12 = dt(gu, e2), m21 = dt(gv, e1), m22 = dt(gv, e2)
  const r = Math.hypot(m21, m22)
  if (r < 1e-9) return null
  const c = m22 / r, s = m21 / r
  const A = m11 * c - m12 * s, B = m11 * s + m12 * c
  if (Math.abs(A) < 1e-9) return null
  const out: Projection = { scale: [1 / A, 1 / r], shift: [du, dv], rotate: (Math.atan2(s, c) * 180) / Math.PI, kind: 'free', basis: [e1, e2] }
  if (Math.abs(B / A) > 1e-9) out.shear = B / A
  return out
}
