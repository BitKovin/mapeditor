// The Lit view's geometry: the Core's lit mesh (positions + lightmap coordinates, core/lighting_service.h) given albedo texture coordinates.
// The Core does not know texture projections (they live in the document), so it sends a face table (one entry per surface piece: the authored
// face it lies on and its triangle count, in vertex order) and the Shell projects the albedo exactly like it does for carved fragments
// (model/derived.ts): the parent face's projection applied to the piece's own points.
import type { LitFace, LitFaceMap } from '../net/protocol'
import type { DecalEntry } from '../model/derived'
import type { World } from '../model/derived'
import { brushIndex } from '../model/derived'
import type { Vec3 } from '../model/math'
import { planarCoordsProjected } from '../model/uv'
import type { FacePlan } from '../model/uvmesh'
import { faceUvAt, planForFace, retexture } from '../model/uvmesh'
import type { Relief } from '../model/relief'
import type { NeighbourFn } from '../model/reliefApply'
import { neighbourFn, newBudget, pieceBorder, planHasRelief, polygonBorders, reliefOf } from '../model/reliefApply'
import type { ReliefBudget } from '../model/reliefApply'
import { reliefTriangle } from '../model/reliefApply'
import type { LitAnalysis } from './litStale'
import { staleKey } from './litStale'

/** Floats per vertex of the lit mesh as the Core sends it: position (3), normal (3), lightmap u, v, page, style mask (3). */
const CORE_FLOATS = 12
/** Floats per vertex of the textured lit mesh: position (3), normal (3), lightmap u, v, page, style mask (3), albedo u, v. */
export const LIT_TEX_FLOATS = 14

/**
 * A run of lit mesh triangles (Core layout, `count` vertices from vertex `first`) with the texture's relief laid over every triangle: the geometry of
 * model/reliefApply.ts `reliefTriangle` (the same as the Unlit view), lightmap coordinates taken from the flat triangle. The lightmap uv is affine in the
 * position on the face plane, and every relief vertex projects to a point of that plane with the albedo coordinate `uv` as its barycentric key, so
 * the new vertices interpolate lightmap u / v from the flat triangle's corners; page and style mask come from its first corner.
* The normal slot carries the unit relief normal (the lit shaders read `length(normal)` as a brightness factor, 1 here): the light of a relief is the bake's,
 * which sees the relief (relief_geometry in the Core: it casts shadows, and every texel is lit where the relief really is). `uv[k]` is the albedo coordinate of vertex k of the run. A triangle that cannot be expanded stays flat.
 * Returns LIT_TEX_FLOATS per vertex (position, normal, lightmap u, v, page, style mask, albedo u, v).
 * `borders[t]` = the border bits of triangle t of the run (reliefApply.ts reliefTriangle: which of its edges are the outline of the face).
 */
export function displaceLit(core: ArrayLike<number>, first: number, count: number, uv: ReadonlyArray<readonly [number, number]>, relief: Relief, budget: ReliefBudget, borders?: readonly number[], neighbour?: NeighbourFn): number[] {
  const out: number[] = []
  for (let t = 0; t + 2 < count; t += 3) {
    const o = [(first + t) * CORE_FLOATS, (first + t + 1) * CORE_FLOATS, (first + t + 2) * CORE_FLOATS]
    const pos = o.map((i) => [core[i], core[i + 1], core[i + 2]] as Vec3) as [Vec3, Vec3, Vec3]
    const nF: Vec3 = [core[o[0] + 3], core[o[0] + 4], core[o[0] + 5]]
    const tuv: [[number, number], [number, number], [number, number]] = [[uv[t][0], uv[t][1]], [uv[t + 1][0], uv[t + 1][1]], [uv[t + 2][0], uv[t + 2][1]]]
    const got = reliefTriangle(pos, tuv, nF, relief, budget, borders?.[t / 3] ?? 0, neighbour)
    if (!got) { for (let c = 0; c < 3; c++) { for (let k = 0; k < CORE_FLOATS; k++) out.push(core[o[c] + k]); out.push(tuv[c][0], tuv[c][1]) } continue }
    const [a0, a1, a2] = tuv
    const det = (a1[0] - a0[0]) * (a2[1] - a0[1]) - (a2[0] - a0[0]) * (a1[1] - a0[1])
    for (let i = 0; i + 7 < got.length; i += 8) {
      const u = got[i + 6], v = got[i + 7]
      const w1 = ((u - a0[0]) * (a2[1] - a0[1]) - (v - a0[1]) * (a2[0] - a0[0])) / det
      const w2 = ((a1[0] - a0[0]) * (v - a0[1]) - (a1[1] - a0[1]) * (u - a0[0])) / det
      const w0 = 1 - w1 - w2
      out.push(got[i], got[i + 1], got[i + 2], got[i + 3], got[i + 4], got[i + 5],
        core[o[0] + 6] * w0 + core[o[1] + 6] * w1 + core[o[2] + 6] * w2, core[o[0] + 7] * w0 + core[o[1] + 7] * w1 + core[o[2] + 7] * w2,
        core[o[0] + 8], core[o[0] + 9], core[o[0] + 10], core[o[0] + 11], u, v)
    }
  }
  return out
}

