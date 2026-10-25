// Derived geometry from Core (DESIGN.md §5.6): boolean carving and decals. The document is never changed;
// this turns Core's `derived_geometry` message into drawable / pickable meshes and decides, per brush and
// per decal, whether they can still be trusted.
//
//  * Rule 1 (the Shell never waits): while an edit that touches a carved brush (or a decal, or the surface
//    under it) is still on its way to Core, that thing is drawn as authored and swaps to the result when it
//    arrives. Everything the edit did not touch keeps showing its derived result.
//  * Fragments are convex; a face with a hole is several fragments. `boundaryEdges` drops the seams
//    between fragments of the same authored face so outlines show the shape, not the tessellation.
//  * Prefab content takes part: entries carry an `instance` path ("" = the document itself).
import type { Brush, Face, MapDocument } from './document'
import { classOf } from './document'
import type { ResolvedEntity } from './expand'
import { Vec3, cross, normalize, sub } from './math'
import { planarCoordsProjected } from './uv'
import { brushTextureSignature, faceUvAt, lerpVec3, planForFace, retexture } from './uvmesh'
import { texEnv } from './texprops'
import type { NeighbourFn } from './reliefApply'
import { fanBorder, neighbourFn, newBudget, planHasRelief, planWithRelief, polygonBorders, reliefOf, reliefTriangle } from './reliefApply'

/** `instance` = the instance that owns `brush` (a prefab's subtractor may cut the world, so it differs from the carved brush's own). Absent = the entry's instance. */
export interface DerivedFragment { brush: string; instance?: string; face: number; v: number[] }
/** `subtractorInstances[i]` = the instance of `subtractors[i]`. Absent = all in the entry's instance. */
export interface DerivedEntry { entityId: string; instance: string; brushId: string; subtractors: string[]; subtractorInstances?: string[]; fragments: DerivedFragment[] }
export interface DecalFragment { entityId: string; instance: string; brush: string; face: number; v: number[]; uv: number[] }
export interface DecalEntry { decalId: string; instance: string; texture: string; fragments: DecalFragment[] }
export interface DerivedResult { rev: number; carved: DerivedEntry[]; skipped: { entityId: string; instance?: string; brushId: string }[]; decals: DecalEntry[] }

/** The document plus its expanded prefab content: what Core derived from, and what is on screen now. */
export interface World { doc: MapDocument; prefabs: ResolvedEntity[] }

/** A pickable triangle of a carved surface: it names the authored face it lies on, and who owns it. */
export interface PickTri { a: Vec3; b: Vec3; c: Vec3; normal: Vec3; entityId: string; /** Instance that owns entityId/brushId ("" = the document). */ instance: string; brushId: string; faceId: number }

/** Vertex layout: position(3) normal(3) uv(2), one run per texture. */
export interface CarvedMesh {
  textures: Map<string, number[]>
  /** Line-segment endpoints (x,y,z pairs) of the outline. */
  edges: number[]
  pickTris: PickTri[]
}

export const instanceOf = (r: ResolvedEntity): string => r.instancePath.slice(1).join('/')
/** Key of a carved brush; the document's own brushes keep the short form `entityId/brushId`. */
export const carvedKey = (entityId: string, brushId: string, instance = ''): string => (instance ? `${instance}|${entityId}/${brushId}` : `${entityId}/${brushId}`)

export interface Owned { brush: Brush; entityId: string }
export function brushIndex(w: World, instance: string): Map<string, Owned> {
  const m = new Map<string, Owned>()
  for (const e of w.doc.entities) for (const b of e.brushes ?? []) m.set(b.id, { brush: b, entityId: e.id }) // the root's brushes are visible from everywhere
  if (instance) for (const r of w.prefabs) if (instanceOf(r) === instance) for (const b of r.entity.brushes ?? []) m.set(b.id, { brush: b, entityId: r.entity.id })
  return m
}

const fragInstance = (entry: DerivedEntry, f: DerivedFragment): string => f.instance ?? entry.instance
const subtractorInstance = (entry: DerivedEntry, i: number): string => entry.subtractorInstances?.[i] ?? entry.instance

const vert = (v: number[], i: number): Vec3 => [v[i * 3], v[i * 3 + 1], v[i * 3 + 2]]
function polygonNormal(v: number[]): Vec3 {
  const n = v.length / 3
  const p0 = vert(v, 0)
  let s: Vec3 = [0, 0, 0]
  for (let i = 1; i + 1 < n; i++) {
    const c = cross(sub(vert(v, i), p0), sub(vert(v, i + 1), p0))
    s = [s[0] + c[0], s[1] + c[1], s[2] + c[2]]
  }
  return normalize(s)
}

