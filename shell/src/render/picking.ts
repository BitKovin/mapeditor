import type { Brush, Entity } from '../model/document'
import { classOf, isBrushEntity } from '../model/document'
import { Box, boxFromQuake } from '../model/entityBox'
import type { OrientedBox } from '../model/pointBox'
import { pointBox, toLocal, unrotate } from '../model/pointBox'
import { faceNormal, triangulateFace } from '../model/geometry'
import { Vec3, cross, dot, sub } from '../model/math'
import type { CarvedMesh, PickTri } from '../model/derived'
import { carvedKey } from '../model/derived'
import type { Ray } from './camera'

export interface Hit { /** The entity object that was hit (prefab content is not unique by id). */ entity: Entity; entityId: string; brushId?: string; faceId?: number; t: number; point: Vec3; normal: Vec3 }

interface Tri { a: Vec3; b: Vec3; c: Vec3; faceId: number; normal: Vec3 }
const triCache = new WeakMap<Brush, Tri[]>()
function triangles(b: Brush): Tri[] {
  let t = triCache.get(b)
  if (!t) {
    t = b.faces.flatMap((f) => {
      const normal = faceNormal(b, f)
      return triangulateFace(b, f).map(([i, j, k]) => ({ a: b.vertices[i], b: b.vertices[j], c: b.vertices[k], faceId: f.id, normal }))
    })
    triCache.set(b, t)
  }
  return t
}

/** Two-sided Moller-Trumbore. Returns the ray parameter, or null. */
export function rayTriangle(r: Ray, a: Vec3, b: Vec3, c: Vec3): number | null {
  const e1 = sub(b, a), e2 = sub(c, a)
  const p = cross(r.dir, e2)
  const det = dot(e1, p)
  if (Math.abs(det) < 1e-12) return null
  const inv = 1 / det
  const tv = sub(r.origin, a)
  const u = dot(tv, p) * inv
  if (u < 0 || u > 1) return null
  const q = cross(tv, e1)
  const v = dot(r.dir, q) * inv
  if (v < 0 || u + v > 1) return null
  const t = dot(e2, q) * inv
  return t > 0 ? t : null
}

export function rayBox(r: Ray, min: Vec3, max: Vec3): number | null {
  let t0 = 0, t1 = Infinity
  for (let i = 0; i < 3; i++) {
    if (Math.abs(r.dir[i]) < 1e-12) { if (r.origin[i] < min[i] || r.origin[i] > max[i]) return null; continue }
    let a = (min[i] - r.origin[i]) / r.dir[i], b = (max[i] - r.origin[i]) / r.dir[i]
    if (a > b) [a, b] = [b, a]
    t0 = Math.max(t0, a); t1 = Math.min(t1, b)
    if (t0 > t1) return null
  }
  return t0
}

const add3 = (o: Vec3, d: Vec3, t: number): Vec3 => [o[0] + d[0] * t, o[1] + d[1] * t, o[2] + d[2] * t]

/** Ray against a box that is turned about its origin: the ray goes into the box's own frame (a rotation keeps distances, so `t` is the same). */
export function rayOrientedBox(r: Ray, b: OrientedBox): number | null {
  return rayBox({ origin: toLocal(b, r.origin), dir: unrotate(b.rot, r.dir) }, b.min, b.max)
}


const DEFAULT_ENTITY_BOX: Box = boxFromQuake(null)

export interface PickOptions {
  /** Boolean results: a carved brush is picked by its real (carved) surface, cut walls included. */
  carved?: Map<string, CarvedMesh>
  /** Prefab content: entity object -> instance path ("" = the document itself), to find its carved mesh. */
  instances?: Map<Entity, string>
  /** False when subtractive brush shapes are hidden: they must not catch clicks either. Default true. */
  includeSubtractive?: boolean
  /** Bounding box of a point-entity class relative to its origin (editor space); the default box when absent. */
  entityBox?: (classname: string) => Box
}

/**
 * Nearest selectable thing under the ray. Brush hits select the brush; point entities use the box from their .ent definition, turned with their angles (decals: their projector bounds).
 * Carved brushes are hit where they really are: the hole you cut is clickable, and a click on a cut wall
 * names the subtractor's face (that is the face whose texture it shows).
 */
export function pick(r: Ray, entities: Entity[], opts: PickOptions = {}): Hit | null {
  let best: Hit | null = null
  const consider = (h: Hit) => { if (!best || h.t < best.t) best = h }
  const facing = (n: Vec3): Vec3 => (dot(n, r.dir) > 0 ? [-n[0], -n[1], -n[2]] : n) // the normal that faces the viewer, so drawing on a back face works too
  // a cut wall belongs to the subtractor's entity, which may be prefab content other than the carved brush's own entity
  const ownerOf = (tri: PickTri, fallback: Entity): Entity => {
    if (tri.entityId === fallback.id && (opts.instances?.get(fallback) ?? '') === tri.instance) return fallback
    return entities.find((x) => x.id === tri.entityId && (opts.instances?.get(x) ?? '') === tri.instance) ?? fallback
  }
  for (const e of entities) {
    if (!isBrushEntity(e) && classOf(e) !== 'worldspawn') {
      // the class box turned with the entity's angles; a decal is hit by its projector bounds. Lights are hit by their (undrawn) box.
      const t = rayOrientedBox(r, pointBox(e, (c) => opts.entityBox?.(c) ?? DEFAULT_ENTITY_BOX))
      if (t !== null) consider({ entity: e, entityId: e.id, t, point: add3(r.origin, r.dir, t), normal: [0, 1, 0] })
      continue
    }
    for (const b of e.brushes ?? []) {
      if (b.op === 'subtract' && opts.includeSubtractive === false) continue
      const carved = b.op !== 'subtract' ? opts.carved?.get(carvedKey(e.id, b.id, opts.instances?.get(e) ?? '')) : undefined
      if (carved) {
        for (const tri of carved.pickTris) {
          const t = rayTriangle(r, tri.a, tri.b, tri.c)
          if (t !== null) consider({ entity: ownerOf(tri, e), entityId: tri.entityId, brushId: tri.brushId, faceId: tri.faceId, t, point: add3(r.origin, r.dir, t), normal: facing(tri.normal) })
        }
        continue
      }
      for (const tri of triangles(b)) {
        const t = rayTriangle(r, tri.a, tri.b, tri.c)
        if (t !== null) consider({ entity: e, entityId: e.id, brushId: b.id, faceId: tri.faceId, t, point: add3(r.origin, r.dir, t), normal: facing(tri.normal) })
      }
    }
  }
  return best
}