/** The border bits of a triangle from the outline flags of its three edges (reliefApply.ts polygonBorders). */
const triangleBits = (flags: readonly (readonly boolean[])[]): number[] => flags.map((e) => (e[0] ? 1 : 0) | (e[1] ? 2 : 0) | (e[2] ? 4 : 0))
const runTriangles = (core: ArrayLike<number>, first: number, count: number): Vec3[][] => {
  const out: Vec3[][] = []
  for (let t = 0; t + 2 < count; t += 3) out.push([0, 1, 2].map((c) => { const o = (first + t + c) * CORE_FLOATS; return [core[o], core[o + 1], core[o + 2]] as Vec3 }))
  return out
}

/**
 * The border bits per triangle of every piece of the faces that show a relief (`wanted`): the pieces that lie on one authored face (and side) are one
 * face, whose outline closes the relief with connecting walls (reliefApply.ts); the seams between its pieces do not. Keyed by the piece.
 */
function pieceBorders(mesh: ArrayLike<number>, faces: readonly LitFace[], stale: LitAnalysis | null | undefined, wanted: (f: LitFace) => boolean): Map<LitFace, number[]> {
  const total = Math.floor(mesh.length / CORE_FLOATS)
  const groups = new Map<string, { tris: Vec3[][]; slots: { face: LitFace; k: number }[] }>()
  let at = 0
  for (const f of faces) {
    const verts = f.n * 3
    if (at + verts > total) break
    if (!stale?.stale.has(staleKey(f.bi, f.b)) && wanted(f)) {
      const tris = runTriangles(mesh, at, verts)
      const o = at * CORE_FLOATS
      const key = `${f.i}|${f.e}|${f.bi}|${f.b}|${f.f}|${Math.round(mesh[o + 3] * 100)},${Math.round(mesh[o + 4] * 100)},${Math.round(mesh[o + 5] * 100)}` // the normal tells a cut wall from the face's front
      let g = groups.get(key)
      if (!g) { g = { tris: [], slots: [] }; groups.set(key, g) }
      tris.forEach((t, k) => { g!.tris.push(t); g!.slots.push({ face: f, k }) })
    }
    at += verts
  }
  const out = new Map<LitFace, number[]>()
  for (const g of groups.values()) {
    const bits = triangleBits(polygonBorders(g.tris))
    g.slots.forEach((slot, m) => { const list = out.get(slot.face) ?? []; out.set(slot.face, list); list[slot.k] = bits[m] })
  }
  return out
}

