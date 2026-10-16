// Rotate / scale / grab maths for object mode and mesh mode (DESIGN.md §8, §17). Pure functions: the viewport
// computes a parameter from the mouse (or typed digits) and asks for the result on every move, always from the
// document as it was when the operation began.
import type { Brush, Entity, MapDocument, PrefabInstance } from './document'
import { classOf, isBrushEntity, originOf } from './document'
import { brushBounds } from './geometry'
import { projectFace } from './uv'
import { Vec3, add, cross, dot, normalize, scale, sub } from './math'
import { Mat3, anglesFromRotation, apply3, det3, mul3, rotationFromAngles } from './rotation'
import { LIGHT_KINDS, isModernLight } from './lights'
import { mapShape } from './parametric'
import type { Selection } from '../state/store'

export type TransformKind = 'grab' | 'rotate' | 'scale'
export type PointMap = (p: Vec3) => Vec3

/** Rodrigues rotation of `p` about the line through `pivot` with unit direction `axis`. */
export function rotateAbout(pivot: Vec3, axis: Vec3, angle: number): PointMap {
  const k = normalize(axis), c = Math.cos(angle), s = Math.sin(angle)
  return (p) => {
    const v = sub(p, pivot)
    const r = add(add(scale(v, c), scale(cross(k, v), s)), scale(k, dot(k, v) * (1 - c)))
    return add(pivot, r)
  }
}

export const scaleAbout = (pivot: Vec3, f: Vec3): PointMap => (p) => [pivot[0] + (p[0] - pivot[0]) * f[0], pivot[1] + (p[1] - pivot[1]) * f[1], pivot[2] + (p[2] - pivot[2]) * f[2]]
export const translateBy = (d: Vec3): PointMap => (p) => add(p, d)

/** Scaling by a negative factor along an odd number of axes mirrors the mesh, which turns it inside out. */
export const mirrors = (f: Vec3): boolean => f.filter((x) => x < 0).length % 2 === 1

/**
 * Maps a brush's points. With `lockUv` (the default; the toolbar's UV lock) every face keeps the UVs it has now,
 * stored per corner, so the texture travels with the face. Without it the world-aligned projection is applied
 * again at the new place (the texture stays put in the world and slides across the face).
 */
export function mapBrushPoints(b: Brush, map: PointMap, flip = false, lockUv = true): Brush {
  const vertices = b.vertices.map(map)
  const moved: Brush = { ...b, vertices }
  const faces = b.faces.map((f) => {
    const loop = flip ? [...f.loop].reverse() : f.loop
    const base = f.uvs && f.uvs.length === f.loop.length ? f.uvs : null
    let uvs: [number, number][] | undefined
    if (lockUv) uvs = base ?? projectFace(b, f) // bake: the UVs as they look before the move
    uvs = uvs && flip ? [...uvs].reverse() : uvs
    const nf = { ...f, loop, ...(uvs ? { uvs } : {}) }
    if (!lockUv && base) nf.uvs = projectFace({ ...moved, faces: [{ ...nf, loop }] }, { ...nf, loop }) // unlocked: re-project
    else if (!lockUv) delete (nf as { uvs?: unknown }).uvs
    return nf
  })
  return mapShape({ ...moved, faces }, map) // a parametric shape's box moves with it (REQUIREMENTS §22 P1)
}

const tidy = (x: number): number => { const r = Math.round(x * 1e5) / 1e5; return Object.is(r, -0) ? 0 : r }
const fmt = (x: number): string => String(tidy(x))
const numKey = (e: Entity, key: string, dflt: number): number => { const n = Number(e.keyvalues?.[key]); return e.keyvalues?.[key] !== undefined && e.keyvalues[key].trim() !== '' && Number.isFinite(n) ? n : dflt }

