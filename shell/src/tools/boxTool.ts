// Box-draw tool math (pure). Interaction, in the standard editor way:
//   1. LMB down on a surface (or on an imaginary plane if the ray hits nothing) and drag:
//      draws a rectangle on that plane.
//   2. Release: move the mouse to set the height perpendicular to the plane, LMB again to commit.
// Shift keeps proportions: phase 1 makes the rectangle a square; phase 2 makes the height equal
// to the square's side (a cube). Esc cancels. Optional grid snapping.
import type { Brush } from '../model/document'
import { signedVolume } from '../model/geometry'
import { Vec3, add, cross, dot, normalize, scale, snap, sub } from '../model/math'
import type { Ray } from '../render/camera'

export interface DrawPlane { origin: Vec3; normal: Vec3; u: Vec3; v: Vec3 }

const AXES: Vec3[] = [[1, 0, 0], [0, 1, 0], [0, 0, 1]]

/** Plane through `point` with the given normal. Axis-aligned normals get world-axis basis vectors. */
export function planeThrough(point: Vec3, normal: Vec3): DrawPlane {
  const n = normalize(normal)
  const k = [0, 1, 2].find((i) => Math.abs(Math.abs(n[i]) - 1) < 1e-6)
  if (k !== undefined) {
    const [ui, vi] = [(k + 1) % 3, (k + 2) % 3]
    // (u, v, n) right-handed so the box builds with consistent winding
    return { origin: point, normal: n, u: AXES[ui], v: AXES[vi] }
  }
  const helper: Vec3 = Math.abs(n[1]) < 0.9 ? [0, 1, 0] : [1, 0, 0]
  const u = normalize(cross(helper, n))
  return { origin: point, normal: n, u, v: cross(n, u) }
}

/**
 * The plane to draw on when the ray hits no geometry: the world ground (y = 0) if the ray reaches
 * it within `maxDistance`, otherwise a horizontal plane `distance` metres out along the ray.
 */
export function fallbackPlane(r: Ray, distance = 10, maxDistance = 200): DrawPlane {
  if (r.dir[1] < -1e-6) {
    const t = -r.origin[1] / r.dir[1]
    if (t > 0 && t < maxDistance) return planeThrough(add(r.origin, scale(r.dir, t)), [0, 1, 0])
  }
  return planeThrough(add(r.origin, scale(r.dir, distance)), [0, 1, 0])
}

export function rayPlane(r: Ray, p: DrawPlane): Vec3 | null {
  const d = dot(p.normal, r.dir)
  if (Math.abs(d) < 1e-9) return null
  const t = dot(p.normal, sub(p.origin, r.origin)) / d
  return t > 0 ? add(r.origin, scale(r.dir, t)) : null
}

/**
 * Snaps a point lying on `plane` to the world grid measured along the plane's own axes (u, v) from the world
 * origin. For the usual axis-aligned planes that is exactly "snap the two in-plane world coordinates"; for a
 * slanted surface the start point still lands on a grid line of that surface. The offset along the normal
 * is left alone (the point stays on the plane).
 */
export function snapToPlane(p: Vec3, plane: DrawPlane, step: number): Vec3 {
  if (step <= 0) return p
  const a = dot(p, plane.u), b = dot(p, plane.v)
  const da = snap(a, step) - a, db = snap(b, step) - b
  return [p[0] + plane.u[0] * da + plane.v[0] * db, p[1] + plane.u[1] * da + plane.v[1] * db, p[2] + plane.u[2] * da + plane.v[2] * db]
}

export interface Rect { origin: Vec3; du: number; dv: number }

/** Rectangle from the drag start to the current point. `square` (Shift) forces |du| = |dv|. */
export function rectFromDrag(plane: DrawPlane, start: Vec3, current: Vec3, square: boolean): Rect {
  const d = sub(current, start)
  let du = dot(d, plane.u), dv = dot(d, plane.v)
  if (square) {
    const s = Math.max(Math.abs(du), Math.abs(dv))
    du = (du < 0 ? -1 : 1) * s
    dv = (dv < 0 ? -1 : 1) * s
  }
  return { origin: start, du, dv }
}

/** Height for phase 2. `raw` = signed distance of the mouse along the normal. */
export function heightFor(raw: number, rect: Rect, cube: boolean, step: number): number {
  if (cube) return (raw < 0 ? -1 : 1) * Math.max(Math.abs(rect.du), Math.abs(rect.dv))
  return step > 0 ? snap(raw, step) : raw
}

export const MIN_SIZE = 1e-4

/** An oriented box brush: the rectangle extruded `height` along the plane normal. Null if degenerate. */
export function buildBrush(id: string, plane: DrawPlane, rect: Rect, height: number, texture = 'dev/grey'): Brush | null {
  if (Math.abs(rect.du) < MIN_SIZE || Math.abs(rect.dv) < MIN_SIZE || Math.abs(height) < MIN_SIZE) return null
  const [a0, a1] = [Math.min(0, rect.du), Math.max(0, rect.du)]
  const [b0, b1] = [Math.min(0, rect.dv), Math.max(0, rect.dv)]
  const at = (a: number, b: number, h: number): Vec3 =>
    add(add(add(rect.origin, scale(plane.u, a)), scale(plane.v, b)), scale(plane.normal, h))
  const clean = (v: Vec3): Vec3 => [Math.round(v[0] * 1e9) / 1e9, Math.round(v[1] * 1e9) / 1e9, Math.round(v[2] * 1e9) / 1e9] // no 3.0000000000000004 in map files
  const vertices: Vec3[] = [
    at(a0, b0, 0), at(a1, b0, 0), at(a1, b1, 0), at(a0, b1, 0),
    at(a0, b0, height), at(a1, b0, height), at(a1, b1, height), at(a0, b1, height),
  ].map(clean)
  const loops = [[0, 3, 2, 1], [4, 5, 6, 7], [0, 4, 7, 3], [1, 2, 6, 5], [0, 1, 5, 4], [3, 7, 6, 2]]
  let brush: Brush = { id, op: 'add', vertices, faces: loops.map((loop, i) => ({ id: i, loop, texture })) }
  if (signedVolume(brush) < 0) brush = { ...brush, faces: brush.faces.map((f) => ({ ...f, loop: [...f.loop].reverse() })) }
  return brush
}

/** Wire lines (pairs of points, flat array) previewing the rectangle, or the box once `height` is set. */
export function previewLines(plane: DrawPlane, rect: Rect, height: number | null): number[] {
  const brush = buildBrush('p', plane, rect, height ?? 1e-3)
  if (!brush) return []
  const out: number[] = []
  const edges = height === null ? [[0, 1], [1, 2], [2, 3], [3, 0]] : [[0, 1], [1, 2], [2, 3], [3, 0], [4, 5], [5, 6], [6, 7], [7, 4], [0, 4], [1, 5], [2, 6], [3, 7]]
  for (const [a, b] of edges) out.push(...brush.vertices[a], ...brush.vertices[b])
  return out
}