/** The albedo coordinates of a run of lit mesh vertices as pairs (the parent face's projection applied to the piece's own points). */
function pieceUv(mesh: ArrayLike<number>, first: number, count: number, uvAt: ((p: Vec3) => [number, number]) | undefined): [number, number][] {
  const pts: Vec3[] = []
  for (let k = 0; k < count; k++) { const o = (first + k) * CORE_FLOATS; pts.push([mesh[o], mesh[o + 1], mesh[o + 2]]) }
  const o0 = first * CORE_FLOATS
  return uvAt ? pts.map(uvAt) : (planarCoordsProjected([mesh[o0 + 3], mesh[o0 + 4], mesh[o0 + 5]], pts, undefined) as [number, number][])
}

/**
 * The pieces a repetition plan (mirror, anchor, alternate texture, edge clamp) cuts the `nTris` lit mesh triangles at vertex `at` into, each as three Core
 * vertices (the lightmap coordinates interpolated by the barycentric weights: the lightmap uv is affine in the position; page and style mask from the
 * source triangle's first vertex) with the albedo coordinates and the texture the piece samples.
 */
function planTriangles(mesh: ArrayLike<number>, at: number, nTris: number, plan: FacePlan, borders?: readonly number[]): { texture: string; core: number[]; uv: [number, number][]; border: number }[] {
  const tris: [Vec3, Vec3, Vec3][] = []
  for (let t = 0; t < nTris; t++) tris.push([0, 1, 2].map((c) => { const o = (at + t * 3 + c) * CORE_FLOATS; return [mesh[o], mesh[o + 1], mesh[o + 2]] as Vec3 }) as [Vec3, Vec3, Vec3])
  return retexture(plan, tris).map((o) => {
    const base = (at + o.src * 3) * CORE_FLOATS
    const core: number[] = []
    for (const v of o.verts) {
      const w = v.w
      for (let c = 0; c < CORE_FLOATS; c++) {
        const interpolated = c <= 2 || c === 6 || c === 7
        core.push(interpolated ? mesh[base + c] * w[0] + mesh[base + CORE_FLOATS + c] * w[1] + mesh[base + 2 * CORE_FLOATS + c] * w[2] : mesh[base + c])
      }
    }
    return { texture: o.texture, core, uv: o.verts.map((v) => [v.uv[0], v.uv[1]] as [number, number]), border: pieceBorder(o.verts, borders?.[o.src] ?? 0) }
  })
}

/**
 * The mesh the Lighting-only view draws (Core layout) when some face has a relief: pieces of stale brushes left out, relief laid over the pieces whose
 * texture has one, the stale brushes' current faces (overlay) after them. null when no piece or overlay face has a relief (the plain mesh is the Core's).
 */