/** Triangles (fan) + outline + pick triangles for one carved brush, textured from each fragment's authored parent face. */
export function buildCarvedMesh(entry: DerivedEntry, w: World): CarvedMesh {
  const indexes = new Map<string, Map<string, Owned>>()
  const brushesOf = (instance: string) => { let m = indexes.get(instance); if (!m) { m = brushIndex(w, instance); indexes.set(instance, m) } return m }
  const textures = new Map<string, number[]>()
  const pickTris: PickTri[] = []
  const budget = newBudget()
  const uvFns = new Map<Face, (p: Vec3) => [number, number]>()
  // which fragment edges are the outline of their authored face: a relief is closed there by connecting walls (reliefApply.ts), not over the seams
  const groups = new Map<string, DerivedFragment[]>()
  for (const f of entry.fragments) { const k = `${f.brush}#${f.face}`; const g = groups.get(k) ?? []; groups.set(k, g); g.push(f) }
  const borders = new Map<DerivedFragment, boolean[]>()
  const neighbours = new Map<Face, NeighbourFn>() // what stands next to each authored face's edges (reliefApply.ts neighbourFn)
  const bordersOf = (f: DerivedFragment): boolean[] => {
    let b = borders.get(f)
    if (!b) {
      const g = groups.get(`${f.brush}#${f.face}`) ?? [f]
      const per = polygonBorders(g.map((x) => Array.from({ length: x.v.length / 3 }, (_, i) => vert(x.v, i))))
      g.forEach((x, i) => borders.set(x, per[i]))
      b = borders.get(f)!
    }
    return b
  }
  for (const f of entry.fragments) {
    const n = f.v.length / 3
    if (n < 3) continue
    const instance = fragInstance(entry, f)
    const owned = brushesOf(instance).get(f.brush)
    const parent: Face | undefined = owned?.brush.faces.find((x) => x.id === f.face)
    const normal = polygonNormal(f.v)
    const pts: Vec3[] = Array.from({ length: n }, (_, i) => vert(f.v, i))
    const relief = reliefOf(parent?.texture) // texture relief on the fragment (model/reliefApply.ts)
    const plan = parent && owned ? planForFace(owned.brush, parent) : null // repetition behaviour: cut along the fold lines (model/uvmesh.ts); relief follows it
    // the UVs the face itself is drawn with (stored uvs, which UV lock moves with the face, before the projection): no jump between face and fragments
    let uvAt = parent && owned ? uvFns.get(parent) : undefined
    if (parent && owned && !uvAt) { uvAt = faceUvAt(owned.brush, parent); uvFns.set(parent, uvAt) }
    const uv = plan ? [] : uvAt ? pts.map(uvAt) : planarCoordsProjected(normal, pts, undefined)
    const tex = parent?.texture ?? 'dev/grey'
    const list = textures.get(tex) ?? []
    textures.set(tex, list)
    const edgeB = relief || (plan && planHasRelief(plan)) ? bordersOf(f) : undefined
    let nb: NeighbourFn | undefined
    if (edgeB && parent && owned) { nb = neighbours.get(parent); if (!nb) { nb = neighbourFn(owned.brush, parent); neighbours.set(parent, nb) } }
    for (let i = 1; i + 1 < n; i++) {
      const border = edgeB ? fanBorder(i, n, edgeB) : 0
      if (relief && !plan) {
        const got = reliefTriangle([pts[0], pts[i], pts[i + 1]], [uv[0], uv[i], uv[i + 1]], normal, relief, budget, border, nb)
        if (got) { for (const x of got) list.push(x); pickTris.push({ a: pts[0], b: pts[i], c: pts[i + 1], normal, entityId: owned?.entityId ?? entry.entityId, instance, brushId: f.brush, faceId: f.face }); continue }
      }
      if (plan) {
        const tri: [Vec3, Vec3, Vec3] = [pts[0], pts[i], pts[i + 1]]
        if (planHasRelief(plan)) planWithRelief(plan, [tri], normal, textures, budget, [border], nb) // every piece takes the relief of the texture it samples
        else for (const o of retexture(plan, [tri])) {
          const l = textures.get(o.texture) ?? []
          textures.set(o.texture, l)
          for (const v of o.verts) l.push(...lerpVec3(tri, v.w), ...normal, v.uv[0], v.uv[1])
        }
      } else for (const k of [0, i, i + 1]) list.push(...pts[k], ...normal, uv[k][0], uv[k][1])
      // cut walls belong to the subtractor's face (that is what carries their texture); the rest to the carved brush
      pickTris.push({ a: pts[0], b: pts[i], c: pts[i + 1], normal, entityId: owned?.entityId ?? entry.entityId, instance, brushId: f.brush, faceId: f.face })
    }
  }
  return { textures, edges: boundaryEdges(entry.fragments), pickTris }
}