/** The linear part of a point map (what it does to directions), probed through three unit steps like mapInstancePoints does. */
export function linearPart(map: PointMap, at: Vec3 = [0, 0, 0]): Mat3 {
  const o2 = map(at)
  const cols = ([[1, 0, 0], [0, 1, 0], [0, 0, 1]] as Vec3[]).map((e) => sub(map(add(at, e)), o2))
  return [cols[0][0], cols[1][0], cols[2][0], cols[0][1], cols[1][1], cols[2][1], cols[0][2], cols[1][2], cols[2][2]]
}
const col = (m: Mat3, i: number): Vec3 => [m[i], m[3 + i], m[6 + i]]
const len3 = (v: Vec3): number => Math.hypot(v[0], v[1], v[2])

/** Frame of the keys a classname understands: which local axis `width`, `height`, `depth` run along (lights: core/lights.h, decals: core/decal.h). */
function localAxes(_e: Entity, R: Mat3, decal: boolean): { right: Vec3; up: Vec3; dir: Vec3 } {
  if (decal) { // dir = R*(0,-1,0), up = R*(0,0,-1), right = dir x up
    const dir = apply3(R, [0, -1, 0]), up = apply3(R, [0, 0, -1])
    return { right: cross(dir, up), up, dir }
  }
  return { right: apply3(R, [1, 0, 0]), up: apply3(R, [0, 1, 0]), dir: apply3(R, [0, 0, 1]) }
}

/**
 * What a rotate / scale does to a POINT entity besides moving it, read off the linear part of the point map:
 *  - a rotation turns `angles` (every entity that can face somewhere: lights, decals, models), composed with the angles it has;
 *  - a scale scales what has a size: light `radius` (attenuation) and `source_size` (point / spot), rect light `width` / `height`, decal `width` / `height` /
 *    `depth` (along the decal's own axes), model scale `modelscale_vec` (along its own axes) and a plain `radius`.
 * Sizes keep their unit: the factor of a size along a local axis is the length of that axis after the map.
 */
export function mapEntityProps(e: Entity, map: PointMap): Entity {
  const M = linearPart(map)
  if (M.every((v, i) => nearly(v, i % 4 === 0 ? 1 : 0))) return e
  const R = rotationFromAngles(e.angles ?? [0, 0, 0])
  const det = det3(M)
  const orthonormal = nearly(Math.abs(det), 1, 1e-6) && [0, 1, 2].every((i) => nearly(len3(col(M, i)), 1, 1e-6)) && nearly(dot(col(M, 0), col(M, 1)), 0, 1e-6) && nearly(dot(col(M, 0), col(M, 2)), 0, 1e-6)
  let out = e
  if (orthonormal) {
    if (det > 0) {
      const a = anglesFromRotation(mul3(M, R))
      const next: Vec3 = [tidy(a[0]), tidy(a[1]), tidy(a[2])]
      const cur: Vec3 = e.angles ?? [0, 0, 0]
      if (!next.every((v, i) => nearly(v, cur[i], 1e-5))) out = { ...e, angles: next }
    }
    return out
  }
  const uni = Math.cbrt(Math.abs(det))
  if (!(uni > 1e-6)) return e
  const cls = e.classname ?? ''
  const kv: Record<string, string> = { ...(e.keyvalues ?? {}) }
  const set = (key: string, v: number, lo: number, hi: number) => { kv[key] = fmt(Math.min(hi, Math.max(lo, v))) }
  const isLight = isModernLight(cls), isDecal = cls === 'decal'
  const ax = localAxes(e, R, isDecal)
  const along = (v: Vec3) => len3(apply3(M, v))
  if (isLight) {
    const kind = LIGHT_KINDS[cls]
    if (kind !== 'directional' && kind !== 'sky') set('radius', numKey(e, 'radius', 10) * uni, 0.1, 1000)
    if (kind === 'point' || kind === 'spot') { const s = numKey(e, 'source_size', 0); if (s > 0) set('source_size', s * uni, 0, 10) }
    if (kind === 'rect') {
      set('width', numKey(e, 'width', 1) * along(ax.right), 0.01, 100)
      set('height', numKey(e, 'height', 1) * along(ax.up), 0.01, 100)
    }
  } else if (isDecal) {
    set('width', numKey(e, 'width', 1) * along(ax.right), 0.001, 1000)
    set('height', numKey(e, 'height', 1) * along(ax.up), 0.001, 1000)
    set('depth', numKey(e, 'depth', 0.5) * along(ax.dir), 0.001, 1000)
  } else {
    if (kv.radius !== undefined && Number.isFinite(Number(kv.radius))) set('radius', Number(kv.radius) * uni, 0, 1e6)
    if (kv.modelscale !== undefined && kv.modelscale_vec === undefined && Number.isFinite(Number(kv.modelscale))) set('modelscale', Number(kv.modelscale) * uni, 1e-4, 1e4)
    else {
      const cur = (kv.modelscale_vec ?? '1 1 1').trim().split(/\s+/).map(Number)
      const base: Vec3 = cur.length === 3 && cur.every((x) => Number.isFinite(x) && x !== 0) ? [cur[0], cur[1], cur[2]] : [1, 1, 1]
      const fac: Vec3 = [along(apply3(R, [1, 0, 0])), along(apply3(R, [0, 1, 0])), along(apply3(R, [0, 0, 1]))]
      const next = base.map((b, i) => tidy(b * fac[i]))
      if (next.every((x) => x === 1)) delete kv.modelscale_vec
      else kv.modelscale_vec = next.join(' ')
    }
  }
  return { ...out, keyvalues: kv }
}