export function buildReliefPlainMesh(mesh: Float32Array, faces: readonly LitFace[], world: World, stale: LitAnalysis | null): Float32Array | null {
  const indexes = new Map<string, ReturnType<typeof brushIndex>>()
  const owner = (instance: string) => { let m = indexes.get(instance); if (!m) { m = brushIndex(world, instance); indexes.set(instance, m) } return m }
  const infoOf = (f: LitFace) => {
    const brush = owner(f.bi).get(f.b)?.brush
    const parent = brush?.faces.find((x) => x.id === f.f)
    return { brush, parent, plan: brush && parent ? planForFace(brush, parent) : null }
  }
  // nothing to do (the common case) = the Core's mesh is used as it is
  if (!faces.some((f) => reliefOf(f.t) || (() => { const { plan } = infoOf(f); return plan !== null && planHasRelief(plan) })()) && !stale?.overlay.some((o) => reliefOf(o.texture))) return null
  const budget = newBudget()
  const out: number[] = []
  const total = Math.floor(mesh.length / CORE_FLOATS)
  const borders = pieceBorders(mesh, faces, stale, (f) => !!reliefOf(f.t) || (() => { const { plan } = infoOf(f); return plan !== null && planHasRelief(plan) })())
  const pushRelief = (core: ArrayLike<number>, first: number, count: number, uv: [number, number][], relief: Relief, border?: readonly number[], nb?: NeighbourFn): void => {
    const d = displaceLit(core, first, count, uv, relief, budget, border, nb)
    for (let i = 0; i < d.length; i += LIT_TEX_FLOATS) for (let k = 0; k < CORE_FLOATS; k++) out.push(d[i + k]) // the albedo coordinates are not needed here
  }
  let at = 0
  for (const f of faces) {
    const verts = f.n * 3
    if (at + verts > total) break
    if (!stale?.stale.has(staleKey(f.bi, f.b))) {
      const relief = reliefOf(f.t)
      const { brush, parent, plan } = infoOf(f)
      const nb = brush && parent ? neighbourFn(brush, parent) : undefined
      if (plan && planHasRelief(plan)) {
        // repetition behaviour decides where each tile of the relief goes: the pieces carry the uv (and texture) the relief is placed by
        for (const piece of planTriangles(mesh, at, f.n, plan, borders.get(f))) {
          const r = reliefOf(piece.texture)
          if (r) pushRelief(piece.core, 0, 3, piece.uv, r, [piece.border], nb)
          else out.push(...piece.core)
        }
      } else if (relief && !plan) pushRelief(mesh, at, verts, pieceUv(mesh, at, verts, brush && parent ? faceUvAt(brush, parent) : undefined), relief, borders.get(f), nb)
      else for (let i = at * CORE_FLOATS; i < (at + verts) * CORE_FLOATS; i++) out.push(mesh[i])
    }
    at += verts
  }
  for (const o of stale?.overlay ?? []) {
    const relief = reliefOf(o.texture)
    if (relief) pushRelief(o.core, 0, o.vertexCount, Array.from({ length: o.vertexCount }, (_, k) => [o.uv[k * 2], o.uv[k * 2 + 1]] as [number, number]), relief, triangleBits(polygonBorders(runTriangles(o.core, 0, o.vertexCount))))
    else for (const v of o.core) out.push(v)
  }
  return Float32Array.from(out)
}

export interface LitGroup { texture: string; first: number; count: number }
export interface LitDraw { data: Float32Array; groups: LitGroup[]; vertexCount: number }

/**
 * Regroups the lit mesh per texture (one draw call each) and appends the albedo UV. Faces whose triangles do not add up to the mesh are
 * ignored from the first mismatch on (a stale table must never read outside the vertices).
 */