const Q = 1e4
const qkey = (p: Vec3): string => `${Math.round(p[0] * Q)},${Math.round(p[1] * Q)},${Math.round(p[2] * Q)}`

/**
 * Outline of the union of fragments that share an authored parent face: every polygon edge, minus the
 * ones that also appear (reversed) in a neighbouring fragment. Edges are first split at any vertex of
 * the group lying on them, so T-junctions left by the BSP split cancel too.
 */
export function boundaryEdges(fragments: { brush: string; face: number; v: number[] }[]): number[] {
  const groups = new Map<string, typeof fragments>()
  for (const f of fragments) {
    const k = `${f.brush}#${f.face}`
    const g = groups.get(k) ?? []
    groups.set(k, g)
    g.push(f)
  }
  const out: number[] = []
  for (const frags of groups.values()) {
    const verts = new Map<string, Vec3>()
    for (const f of frags) for (let i = 0; i < f.v.length / 3; i++) verts.set(qkey(vert(f.v, i)), vert(f.v, i))
    const all = [...verts.values()]
    const seg = new Map<string, { a: Vec3; b: Vec3 }>()
    const add = (a: Vec3, b: Vec3) => {
      const ka = qkey(a), kb = qkey(b)
      if (ka === kb) return
      const rev = `${kb}>${ka}`
      if (seg.delete(rev)) return // the neighbour has the same edge the other way round: internal seam
      seg.set(`${ka}>${kb}`, { a, b })
    }
    for (const f of frags) {
      const n = f.v.length / 3
      for (let i = 0; i < n; i++) {
        const a = vert(f.v, i), b = vert(f.v, (i + 1) % n)
        const d = sub(b, a), len2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2]
        if (len2 < 1e-12) continue
        const on: { t: number; p: Vec3 }[] = []
        for (const p of all) {
          const t = ((p[0] - a[0]) * d[0] + (p[1] - a[1]) * d[1] + (p[2] - a[2]) * d[2]) / len2
          if (t <= 1e-6 || t >= 1 - 1e-6) continue
          const c: Vec3 = [a[0] + d[0] * t - p[0], a[1] + d[1] * t - p[1], a[2] + d[2] * t - p[2]]
          if (c[0] * c[0] + c[1] * c[1] + c[2] * c[2] < 1e-10) on.push({ t, p })
        }
        on.sort((x, y) => x.t - y.t)
        let prev = a
        for (const { p } of on) { add(prev, p); prev = p }
        add(prev, b)
      }
    }
    for (const { a, b } of seg.values()) out.push(...a, ...b)
  }
  return out
}

type MeshCache = WeakMap<DerivedEntry, { deps: unknown[]; mesh: CarvedMesh; rev: number; sig: string }>

/** Does `id` name the same brush object now as when Core computed the result? (prefab content: same expansion) */
function sameBrush(id: string, instance: string, now: World, then: World): boolean {
  const a = brushIndex(now, instance).get(id)?.brush, b = brushIndex(then, instance).get(id)?.brush
  if (!a || !b) return false
  return a === b || (instance !== '' && now.prefabs === then.prefabs)
}

const texSignatureOf = (parents: (Brush | undefined)[]): string => parents.map((b) => (b ? brushTextureSignature(b) : '')).join('#')

/**
 * Which carved brushes may be drawn from `result` right now. `basis` is the world Core computed it from. A
 * brush qualifies only while it, its owner's class and every subtractor that carved it are unchanged since
 * then; anything else falls back to the authored geometry until Core answers again.
 */
