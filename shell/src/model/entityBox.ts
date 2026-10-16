// Point-entity bounding boxes. The game's .ent file gives a box in Quake units in Quake's axis
// convention (Z up, +Y forward); the editor is metric and Y-up (DESIGN.md §4). This is the one place
// that converts between the two, so nothing else needs to know Quake's conventions.
//
//   editor = ( qx, qz, -qy ) / 32          32 Quake units = 1 metre
//
// It is a pure rotation (no mirroring): Quake X x Y = Z maps to editor X x (-Z) = Y, so handedness
// is kept and the box of an entity is the same box a Quake editor would show, just in metres and Y-up.
import type { Vec3 } from './math'

export const QUAKE_UNITS_PER_METER = 32
export interface Box { min: Vec3; max: Vec3 }
/** The box as written in a .ent file: min xyz, max xyz, in Quake units. */
export type QuakeBox = [number, number, number, number, number, number]

/** What a class without a usable `box` gets: 16 Quake units (half a metre) on a side, centred on the origin. */
export const DEFAULT_QUAKE_BOX: QuakeBox = [-8, -8, -8, 8, 8, 8]

const clean = (n: number): number => (n === 0 ? 0 : n) // no -0 in anything that is compared or printed

/** A point or vector from Quake space and units to editor space and metres. */
export const quakeToEditor = (v: Vec3): Vec3 => [clean(v[0] / QUAKE_UNITS_PER_METER), clean(v[2] / QUAKE_UNITS_PER_METER), clean(-v[1] / QUAKE_UNITS_PER_METER)]
/** The inverse of quakeToEditor (for export and for showing Quake numbers). */
export const editorToQuake = (v: Vec3): Vec3 => [clean(v[0] * QUAKE_UNITS_PER_METER), clean(-v[2] * QUAKE_UNITS_PER_METER), clean(v[1] * QUAKE_UNITS_PER_METER)]

const usable = (b: readonly number[] | null | undefined): b is QuakeBox =>
  !!b && b.length === 6 && b.every(Number.isFinite) && b[3] >= b[0] && b[4] >= b[1] && b[5] >= b[2]

/** A class's box in editor space, relative to the entity origin. Converting two corners flips which one is the minimum on Y/Z, so the result is re-sorted. */
export function boxFromQuake(box: readonly number[] | null | undefined): Box {
  const b = usable(box) ? box : DEFAULT_QUAKE_BOX
  const a = quakeToEditor([b[0], b[1], b[2]]), c = quakeToEditor([b[3], b[4], b[5]])
  return { min: [Math.min(a[0], c[0]), Math.min(a[1], c[1]), Math.min(a[2], c[2])], max: [Math.max(a[0], c[0]), Math.max(a[1], c[1]), Math.max(a[2], c[2])] }
}

/** The same box moved to `origin`. */
export const boxAt = (box: Box, origin: Vec3): Box => ({
  min: [origin[0] + box.min[0], origin[1] + box.min[1], origin[2] + box.min[2]],
  max: [origin[0] + box.max[0], origin[1] + box.max[1], origin[2] + box.max[2]],
})

export const boxSize = (box: Box): Vec3 => [box.max[0] - box.min[0], box.max[1] - box.min[1], box.max[2] - box.min[2]]

/**
 * Where an entity's origin goes so that its box rests on the surface it was dropped on: the hit point,
 * pushed out along the surface normal by the box's extent on that side. A floor (normal up) puts the
 * box's bottom on the floor, a wall puts the box against it. Axes the normal barely points along keep the hit point.
 */
export function originOnSurface(point: Vec3, normal: Vec3, box: Box): Vec3 {
  const o: Vec3 = [point[0], point[1], point[2]]
  for (let i = 0; i < 3; i++) {
    if (normal[i] > 0.5) o[i] = point[i] - box.min[i]
    else if (normal[i] < -0.5) o[i] = point[i] - box.max[i]
  }
  return o
}
