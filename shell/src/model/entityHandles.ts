// Entity handles in the viewport (REQUIREMENTS §33 H1 / H2): the origin square (drags `origin` only) and the angle diamond at the tip of the
// direction arrow (turns `angles` to look at the cursor). Pure maths, no GL: where the handles are, how a drag maps to new values, their lines.
import type { Entity } from './document'
import { classOf, isBrushEntity } from './document'
import type { Box } from './entityBox'
import { Vec3, add, length, scale } from './math'
import { directionSpan } from './arrows'

/** Whether a selected entity gets the angle handle: point entities that draw a direction arrow (not decals, not worldspawn). */
export const hasAngleHandle = (e: Entity): boolean => !isBrushEntity(e) && classOf(e) !== 'worldspawn' && classOf(e) !== 'decal'

/** Where the angle handle starts (the arrow's base) and sits (the arrow's tip). */
export function angleHandle(e: Entity, boxOf: (classname: string) => Box): { base: Vec3; tip: Vec3 } {
  const { from, dir, len } = directionSpan(e, boxOf)
  return { base: from, tip: add(from, scale(dir, len)) }
}

const DEG = 180 / Math.PI
const tidy = (x: number): number => { const r = Math.round(x * 1e6) / 1e6; return Object.is(r, -0) ? 0 : r }

/**
 * Angles that make the entity look along `dir` (forward = R(angles) * (0,0,1) = (sin yaw cos pitch, -sin pitch, cos yaw cos pitch)).
 * `keepPitch`: only the yaw follows (the drag plane is horizontal). Roll is kept. `snapDeg` > 0 rounds yaw and pitch to that step.
 */
export function anglesToward(cur: Vec3 | undefined, dir: Vec3, keepPitch: boolean, snapDeg = 0): Vec3 | null {
  const l = length(dir)
  if (l < 1e-9) return null
  const d = scale(dir, 1 / l)
  const a = cur ?? [0, 0, 0]
  const horiz = Math.hypot(d[0], d[2])
  let yaw = horiz > 1e-6 ? Math.atan2(d[0], d[2]) * DEG : a[1]
  let pitch = keepPitch ? a[0] : Math.asin(Math.max(-1, Math.min(1, -d[1]))) * DEG
  if (snapDeg > 0) { yaw = Math.round(yaw / snapDeg) * snapDeg; if (!keepPitch) pitch = Math.round(pitch / snapDeg) * snapDeg }
  return [tidy(pitch), tidy(yaw), a[2]]
}

/** Square (origin handle) or diamond (angle handle) of half size `half` facing the camera (`right` / `up`), plus a cross for the origin. */
export function handleLines(c: Vec3, half: number, right: Vec3, up: Vec3, kind: 'origin' | 'angle'): number[] {
  const at = (a: number, b: number): Vec3 => add(c, add(scale(right, a * half), scale(up, b * half)))
  const pts = kind === 'origin' ? [at(-1, -1), at(1, -1), at(1, 1), at(-1, 1)] : [at(0, -1.3), at(1.3, 0), at(0, 1.3), at(-1.3, 0)]
  const out: number[] = []
  for (let i = 0; i < 4; i++) out.push(...pts[i], ...pts[(i + 1) % 4])
  if (kind === 'origin') out.push(...at(-0.5, 0), ...at(0.5, 0), ...at(0, -0.5), ...at(0, 0.5))
  return out
}

