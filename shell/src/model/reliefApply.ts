// Applying a texture's relief (model/relief.ts) to world geometry for drawing: the relief tile is repeated over a face the way the texture is (one tile per UV
// repeat), cut at the face's border, and every point is pushed along the face normal by the relief's depth (metres; > 0 out of the surface, < 0 into it).
// It is structural drawing geometry only: it makes no portals and picking / outlines keep using the flat face.
//
// Conventions: a face's UV is in repeats with v running DOWN the picture; the relief is stored with y UP, so relief (x, y) sits at UV (i + x, j + 1 - y)
// in the repeat (i, j). Side walls of the relief (faces that stand on the surface) have no area seen from the front; they are cut by the footprint line.
import type { Vec3 } from './math'
import { Relief, isFlatRelief, reliefToBrush } from './relief'
import { texEnv } from './texprops'
import { faceNormal, triangulateFace } from './geometry'
import type { Brush, Face } from './document'
import type { FacePlan } from './uvmesh'
import { faceUvAt, lerpVec3, planForFace, retexture } from './uvmesh'

type V2 = [number, number]
interface P { u: number; v: number }
/** Output vertex layout: position (3), normal (3), uv (2): what the mesh buffers use. */
export const RELIEF_FLOATS = 8
/** Most repeats one triangle is expanded into (a huge wall with a tiny tile stays flat). */
export const MAX_RELIEF_CELLS = 4096
/** Triangle budget shared by one mesh build. */
export interface ReliefBudget { left: number }
export const newBudget = (n = 300000): ReliefBudget => ({ left: n })

