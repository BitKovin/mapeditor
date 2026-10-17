// Size of the selection in the views (REQUIREMENTS §33 H7): one label per axis of the selected brushes' box (never the same number twice), or a
// selected face's width / height. Pure: the viewport passes how world points land on the screen and how many metres one pixel is at a point.
import type { Brush, Face } from './document'
import { faceNormal } from './geometry'
import { Vec3, add, cross, dot, length, normalize, scale, sub } from './math'

export interface SizeLabel { text: string; at: Vec3 }
export type ScreenOf = (p: Vec3) => { x: number; y: number } | null

export const fmtSize = (m: number): string => `${+m.toFixed(3)} m`
const PX = 14 // how far a label sits outside its edge, in pixels

/** Drops labels whose text would cover an earlier one on screen (a small or far box): never two numbers on top of each other. */
export function withoutOverlaps(labels: SizeLabel[], screen: ScreenOf): SizeLabel[] {
  const kept: { l: SizeLabel; x: number; y: number; w: number }[] = []
  for (const l of labels) {
    const s = screen(l.at)
    if (!s) continue
    const w = l.text.length * 7 + 10
    if (kept.some((k) => Math.abs(k.x - s.x) * 2 < k.w + w && Math.abs(k.y - s.y) < 18)) continue
    kept.push({ l, x: s.x, y: s.y, w })
  }
  return kept.map((k) => k.l)
}

/**
 * Labels for an axis-aligned box: for each axis (except `skipAxis`, the view direction of an orthographic view) the edge along it that lies
 * furthest out on screen from the box's centre (the outline, so the text never sits inside the box), its label just outside it.
 */
export function boxSizeLabels(min: Vec3, max: Vec3, screen: ScreenOf, mpp: (p: Vec3) => number, skipAxis = -1): SizeLabel[] {
  const c: Vec3 = [(min[0] + max[0]) / 2, (min[1] + max[1]) / 2, (min[2] + max[2]) / 2]
  const sc = screen(c)
  if (!sc) return []
  const out: SizeLabel[] = []
  for (let i = 0; i < 3; i++) {
    const size = max[i] - min[i]
    if (i === skipAxis || size < 1e-6) continue
    const [j, k] = [(i + 1) % 3, (i + 2) % 3]
    let best: { at: Vec3; score: number } | null = null
    for (const a of [min[j], max[j]])
      for (const b of [min[k], max[k]]) {
        const m: Vec3 = [0, 0, 0]
        m[i] = c[i]; m[j] = a; m[k] = b
        const s = screen(m)
        if (!s) continue
        // furthest from the centre on screen; ties (orthographic views) go to the bottom, then the left
        const score = Math.hypot(s.x - sc.x, s.y - sc.y) + s.y * 1e-6 - s.x * 1e-9
        if (!best || score > best.score + 1e-9) best = { at: m, score }
      }
    if (!best) continue
    const away = sub(best.at, c)
    away[i] = 0
    const d = length(away) > 1e-12 ? normalize(away) : ([0, 0, 0] as Vec3)
    out.push({ text: fmtSize(size), at: add(best.at, scale(d, PX * mpp(best.at))) })
  }
  return withoutOverlaps(out, screen)
}

/** Width (along the face's longest edge) and height of a face, written outside the face's own rectangle: below it and to its right. */
export function faceSizeLabels(b: Brush, f: Face, mpp: (p: Vec3) => number): SizeLabel[] {
  const pts = f.loop.map((i) => b.vertices[i])
  if (pts.length < 3) return []
  let u: Vec3 = [1, 0, 0], longest = -1
  for (let i = 0; i < pts.length; i++) {
    const e = sub(pts[(i + 1) % pts.length], pts[i]), l = length(e)
    if (l > longest) { longest = l; u = scale(e, 1 / l) }
  }
  const n = faceNormal(b, f), v = normalize(cross(n, u))
  const us = pts.map((p) => dot(p, u)), vs = pts.map((p) => dot(p, v))
  const [u0, u1, v0, v1] = [Math.min(...us), Math.max(...us), Math.min(...vs), Math.max(...vs)]
  const d0 = dot(pts[0], n)
  const point = (a: number, c: number): Vec3 => add(add(scale(u, a), scale(v, c)), scale(n, d0))
  const out: SizeLabel[] = []
  const bottom = point((u0 + u1) / 2, v0), right = point(u1, (v0 + v1) / 2)
  if (u1 - u0 > 1e-6) out.push({ text: fmtSize(u1 - u0), at: add(bottom, scale(v, -PX * mpp(bottom))) })
  if (v1 - v0 > 1e-6) out.push({ text: fmtSize(v1 - v0), at: add(right, scale(u, PX * mpp(right))) })
  return out
}
