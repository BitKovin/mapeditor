// Radiant-style whole-brush operations on convex brushes: clip, hollow, merge (convex hull), snap to grid.
import type { Brush, Face } from './document'
import { Vec3, add, cross, dot, length, normalize, scale, sub } from './math'
import { faceNormal, signedVolume } from './geometry'
import { isProperSolid } from './meshOps'

const EPS = 1e-6
export interface Plane { n: Vec3; d: number } // points p with n.p = d; "front" is n.p > d

export const planeFromPoints = (a: Vec3, b: Vec3, c: Vec3): Plane | null => {
  const n = cross(sub(b, a), sub(c, a))
  if (length(n) < 1e-9) return null
  const u = normalize(n)
  return { n: u, d: dot(u, a) }
}
export const facePlane = (b: Brush, f: Face): Plane => { const n = faceNormal(b, f); return { n, d: dot(n, b.vertices[f.loop[0]]) } }

/** Orders coplanar points CCW as seen from outside (looking against `n`). */
function orderAround(pts: Vec3[], idx: number[], n: Vec3): number[] {
  const c = scale(idx.reduce<Vec3>((s, i) => add(s, pts[i]), [0, 0, 0]), 1 / idx.length)
  const ref = Math.abs(n[1]) < 0.9 ? [0, 1, 0] as Vec3 : [1, 0, 0] as Vec3
  const u = normalize(cross(ref, n)), v = cross(n, u)
  return [...idx].sort((a, b) => Math.atan2(dot(sub(pts[a], c), v), dot(sub(pts[a], c), u)) - Math.atan2(dot(sub(pts[b], c), v), dot(sub(pts[b], c), u)))
}

/** Compacts unused vertices away and drops degenerate faces. */
function tidy(vertices: Vec3[], faces: Face[]): Brush | null {
  const fs = faces.filter((f) => f.loop.length >= 3)
  const used = [...new Set(fs.flatMap((f) => f.loop))].sort((a, b) => a - b)
  const remap = new Map(used.map((o, i) => [o, i]))
  const out = { id: '', vertices: used.map((i) => vertices[i]), faces: fs.map((f) => ({ ...f, loop: f.loop.map((i) => remap.get(i)!) })) } as Brush
  return out.faces.length >= 4 ? out : null
}

/**
 * Keeps the part of convex brush `b` BEHIND `pl` (n.p <= d). Returns the brush (same id/texture data, plus a cap face),
 * the brush itself if nothing is cut, or null when nothing remains.
 */
export function clipBrush(b: Brush, pl: Plane): Brush | null {
  const s = b.vertices.map((v) => dot(pl.n, v) - pl.d)
  if (s.every((x) => x <= EPS)) return b
  if (s.every((x) => x >= -EPS)) return null
  const vertices: Vec3[] = [...b.vertices]
  const cut = new Map<string, number>() // edge -> new vertex
  const cutVertex = (i: number, j: number): number => {
    const key = i < j ? `${i}_${j}` : `${j}_${i}`
    let k = cut.get(key)
    if (k === undefined) { const t = s[i] / (s[i] - s[j]); vertices.push(add(vertices[i], scale(sub(vertices[j], vertices[i]), t))); k = vertices.length - 1; cut.set(key, k) }
    return k
  }
  const onPlane = new Set<number>()
  const faces: Face[] = []
  for (const f of b.faces) {
    const out: number[] = [], uv: [number, number][] = []
    const hasUv = !!f.uvs && f.uvs.length === f.loop.length
    for (let k = 0; k < f.loop.length; k++) {
      const i = f.loop[k], j = f.loop[(k + 1) % f.loop.length]
      const si = s[i], sj = s[j]
      if (si <= EPS) { out.push(i); if (hasUv) uv.push(f.uvs![k]); if (Math.abs(si) <= EPS) onPlane.add(i) }
      if ((si < -EPS && sj > EPS) || (si > EPS && sj < -EPS)) {
        const nv = cutVertex(i, j); out.push(nv); onPlane.add(nv)
        if (hasUv) { const t = si / (si - sj), a = f.uvs![k], c = f.uvs![(k + 1) % f.loop.length]; uv.push([a[0] + (c[0] - a[0]) * t, a[1] + (c[1] - a[1]) * t]) }
      }
    }
    const clean: number[] = [], cuv: [number, number][] = []
    out.forEach((v, k) => { if (v !== out[(k + out.length - 1) % out.length] || out.length === 1) { clean.push(v); if (hasUv) cuv.push(uv[k]) } })
    if (clean.length >= 3) faces.push({ ...f, loop: clean, ...(hasUv ? { uvs: cuv } : {}) })
  }
  const capIdx = orderAround(vertices, [...onPlane], pl.n)
  if (capIdx.length < 3) return null
  const nextId = Math.max(-1, ...b.faces.map((f) => f.id)) + 1
  faces.push({ id: nextId, loop: capIdx, texture: b.faces.find((f) => f.texture)?.texture })
  const r = tidy(vertices, faces)
  if (!r) return null
  const out = { ...b, vertices: r.vertices, faces: r.faces }
  return signedVolume(out) > 1e-9 && isProperSolid(out) ? out : null
}