/** Is Core's result for this carved brush still the result for the world now? (the brush, its owner's class and every subtractor that carved it are unchanged) */
function entryUsable(entry: DerivedEntry, basis: World, current: World): boolean {
  const ownerNow = entry.instance ? current.prefabs.find((r) => instanceOf(r) === entry.instance && r.entity.id === entry.entityId)?.entity : current.doc.entities.find((e) => e.id === entry.entityId)
  const ownerThen = entry.instance ? basis.prefabs.find((r) => instanceOf(r) === entry.instance && r.entity.id === entry.entityId)?.entity : basis.doc.entities.find((e) => e.id === entry.entityId)
  if (!ownerNow || !ownerThen || classOf(ownerNow) !== classOf(ownerThen)) return false
  if (!sameBrush(entry.brushId, entry.instance, current, basis)) return false
  return entry.subtractors.every((id, i) => sameBrush(id, subtractorInstance(entry, i), current, basis))
}

/** The entries of `result` that can be trusted for `current` (see `usableCarved`). */
export const usableEntries = (result: DerivedResult, basis: World, current: World): DerivedEntry[] => result.carved.filter((e) => entryUsable(e, basis, current))

export function usableCarved(result: DerivedResult, basis: World, current: World, cache: MeshCache): Map<string, CarvedMesh> {
  const out = new Map<string, CarvedMesh>()
  if (result.carved.length === 0) return out
  for (const entry of result.carved) {
    if (!entryUsable(entry, basis, current)) continue
    // fragments also borrow texture/projection from their parent faces; rebuild when those change
    const seen = new Set<string>(), parents: (Brush | undefined)[] = []
    for (const f of entry.fragments) {
      const inst = fragInstance(entry, f), key = `${inst}|${f.brush}`
      if (seen.has(key)) continue
      seen.add(key)
      parents.push(brushIndex(current, inst).get(f.brush)?.brush)
    }
    const hit = cache.get(entry)
    let mesh = hit && hit.deps.length === parents.length && hit.deps.every((d, i) => d === parents[i]) ? hit.mesh : null
    // the textures' sizes / properties changed (repetition behaviour changes the UVs): rebuild only when this entry's textures are affected
    const sig = texSignatureOf(parents)
    if (mesh && hit && hit.rev !== texEnv.rev) { if (hit.sig === sig) hit.rev = texEnv.rev; else mesh = null }
    if (!mesh) { mesh = buildCarvedMesh(entry, current); cache.set(entry, { deps: parents, mesh, rev: texEnv.rev, sig }) }
    out.set(carvedKey(entry.entityId, entry.brushId, entry.instance), mesh)
  }
  return out
}

// ---- decals -------------------------------------------------------------------------------------
export interface DecalMesh { decalId: string; textures: Map<string, number[]>; mesh: CarvedMesh }

/** Draws a decal from its fragments: positions, the surface normal, and the projector's uv. */
export function buildDecalMesh(d: DecalEntry): CarvedMesh {
  const textures = new Map<string, number[]>()
  const list: number[] = []
  const pickTris: PickTri[] = []
  for (const f of d.fragments) {
    const n = f.v.length / 3
    if (n < 3) continue
    const normal = polygonNormal(f.v)
    for (let i = 1; i + 1 < n; i++) for (const k of [0, i, i + 1]) list.push(...vert(f.v, k), ...normal, f.uv[k * 2], f.uv[k * 2 + 1])
  }
  if (list.length) textures.set(d.texture, list)
  return { textures, edges: [], pickTris }
}

/**
 * Decals to draw now. A decal is trusted while its projector entity and every brush it landed on are unchanged
 * since Core computed it; otherwise it is hidden until Core answers (a decal has no authored fallback shape).
 */
export function usableDecalEntries(result: DerivedResult, basis: World, current: World): DecalEntry[] {
  const out: DecalEntry[] = []
  for (const d of result.decals) {
    const find = (w: World) => (d.instance ? w.prefabs.find((r) => instanceOf(r) === d.instance && r.entity.id === d.decalId)?.entity : w.doc.entities.find((e) => e.id === d.decalId))
    const now = find(current), then = find(basis)
    if (!now || !then || (now !== then && !(d.instance && current.prefabs === basis.prefabs))) continue
    if (!d.fragments.every((f) => sameBrush(f.brush, f.instance, current, basis))) continue
    out.push(d)
  }
  return out
}

export function usableDecals(result: DerivedResult, basis: World, current: World, cache: WeakMap<DecalEntry, CarvedMesh>): CarvedMesh[] {
  return usableDecalEntries(result, basis, current).map((d) => {
    let mesh = cache.get(d)
    if (!mesh) { mesh = buildDecalMesh(d); cache.set(d, mesh) }
    return mesh
  })
}