export function mapEntityPoints(e: Entity, map: PointMap, flip = false, yawDelta = 0, lockUv = true): Entity {
  const isPoint = !(e.brushes && e.brushes.length)
  // point entities: angles and sizes follow the map (a rotation of any axis, a scale); brush entities only turn their yaw (legacy) when asked to
  const props = isPoint ? mapEntityProps(e, map) : e
  const angles: Vec3 | undefined = !isPoint && yawDelta && e.angles ? [e.angles[0], e.angles[1] + yawDelta, e.angles[2]] : props.angles
  return { ...props, origin: map(originOf(e)), ...(angles ? { angles } : {}), ...(e.brushes ? { brushes: e.brushes.map((b) => mapBrushPoints(b, map, flip, lockUv)) } : {}) }
}

/** What a set of selections covers: whole entities, brushes whose entity is not itself selected, and prefab instances. */
export function selectionTargets(doc: MapDocument, sels: Selection[]): { entities: Set<string>; brushes: Map<string, Set<string>>; instances: Set<string> } {
  const entities = new Set<string>(), brushes = new Map<string, Set<string>>(), instances = new Set<string>()
  for (const s of sels) if (s.instanceId !== undefined) instances.add(s.instanceId)
  for (const s of sels) if (s.instanceId === undefined && !s.brushId) entities.add(s.entityId)
  for (const s of sels) if (s.instanceId === undefined && s.brushId && !entities.has(s.entityId)) brushes.set(s.entityId, (brushes.get(s.entityId) ?? new Set()).add(s.brushId))
  void doc
  return { entities, brushes, instances }
}

const nearly = (a: number, b: number, eps = 1e-6): boolean => Math.abs(a - b) < eps

/**
 * Moves a prefab instance by an affine point map (grab / rotate / scale about a pivot). The map is probed for its linear part:
 * identity -> only the origin moves; a rotation composes into the instance's `angles`; a pure scale multiplies its `scale`
 * (only when that is expressible: no rotation on the instance, or a uniform factor). Anything else moves the origin only.
 */
