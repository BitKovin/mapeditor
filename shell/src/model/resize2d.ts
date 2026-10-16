// Radiant-style 2D resize of the selection in an orthographic view: eight handles (4 sides, 4 corners) on the selection's
// bounding rectangle. Dragging one moves its side(s) while the opposite side stays fixed; every point is scaled
// linearly between them, so a rectangle resizes and a slanted brush keeps its shape. Pure functions; the viewport
// supplies pixel deltas and applies the returned point map to the document as it was when the drag began.
import { Vec3 } from './math'
import type { PointMap } from './transform'

/** A screen axis expressed as a world axis: `axis` 0..2 and `sign` +1 when the screen axis points along it. */
export interface ScreenAxis { axis: 0 | 1 | 2; sign: 1 | -1 }
export interface Handle2D { id: string; h: -1 | 0 | 1; v: -1 | 0 | 1; pos: Vec3 }

/** The world axis a (unit, axis-aligned) screen direction points along. */
export function screenAxis(dir: Vec3): ScreenAxis {
  let a = 0
  for (let i = 1; i < 3; i++) if (Math.abs(dir[i]) > Math.abs(dir[a])) a = i
  return { axis: a as 0 | 1 | 2, sign: dir[a] >= 0 ? 1 : -1 }
}

/** The 8 handles around [min, max], positioned on the plane of the bounds' centre (h: -1 left .. 1 right, v: -1 bottom .. 1 top). */
export function handles2D(min: Vec3, max: Vec3, right: ScreenAxis, up: ScreenAxis): Handle2D[] {
  const out: Handle2D[] = []
  const side = (ax: ScreenAxis, k: number): number => {
    const lo = min[ax.axis], hi = max[ax.axis]
    return k === 0 ? (lo + hi) / 2 : (k * ax.sign > 0 ? hi : lo)
  }
  for (const v of [1, 0, -1] as const) for (const h of [-1, 0, 1] as const) {
    if (h === 0 && v === 0) continue
    const pos: Vec3 = [(min[0] + max[0]) / 2, (min[1] + max[1]) / 2, (min[2] + max[2]) / 2]
    pos[right.axis] = side(right, h); pos[up.axis] = side(up, v)
    out.push({ id: `${h},${v}`, h, v, pos })
  }
  return out
}

const snapTo = (x: number, step: number): number => (step > 0 ? Math.round(x / step) * step : x)

/**
 * Point map for dragging handle (h, v) by (dr, du) metres along the screen axes. The moved side lands on the grid
 * (`step` 0 = free) and keeps at least `minSize` thickness; nothing outside the dragged axes changes.
 */
export function resizeMap(min: Vec3, max: Vec3, right: ScreenAxis, up: ScreenAxis, h: -1 | 0 | 1, v: -1 | 0 | 1, dr: number, du: number, step: number, minSize = 0.01): PointMap {
  const axes: { a: ScreenAxis; k: number; d: number }[] = [{ a: right, k: h, d: dr }, { a: up, k: v, d: du }]
  const scaleBy: Vec3 = [1, 1, 1], anchor: Vec3 = [0, 0, 0]
  for (const { a, k, d } of axes) {
    if (k === 0) continue
    const i = a.axis, lo = min[i], hi = max[i]
    const movesHigh = k * a.sign > 0 // the dragged side is the max side of this world axis
    const moved = snapTo((movesHigh ? hi : lo) + d * a.sign, step) // screen delta -> world delta along the axis
    const size = movesHigh ? moved - lo : hi - moved
    const newSize = Math.max(minSize, size)
    anchor[i] = movesHigh ? lo : hi
    scaleBy[i] = hi - lo > 1e-9 ? newSize / (hi - lo) : 1
  }
  return (p) => [0, 1, 2].map((i) => (scaleBy[i] === 1 ? p[i] : anchor[i] + (p[i] - anchor[i]) * scaleBy[i])) as Vec3
}

/** Pixel offset -> metres along the screen axes (screen y points down). */
export const pixelsToMetres = (dxPx: number, dyPx: number, mpp: number): [number, number] => [dxPx * mpp, -dyPx * mpp]
