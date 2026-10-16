// Applying a texture's relief (model/relief.ts) to world geometry for drawing: the relief tile is repeated over a face the way the texture is (one tile per UV
// repeat), cut at the face's border, and every point is pushed along the face normal by the relief's depth (metres; > 0 out of the surface, < 0 into it).
// It is structural drawing geometry only: it makes no portals and picking / outlines keep using the flat face.
//
// Conventions: a face's UV is in repeats with v running DOWN the picture; the relief is stored with y UP, so relief (x, y) sits at UV (i + x, j + 1 - y)
// in the repeat (i, j). Side walls of the relief (faces that stand on the surface) have no area seen from the front; they are cut by the footprint line.
import type { Vec3 } from './math'
import { Relief, isFlatRelief, reliefToBrush } from './relief'
import { texEnv } from './texprops'
import { triangulateFace } from './geometry'
import type { FacePlan } from './uvmesh'
import { lerpVec3, retexture } from './uvmesh'

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

/**
 * The relief laid over one triangle: `tri` world corners, `uv` their UV (repeats), `n` the face's outward normal. Returns interleaved vertices (position,
 * normal, uv) of whole triangles, or null when the triangle should stay flat (degenerate UV, too many repeats, budget used up).
 */
export function reliefTriangle(tri: readonly [Vec3, Vec3, Vec3], uv: readonly [V2, V2, V2], n: Vec3, relief: Relief, budget?: ReliefBudget): number[] | null {
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
  return out
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
export function planWithRelief(plan: FacePlan, tris: readonly (readonly [Vec3, Vec3, Vec3])[], normal: Vec3, out: Map<string, number[]>, budget?: ReliefBudget): void {
  for (const o of retexture(plan, tris)) {
    const tri = tris[o.src]
    const pos = o.verts.map((v) => lerpVec3(tri, v.w)) as [Vec3, Vec3, Vec3]
    const uv = o.verts.map((v) => v.uv) as [V2, V2, V2]
    const list = out.get(o.texture) ?? []
    out.set(o.texture, list)
    const relief = reliefOf(o.texture)
    const got = relief ? reliefTriangle(pos, uv, normal, relief, budget) : null
    if (got) { for (const x of got) list.push(x); continue }
    for (let k = 0; k < 3; k++) list.push(...pos[k], ...normal, uv[k][0], uv[k][1])
  }
}
