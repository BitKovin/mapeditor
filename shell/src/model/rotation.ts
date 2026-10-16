// Rotation maths shared by prefab expansion and the transform tools (mirrors core/src/prefab.cpp).
// angles = [pitch, yaw, roll] in degrees; the matrix is Ry(yaw) * Rx(pitch) * Rz(roll) applied to a column vector
// (the decal convention, DESIGN 18.3). Double precision throughout.
import type { Vec3 } from './math'

export type Mat3 = [number, number, number, number, number, number, number, number, number] // row-major
export const I3: Mat3 = [1, 0, 0, 0, 1, 0, 0, 0, 1]
export const DEG = Math.PI / 180

export const mul3 = (a: Mat3, b: Mat3): Mat3 => {
  const r = new Array(9).fill(0) as Mat3
  for (let i = 0; i < 3; i++) for (let j = 0; j < 3; j++) r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j]
  return r
}
export const apply3 = (m: Mat3, v: Vec3): Vec3 => [m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2], m[6] * v[0] + m[7] * v[1] + m[8] * v[2]]
export const det3 = (m: Mat3): number => m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6])

export function rotationFromAngles(a: Vec3): Mat3 {
  const p = a[0] * DEG, y = a[1] * DEG, r = a[2] * DEG
  const cp = Math.cos(p), sp = Math.sin(p), cy = Math.cos(y), sy = Math.sin(y), cr = Math.cos(r), sr = Math.sin(r)
  return [
    cy * cr + sy * sp * sr, -cy * sr + sy * sp * cr, sy * cp,
    cp * sr, cp * cr, -sp,
    -sy * cr + cy * sp * sr, sy * sr + cy * sp * cr, cy * cp,
  ]
}

export function anglesFromRotation(R: Mat3): Vec3 {
  const sp = Math.max(-1, Math.min(1, -R[5]))
  const pitch = Math.asin(sp)
  let yaw: number, roll: number
  if (Math.abs(sp) < 1 - 1e-9) { yaw = Math.atan2(R[2], R[8]); roll = Math.atan2(R[3], R[4]) }
  else { roll = 0; yaw = sp > 0 ? Math.atan2(R[1], R[0]) : Math.atan2(-R[1], R[0]) } // gimbal lock: all in yaw
  return [pitch / DEG, yaw / DEG, roll / DEG]
}
