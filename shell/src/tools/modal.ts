// Modal operators (G / R / S / E / I / Ctrl+B / Ctrl+R): start, move the mouse or type a number, click or
// Enter to confirm, Esc or RMB to cancel. This file holds the maths that turns mouse/typed input into a
// parameter; the viewport feeds it and applies the result through Store.dragUpdate so a whole modal is one
// undo step. Pure and unit-tested.
import { Vec3, dot, length, normalize, scale, sub } from '../model/math'
import type { Ray } from '../render/camera'
import { AXIS_DIR, Axis, lineDistance } from '../render/gizmo'

export type ModalKind = 'grab' | 'rotate' | 'scale' | 'extrude' | 'inset' | 'bevel' | 'loopcut'

/** Numeric entry: digits, one '.', leading '-', Backspace. Returns the new buffer, or null if the key is not numeric. */
export function typeInto(buf: string, key: string): string | null {
  if (/^[0-9]$/.test(key)) return buf + key
  if (key === '.' || key === ',') return buf.includes('.') ? buf : (buf === '' || buf === '-' ? buf + '0.' : buf + '.')
  if (key === '-') return buf.startsWith('-') ? buf.slice(1) : '-' + buf
  if (key === 'Backspace') return buf.slice(0, -1)
  return null
}
export const typedValue = (buf: string): number | null => {
  if (buf === '' || buf === '-' || buf === '.') return null
  const v = Number(buf)
  return Number.isFinite(v) ? v : null
}

const snapTo = (v: number, step: number): number => (step > 0 ? Math.round(v / step) * step : v)

/** Where the ray meets the plane through `point` facing the camera. */
export function planePoint(ray: Ray, point: Vec3, normal: Vec3): Vec3 | null {
  const d = dot(normal, ray.dir)
  if (Math.abs(d) < 1e-9) return null
  const t = dot(normal, sub(point, ray.origin)) / d
  return t > 0 ? [ray.origin[0] + ray.dir[0] * t, ray.origin[1] + ray.dir[1] * t, ray.origin[2] + ray.dir[2] * t] : null
}

/**
 * Grab: translation vector. Unconstrained it slides on the plane through the pivot facing the camera; with an
 * axis it follows the mouse along that axis. `start` is the point under the mouse when the modal began.
 */
export function grabDelta(ray: Ray, start: Vec3, pivot: Vec3, camForward: Vec3, axis: Axis | null, step: number): Vec3 {
  if (axis === null) {
    const p = planePoint(ray, pivot, camForward)
    if (!p) return [0, 0, 0]
    return [snapTo(p[0] - start[0], step), snapTo(p[1] - start[1], step), snapTo(p[2] - start[2], step)]
  }
  const d = AXIS_DIR[axis]
  const t = lineDistance(ray, pivot, d), t0 = lineDistance({ origin: ray.origin, dir: normalize(sub(start, ray.origin)) }, pivot, d)
  if (t === null || t0 === null) return [0, 0, 0]
  return scale(d, snapTo(t - t0, step))
}

/** Signed distance of the mouse along a line (extrude along a normal, loop-cut slide). */
export function alongLine(ray: Ray, origin: Vec3, dir: Vec3): number | null { return lineDistance(ray, origin, dir) }

/** Rotation angle (radians) from the mouse's angle around the pivot on screen. `flip` when the axis points away from the viewer. */
export function rotateAngle(pivot: { x: number; y: number }, from: { x: number; y: number }, to: { x: number; y: number }, flip: boolean, stepRad: number): number {
  const a0 = Math.atan2(from.y - pivot.y, from.x - pivot.x), a1 = Math.atan2(to.y - pivot.y, to.x - pivot.x)
  let d = a1 - a0
  while (d > Math.PI) d -= 2 * Math.PI
  while (d < -Math.PI) d += 2 * Math.PI
  d = flip ? -d : d // screen y points down, so a counter-clockwise mouse move has a negative atan2 change
  return stepRad > 0 ? Math.round(d / stepRad) * stepRad : d
}

/** Scale factor: distance of the mouse from the pivot now vs at the start. */
export function scaleFactor(pivot: { x: number; y: number }, from: { x: number; y: number }, to: { x: number; y: number }, step: number): number {
  const d0 = Math.hypot(from.x - pivot.x, from.y - pivot.y), d1 = Math.hypot(to.x - pivot.x, to.y - pivot.y)
  const f = d0 < 4 ? 1 : d1 / d0
  return step > 0 ? Math.max(step, Math.round(f / step) * step) : f
}

/** Scale vector for a factor on an optional axis. */
export const scaleVector = (f: number, axis: Axis | null): Vec3 => (axis === null ? [f, f, f] : [axis === 0 ? f : 1, axis === 1 ? f : 1, axis === 2 ? f : 1])

/** Width for inset / bevel from how far the mouse moved away from where it started, in metres at the pivot's depth. */
export function widthFromMouse(from: { x: number; y: number }, to: { x: number; y: number }, metresPerPixel: number, step: number): number {
  const w = Math.max(0, Math.hypot(to.x - from.x, to.y - from.y) * metresPerPixel)
  return step > 0 ? Math.round(w / step) * step : w
}

export const metresPerPixel = (distance: number, fovY: number, heightPx: number): number => (2 * distance * Math.tan(fovY / 2)) / Math.max(1, heightPx)

/** One wheel notch up adds a bevel segment, one down removes one; always within 1..max. */
export const stepSegments = (current: number, deltaY: number, max: number): number => Math.max(1, Math.min(max, current + (deltaY < 0 ? 1 : deltaY > 0 ? -1 : 0)))

/** The cursor of a modal that has locked the pointer: it keeps moving by the raw mouse movement, so the operation never stops at the screen edge. */
export const advanceVirtualCursor = (p: { x: number; y: number }, dx: number, dy: number): { x: number; y: number } => ({ x: p.x + dx, y: p.y + dy })

export const describeModal = (kind: ModalKind, value: number, axis: Axis | null, typing: string, segments?: number): string => {
  const ax = axis === null ? '' : ` ${'XYZ'[axis]}`
  const v = typing !== '' ? `${typing}_` : kind === 'rotate' ? `${((value * 180) / Math.PI).toFixed(1)}°` : kind === 'scale' ? `×${value.toFixed(3)}` : `${value.toFixed(3)} m`
  const name = { grab: 'Move', rotate: 'Rotate', scale: 'Scale', extrude: 'Extrude', inset: 'Inset', bevel: 'Bevel', loopcut: 'Loop cut' }[kind]
  const seg = kind === 'bevel' && segments !== undefined ? `, ${segments} segment${segments === 1 ? '' : 's'} (wheel)` : ''
  return `${name}${ax}: ${v}${seg}   (type a number · X/Y/Z axis · click/Enter confirm · Esc cancel)`
}
export const len3 = length
