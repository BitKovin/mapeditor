// Radiant-style face drag: the face's PLANE moves along its normal, every other face keeps its plane (no rotation),
// so the dragged face grows or shrinks. On a vertex mesh this means each vertex of the face slides along the edge
// where its two neighbouring planes meet, until it reaches the shifted plane.
import type { Brush, Face } from './document'
import { Vec3, add, cross, dot, length, normalize, scale, sub } from './math'
import { faceNormal, makeBox, signedVolume } from './geometry'
import { isProperSolid } from './meshOps'
import { clipBrush, facePlane, isConvex } from './brushOps'

const EPS = 1e-6
export type DragResult = { brush: Brush } | { error: string }

/** Groups the faces touching a vertex into distinct planes (coplanar faces count once). */
function distinctNormals(b: Brush, faceIdx: number[]): Vec3[] {
  const out: Vec3[] = []
  for (const i of faceIdx) {
    const n = faceNormal(b, b.faces[i])
    if (!out.some((m) => dot(m, n) > 1 - 1e-7)) out.push(n)
  }
  return out
}

/** UV of a point on a plane from a fitted affine map (3 non-collinear corners of the old face -> their uvs). */
function uvFit(b: Brush, f: Face): ((p: Vec3) => [number, number]) | null {
  if (!f.uvs || f.uvs.length !== f.loop.length) return null
  const P = f.loop.map((i) => b.vertices[i])
  for (let i = 1; i + 1 < P.length; i++) {
    const e1 = sub(P[i], P[0]), e2 = sub(P[i + 1], P[0])
    const n = cross(e1, e2)
    const nn = dot(n, n)
    if (nn < 1e-12) continue
    const [u0, v0] = f.uvs[0]
    const d1: [number, number] = [f.uvs[i][0] - u0, f.uvs[i][1] - v0], d2: [number, number] = [f.uvs[i + 1][0] - u0, f.uvs[i + 1][1] - v0]
    return (p) => {
      const q = sub(p, P[0])
      // barycentric-style coefficients a,b with q = a*e1 + b*e2 (q projected on the plane)
      const a = dot(cross(q, e2), n) / nn, c = dot(cross(e1, q), n) / nn
      return [u0 + a * d1[0] + c * d2[0], v0 + a * d1[1] + c * d2[1]]
    }
  }
  return null
}

/**
 * Moves face `faceId` of `b` by `dist` along its outward normal. Returns the new brush or an error text
 * (a vertex where more than two other planes meet cannot slide; a collapsed or inside-out result is refused).
 */
function slideDrag(b: Brush, faceId: number, dist: number): DragResult {
  const fi = b.faces.findIndex((f) => f.id === faceId)
  if (fi < 0) return { error: 'face not found' }
  if (Math.abs(dist) < EPS) return { brush: b }
  const F = b.faces[fi]
  const n = faceNormal(b, F)
  const d1 = dot(n, b.vertices[F.loop[0]]) + dist
  const moved = new Map<number, Vec3>()
  for (const vi of F.loop) {
    const v = b.vertices[vi]
    const touching = b.faces.map((f, i) => (i !== fi && f.loop.includes(vi) ? i : -1)).filter((i) => i >= 0)
    const normals = distinctNormals(b, touching).filter((m) => Math.abs(dot(m, n)) < 1 - 1e-7)
    if (normals.length !== 2) return { error: normals.length > 2 ? 'This face has a corner where 3+ faces meet - drag its vertices instead' : 'This face cannot be dragged (degenerate corner)' }
    const dir = cross(normals[0], normals[1])
    if (length(dir) < EPS) return { error: 'This face cannot be dragged (parallel neighbours)' }
    const u = normalize(dir)
    const k = dot(n, u)
    if (Math.abs(k) < 1e-6) return { error: 'This face cannot be dragged along its normal (edge parallel to the face)' }
    moved.set(vi, add(v, scale(u, (d1 - dot(n, v)) / k)))
  }
  const vertices = b.vertices.map((v, i) => moved.get(i) ?? v)
  const fits = b.faces.map((f) => uvFit(b, f))
  const faces = b.faces.map((f, i) => {
    const fit = fits[i]
    if (!fit) return f
    const proj = (p: Vec3): Vec3 => (i === fi ? sub(p, scale(n, dot(n, p) - dot(n, b.vertices[F.loop[0]]))) : p) // keep the texture where it was on the plane
    return { ...f, uvs: f.loop.map((vi) => fit(proj(vertices[vi]))) }
  })
  const out: Brush = { ...b, vertices, faces }
  if (!out.vertices.every((v) => v.every(Number.isFinite))) return { error: 'drag failed' }
  // the face must still face outward and the solid must stay valid (dragging past the opposite side turns it inside out)
  if (dot(faceNormal(out, out.faces[fi]), n) < 0.99) return { error: 'The face cannot move further (brush would collapse)' }
  if (signedVolume(out) <= 1e-9 * Math.max(1, signedVolume(b))) return { error: 'The face cannot move further (brush would collapse)' }
  if (!isProperSolid(out)) return { error: 'The face cannot move further (brush would become invalid)' }
  return { brush: out }
}

/**
 * Plane-based drag for convex brushes: shifts the face's plane and re-intersects all planes, so corners where 3+ faces
 * meet (pyramid apex) split or merge like in Radiant. Faces keep their ids/textures; faces that no longer touch the
 * solid disappear, and uvs follow the old texture mapping on each plane.
 */