const EPS = 1e-9
const sub = (a: Vec3, b: Vec3): Vec3 => [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
const cross = (a: Vec3, b: Vec3): Vec3 => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
const dot = (a: Vec3, b: Vec3): number => a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
const unit = (a: Vec3): Vec3 | null => { const l = Math.hypot(a[0], a[1], a[2]); return l < 1e-12 ? null : [a[0] / l, a[1] / l, a[2] / l] }

/** The relief of a texture when it has one that changes anything. */
export function reliefOf(texture: string | undefined): Relief | null {
  const r = texEnv.info(texture ?? 'dev/grey')?.props.relief ?? null
  return r && !isFlatRelief(r) ? r : null
}

interface RTri { v: [Vec3, Vec3, Vec3]; wall: boolean }
const triCache = new WeakMap<Relief, RTri[]>()
/** The relief as triangles in relief space (x, y in 0..1, y up, z metres). */
function reliefTris(r: Relief): RTri[] {
  let t = triCache.get(r)
  if (t) return t
  const b = reliefToBrush(r, 1, 1)
  t = []
  for (const f of b.faces) for (const [i, j, k] of triangulateFace(b, f)) {
    const a = b.vertices[i], c = b.vertices[j], d = b.vertices[k]
    const area = (c[0] - a[0]) * (d[1] - a[1]) - (d[0] - a[0]) * (c[1] - a[1])
    t.push({ v: [a, c, d], wall: Math.abs(area) < 1e-9 })
  }
  triCache.set(r, t)
  return t
}

const polyArea = (p: P[]): number => { let s = 0; for (let i = 0; i < p.length; i++) { const a = p[i], b = p[(i + 1) % p.length]; s += a.u * b.v - b.u * a.v } return Math.abs(s) / 2 }

/** Keeps the part of a convex polygon with a*u + b*v + c >= 0. */
function clip(poly: P[], a: number, b: number, c: number): P[] {
  const out: P[] = []
  for (let i = 0; i < poly.length; i++) {
    const p = poly[i], q = poly[(i + 1) % poly.length]
    const fp = a * p.u + b * p.v + c, fq = a * q.u + b * q.v + c
    if (fp >= 0) out.push(p)
    if ((fp >= 0) !== (fq >= 0)) { const t = fp / (fp - fq); out.push({ u: p.u + (q.u - p.u) * t, v: p.v + (q.v - p.v) * t }) }
  }
  return out
}

// ---- connecting walls ------------------------------------------------------------------------------------------------------------------------------
// The surface of a relief ends at the border of its repeat and at the outline of the face. Where the height there is not 0, the neighbouring repeat does
// not continue it (a different height, or none at all): the gap between the two edges is closed by a wall standing on the line, from one height to the other
// (between repeats) or down to the surface (the face's outline). Heights come from the relief's own surface triangles, sampled along the line.
/** The relief's height along a line: from parameter t0 to t1 (0..1 of the line) the height runs linearly from z0 to z1. */
export interface Span { t0: number; t1: number; z0: number; z1: number }
/**
 * The face next to an outline edge of a face, when it stands at a convex corner with it and shows a relief: its outward normal and its relief's heights
 * (along that normal) along the edge, as spans over t = 0..1 from the edge's first end to its second. The wall that closes the outline then runs from the
 * relief of the face to the relief of the neighbour (a bevel across the corner) instead of down to the surface.
 */
/** Adjacent relief surface coverage. Missing spans mean there is no neighbour surface there, not a flat surface to bridge to. */
export interface Neighbour { n: Vec3; spans: Span[] }
export type NeighbourFn = (a: Vec3, b: Vec3) => Neighbour | null
/** The relief tile's triangles that have area seen from the front (walls have none). */
const surfaceTris = (rts: readonly RTri[]): RTri[] => rts.filter((r) => !r.wall)
const SIDE_SHIFT = 1e-7

/**
 * The heights of the relief along the UV line a -> b as seen from its `side` (a unit UV vector pointing off the line): the line is moved a hair to that
 * side, so a line along a tile border or a relief edge reads the repeat / face on that side of it.
 */
function profile(surface: readonly RTri[], a: V2, b: V2, side: V2): Span[] {
  const ax = a[0] + side[0] * SIDE_SHIFT, ay = a[1] + side[1] * SIDE_SHIFT, bx = b[0] + side[0] * SIDE_SHIFT, by = b[1] + side[1] * SIDE_SHIFT
  const dx = bx - ax, dy = by - ay
  const out: Span[] = []
  const i0 = Math.floor(Math.min(ax, bx)), i1 = Math.ceil(Math.max(ax, bx)) - 1, j0 = Math.floor(Math.min(ay, by)), j1 = Math.ceil(Math.max(ay, by)) - 1
  // t range of x0 + t * d >= lo (kind 1) / <= hi (kind 2) on an axis; false when the line misses
  const range = (p: number, d: number, lo: number, hi: number, r: [number, number]): boolean => {
    if (Math.abs(d) < 1e-15) return p >= lo && p <= hi
    let t0 = (lo - p) / d, t1 = (hi - p) / d
    if (t0 > t1) { const s = t0; t0 = t1; t1 = s }
    r[0] = Math.max(r[0], t0); r[1] = Math.min(r[1], t1)
    return r[0] < r[1]
  }
  for (let j = j0; j <= j1; j++) for (let i = i0; i <= i1; i++) {
    const cell: [number, number] = [0, 1]
    if (!range(ax, dx, i, i + 1, cell) || !range(ay, dy, j, j + 1, cell)) continue
    for (const rt of surface) {
      const Q = rt.v.map((p) => [i + p[0], j + 1 - p[1], p[2]] as Vec3)
      const so = ((Q[1][0] - Q[0][0]) * (Q[2][1] - Q[0][1]) - (Q[2][0] - Q[0][0]) * (Q[1][1] - Q[0][1])) > 0 ? 1 : -1
      const r: [number, number] = [cell[0], cell[1]]
      let hit = true
      for (let k = 0; k < 3 && hit; k++) {
        const p = Q[k], q = Q[(k + 1) % 3]
        const ex = q[0] - p[0], ey = q[1] - p[1]
        const ha = -ey * so, hb = ex * so, hc = (ey * p[0] - ex * p[1]) * so
        const f0 = ha * ax + hb * ay + hc, f1 = ha * bx + hb * by + hc
        if (f0 < 0 && f1 < 0) { hit = false; break }
        if (f0 < 0) r[0] = Math.max(r[0], f0 / (f0 - f1))
        else if (f1 < 0) r[1] = Math.min(r[1], f0 / (f0 - f1))
        if (r[1] - r[0] < 1e-9) hit = false
      }
      if (!hit) continue
      const d = (Q[1][0] - Q[0][0]) * (Q[2][1] - Q[0][1]) - (Q[2][0] - Q[0][0]) * (Q[1][1] - Q[0][1])
      const zAt = (t: number): number => {
        const x = ax + dx * t - Q[0][0], y = ay + dy * t - Q[0][1]
        const s = (x * (Q[2][1] - Q[0][1]) - y * (Q[2][0] - Q[0][0])) / d, u = ((Q[1][0] - Q[0][0]) * y - (Q[1][1] - Q[0][1]) * x) / d
        return Q[0][2] + (Q[1][2] - Q[0][2]) * s + (Q[2][2] - Q[0][2]) * u
      }
      out.push({ t0: r[0], t1: r[1], z0: zAt(r[0]), z1: zAt(r[1]) })
    }
  }
  return out.sort((x, y) => x.t0 - y.t0)
}

/** The height of `spans` at the interval ta..tb (inside one span), as [at ta, at tb]; null where the relief has no surface. */
function heightsOver(spans: readonly Span[], ta: number, tb: number): [number, number] | null {
  for (const s of spans) {
    if (s.t0 > ta + 1e-9 || s.t1 < tb - 1e-9) continue
    const k = (s.z1 - s.z0) / (s.t1 - s.t0)
    return [s.z0 + k * (ta - s.t0), s.z0 + k * (tb - s.t0)]
  }
  return null
}

/** Pieces over t where the height of `f` runs from `f0` to `f1` and that of `g` (0 when null, used for internal repeat seams) runs from `g0` to `g1`. */
interface Band { ta: number; tb: number; f0: number; f1: number; g0: number; g1: number }
function bands(f: readonly Span[], g: readonly Span[] | null, bridge = false): Band[] {
  const cuts = new Set<number>()
  for (const s of f) { cuts.add(s.t0); cuts.add(s.t1) }
  if (g) for (const s of g) { cuts.add(s.t0); cuts.add(s.t1) }
  const ts = [...cuts].sort((a, b) => a - b)
  const out: Band[] = []
  const add = (ta: number, tb: number, f0: number, f1: number, g0: number, g1: number): void => {
    if (bridge ? Math.max(Math.abs(f0), Math.abs(g0), Math.abs(f1), Math.abs(g1)) < 1e-6 : Math.abs(f0 - g0) < 1e-6 && Math.abs(f1 - g1) < 1e-6) return
    out.push({ ta, tb, f0, f1, g0, g1 })
  }
  for (let k = 0; k + 1 < ts.length; k++) {
    const ta = ts[k], tb = ts[k + 1]
    if (tb - ta < 1e-9) continue
    const hf = heightsOver(f, ta, tb)
    // A missing span means there is no relief surface on that side of this interval.
    // Do not invent a zero-height span: it creates a fake connector to the underlying
    // brush wall at every end of a disconnected relief segment.
    const hg = g ? heightsOver(g, ta, tb) : [0, 0] as [number, number]
    if (!hf || !hg) continue
    const d0 = hf[0] - hg[0], d1 = hf[1] - hg[1]
    if (!bridge && d0 * d1 < 0) { // the two heights cross: two bands, each with one of them on top
      const s = d0 / (d0 - d1), tm = ta + (tb - ta) * s, fm = hf[0] + (hf[1] - hf[0]) * s
      add(ta, tm, hf[0], fm, hg[0], fm)
      add(tm, tb, fm, hf[1], fm, hg[1])
    } else add(ta, tb, hf[0], hf[1], hg[0], hg[1])
  }
  return out
}

/**
 * What stands next to the outline edges of `face` of `brush` (reliefTriangle's `neighbour`): the other face of the brush that has the edge, when it lies
 * behind the face's plane (a convex corner) and shows a relief of its own that is laid out by its UV map alone (no repeat behaviour: such a face keeps the
 * wall down to the surface).
 */
export function neighbourFn(brush: Brush, face: Face): NeighbourFn {
  const nA = faceNormal(brush, face)
  const base = brush.vertices[face.loop[0]]
  interface Other { n: Vec3; loop: Vec3[]; uvAt: (p: Vec3) => V2; centre: Vec3; surface: RTri[] }
  let others: Other[] | null = null
  const load = (): Other[] => {
    const out: Other[] = []
    for (const o of brush.faces) {
      if (o.id === face.id) continue
      const r = reliefOf(o.texture)
      if (!r || planForFace(brush, o)) continue
      const loop = o.loop.map((i) => brush.vertices[i])
      const centre: Vec3 = [0, 0, 0]
      for (const v of loop) for (let k = 0; k < 3; k++) centre[k] += v[k] / loop.length
      if (dot(nA, sub(centre, base)) > -1e-9) continue // not behind the face: a flat or concave corner
      out.push({ n: faceNormal(brush, o), loop, uvAt: faceUvAt(brush, o), centre, surface: surfaceTris(reliefTris(r)) })
    }
    return out
  }
  const onSegment = (p: Vec3, a: Vec3, b: Vec3): boolean => {
    const d = sub(b, a), len2 = dot(d, d)
    if (len2 < 1e-12) return false
    const t = dot(sub(p, a), d) / len2
    if (t < -1e-6 || t > 1 + 1e-6) return false
    const c = sub([a[0] + d[0] * t, a[1] + d[1] * t, a[2] + d[2] * t], p)
    return dot(c, c) < 1e-10
  }
  return (a, b) => {
    others ??= load()
    for (const o of others) for (let k = 0; k < o.loop.length; k++) {
      const p = o.loop[k], q = o.loop[(k + 1) % o.loop.length]
      if (!onSegment(a, p, q) || !onSegment(b, p, q)) continue
      const uv0 = o.uvAt(a), uv1 = o.uvAt(b), c = o.uvAt(o.centre)
      const dx = uv1[0] - uv0[0], dy = uv1[1] - uv0[1], len = Math.hypot(dx, dy)
      if (len < 1e-12) return null
      // the side of the edge the neighbour's face is on, in its UV
      let side: V2 = [-dy / len, dx / len]
      if ((c[0] - uv0[0]) * side[0] + (c[1] - uv0[1]) * side[1] < 0) side = [-side[0], -side[1]]
      return { n: o.n, spans: profile(o.surface, uv0, uv1, side) }
    }
    return null
  }
}

/**
 * The relief laid over one triangle: `tri` world corners, `uv` their UV (repeats), `n` the face's outward normal. Returns interleaved vertices (position,
 * normal, uv) of whole triangles, or null when the triangle should stay flat (degenerate UV, too many repeats, budget used up).
 * `border`: bit k set = the triangle's edge k (corner k to corner k + 1) lies on the outline of the face (a triangle cut out of the middle of a face has no
 * border bits: its neighbours carry on the relief). An outline connector is generated only if `neighbour` finds actual relief geometry on the adjacent face; the
 * face carrying this relief is never used as the other side of a connector.
 */
export function reliefTriangle(tri: readonly [Vec3, Vec3, Vec3], uv: readonly [V2, V2, V2], n: Vec3, relief: Relief, budget?: ReliefBudget, border = 0, neighbour?: NeighbourFn): number[] | null {
  const [a0, a1, a2] = uv
  const det = (a1[0] - a0[0]) * (a2[1] - a0[1]) - (a2[0] - a0[0]) * (a1[1] - a0[1])
  if (Math.abs(det) < EPS) return null
  const sb = det > 0 ? 1 : -1 // orientation of the triangle in UV
  const u0 = Math.min(a0[0], a1[0], a2[0]), u1 = Math.max(a0[0], a1[0], a2[0]), v0 = Math.min(a0[1], a1[1], a2[1]), v1 = Math.max(a0[1], a1[1], a2[1])
  const i0 = Math.floor(u0 + 1e-9), i1 = Math.ceil(u1 - 1e-9) - 1, j0 = Math.floor(v0 + 1e-9), j1 = Math.ceil(v1 - 1e-9) - 1
  if ((i1 - i0 + 1) * (j1 - j0 + 1) > MAX_RELIEF_CELLS) return null

  // UV -> world inside the triangle's plane (affine)
  const e1 = sub(tri[1], tri[0]), e2 = sub(tri[2], tri[0])
  const du: V2 = [a1[0] - a0[0], a1[1] - a0[1]], dv: V2 = [a2[0] - a0[0], a2[1] - a0[1]]
  const world = (u: number, v: number): Vec3 => {
    const x = u - a0[0], y = v - a0[1]
    const s = (x * dv[1] - y * dv[0]) / det, t = (du[0] * y - du[1] * x) / det
    return [tri[0][0] + e1[0] * s + e2[0] * t, tri[0][1] + e1[1] * s + e2[1] * t, tri[0][2] + e1[2] * s + e2[2] * t]
  }
  const at = (u: number, v: number, z: number): Vec3 => { const w = world(u, v); return [w[0] + n[0] * z, w[1] + n[1] * z, w[2] + n[2] * z] }
  // handedness of (relief x, relief y) on the surface against the normal: mirrored texture projections flip the winding of walls
  const wu = sub(world(1, 0), world(0, 0)), wv = sub(world(0, 1), world(0, 0))
  const mapSign = dot(cross(wu, [-wv[0], -wv[1], -wv[2]]), n) >= 0 ? 1 : -1

  // the base triangle's edges as half-planes (inside >= 0)
  const U = [a0, a1, a2]
  const edges: [number, number, number][] = []
  for (let k = 0; k < 3; k++) {
    const p = U[k], q = U[(k + 1) % 3]
    const ex = q[0] - p[0], ey = q[1] - p[1]
    // cross(e, x - p) * sb >= 0  =>  (ex*(v-py) - ey*(u-px)) * sb
    edges.push([-ey * sb, ex * sb, (ey * p[0] - ex * p[1]) * sb])
  }

  const rts = reliefTris(relief)
  const out: number[] = []
  const push = (p: Vec3, nn: Vec3, u: number, v: number): void => { out.push(p[0], p[1], p[2], nn[0], nn[1], nn[2], u, v) }
  const base: P[] = [{ u: a0[0], v: a0[1] }, { u: a1[0], v: a1[1] }, { u: a2[0], v: a2[1] }]

  for (let j = j0; j <= j1; j++) for (let i = i0; i <= i1; i++) {
    // the part of the triangle in this repeat
    let cell = clip(clip(clip(clip(base, 1, 0, -i), -1, 0, i + 1), 0, 1, -j), 0, -1, j + 1)
    if (cell.length < 3) cell = []
    for (const rt of rts) {
      const Q = rt.v.map((p) => [i + p[0], j + 1 - p[1], p[2]] as Vec3) // relief corners in UV (z kept)
      if (!rt.wall) {
        if (cell.length === 0) continue
        const so = ((Q[1][0] - Q[0][0]) * (Q[2][1] - Q[0][1]) - (Q[2][0] - Q[0][0]) * (Q[1][1] - Q[0][1])) > 0 ? 1 : -1
        let poly = cell
        for (let k = 0; k < 3 && poly.length >= 3; k++) {
          const p = Q[k], q = Q[(k + 1) % 3]
          const ex = q[0] - p[0], ey = q[1] - p[1]
          poly = clip(poly, -ey * so, ex * so, (ey * p[0] - ex * p[1]) * so)
        }
        if (poly.length < 3 || polyArea(poly) < 1e-12) continue
        // depth at a UV point: barycentric in the relief triangle
        const d = (Q[1][0] - Q[0][0]) * (Q[2][1] - Q[0][1]) - (Q[2][0] - Q[0][0]) * (Q[1][1] - Q[0][1])
        const zAt = (u: number, v: number): number => {
          const x = u - Q[0][0], y = v - Q[0][1]
          const s = (x * (Q[2][1] - Q[0][1]) - y * (Q[2][0] - Q[0][0])) / d, t = ((Q[1][0] - Q[0][0]) * y - (Q[1][1] - Q[0][1]) * x) / d
          return Q[0][2] + (Q[1][2] - Q[0][2]) * s + (Q[2][2] - Q[0][2]) * t
        }
        const corner = Q.map((q) => at(q[0], q[1], q[2]))
        const raw = unit(cross(sub(corner[1], corner[0]), sub(corner[2], corner[0])))
        if (!raw) continue
        const nn: Vec3 = so * sb > 0 ? raw : [-raw[0], -raw[1], -raw[2]]
        const tris = poly.length - 2
        if (budget) { if (budget.left < tris) return null; budget.left -= tris }
        for (let k = 1; k + 1 < poly.length; k++) for (const q of [poly[0], poly[k], poly[k + 1]]) push(at(q.u, q.v, zAt(q.u, q.v)), nn, q.u, q.v)
      } else {
        // side wall: its footprint is a line; cut it to the part inside the base triangle
        let L0 = 0, L1 = 0, best = -1
        for (let x = 0; x < 3; x++) for (let y = x + 1; y < 3; y++) { const l = Math.hypot(Q[x][0] - Q[y][0], Q[x][1] - Q[y][1]); if (l > best) { best = l; L0 = x; L1 = y } }
        if (best < 1e-9) continue
        const P0 = Q[L0], P1 = Q[L1]
        // A relief-data wall on an integer UV line is a tile-boundary side face. Never
        // emit it independently for every repeat: the seam pass below compares the
        // actual relief surfaces on both sides and creates a connector only when needed.
        const onGridLine = (x: number, y: number): boolean => Math.abs(x - y) < 1e-9 && Math.abs(x - Math.round(x)) < 1e-9
        if (onGridLine(P0[0], P1[0]) || onGridLine(P0[1], P1[1])) continue
        let slo = 0, shi = 1
        for (const [ea, eb, ec] of edges) {
          const f0 = ea * P0[0] + eb * P0[1] + ec, f1 = ea * P1[0] + eb * P1[1] + ec
          if (Math.abs(f1 - f0) < 1e-12) { if (f0 < 0) { slo = 1; shi = 0 } continue }
          const s = -f0 / (f1 - f0)
          if (f1 > f0) slo = Math.max(slo, s); else shi = Math.min(shi, s)
        }
        if (shi - slo < 1e-9) continue
        // the wall as a polygon in (s along the footprint, z), same corner order as the relief face
        let poly: P[] = Q.map((q) => ({ u: ((q[0] - P0[0]) * (P1[0] - P0[0]) + (q[1] - P0[1]) * (P1[1] - P0[1])) / (best * best), v: q[2] }))
        poly = clip(clip(poly, 1, 0, -slo), -1, 0, shi)
        if (poly.length < 3) continue
        const tris = poly.length - 2
        if (budget) { if (budget.left < tris) return null; budget.left -= tris }
        const pos = (p: P): Vec3 => at(P0[0] + (P1[0] - P0[0]) * p.u, P0[1] + (P1[1] - P0[1]) * p.u, p.v)
        for (let k = 1; k + 1 < poly.length; k++) {
          let t3 = [poly[0], poly[k], poly[k + 1]]
          if (mapSign < 0) t3 = [t3[0], t3[2], t3[1]]
          const w = t3.map(pos)
          const nn = unit(cross(sub(w[1], w[0]), sub(w[2], w[0])))
          if (!nn) continue
          t3.forEach((p, m) => push(w[m], nn, P0[0] + (P1[0] - P0[0]) * p.u, P0[1] + (P1[1] - P0[1]) * p.u))
        }
      }
    }
  }

  // connecting walls: along the borders between repeats inside the triangle, and along the triangle's edges that are the face's outline
  const surface = surfaceTris(rts)
  let over = false
  /** A wall standing on the UV line a -> b between the heights of `f` (seen from `sideF`, a UV direction) and `g` (the other side of the line; null: the surface, 0). */
  const wall = (a: V2, b: V2, f: Span[], g: Span[] | null, sideF: V2, nb: Neighbour | null = null): void => {
    for (const band of bands(f, g ?? nb?.spans ?? null, nb !== null)) {
      if (over) return
      // the wall belongs to the higher edge and looks at the lower one's side (the outline: the open air outside the face)
      const higherF = band.f0 + band.f1 >= band.g0 + band.g1
      const fd: V2 = g ? (higherF ? [-sideF[0], -sideF[1]] : sideF) : [-sideF[0], -sideF[1]]
      const lp = (t: number): V2 => [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t]
      const mid = lp(0.5), m0 = world(mid[0], mid[1]), m1 = world(mid[0] + fd[0], mid[1] + fd[1])
      // at a corner the wall looks away from both faces
      const facing = nb ? [n[0] + nb.n[0], n[1] + nb.n[1], n[2] + nb.n[2]] as Vec3 : sub(m1, m0)
      const corner = (t: number, z: number, along: Vec3 = n): { w: Vec3; uv: V2 } => { const p = lp(t), e = at(p[0], p[1], 0); return { w: [e[0] + along[0] * z, e[1] + along[1] * z, e[2] + along[2] * z], uv: p } }
      const low = nb ? nb.n : n // the second edge of the wall stands on the surface (g = 0) or on the neighbour's relief
      const ring = [corner(band.ta, band.f0), corner(band.ta, band.g0, low), corner(band.tb, band.g1, low), corner(band.tb, band.f1)]
      if (!nb) { // the two edges stand on one line: they meet where the heights do
        if (Math.abs(band.f0 - band.g0) < 1e-6) ring.splice(1, 1)
        if (Math.abs(band.f1 - band.g1) < 1e-6) ring.splice(ring.length - 1, 1)
      } else { // a bevel: it shrinks to a triangle where an end of the wall lies on the edge itself
        if (Math.hypot(band.f0, band.g0) < 1e-6) ring.splice(1, 1)
        if (Math.hypot(band.f1, band.g1) < 1e-6) ring.splice(ring.length - 1, 1)
      }
      if (ring.length < 3) continue
      const tris = ring.length - 2
      if (budget) { if (budget.left < tris) { over = true; return } budget.left -= tris }
      for (let k = 1; k + 1 < ring.length; k++) {
        let t3 = [ring[0], ring[k], ring[k + 1]]
        let nn = unit(cross(sub(t3[1].w, t3[0].w), sub(t3[2].w, t3[0].w)))
        if (!nn) continue
        if (dot(nn, facing) < 0) { t3 = [t3[0], t3[2], t3[1]]; nn = [-nn[0], -nn[1], -nn[2]] }
        for (const c of t3) push(c.w, nn, c.uv[0], c.uv[1])
      }
    }
  }
  /** Part of the UV line a -> b (as t) that lies inside the base triangle. */
  const inside = (a: V2, b: V2): [number, number] => {
    let lo = 0, hi = 1
    for (const [ea, eb, ec] of edges) {
      const f0 = ea * a[0] + eb * a[1] + ec, f1 = ea * b[0] + eb * b[1] + ec
      if (Math.abs(f1 - f0) < 1e-12) { if (f0 < 0) { lo = 1; hi = 0 } continue }
      const s = -f0 / (f1 - f0)
      if (f1 > f0) lo = Math.max(lo, s); else hi = Math.min(hi, s)
    }
    return [lo, hi]
  }
  const seam = (a: V2, b: V2, side: V2): void => {
    const [lo, hi] = inside(a, b)
    if (hi - lo < 1e-9) return
    const p: V2 = [a[0] + (b[0] - a[0]) * lo, a[1] + (b[1] - a[1]) * lo], q: V2 = [a[0] + (b[0] - a[0]) * hi, a[1] + (b[1] - a[1]) * hi]
    wall(p, q, profile(surface, p, q, side), profile(surface, p, q, [-side[0], -side[1]]), side)
  }
  for (let i = i0 + 1; i <= i1; i++) seam([i, j0], [i, j1 + 1], [-1, 0])
  for (let j = j0 + 1; j <= j1; j++) seam([i0, j], [i1 + 1, j], [0, -1])
  for (let k = 0; k < 3 && !over; k++) {
    if (!(border & (1 << k))) continue
    const p = U[k], q = U[(k + 1) % 3]
    const len = Math.hypot(edges[k][0], edges[k][1])
    if (len < 1e-12) continue
    const nb = neighbour?.(tri[k], tri[(k + 1) % 3]) ?? null
    // Do not close relief ends against the host brush face. Outline connectors are only
    // meaningful at an edge backed by another relief surface.
    if (!nb || nb.spans.length === 0) continue
    const inward: V2 = [edges[k][0] / len, edges[k][1] / len] // the edge's half-plane points into the triangle
    wall(p, q, profile(surface, p, q, inward), null, inward, nb)
  }
  if (over) return null
  return out
}

/**
 * For the polygons that make up one face (the fragments a cut leaves of it): which edge of which polygon is on the outline of their union (true), and which
 * is shared with another of them (false: the relief goes on over it). `edge k` of a polygon runs from its vertex k to k + 1. An edge a neighbour only
 * partly shares counts as outline.
 */
export function polygonBorders(polys: readonly (readonly Vec3[])[]): boolean[][] {
  const Q = 1e4
  const key = (p: Vec3): string => `${Math.round(p[0] * Q)},${Math.round(p[1] * Q)},${Math.round(p[2] * Q)}`
  const all = new Map<string, Vec3>()
  for (const p of polys) for (const v of p) all.set(key(v), v)
  const verts = [...all.values()]
  // every directed edge, cut at the vertices lying on it, so a neighbour that splits an edge in two still matches it
  const pieces = (a: Vec3, b: Vec3): [string, string][] => {
    const d = sub(b, a), len2 = dot(d, d)
    const on: { t: number; p: Vec3 }[] = []
    if (len2 > 1e-12) for (const p of verts) {
      const t = dot(sub(p, a), d) / len2
      if (t <= 1e-6 || t >= 1 - 1e-6) continue
      const c = sub([a[0] + d[0] * t, a[1] + d[1] * t, a[2] + d[2] * t] as Vec3, p)
      if (dot(c, c) < 1e-10) on.push({ t, p })
    }
    on.sort((x, y) => x.t - y.t)
    const out: [string, string][] = []
    let prev = a
    for (const { p } of on) { out.push([key(prev), key(p)]); prev = p }
    out.push([key(prev), key(b)])
    return out
  }
  const have = new Set<string>()
  const cut = polys.map((p) => p.map((a, k) => pieces(a, p[(k + 1) % p.length])))
  for (const edges of cut) for (const e of edges) for (const [x, y] of e) if (x !== y) have.add(`${x}>${y}`)
  return cut.map((edges) => edges.map((e) => e.some(([x, y]) => x !== y && !have.has(`${y}>${x}`))))
}

/** The border bits of the fan triangle (0, i, i + 1) of a polygon with `n` corners, given which of the polygon's edges are on the outline (edge k: corner k to k + 1). */
export function fanBorder(i: number, n: number, edge: readonly boolean[]): number {
  return (i === 1 && edge[0] ? 1 : 0) | (edge[i] ? 2 : 0) | (i + 2 === n && edge[n - 1] ? 4 : 0)
}

/** The border bits of a triangle of vertex indices cut out of a face's `loop`: an edge between neighbours of the loop is the face's outline. */
export function loopBorder(loop: readonly number[], t: readonly [number, number, number]): number {
  const at = t.map((i) => loop.indexOf(i)), n = loop.length
  let bits = 0
  for (let k = 0; k < 3; k++) {
    const d = (at[(k + 1) % 3] - at[k] + n) % n
    if (at[k] >= 0 && at[(k + 1) % 3] >= 0 && (d === 1 || d === n - 1)) bits |= 1 << k
  }
  return bits
}

/** The border bits of a piece of a source triangle (`w` = its corners' weights in it): a piece edge is on the outline when it lies along a border edge of the triangle. */
export function pieceBorder(verts: readonly { w: readonly number[] }[], triBorder: number): number {
  if (!triBorder) return 0
  let bits = 0
  for (let m = 0; m < verts.length; m++) {
    const a = verts[m].w, b = verts[(m + 1) % verts.length].w
    // the triangle's edge k runs from corner k to k + 1: the weight of corner k + 2 is 0 all along it
    for (let k = 0; k < 3; k++) if ((triBorder & (1 << k)) && Math.abs(a[(k + 2) % 3]) < 1e-9 && Math.abs(b[(k + 2) % 3]) < 1e-9) bits |= 1 << m
  }
  return bits
}

/** True when the face's repetition plan can sample a texture that has a relief (its own, or an alternate of an axis). */
export const planHasRelief = (plan: FacePlan): boolean => plan.textures.some((t) => reliefOf(t) !== null)

/**
 * A face's triangles laid out by its repetition plan (mirror, anchor, alternate texture, edge clamp: model/uvmesh.ts) with the relief of each piece's own
 * texture on top. The plan cuts every triangle into pieces along the fold lines and gives each piece's corners their exact texture coordinates, with the
 * texture the piece samples; the relief is placed by exactly those coordinates (a relief tile is one repeat of the texture it belongs to), so a mirrored
 * tile gets a mirrored relief, an anchored edge keeps its relief glued to the face edge, an alternate texture brings its own relief (or none), and
 * the relief of a clamped tile is cut where the tile's inset is. Output per texture: interleaved vertices (position, normal, uv), RELIEF_FLOATS each.
 * A piece that cannot be expanded (see reliefTriangle) is drawn flat.
 */
export function planWithRelief(plan: FacePlan, tris: readonly (readonly [Vec3, Vec3, Vec3])[], normal: Vec3, out: Map<string, number[]>, budget?: ReliefBudget, borders?: readonly number[], neighbour?: NeighbourFn): void {
  for (const o of retexture(plan, tris)) {
    const tri = tris[o.src]
    const pos = o.verts.map((v) => lerpVec3(tri, v.w)) as [Vec3, Vec3, Vec3]
    const uv = o.verts.map((v) => v.uv) as [V2, V2, V2]
    const list = out.get(o.texture) ?? []
    out.set(o.texture, list)
    const relief = reliefOf(o.texture)
    const got = relief ? reliefTriangle(pos, uv, normal, relief, budget, pieceBorder(o.verts, borders?.[o.src] ?? 0), neighbour) : null
    if (got) { for (const x of got) list.push(x); continue }
    for (let k = 0; k < 3; k++) list.push(...pos[k], ...normal, uv[k][0], uv[k][1])
  }
}