export function mapInstancePoints(p: PrefabInstance, map: PointMap): PrefabInstance {
  const o: Vec3 = p.origin ?? [0, 0, 0]
  const o2 = map(o)
  const cols = ([[1, 0, 0], [0, 1, 0], [0, 0, 1]] as Vec3[]).map((e) => sub(map(add(o, e)), o2))
  const M: Mat3 = [cols[0][0], cols[1][0], cols[2][0], cols[0][1], cols[1][1], cols[2][1], cols[0][2], cols[1][2], cols[2][2]]
  const out: PrefabInstance = { ...p, origin: o2 }
  const isIdentity = M.every((v, i) => nearly(v, i % 4 === 0 ? 1 : 0))
  if (isIdentity) return out
  const off = [M[1], M[2], M[3], M[5], M[6], M[7]].every((v) => nearly(v, 0))
  if (off) { // pure (axis-aligned) scale
    const f: Vec3 = [M[0], M[4], M[8]]
    const uniform = nearly(f[0], f[1]) && nearly(f[1], f[2])
    const rotated = (p.angles ?? [0, 0, 0]).some((v) => v !== 0)
    if (uniform || !rotated) { const s = p.scale ?? [1, 1, 1]; out.scale = [s[0] * f[0], s[1] * f[1], s[2] * f[2]] }
    return out
  }
  const orthonormal = nearly(det3(M), 1) && nearly(M[0] * M[0] + M[3] * M[3] + M[6] * M[6], 1)
  if (orthonormal) out.angles = anglesFromRotation(mul3(M, rotationFromAngles(p.angles ?? [0, 0, 0])))
  return out
}

/** Applies a point map to everything the selections cover. `yawDelta` (radians about world Y) also turns entity angles. */
export function transformSelections(doc: MapDocument, sels: Selection[], map: PointMap, flip = false, yawDeg = 0, lockUv = true): MapDocument {
  const { entities, brushes, instances } = selectionTargets(doc, sels)
  let changed = false
  const out = doc.entities.map((e) => {
    if (entities.has(e.id)) { changed = true; return mapEntityPoints(e, map, flip, yawDeg, lockUv) }
    const ids = brushes.get(e.id)
    if (!ids) return e
    changed = true
    return { ...e, brushes: (e.brushes ?? []).map((b) => (ids.has(b.id) ? mapBrushPoints(b, map, flip, lockUv) : b)) }
  })
  if (instances.size) {
    changed = true
    return { ...doc, entities: out, prefabInstances: doc.prefabInstances.map((p) => (instances.has(p.id) ? mapInstancePoints(p, map) : p)) }
  }
  return changed ? { ...doc, entities: out } : doc
}

/** Centre of the selections' bounds; an entity without brushes (or with a custom origin) contributes its origin. */
export function selectionPivot(doc: MapDocument, sels: Selection[]): Vec3 | null {
  const min: Vec3 = [Infinity, Infinity, Infinity], max: Vec3 = [-Infinity, -Infinity, -Infinity]
  let any = false
  const grow = (p: Vec3) => { any = true; for (let i = 0; i < 3; i++) { min[i] = Math.min(min[i], p[i]); max[i] = Math.max(max[i], p[i]) } }
  const { entities, brushes, instances } = selectionTargets(doc, sels)
  for (const p of doc.prefabInstances) if (instances.has(p.id)) grow(p.origin ?? [0, 0, 0])
  for (const e of doc.entities) {
    const ids = brushes.get(e.id)
    if (entities.has(e.id)) {
      const bs = (e.brushes ?? []).map(brushBounds).filter((x) => x !== null)
      if (!isBrushEntity(e) || bs.length === 0 || classOf(e) !== 'worldspawn' && !(e.brushes?.length)) grow(originOf(e))
      for (const b of bs) { grow(b!.min); grow(b!.max) }
    } else if (ids) for (const b of e.brushes ?? []) if (ids.has(b.id)) { const bb = brushBounds(b); if (bb) { grow(bb.min); grow(bb.max) } }
  }
  return any ? [(min[0] + max[0]) / 2, (min[1] + max[1]) / 2, (min[2] + max[2]) / 2] : null
}