export function buildLitDraw(mesh: Float32Array, faces: readonly LitFace[], world: World, stale?: LitAnalysis | null): LitDraw {
  const indexes = new Map<string, ReturnType<typeof brushIndex>>()
  const owner = (instance: string) => { let m = indexes.get(instance); if (!m) { m = brushIndex(world, instance); indexes.set(instance, m) } return m }
  const byTexture = new Map<string, number[]>()
  const budget = newBudget()
  const borders = pieceBorders(mesh, faces, stale, (f) => {
    if (reliefOf(f.t)) return true
    const b = owner(f.bi).get(f.b)?.brush, p = b?.faces.find((x) => x.id === f.f), plan = b && p ? planForFace(b, p) : null
    return plan !== null && planHasRelief(plan)
  })
  let at = 0 // vertex cursor
  const total = Math.floor(mesh.length / CORE_FLOATS)
  for (const f of faces) {
    const verts = f.n * 3
    if (at + verts > total) break
    if (stale?.stale.has(staleKey(f.bi, f.b))) { at += verts; continue } // changed since the Core's mesh: drawn from the overlay below
    const ownerBrush = owner(f.bi).get(f.b)?.brush
    const parent = ownerBrush?.faces.find((x) => x.id === f.f)
    const relief = reliefOf(f.t) // texture relief laid over the piece (displaceLit)
    const plan = ownerBrush && parent ? planForFace(ownerBrush, parent) : null
    const nb = ownerBrush && parent && (relief || plan) ? neighbourFn(ownerBrush, parent) : undefined
    if (plan) {
      // repetition behaviour: every triangle is cut along the fold lines; the new vertices (invisible: same plane) take the lightmap coordinates by
      // interpolation (the lightmap uv is affine in the position), page and style mask from the triangle. A piece takes the relief of the texture it
      // samples (the face's own or an alternate), placed by the piece's own uv: mirrored, anchored and clamped tiles carry their relief with them
      for (const piece of planTriangles(mesh, at, f.n, plan, borders.get(f))) {
        const list = byTexture.get(piece.texture) ?? []
        byTexture.set(piece.texture, list)
        const r = reliefOf(piece.texture)
        if (r) { for (const x of displaceLit(piece.core, 0, 3, piece.uv, r, budget, [piece.border], nb)) list.push(x); continue }
        for (let k = 0; k < 3; k++) { for (let c = 0; c < CORE_FLOATS; c++) list.push(piece.core[k * CORE_FLOATS + c]); list.push(piece.uv[k][0], piece.uv[k][1]) }
      }
      at += verts
      continue
    }
    if (relief) {
      const list = byTexture.get(f.t) ?? []
      byTexture.set(f.t, list)
      for (const x of displaceLit(mesh, at, verts, pieceUv(mesh, at, verts, ownerBrush && parent ? faceUvAt(ownerBrush, parent) : undefined), relief, budget, borders.get(f), nb)) list.push(x)
      at += verts
      continue
    }
    const uvAt = ownerBrush && parent ? faceUvAt(ownerBrush, parent) : undefined
    const pts: Vec3[] = []
    for (let k = 0; k < verts; k++) { const o = (at + k) * CORE_FLOATS; pts.push([mesh[o], mesh[o + 1], mesh[o + 2]]) }
    const o0 = at * CORE_FLOATS
    const normal: Vec3 = [mesh[o0 + 3], mesh[o0 + 4], mesh[o0 + 5]]
    const uv = uvAt ? pts.map(uvAt) : planarCoordsProjected(normal, pts, undefined)
    const list = byTexture.get(f.t) ?? []
    byTexture.set(f.t, list)
    for (let k = 0; k < verts; k++) {
      const o = (at + k) * CORE_FLOATS
      for (let c = 0; c < CORE_FLOATS; c++) list.push(mesh[o + c])
      list.push(uv[k][0], uv[k][1])
    }
    at += verts
  }
  for (const o of stale?.overlay ?? []) {
    const list = byTexture.get(o.texture) ?? []
    byTexture.set(o.texture, list)
    const oRelief = reliefOf(o.texture)
    if (oRelief) {
      for (const x of displaceLit(o.core, 0, o.vertexCount, Array.from({ length: o.vertexCount }, (_, k) => [o.uv[k * 2], o.uv[k * 2 + 1]] as [number, number]), oRelief, budget, triangleBits(polygonBorders(runTriangles(o.core, 0, o.vertexCount))))) list.push(x)
      continue
    }
    for (let k = 0; k < o.vertexCount; k++) { for (let c = 0; c < CORE_FLOATS; c++) list.push(o.core[k * CORE_FLOATS + c]); list.push(o.uv[k * 2], o.uv[k * 2 + 1]) }
  }
  const groups: LitGroup[] = []
  let count = 0
  for (const l of byTexture.values()) count += l.length
  const data = new Float32Array(count)
  let off = 0
  for (const [texture, l] of byTexture) {
    data.set(l, off)
    groups.push({ texture, first: off / LIT_TEX_FLOATS, count: l.length / LIT_TEX_FLOATS })
    off += l.length
  }
  return { data, groups, vertexCount: count / LIT_TEX_FLOATS }
}

/** Key of a face in `LightmapStore.faceMaps`. */
const faceKey = (instance: string, entityId: string, brushId: string, faceId: number): string => `${instance}|${entityId}|${brushId}|${faceId}`

function polygonNormal(v: readonly number[]): Vec3 {
  let nx = 0, ny = 0, nz = 0
  const n = v.length / 3
  for (let i = 0; i < n; i++) {
    const a = i * 3, b = ((i + 1) % n) * 3
    nx += (v[a + 1] - v[b + 1]) * (v[a + 2] + v[b + 2]); ny += (v[a + 2] - v[b + 2]) * (v[a] + v[b]); nz += (v[a] - v[b]) * (v[a + 1] + v[b + 1])
  }
  const l = Math.hypot(nx, ny, nz) || 1
  return [nx / l, ny / l, nz / l]
}

