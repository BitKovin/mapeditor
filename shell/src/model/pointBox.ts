// The box of a point entity as the editor shows and picks it: the class box from the .ent (decals: their projector volume), turned with the entity's
// `angles` about its origin. Pure maths, no GL. The viewport draws it, the picker hits it, the arrows start from its centre.
import type { Entity } from './document'
import { classOf, originOf } from './document'
import { decalLocalBox } from './decal'
import type { Box } from './entityBox'
import { isModernLight } from './lights'
import { entityModelBox } from './modelEntity'
import type { Vec3 } from './math'
import type { Mat3 } from './rotation'
import { apply3, rotationFromAngles } from './rotation'

/** A box in the frame of an entity: `min` / `max` are relative to `origin` and turned by `rot` (a rotation matrix, row-major). */
export interface OrientedBox { origin: Vec3; rot: Mat3; min: Vec3; max: Vec3 }

/**
 * Whether the viewport draws the box of this entity. Modern lights have a gizmo of their own and decals show their projector bounds, so a filled box
 * would only hide them; both stay selectable through their box (see pointBox).
 */
export const drawsBox = (e: Entity): boolean => !isModernLight(e.classname) && classOf(e) !== 'decal'

/** The oriented box of a point entity: a decal's projector volume, the bounds of its model (model_path, once loaded), otherwise the class box (`boxOf`), rotated by `angles` about the origin. */
export function pointBox(e: Entity, boxOf: (classname: string) => Box): OrientedBox {
  const local = classOf(e) === 'decal' ? decalLocalBox(e) : entityModelBox(e) ?? boxOf(classOf(e))
  return { origin: originOf(e), rot: rotationFromAngles(e.angles ?? [0, 0, 0]), min: local.min, max: local.max }
}

/** `v` (relative to the box origin) in world space. */
export const toWorld = (b: OrientedBox, v: Vec3): Vec3 => {
  const r = apply3(b.rot, v)
  return [b.origin[0] + r[0], b.origin[1] + r[1], b.origin[2] + r[2]]
}
/** The inverse: a world point in the box's own frame. The matrix is a rotation, so its transpose undoes it. */
export const toLocal = (b: OrientedBox, p: Vec3): Vec3 => unrotate(b.rot, [p[0] - b.origin[0], p[1] - b.origin[1], p[2] - b.origin[2]])
export const unrotate = (m: Mat3, v: Vec3): Vec3 => [m[0] * v[0] + m[3] * v[1] + m[6] * v[2], m[1] * v[0] + m[4] * v[1] + m[7] * v[2], m[2] * v[0] + m[5] * v[1] + m[8] * v[2]]

/** The eight corners of an axis-aligned box in a fixed order (bottom face 0..3, top face 4..7); the line / triangle builders index into it. */
export const cornersOf = (min: Vec3, max: Vec3): Vec3[] => [
  [min[0], min[1], min[2]], [max[0], min[1], min[2]], [max[0], max[1], min[2]], [min[0], max[1], min[2]],
  [min[0], min[1], max[2]], [max[0], min[1], max[2]], [max[0], max[1], max[2]], [min[0], max[1], max[2]],
]
/** The same eight corners of an oriented box, in world space. */
export const worldCorners = (b: OrientedBox): Vec3[] => cornersOf(b.min, b.max).map((c) => toWorld(b, c))

/** The axis-aligned box around an oriented one. */
export function boundsOfBox(b: OrientedBox): Box {
  const min: Vec3 = [Infinity, Infinity, Infinity], max: Vec3 = [-Infinity, -Infinity, -Infinity]
  for (const c of worldCorners(b)) for (let i = 0; i < 3; i++) { min[i] = Math.min(min[i], c[i]); max[i] = Math.max(max[i], c[i]) }
  return { min, max }
}
