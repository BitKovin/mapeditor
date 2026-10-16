// Repetition behaviour as UV data (REQUIREMENTS §19 A4-A6).
//
// The texture sampler is always linear + REPEAT. Mirroring and anchoring cannot be expressed by that sampler on one polygon, so they are
// expressed in the geometry's UVs instead: a face's triangles are cut along the lines where the mapping folds, jumps or changes texture, and every
// piece gets its own exact UVs. The cut points are *invisible vertices*: they lie on the face's plane, on its triangles' edges and inside them, so
// the picture is unchanged except for the UVs; the brush mesh, the face outline and picking never see them.
//
// The mapping of one face is separable. A face has an affine map  p -> (u, v)  (in repeats; from its stored uvs or its projection). Per axis, the
// texture's repeat setting turns u (or v) into a piecewise linear function  t -> t'  with a list of segments (`Seg`): over each segment the
// function is `s * t + c`, and the segment says which texture (the face's own or the axis's alternate) it samples. A cell is a pair of segments
// (one per axis); the triangle clipped to the cell is one piece, whose UVs are exact because the map is linear inside the cell.
//
// Anchoring is relative to the AUTHORED face (its loop's extent in u / v), so every fragment of a carved face agrees with it.
import type { Brush, Face } from './document'
import { faceNormal, triangulateFace } from './geometry'
import type { Vec3 } from './math'
import { AxisRepeat, TexInfo, Vec2, edgeClamp, parseFaceRepeat, withFaceRepeat, isPlainProps, naturalSize, propsToJson, texEnv } from './texprops'
import { Projection, projectPoints, projectionOf } from './uv'
import { groupEnv, groupTextures, parseFaceLayout } from './texgroup'
import { layoutPlanFor } from './layoutPlan'

/** a*x + b*y + c*z + d */
export type Aff = [number, number, number, number]
export interface UvAffine { u: Aff; v: Aff }
export interface Extent { umin: number; umax: number; vmin: number; vmax: number }

/** More cells than this per face and the face falls back to plain repeat (a 200 m wall with a 5 cm mirrored tile would be a mesh of 4000 pieces). */
export const MAX_CELLS = 4096
const MAX_SEGS = 1024

const evalAff = (a: Aff, p: Vec3): number => a[0] * p[0] + a[1] * p[1] + a[2] * p[2] + a[3]

/** The affine map of the face's projection parameters. */
export function projectionAffine(n: Vec3, p: Projection): UvAffine {
  const at = (q: Vec3) => projectPoints(n, [q], p)[0]
  const o = at([0, 0, 0]), x = at([1, 0, 0]), y = at([0, 1, 0]), z = at([0, 0, 1])
  return { u: [x[0] - o[0], y[0] - o[0], z[0] - o[0], o[0]], v: [x[1] - o[1], y[1] - o[1], z[1] - o[1], o[1]] }
}

/** Solves the 4x4 system M x = r (Gaussian elimination with pivoting); null when singular. */
function solve4(M: number[][], r: number[]): number[] | null {
  const A = M.map((row, i) => [...row, r[i]])
  for (let c = 0; c < 4; c++) {
    let p = c
    for (let i = c + 1; i < 4; i++) if (Math.abs(A[i][c]) > Math.abs(A[p][c])) p = i
    if (Math.abs(A[p][c]) < 1e-12) return null
    ;[A[c], A[p]] = [A[p], A[c]]
    for (let i = 0; i < 4; i++) {
      if (i === c) continue
      const f = A[i][c] / A[c][c]
      for (let j = c; j < 5; j++) A[i][j] -= f * A[c][j]
    }
  }
  return A.map((row, i) => row[4] / row[i])
}

