// "Select inside" / "Select touching" (Radiant): use the selected brushes as a volume and find the other brushes in it.
// Both work on convex brushes with exact tests: inside = every vertex behind every face plane; touching = the two convex
// solids are not separated (separating-axis test over face normals and edge-edge axes), contact counts as touching.
import type { Brush } from './document'
import { Vec3, cross, dot, length, sub } from './math'
import { facePlane } from './brushOps'

const EPS = 1e-6

/** Every vertex of `b` lies on or inside the convex `volume`. */
export function brushInside(volume: Brush, b: Brush): boolean {
  const planes = volume.faces.map((f) => facePlane(volume, f))
  return b.vertices.every((v) => planes.every((p) => dot(p.n, v) - p.d <= EPS))
}

function edges(b: Brush): Vec3[] {
  const out: Vec3[] = []
  for (const f of b.faces) for (let i = 0; i < f.loop.length; i++) out.push(sub(b.vertices[f.loop[(i + 1) % f.loop.length]], b.vertices[f.loop[i]]))
  return out
}

const range = (b: Brush, axis: Vec3): [number, number] => {
  let lo = Infinity, hi = -Infinity
  for (const v of b.vertices) { const d = dot(axis, v); if (d < lo) lo = d; if (d > hi) hi = d }
  return [lo, hi]
}

/** The convex solids overlap or touch (a gap larger than EPS along some separating axis means no). */
export function brushesTouch(a: Brush, b: Brush): boolean {
  const axes: Vec3[] = [...a.faces.map((f) => facePlane(a, f).n), ...b.faces.map((f) => facePlane(b, f).n)]
  const ea = edges(a), eb = edges(b)
  for (const x of ea) for (const y of eb) {
    const c = cross(x, y), l = length(c)
    if (l > 1e-9) axes.push([c[0] / l, c[1] / l, c[2] / l])
  }
  return axes.every((ax) => { const [a0, a1] = range(a, ax), [b0, b1] = range(b, ax); return a1 >= b0 - EPS && b1 >= a0 - EPS })
}
