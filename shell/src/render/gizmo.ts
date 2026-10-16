// Translate gizmo math: hit-testing the three axis handles and turning a mouse ray into a
// distance along an axis. Rendering lives in renderer.ts.
import { Vec3, add, dot, length, scale, snap, sub } from '../model/math'
import type { Ray } from './camera'

export type Axis = 0 | 1 | 2
export const AXIS_DIR: Vec3[] = [[1, 0, 0], [0, 1, 0], [0, 0, 1]]

/** Parameters (s along the ray, t along the axis line) of the closest points between two lines. */
function closestParams(r: Ray, o: Vec3, d: Vec3): { s: number; t: number } | null {
  const w = sub(r.origin, o)
  const a = dot(r.dir, r.dir), b = dot(r.dir, d), c = dot(d, d)
  const dd = dot(r.dir, w), e = dot(d, w)
  const denom = a * c - b * b
  if (Math.abs(denom) < 1e-9) return null // ray parallel to the axis
  return { s: (b * e - c * dd) / denom, t: (a * e - b * dd) / denom }
}

/** Signed distance along the axis (from `pivot`) of the point closest to the mouse ray. */
export function axisDistance(r: Ray, pivot: Vec3, axis: Axis): number | null {
  return closestParams(r, pivot, AXIS_DIR[axis])?.t ?? null
}

/** Same, for an arbitrary unit direction (used by the box tool's height phase). */
export function lineDistance(r: Ray, origin: Vec3, dir: Vec3): number | null {
  return closestParams(r, origin, dir)?.t ?? null
}

/** Which axis handle (length `len`, pick radius `tol`, both in metres) is under the ray. */
export function hitAxis(r: Ray, pivot: Vec3, len: number, tol: number): Axis | null {
  let best: { axis: Axis; dist: number } | null = null
  for (const axis of [0, 1, 2] as Axis[]) {
    const p = closestParams(r, pivot, AXIS_DIR[axis])
    if (!p || p.s < 0) continue
    const t = Math.max(0, Math.min(len, p.t)) // clamp to the visible handle
    const onAxis = add(pivot, scale(AXIS_DIR[axis], t))
    const onRay = add(r.origin, scale(r.dir, p.s))
    const dist = length(sub(onAxis, onRay))
    if (dist <= tol && (!best || dist < best.dist)) best = { axis, dist }
  }
  return best ? best.axis : null
}

/** Is the single handle from `pivot` along unit `dir` (length `len`) under the ray? */
export function hitDir(r: Ray, pivot: Vec3, dir: Vec3, len: number, tol: number): boolean {
  const p = closestParams(r, pivot, dir)
  if (!p || p.s < 0) return false
  const t = Math.max(0, Math.min(len, p.t))
  return length(sub(add(pivot, scale(dir, t)), add(r.origin, scale(r.dir, p.s)))) <= tol
}

export type GizmoMode = 'move' | 'rotate' | 'scale'

/** The point where the ray meets the plane through `pivot` perpendicular to `axis` (null when parallel / behind). */
function planeHit(r: Ray, pivot: Vec3, axis: Axis): Vec3 | null {
  const n = AXIS_DIR[axis], denom = dot(r.dir, n)
  if (Math.abs(denom) < 1e-9) return null
  const t = dot(sub(pivot, r.origin), n) / denom
  return t < 0 ? null : add(r.origin, scale(r.dir, t))
}
export const ringBasis = (axis: Axis): [Vec3, Vec3] => (axis === 0 ? [[0, 1, 0], [0, 0, 1]] : axis === 1 ? [[0, 0, 1], [1, 0, 0]] : [[1, 0, 0], [0, 1, 0]])

/** Which rotation ring (radius `radius`, tolerance `tol`) is under the ray: the one whose circle the ray passes closest to. */
export function hitRing(r: Ray, pivot: Vec3, radius: number, tol: number): Axis | null {
  let best: { axis: Axis; err: number } | null = null
  for (const axis of [0, 1, 2] as Axis[]) {
    const p = planeHit(r, pivot, axis)
    if (!p) continue
    const err = Math.abs(length(sub(p, pivot)) - radius)
    if (err <= tol && (!best || err < best.err)) best = { axis, err }
  }
  return best ? best.axis : null
}

/** Angle (radians) of the ray's hit on the ring plane, measured about `axis` (right-handed). */
export function ringAngle(r: Ray, pivot: Vec3, axis: Axis): number | null {
  const p = planeHit(r, pivot, axis)
  if (!p) return null
  const [u, v] = ringBasis(axis), q = sub(p, pivot)
  return Math.atan2(dot(q, v), dot(q, u))
}

/** Smallest signed difference between two angles. */
export const angleDelta = (a: number, b: number): number => { let d = b - a; while (d > Math.PI) d -= 2 * Math.PI; while (d < -Math.PI) d += 2 * Math.PI; return d }

/** Points of the ring (flat line endpoints) for drawing. */
export function ringLines(pivot: Vec3, axis: Axis, radius: number, segments = 48): number[] {
  const [u, v] = ringBasis(axis), out: number[] = []
  const at = (k: number): Vec3 => { const a = (k / segments) * Math.PI * 2; return add(pivot, add(scale(u, Math.cos(a) * radius), scale(v, Math.sin(a) * radius))) }
  for (let k = 0; k < segments; k++) out.push(...at(k), ...at(k + 1))
  return out
}

/** Handle length that keeps the gizmo a constant apparent size. */
export const gizmoLength = (cameraDistance: number): number => cameraDistance * 0.18

/** Movement along `axis` for a drag that started at `startT`, snapped to `step` (0 = free). */
export function dragDelta(r: Ray, pivot: Vec3, axis: Axis, startT: number, step: number): Vec3 {
  const t = axisDistance(r, pivot, axis)
  if (t === null) return [0, 0, 0]
  return scale(AXIS_DIR[axis], snap(t - startT, step))
}