/** The affine map that reproduces stored per-corner uvs exactly (tolerance `tol`), or null when they are not affine in the plane. */
export function affineFromCorners(pts: Vec3[], uvs: readonly (readonly [number, number])[], n: Vec3, tol = 1e-3): UvAffine | null {
  if (pts.length < 3 || uvs.length !== pts.length) return null
  // the three corners spanning the largest triangle
  let best = -1, bi = 1, bj = 2
  for (let i = 1; i < pts.length; i++) for (let j = i + 1; j < pts.length; j++) {
    const a = [pts[i][0] - pts[0][0], pts[i][1] - pts[0][1], pts[i][2] - pts[0][2]], b = [pts[j][0] - pts[0][0], pts[j][1] - pts[0][1], pts[j][2] - pts[0][2]]
    const c = Math.hypot(a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
    if (c > best) { best = c; bi = i; bj = j }
  }
  if (best < 1e-9) return null
  const idx = [0, bi, bj]
  // rows: value at the three corners, plus "no gradient along the normal" (the map is only defined on the plane)
  const M = [...idx.map((i) => [pts[i][0], pts[i][1], pts[i][2], 1]), [n[0], n[1], n[2], 0]]
  const solveFor = (k: 0 | 1): Aff | null => {
    const s = solve4(M, [...idx.map((i) => uvs[i][k]), 0])
    return s ? [s[0], s[1], s[2], s[3]] : null
  }
  const u = solveFor(0), v = solveFor(1)
  if (!u || !v) return null
  for (let i = 0; i < pts.length; i++) {
    if (Math.abs(evalAff(u, pts[i]) - uvs[i][0]) > tol * Math.max(1, Math.abs(uvs[i][0])) || Math.abs(evalAff(v, pts[i]) - uvs[i][1]) > tol * Math.max(1, Math.abs(uvs[i][1]))) return null
  }
  return { u, v }
}

/** The face's own (u, v) map: its stored per-corner uvs when they are affine, else its projection. */
export function faceAffine(b: Brush, f: Face): UvAffine {
  const n = faceNormal(b, f)
  if (f.uvs && f.uvs.length === f.loop.length) {
    const m = affineFromCorners(f.loop.map((i) => b.vertices[i]), f.uvs, n)
    if (m) return m
  }
  return projectionAffine(n, projectionOf(f))
}

/**
 * The UV a face is drawn with, at any point of its plane (derived fragments, the bake export, the Lit view): exactly what the face's own mesh gives.
 *  - stored per-corner uvs that are affine (the usual case, also after moving the brush with UV lock): that affine map;
 *  - stored uvs that are not affine (a corner moved with UV lock): interpolated per triangle of the face's triangulation, like the drawn mesh;
 *  - no stored uvs: the projection.
 * The stored uvs, not `projection`, are authoritative: UV lock changes the first while leaving the second as it was.
 */
export function faceUvAt(b: Brush, f: Face): (p: Vec3) => [number, number] {
  const n = faceNormal(b, f)
  if (f.uvs && f.uvs.length === f.loop.length) {
    const aff = affineFromCorners(f.loop.map((i) => b.vertices[i]), f.uvs, n)
    if (aff) return (p) => [evalAff(aff.u, p), evalAff(aff.v, p)]
    const stored = f.uvs
    const tris = triangulateFace(b, f).map((t) => ({ p: t.map((i) => b.vertices[i]) as [Vec3, Vec3, Vec3], uv: t.map((i) => stored[f.loop.indexOf(i)]) }))
    return (p) => {
      let best = tris[0], bw: [number, number, number] = [1, 0, 0], bm = -Infinity
      for (const t of tris) {
        const w = barycentric(p, t.p, n)
        const m = Math.min(w[0], w[1], w[2]) // the triangle that contains p (or the nearest one: extrapolates)
        if (m > bm) { bm = m; best = t; bw = w }
      }
      return [bw[0] * best.uv[0][0] + bw[1] * best.uv[1][0] + bw[2] * best.uv[2][0], bw[0] * best.uv[0][1] + bw[1] * best.uv[1][1] + bw[2] * best.uv[2][1]]
    }
  }
  const m = projectionAffine(n, projectionOf(f))
  return (p) => [evalAff(m.u, p), evalAff(m.v, p)]
}

/** Barycentric weights of `p` (on the triangle's plane) in a triangle `t` with normal `n`. */
function barycentric(p: Vec3, t: [Vec3, Vec3, Vec3], n: Vec3): [number, number, number] {
  const sub = (a: Vec3, c: Vec3): Vec3 => [a[0] - c[0], a[1] - c[1], a[2] - c[2]]
  const cr = (a: Vec3, c: Vec3): Vec3 => [a[1] * c[2] - a[2] * c[1], a[2] * c[0] - a[0] * c[2], a[0] * c[1] - a[1] * c[0]]
  const dt = (a: Vec3, c: Vec3): number => a[0] * c[0] + a[1] * c[1] + a[2] * c[2]
  const area = dt(cr(sub(t[1], t[0]), sub(t[2], t[0])), n)
  if (Math.abs(area) < 1e-14) return [1, 0, 0]
  const w0 = dt(cr(sub(t[1], p), sub(t[2], p)), n) / area, w1 = dt(cr(sub(t[2], p), sub(t[0], p)), n) / area
  return [w0, w1, 1 - w0 - w1]
}

export function extentOf(map: UvAffine, pts: Vec3[]): Extent {
  const e: Extent = { umin: Infinity, umax: -Infinity, vmin: Infinity, vmax: -Infinity }
  for (const p of pts) {
    const u = evalAff(map.u, p), v = evalAff(map.v, p)
    e.umin = Math.min(e.umin, u); e.umax = Math.max(e.umax, u); e.vmin = Math.min(e.vmin, v); e.vmax = Math.max(e.vmax, v)
  }
  return e
}

// ---- one axis -------------------------------------------------------------------------------------------------------------------------------------

/** Over t in [lo, hi] the axis function is  t' = s * t + c  and samples `role` ('alt' = the axis's alternate texture; its t' is in that texture's repeats). */
export interface Seg { lo: number; hi: number; s: number; c: number; role: 'main' | 'alt'; /** Edge clamp: the integer tile of the sampled texture this segment lies in (its t' is within [tile, tile + 1]); undefined = no edge protection. */ tile?: number }

/**
 * Distance (in texels of the sampled texture) the UVs keep from the texture's border where the border must not blend with the opposite edge.
 * A texel of mip level L is centred 0.5 * 2^L texels in, so 0.5 is exactly clean at level 0, 1 up to level 1, 2 up to level 2. The inset squeezes the tile
 * by twice that, so it is about 1% of the size, between half a texel (tiny textures) and 2 texels (2% at most for 100 pixels, less for bigger).
 */
export const edgeInsetTexels = (size: number): number => Math.max(0.5, Math.min(2, size * 0.01))
/** The alternate texture of an axis seen from the main one: `k` = main natural size / alternate natural size along this axis, `off` = its phase along it. */
export interface AltRatio { k: number; off: number }

const parity = (m: number): number => ((m % 2) + 2) % 2

/**
 * The segments covering [lo, hi] of one axis. `t` is the anchored coordinate: with Anchor 'end' the face's end edge is at t = 1 and the open side is t < 0;
 * with Anchor 'start' the face's start edge is at t = 0 and the open side is t > 1. Returns null when more than MAX_SEGS segments would be needed.
 */
export function axisSegments(cfg: AxisRepeat, alt: AltRatio | null, lo: number, hi: number, tiles?: { main: boolean; alt: boolean }): Seg[] | null {
  const segs = axisSegmentsRaw(cfg, alt, lo, hi)
  return segs && tiles && (tiles.main || tiles.alt) ? splitAtTiles(segs, tiles) : segs
}

/** Cuts the segments of the selected roles at every integer of t' and records the tile: afterwards each lies inside ONE tile of its texture (edge clamp). */
function splitAtTiles(segs: Seg[], tiles: { main: boolean; alt: boolean }): Seg[] | null {
  const out: Seg[] = []
  for (const g of segs) {
    if (!(g.role === 'main' ? tiles.main : tiles.alt) || Math.abs(g.s) < 1e-12) { out.push(g); continue }
    const a = g.s * g.lo + g.c, b = g.s * g.hi + g.c
    const mLo = Math.floor(Math.min(a, b) + 1e-9), mHi = Math.floor(Math.max(a, b) - 1e-9)
    if (mHi - mLo > MAX_SEGS) return null
    for (let m = mLo; m <= mHi; m++) {
      // t where t' = m and t' = m + 1
      const t0 = (m - g.c) / g.s, t1 = (m + 1 - g.c) / g.s
      const lo = Math.max(g.lo, Math.min(t0, t1)), hi = Math.min(g.hi, Math.max(t0, t1))
      if (hi - lo > 1e-9 || (g.hi - g.lo <= 1e-9 && lo === g.lo)) out.push({ ...g, lo, hi, tile: m })
    }
    if (out.length > MAX_SEGS) return null
  }
  return out
}

function axisSegmentsRaw(cfg: AxisRepeat, alt: AltRatio | null, lo: number, hi: number): Seg[] | null {
  if (!(hi >= lo)) return []
  const out: Seg[] = []
  const push = (a: number, b: number, s: number, c: number, role: 'main' | 'alt' = 'main'): boolean => {
    if (b - a <= 1e-12 && !(a === lo && b === hi)) return true
    out.push({ lo: a, hi: b, s, c, role })
    return out.length <= MAX_SEGS
  }
  if (cfg.mode === 'repeat') return [{ lo, hi, s: 1, c: 0, role: 'main' }]
  if (cfg.mode === 'mirror') {
    for (let k = Math.floor(lo); k <= Math.floor(hi) && k <= Math.floor(lo) + MAX_SEGS + 1; k++) {
      const a = Math.max(lo, k), b = Math.min(hi, k + 1)
      if (!push(a, b, parity(k) === 0 ? 1 : -1, parity(k) === 0 ? -k : k + 1)) return null
    }
    return out.length <= MAX_SEGS ? out : null
  }
  // ---- anchor
  const fromEnd = cfg.anchor === 'end'
  if (fromEnd) {
    if (hi > 0 && !push(Math.max(lo, 0), hi, 1, 0)) return null
    if (lo >= 0) return out
    // the open side: x = -t from xlo to xhi
    const xlo = -Math.min(hi, 0), xhi = -lo
    const ext = extension(cfg, alt, xlo, xhi, true, out, push)
    return ext ? ext.sort((a, b) => a.lo - b.lo) : null
  }
  if (lo < 1 && !push(lo, Math.min(hi, 1), 1, 0)) return null
  if (hi <= 1) return out
  const xlo = Math.max(lo, 1) - 1, xhi = hi - 1
  return extension(cfg, alt, xlo, xhi, false, out, push)
}

type Push = (a: number, b: number, s: number, c: number, role?: 'main' | 'alt') => boolean

/** The segments beyond the open edge. `x` = distance past the edge in repeats (>= 0); `low` = the open side is t < 0 (Anchor 'end'), else t > 1. */
function extension(cfg: AxisRepeat, alt: AltRatio | null, xlo: number, xhi: number, low: boolean, out: Seg[], push: Push): Seg[] | null {
  // x -> t, and a segment [xa, xb] -> [ta, tb] (ordered)
  const T = (x: number): number => (low ? -x : 1 + x)
  const seg = (xa: number, xb: number, s: number, c: number, role: 'main' | 'alt'): boolean => (low ? push(T(xb), T(xa), s, c, role) : push(T(xa), T(xb), s, c, role))
  if (alt) {
    // tB = k * (t - edge) + off, edge = 0 for the low side and 1 for the high side
    const edge = low ? 0 : 1, k = alt.k, off = alt.off
    if (!(k > 1e-9)) return null
    if (cfg.fill === 'repeat') {
      // one linear segment (the sampler repeats)
      return seg(xlo, xhi, k, off - k * edge, 'alt') ? out : null
    }
    // mirror: cut where tB is an integer m
    const tbOf = (x: number): number => k * (T(x) - edge) + off
    const a = tbOf(xlo), b = tbOf(xhi)
    const mLo = Math.floor(Math.min(a, b)), mHi = Math.floor(Math.max(a, b))
    if (mHi - mLo > MAX_SEGS) return null
    for (let m = mLo; m <= mHi; m++) {
      // t range of tB in [m, m+1]
      const t0 = (m - off) / k + edge, t1 = (m + 1 - off) / k + edge
      const ta = Math.max(Math.min(t0, t1), low ? T(xhi) : T(xlo)), tb = Math.min(Math.max(t0, t1), low ? T(xlo) : T(xhi))
      const even = parity(m) === 0
      if (!push(ta, tb, even ? k : -k, even ? off - k * edge - m : m + 1 + k * edge - off, 'alt')) return null
    }
    return out
  }
  // no alternate texture: continue the band. Anchor end: band = [0, split] (t' counts the fixed texture), Anchor start: band = [split, 1].
  const split = cfg.split
  const bw = low ? split : 1 - split
  if (!(bw > 1e-6)) return null
  const jLo = Math.floor(xlo / bw), jHi = Math.floor(xhi / bw)
  if (jHi - jLo > MAX_SEGS) return null
  for (let j = jLo; j <= jHi; j++) {
    const xa = Math.max(xlo, j * bw), xb = Math.min(xhi, (j + 1) * bw)
    let s: number, c: number
    if (low) {
      if (cfg.fill === 'mirror' && parity(j) === 0) { s = -1; c = -j * bw } else { s = 1; c = (j + 1) * bw }
    } else {
      if (cfg.fill === 'mirror' && parity(j) === 0) { s = -1; c = 2 + j * bw } else { s = 1; c = split - 1 - j * bw }
    }
    if (!seg(xa, xb, s, c, 'main')) return null
  }
  return out
}

/** Reference implementation of the axis function at one point (the segments must agree with it everywhere; used by the tests). */
export function axisValueAt(cfg: AxisRepeat, alt: AltRatio | null, t: number): { t: number; role: 'main' | 'alt' } {
  const frac = (x: number, p: number): number => { const m = ((x % p) + p) % p; return m }
  if (cfg.mode === 'repeat') return { t, role: 'main' }
  if (cfg.mode === 'mirror') { const m = frac(t, 2); return { t: m <= 1 ? m : 2 - m, role: 'main' } }
  const low = cfg.anchor === 'end'
  const open = low ? t < 0 : t > 1
  if (!open) return { t, role: 'main' }
  const x = low ? -t : t - 1
  if (alt) {
    const tb = alt.k * (low ? t : t - 1) + alt.off
    if (cfg.fill === 'repeat') return { t: tb, role: 'alt' }
    const m = frac(tb, 2)
    return { t: m <= 1 ? m : 2 - m, role: 'alt' }
  }
  const bw = low ? cfg.split : 1 - cfg.split
  const j = Math.floor(x / bw), r = x - j * bw
  if (low) return { t: cfg.fill === 'mirror' && parity(j) === 0 ? r : cfg.split - r, role: 'main' }
  return { t: cfg.fill === 'mirror' && parity(j) === 0 ? 1 - r : cfg.split + r, role: 'main' }
}

// ---- a face ---------------------------------------------------------------------------------------------------------------------------------------

/** One convex piece of a triangle: each vertex as barycentric weights of the source triangle plus its UV, and the texture it samples. */
export interface Piece { texture: string; verts: { w: [number, number, number]; uv: Vec2 }[] }

export interface FacePlan {
  /** The face's own texture. */
  texture: string
  /** Every texture a piece can sample: the face's own and the alternates of its axes (the relief of each applies to its pieces, model/reliefApply.ts). */
  textures: string[]
  /** The pieces of one triangle (world positions); each is convex and keeps the triangle's winding. */
  pieces(tri: readonly [Vec3, Vec3, Vec3]): Piece[]
}

interface CV { u: number; v: number; w: [number, number, number] }

/** Clips a convex polygon to u >= bound (keepGE) / u <= bound, along coordinate `k`. Exact at the boundary: new vertices lie ON it. */
function clip(poly: CV[], k: 'u' | 'v', bound: number, keepGE: boolean): CV[] {
  if (!Number.isFinite(bound)) return poly
  const out: CV[] = []
  const inside = (p: CV): boolean => (keepGE ? p[k] >= bound : p[k] <= bound)
  for (let i = 0; i < poly.length; i++) {
    const a = poly[i], b = poly[(i + 1) % poly.length]
    const ia = inside(a), ib = inside(b)
    if (ia) out.push(a)
    if (ia !== ib) {
      const t = (bound - a[k]) / (b[k] - a[k])
      const p: CV = { u: a.u + (b.u - a.u) * t, v: a.v + (b.v - a.v) * t, w: [a.w[0] + (b.w[0] - a.w[0]) * t, a.w[1] + (b.w[1] - a.w[1]) * t, a.w[2] + (b.w[2] - a.w[2]) * t] }
      p[k] = bound
      out.push(p)
    }
  }
  return out
}

const area = (p: CV[]): number => {
  let s = 0
  for (let i = 0; i < p.length; i++) { const a = p[i], b = p[(i + 1) % p.length]; s += a.u * b.v - b.u * a.v }
  return Math.abs(s) / 2
}

export interface PlanInput {
  /** The face's texture (draft or stored properties included). */
  texture: TexInfo
  /** Looks up any other texture (the alternates). */
  info: (path: string) => TexInfo | undefined
  map: UvAffine
  extent: Extent
}

/** null = nothing to cut: the face keeps its plain UVs (plain props, texture not loaded, degenerate map, or too many cells). */
export function makePlan(input: PlanInput): FacePlan | null {
  const { texture: A, map, extent: ext } = input
  const { u: cu, v: cv } = A.props.repeat
  if (!(A.width > 0 && A.height > 0 && A.ppm > 0)) return null
  const clampU = edgeClamp(A, 0), clampV = edgeClamp(A, 1)
  if (isPlainProps(A.props) && !clampU && !clampV) return null
  const natA = naturalSize(A)

  const altOf = (cfg: AxisRepeat, axis: 0 | 1): { info: TexInfo; ratio: AltRatio; cross: AltRatio } | null => {
    if (cfg.mode !== 'anchor' || !cfg.alt) return null
    const B = input.info(cfg.alt.texture)
    if (!B || !(B.width > 0 && B.height > 0 && B.ppm > 0)) return null
    const nb = naturalSize(B)
    const along = axis, across = 1 - axis
    return {
      info: B,
      ratio: { k: natA[along] / nb[along], off: cfg.alt.offset[along] },
      cross: { k: natA[across] / nb[across], off: cfg.alt.offset[across] },
    }
  }
  const altU = altOf(cu, 0), altV = altOf(cv, 1)
  const tilesU = { main: clampU, alt: !!altU && edgeClamp(altU.info, 0) }, tilesV = { main: clampV, alt: !!altV && edgeClamp(altV.info, 1) }
  const inset = (x: number, tile: number | undefined, size: number): number => (tile === undefined ? x : tile + edgeInsetTexels(size) / size + (x - tile) * (1 - (2 * edgeInsetTexels(size)) / size))
  // the anchored coordinate: t = virtual + shift
  const shiftU = cu.mode === 'anchor' ? (cu.anchor === 'end' ? 1 - ext.umax : -ext.umin) : 0
  const shiftV = cv.mode === 'anchor' ? (cv.anchor === 'end' ? 1 - ext.vmax : -ext.vmin) : 0

  // refuse faces that would need too many pieces
  const su0 = axisSegments(cu, altU?.ratio ?? null, ext.umin + shiftU, ext.umax + shiftU, tilesU)
  const sv0 = axisSegments(cv, altV?.ratio ?? null, ext.vmin + shiftV, ext.vmax + shiftV, tilesV)
  if (!su0 || !sv0 || su0.length * sv0.length > MAX_CELLS) return null

  const resolve = (su: Seg, sv: Seg, tu: number, tv: number): { texture: string; uv: Vec2 } => {
    const u1 = su.s * tu + su.c, v1 = sv.s * tv + sv.c
    if (sv.role === 'alt' && altV) {
      const o = cv.alt!.offset
      return { texture: altV.info.path, uv: [su.role === 'alt' ? altV.cross.k * tu + o[0] : altV.cross.k * u1 + o[0], inset(v1, sv.tile, altV.info.height)] }
    }
    if (su.role === 'alt' && altU) {
      const o = cu.alt!.offset
      return { texture: altU.info.path, uv: [inset(u1, su.tile, altU.info.width), altU.cross.k * v1 + o[1]] }
    }
    return { texture: A.path, uv: [inset(u1, su.tile, A.width), inset(v1, sv.tile, A.height)] }
  }

  return {
    texture: A.path,
    textures: [...new Set([A.path, ...(altU ? [altU.info.path] : []), ...(altV ? [altV.info.path] : [])])],
    pieces(tri) {
      const verts: CV[] = tri.map((p, i) => ({ u: evalAff(map.u, p) + shiftU, v: evalAff(map.v, p) + shiftV, w: [i === 0 ? 1 : 0, i === 1 ? 1 : 0, i === 2 ? 1 : 0] as [number, number, number] }))
      const umin = Math.min(...verts.map((q) => q.u)), umax = Math.max(...verts.map((q) => q.u))
      const vmin = Math.min(...verts.map((q) => q.v)), vmax = Math.max(...verts.map((q) => q.v))
      const segsU = axisSegments(cu, altU?.ratio ?? null, umin, umax, tilesU), segsV = axisSegments(cv, altV?.ratio ?? null, vmin, vmax, tilesV)
      if (!segsU || !segsV) return [plainPiece(A.path, verts, shiftU, shiftV)]
      const out: Piece[] = []
      for (const su of segsU) {
        const strip = clip(clip(verts, 'u', su.lo, true), 'u', su.hi, false)
        if (strip.length < 3 || area(strip) < 1e-10) continue
        for (const sv of segsV) {
          const cell = clip(clip(strip, 'v', sv.lo, true), 'v', sv.hi, false)
          if (cell.length < 3 || area(cell) < 1e-10) continue
          const first = resolve(su, sv, cell[0].u, cell[0].v)
          out.push({ texture: first.texture, verts: cell.map((q) => ({ w: q.w, uv: resolve(su, sv, q.u, q.v).uv })) })
        }
      }
      return out
    },
  }
}

const plainPiece = (texture: string, verts: CV[], su: number, sv: number): Piece => ({ texture, verts: verts.map((q) => ({ w: q.w, uv: [q.u - su, q.v - sv] as Vec2 })) })

// ---- caches and the entry points the renderers use -------------------------------------------------------------------------------------------------

const planCache = new WeakMap<Brush, Map<number, { rev: number; key: string; plan: FacePlan | null }>>()

/** The plan of an AUTHORED face (the same for all its carved fragments); null = draw it plainly. Cached per brush object and texture environment revision. */
export function planForFace(b: Brush, f: Face): FacePlan | null {
  const rev = texEnv.rev
  let byFace = planCache.get(b)
  if (!byFace) { byFace = new Map(); planCache.set(b, byFace) }
  const over = parseFaceRepeat(f.repeat)
  const layout = parseFaceLayout(f.layout)
  const key = (over ? JSON.stringify(over) : '') + (layout ? `|L${JSON.stringify(f.layout)}` : '')
  const hit = byFace.get(f.id)
  if (hit && hit.rev === rev && hit.key === key) return hit.plan
  if (layout && f.loop.length >= 3) {
    // a texture group lays the face out (model/layoutPlan.ts); while it cannot (not loaded yet) the plain plan below is used
    const lp = layoutPlanFor(b, f, layout).plan
    if (lp) { byFace.set(f.id, { rev, key, plan: lp }); return lp }
  }
  const path = f.texture ?? 'dev/grey'
  const base = texEnv.info(path)
  const A = base ? withFaceRepeat(base, over) : undefined
  let plan: FacePlan | null = null
  if (A && (!isPlainProps(A.props) || edgeClamp(A, 0) || edgeClamp(A, 1)) && f.loop.length >= 3) {
    const map = faceAffine(b, f)
    plan = makePlan({ texture: A, info: (p) => texEnv.info(p), map, extent: extentOf(map, f.loop.map((i) => b.vertices[i])) })
  }
  byFace.set(f.id, { rev, key, plan })
  return plan
}

/** One output triangle: the source triangle it came from, and per vertex the barycentric weights and the UV. */
export interface RetexTri { texture: string; src: number; verts: { w: [number, number, number]; uv: Vec2 }[] }

/** The triangles of `tris` cut by the plan and fan-triangulated per piece. */
export function retexture(plan: FacePlan, tris: readonly (readonly [Vec3, Vec3, Vec3])[]): RetexTri[] {
  const out: RetexTri[] = []
  tris.forEach((t, src) => {
    for (const piece of plan.pieces(t)) {
      const v = piece.verts
      for (let i = 1; i + 1 < v.length; i++) out.push({ texture: piece.texture, src, verts: [v[0], v[i], v[i + 1]] })
    }
  })
  return out
}

export const lerpVec3 = (tri: readonly [Vec3, Vec3, Vec3], w: readonly [number, number, number]): Vec3 => [
  tri[0][0] * w[0] + tri[1][0] * w[1] + tri[2][0] * w[2],
  tri[0][1] * w[0] + tri[1][1] * w[1] + tri[2][1] * w[2],
  tri[0][2] * w[0] + tri[1][2] * w[1] + tri[2][2] * w[2],
]


/**
 * A string that changes exactly when something the UVs of faces with these textures depend on changes: a texture's size, its pixels per meter, its
 * properties, or the size of an alternate texture. Caches of built meshes compare it after `texEnv.rev` moved, so only the meshes that really use a
 * changed texture are rebuilt.
 */
export function textureSignature(paths: Iterable<string>): string {
  const parts: string[] = []
  for (const p of [...new Set(paths)].sort()) {
    const i = texEnv.info(p)
    if (!i) { parts.push(`${p}:?`); continue }
    parts.push(`${p}:${i.width}x${i.height}@${i.ppm}:${i.seamless ? i.seamless.map(Number).join('') : '?'}:${JSON.stringify(propsToJson(i.props))}`)
    for (const ax of [i.props.repeat.u, i.props.repeat.v]) {
      if (ax.mode !== 'anchor' || !ax.alt) continue
      const a = texEnv.info(ax.alt.texture)
      parts.push(`>${ax.alt.texture}:${a ? `${a.width}x${a.height}@${a.ppm}:${JSON.stringify(propsToJson(a.props).relief ?? null)}` : '?'}`) // the alternate's relief is drawn on the pieces that sample it
    }
  }
  return parts.join('|')
}

/** Signature part of a face's texture group: the group's file version and every slot texture it can use (their size, properties and relief). */
function layoutSignature(f: Face): { paths: string[]; sig: string } {
  const l = parseFaceLayout(f.layout)
  const g = l ? groupEnv.get(l.group) : undefined
  if (!l) return { paths: [], sig: '' }
  return { paths: g ? groupTextures(g) : [], sig: `L:${l.group}@${groupEnv.version(l.group)}` }
}

export function brushTextureSignature(b: Brush): string {
  const paths: string[] = [], extra: string[] = []
  for (const f of b.faces) {
    paths.push(f.texture ?? 'dev/grey')
    if (f.layout !== undefined) { const l = layoutSignature(f); paths.push(...l.paths); if (l.sig) extra.push(l.sig) }
  }
  return textureSignature(paths) + (extra.length ? `#${[...new Set(extra)].sort().join(',')}` : '')
}
