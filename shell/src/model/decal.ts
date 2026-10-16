// Decal projector orientation (mirrors core/src/decal.cpp; see its header for the convention).
// angles are degrees (pitch, yaw, roll); the projector looks along  dir = Ry(yaw) Rx(pitch) Rz(roll) * (0,-1,0).
import type { Entity } from './document'
import type { Box } from './entityBox'
import { Vec3, normalize } from './math'

const RAD = Math.PI / 180

/** The projection direction for given angles (the same maths Core uses). */
export function decalDirection(angles: Vec3): Vec3 {
  const p = angles[0] * RAD, y = angles[1] * RAD, r = angles[2] * RAD
  const ax = 0 * Math.cos(r) - -1 * Math.sin(r), ay = 0 * Math.sin(r) + -1 * Math.cos(r), az = 0
  const by = ay * Math.cos(p) - az * Math.sin(p), bz = ay * Math.sin(p) + az * Math.cos(p)
  return [ax * Math.cos(y) + bz * Math.sin(y), by, -ax * Math.sin(y) + bz * Math.cos(y)]
}

/** Angles (roll 0) that make a decal project along `dir` (into a surface whose outward normal is `-dir`). */
export function anglesForDirection(dir: Vec3): Vec3 {
  const d = normalize(dir)
  const pitch = Math.acos(Math.max(-1, Math.min(1, -d[1])))
  const s = Math.sin(pitch)
  const yaw = s < 1e-6 ? 0 : Math.atan2(-d[0], -d[2])
  return [pitch / RAD, yaw / RAD, 0]
}

const size = (v: string | undefined, dflt: number): number => { const n = Number(v); return Number.isFinite(n) && n > 1e-6 ? n : dflt }

/** Half the projector's width, height and depth in metres (the defaults Core uses when a key is missing or not a positive number). */
export function decalHalfSize(e: Entity): { hw: number; hh: number; hd: number } {
  const kv = e.keyvalues
  return { hw: size(kv?.width, 1) / 2, hh: size(kv?.height, 1) / 2, hd: size(kv?.depth, 0.5) / 2 }
}

/**
 * The projector volume in the entity's own frame (before `angles`): width along x, depth along y (the projection axis), height along z, centred on
 * the origin. Rotated by R(angles) it is the box core/src/decal.cpp projects through, the same one the editor draws as the decal's bounds.
 */
export function decalLocalBox(e: Entity): Box {
  const { hw, hh, hd } = decalHalfSize(e)
  return { min: [-hw, -hd, -hh], max: [hw, hd, hh] }
}