function planeDrag(b: Brush, faceId: number, dist: number): DragResult {
  const fi = b.faces.findIndex((f) => f.id === faceId)
  const planes = b.faces.map((f) => facePlane(b, f))
  const orig = planes.map((p) => ({ ...p }))
  planes[fi] = { n: planes[fi].n, d: planes[fi].d + dist }
  const lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity]
  for (const v of b.vertices) for (let k = 0; k < 3; k++) { lo[k] = Math.min(lo[k], v[k]); hi[k] = Math.max(hi[k], v[k]) }
  const span = Math.max(...hi.map((h, k) => h - lo[k]), 1)
  const R = (span + Math.abs(dist)) * 1000
  const c = lo.map((l, k) => (l + hi[k]) / 2)
  let cur: Brush | null = makeBox('t', [c[0] - R, c[1] - R, c[2] - R], [c[0] + R, c[1] + R, c[2] + R])
  for (const pl of planes) { cur = cur && clipBrush(cur, pl); if (!cur) return { error: 'The face cannot move further (brush would collapse)' } }
  if (cur.vertices.some((v) => v.some((x) => Math.abs(x) > R * 0.5))) return { error: 'The face cannot move further (brush would become unbounded)' }
  const fits = b.faces.map((f) => uvFit(b, f))
  const faces: Face[] = []
  for (const rf of cur.faces) {
    const pl = facePlane(cur, rf)
    const i = planes.findIndex((q) => dot(q.n, pl.n) > 1 - 1e-7 && Math.abs(q.d - pl.d) < 1e-5 * Math.max(1, R / 1000))
    if (i < 0) return { error: 'drag failed' }
    const f = b.faces[i]
    const fit = fits[i]
    const out: Face = { ...f, loop: rf.loop }
    delete (out as { uvs?: unknown }).uvs
    if (fit) out.uvs = rf.loop.map((vi) => {
      const p = cur!.vertices[vi]
      const q = i === fi ? sub(p, scale(planes[i].n, dot(planes[i].n, p) - orig[i].d)) : p
      return fit(q)
    })
    faces.push(out)
  }
  if (!faces.some((f) => f.id === faceId)) return { error: 'The face cannot move further (brush would collapse)' }
  const out: Brush = { ...b, vertices: cur.vertices, faces }
  if (signedVolume(out) <= 1e-9 || !isProperSolid(out)) return { error: 'The face cannot move further (brush would become invalid)' }
  return { brush: out }
}

/** Moves face `faceId` by `dist` along its normal (see slideDrag; convex brushes fall back to planeDrag at 3+ face corners). */
export function dragFace(b: Brush, faceId: number, dist: number): DragResult {
  const r = slideDrag(b, faceId, dist)
  if ('error' in r && /3\+ faces/.test(r.error) && isConvex(b)) return planeDrag(b, faceId, dist)
  return r
}

/**
 * Alt-drag "extend face": a NEW brush grows out of face `faceId` of `b` by `dist` (> 0) along the face's normal, as a prism
 * over the face's outline. The source brush is untouched. Texture continues: the new end cap keeps the face's mapping, and
 * each side wall continues the texture mapping of the face of `b` it grows from (the neighbour across that edge), so
 * patterns run on across the join. The result keeps the source's `op` and carve scope; `newId` names it. Face ids: 0 = end
 * cap (the face to keep dragging), 1 = base, 2.. = side walls in outline order.
 */
export function extendFace(b: Brush, faceId: number, dist: number, newId: string): DragResult {
  const F = b.faces.find((f) => f.id === faceId)
  if (!F) return { error: 'face not found' }
  if (!(dist > 1e-6)) return { error: 'Drag outward to extend the face' }
  const n = faceNormal(b, F)
  const L = F.loop, k = L.length
  const base = L.map((i) => b.vertices[i])
  const offset = scale(n, dist)
  const vertices: Vec3[] = [...base, ...base.map((p) => add(p, offset))]
  const strip = (f: Face): Face => {
    const { id: _i, loop: _l, uvs: _u, ...rest } = f
    return rest as Face
  }
  const cap: Face = { ...strip(F), id: 0, loop: L.map((_, i) => k + i) }
  const bottom: Face = { ...strip(F), id: 1, loop: L.map((_, i) => k - 1 - i) }
  if (F.uvs && F.uvs.length === k) { const uv = (): [number, number][] => F.uvs!.map((u) => [u[0], u[1]]); cap.uvs = uv(); bottom.uvs = uv().reverse() }
  const faces: Face[] = [cap, bottom]
  for (let i = 0; i < k; i++) {
    const j = (i + 1) % k
    // the face of b across the edge L[i] -> L[j] contains that edge the other way round
    const g = b.faces.find((f) => f !== F && f.loop.some((v, x) => v === L[j] && f.loop[(x + 1) % f.loop.length] === L[i]))
    const loop = [i, j, k + j, k + i]
    const wall: Face = { ...strip(g ?? F), id: 2 + i, loop }
    const fit = g ? uvFit(b, g) : null
    if (fit) wall.uvs = loop.map((vi) => fit(vertices[vi]))
    faces.push(wall)
  }
  const out: Brush = { id: newId, op: b.op ?? 'add', ...(b.carve ? { carve: { ...b.carve } } : {}), vertices, faces }
  if (!isProperSolid(out)) return { error: 'This face cannot be extended (the result would not be a valid solid)' }
  return { brush: out }
}
