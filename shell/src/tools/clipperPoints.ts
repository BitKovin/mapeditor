// Placing and dragging clipper points. Pure helpers; the viewport owns the state.
import type { Ray } from '../render/camera'
import type { Vec3 } from '../model/math'
import { dot, length, sub } from '../model/math'
import type { Project } from './meshPick'

/** Where the ray meets the plane through `planePoint` with normal `n`; null when parallel or behind the origin. */
export function rayPlaneHit(ray: Ray, planePoint: Vec3, n: Vec3): Vec3 | null {
  const den = dot(ray.dir, n)
  if (Math.abs(den) < 1e-9) return null
  const t = dot(sub(planePoint, ray.origin), n) / den
  if (!Number.isFinite(t)) return null
  return [ray.origin[0] + ray.dir[0] * t, ray.origin[1] + ray.dir[1] * t, ray.origin[2] + ray.dir[2] * t]
}

const snap1 = (x: number, step: number): number => (step > 0 ? Math.round(x / step) * step : x)

/** Snaps every coordinate to the grid, except `keepAxis` (0..2) which is left alone (the depth axis of an orthographic drag). */
export function snapPoint(p: Vec3, step: number, keepAxis?: number): Vec3 {
  return p.map((x, i) => (i === keepAxis ? x : snap1(x, step))) as Vec3
}

/** Index of the clip point whose screen position is within `radius` px of (mx, my), nearest first; null if none. */
export function hitClipPoint(pts: { p: Vec3 }[], project: Project, mx: number, my: number, radius = 10): number | null {
  let best: number | null = null, bd = radius
  pts.forEach(({ p }, i) => {
    const s = project(p)
    if (!s) return
    const d = Math.hypot(s.x - mx, s.y - my)
    if (d <= bd) { bd = d; best = i }
  })
  return best
}

/** True when the new position would coincide with another clip point (a degenerate plane). */
export const clashes = (pts: { p: Vec3 }[], skip: number, q: Vec3): boolean => pts.some((x, i) => i !== skip && length(sub(x.p, q)) < 1e-9)

/** The world axis (0..2) an orthographic view looks along, or undefined for a perspective view. */
export const depthAxis = (forward: Vec3 | null): number | undefined => {
  if (!forward) return undefined
  const a = [0, 1, 2].find((i) => Math.abs(forward[i]) > 0.999)
  return a
}