/** Clips by the plane: `keep` 'back' (n.p<=d), 'front', or 'both' (split). */
export function clipBy(b: Brush, pl: Plane, keep: 'back' | 'front' | 'both'): { back?: Brush; front?: Brush } {
  const back = keep !== 'front' ? clipBrush(b, pl) : null
  const front = keep !== 'back' ? clipBrush(b, { n: scale(pl.n, -1), d: -pl.d }) : null
  return { ...(back ? { back } : {}), ...(front ? { front } : {}) }
}

/** Radiant "Make Hollow": one wall brush per face, `t` thick, not overlapping. null when the walls would meet. */
export function hollowBrush(b: Brush, t: number): Brush[] | null {
  if (t <= 0) return null
  const planes = b.faces.map((f) => facePlane(b, f))
  const inner = planes.map((p) => ({ n: p.n, d: p.d - t }))
  let core: Brush | null = b
  for (const p of inner) core = core && clipBrush(core, p)
  if (!core || core === b) return null // nothing is left inside -> the walls would fill the whole brush
  const walls: Brush[] = []
  planes.forEach((_, i) => {
    let w: Brush | null = clipBrush(b, { n: scale(planes[i].n, -1), d: -inner[i].d }) // keep n_i.p >= d_i - t
    for (let j = 0; j < i && w; j++) w = clipBrush(w, inner[j])                         // and inside the earlier inner planes
    if (w && w !== b) walls.push(w)
  })
  return walls
}

/** Convex hull of the points as a brush; the faces take the texture `texture`. null for fewer than 4 non-coplanar points. */
export function hullBrush(points: Vec3[], texture?: string): Brush | null {
  const pts: Vec3[] = []
  for (const p of points) if (!pts.some((q) => length(sub(p, q)) < 1e-7)) pts.push(p)
  if (pts.length < 4 || pts.length > 120) return null
  const planes: { n: Vec3; d: number; idx: number[] }[] = []
  for (let i = 0; i < pts.length; i++) for (let j = i + 1; j < pts.length; j++) for (let k = j + 1; k < pts.length; k++) {
    const pl = planeFromPoints(pts[i], pts[j], pts[k])
    if (!pl) continue
    const sides = pts.map((p) => dot(pl.n, p) - pl.d)
    const pos = sides.some((x) => x > EPS), neg = sides.some((x) => x < -EPS)
    if (pos && neg) continue
    const n = pos ? scale(pl.n, -1) : pl.n // outward: everything else behind it
    const d = pos ? -pl.d : pl.d
    if (planes.some((q) => dot(q.n, n) > 1 - 1e-9 && Math.abs(q.d - d) < 1e-7)) continue
    planes.push({ n, d, idx: pts.map((p, ix) => (Math.abs(dot(n, p) - d) <= EPS ? ix : -1)).filter((x) => x >= 0) })
  }
  if (planes.length < 4) return null
  const used = [...new Set(planes.flatMap((p) => p.idx))].sort((a, b) => a - b)
  const remap = new Map(used.map((o, i) => [o, i]))
  const faces: Face[] = planes.map((p, i) => ({ id: i, loop: orderAround(pts, p.idx, p.n).map((x) => remap.get(x)!), texture }))
  const out: Brush = { id: '', op: 'add', vertices: used.map((i) => pts[i]), faces }
  return signedVolume(out) > 1e-9 && isProperSolid(out) ? out : null
}

/** Snaps every vertex to the grid; refused (null) when that would break the solid (a vertex of a slanted face may leave its plane). */
export function snapBrush(b: Brush, step: number): Brush | null {
  if (step <= 0) return b
  const vertices = b.vertices.map((v) => v.map((x) => Math.round(x / step) * step) as Vec3)
  const out = { ...b, vertices }
  if (signedVolume(out) <= 1e-9 || !isProperSolid(out)) return null
  for (const f of out.faces) { // every face must stay planar after snapping
    const n = faceNormal(out, f), p0 = vertices[f.loop[0]]
    if (f.loop.some((i) => Math.abs(dot(n, sub(vertices[i], p0))) > 1e-6)) return null
  }
  return out
}

/** Every vertex lies on or behind every face plane. */
export function isConvex(b: Brush): boolean {
  return b.faces.every((f) => { const p = facePlane(b, f); return b.vertices.every((v) => dot(p.n, v) - p.d <= 1e-6) })
}

/** Intersection of two convex brushes (null when they only touch or are apart). */
export function intersectBrushes(a: Brush, b: Brush): Brush | null {
  let cur: Brush | null = a
  for (const f of b.faces) { cur = cur && clipBrush(cur, facePlane(b, f)); if (!cur) return null }
  return cur
}

/** Exact volume of the union of convex brushes (inclusion-exclusion over their intersections; up to 12 brushes). */
export function unionVolume(brushes: Brush[]): number {
  if (brushes.length > 12) return brushes.reduce((s, b) => s + signedVolume(b), 0)
  let total = 0
  const rec = (start: number, cur: Brush | null, count: number) => {
    for (let i = start; i < brushes.length; i++) {
      const next: Brush | null = cur ? intersectBrushes(cur, brushes[i]) : brushes[i]
      if (!next) continue
      total += (count % 2 === 0 ? 1 : -1) * signedVolume(next)
      rec(i + 1, next, count + 1)
    }
  }
  rec(0, null, 0)
  return total
}