/**
 * Decals of the Lit view that lie on lightmapped faces: the projector's fragments with the lightmap coordinates of the face they lie on
 * (lightmap uv = the face's affine map of the position, same page, same style layers) and the decal's own albedo coordinates. One group per decal
 * texture, vertex layout as the lit mesh (LIT_TEX_FLOATS). Fragments on faces without a lightmap are left out (see buildEntityDecals).
 */
export function buildLitDecals(entries: readonly DecalEntry[], maps: ReadonlyMap<string, LitFaceMap>): LitDraw {
  const byTexture = new Map<string, number[]>()
  for (const d of entries) {
    for (const f of d.fragments) {
      const m = maps.get(faceKey(f.instance, f.entityId, f.brush, f.face))
      const n = f.v.length / 3
      if (!m || n < 3) continue
      const normal = polygonNormal(f.v)
      const list = byTexture.get(d.texture) ?? []
      byTexture.set(d.texture, list)
      const push = (k: number) => {
        const x = f.v[k * 3], y = f.v[k * 3 + 1], z = f.v[k * 3 + 2]
        list.push(x, y, z, normal[0], normal[1], normal[2],
          x * m.u[0] + y * m.u[1] + z * m.u[2] + m.u[3], x * m.v[0] + y * m.v[1] + z * m.v[2] + m.v[3], m.pg, m.k[0], m.k[1], m.k[2], f.uv[k * 2], f.uv[k * 2 + 1])
      }
      for (let i = 1; i + 1 < n; i++) { push(0); push(i); push(i + 1) }
    }
  }
  const groups: LitGroup[] = []
  let count = 0
  for (const l of byTexture.values()) count += l.length
  const data = new Float32Array(count)
  let off = 0
  for (const [texture, l] of byTexture) {
    data.set(l, off)
    groups.push({ texture, first: off / LIT_TEX_FLOATS, count: l.length / LIT_TEX_FLOATS })
    off += l.length
  }
  return { data, groups, vertexCount: count / LIT_TEX_FLOATS }
}

/** Decal fragments on faces of entities that are not lightmapped (doors, lifts): grouped per owning entity and decal texture, vertex layout position (3), normal (3), uv (2). */
export interface EntityDecalGroup { instance: string; entityId: string; texture: string; first: number; count: number }
export function buildEntityDecals(entries: readonly DecalEntry[], maps: ReadonlyMap<string, LitFaceMap>): { data: Float32Array; groups: EntityDecalGroup[]; vertexCount: number } {
  const lists = new Map<string, { instance: string; entityId: string; texture: string; v: number[] }>()
  for (const d of entries) {
    for (const f of d.fragments) {
      const n = f.v.length / 3
      if (n < 3 || maps.has(faceKey(f.instance, f.entityId, f.brush, f.face))) continue
      const key = `${f.instance}|${f.entityId}|${d.texture}`
      let g = lists.get(key)
      if (!g) { g = { instance: f.instance, entityId: f.entityId, texture: d.texture, v: [] }; lists.set(key, g) }
      const normal = polygonNormal(f.v)
      const push = (k: number) => g!.v.push(f.v[k * 3], f.v[k * 3 + 1], f.v[k * 3 + 2], normal[0], normal[1], normal[2], f.uv[k * 2], f.uv[k * 2 + 1])
      for (let i = 1; i + 1 < n; i++) { push(0); push(i); push(i + 1) }
    }
  }
  let total = 0
  for (const g of lists.values()) total += g.v.length
  const data = new Float32Array(total)
  const groups: EntityDecalGroup[] = []
  let off = 0
  for (const g of lists.values()) {
    data.set(g.v, off)
    groups.push({ instance: g.instance, entityId: g.entityId, texture: g.texture, first: off / 8, count: g.v.length / 8 })
    off += g.v.length
  }
  return { data, groups, vertexCount: total / 8 }
}
