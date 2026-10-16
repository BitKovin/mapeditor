// Viewport annotations for entities: the decal projector as an oriented box plus its projection arrow.
// (Direction arrows of point entities and link arrows between entities live in arrows.ts.)
// Pure: returns flat line endpoints (x,y,z, x,y,z, ...) so it can be tested without GL.
import type { Entity } from './document'
import { Vec3, add, cross, normalize, scale, sub } from './math'
import { decalHalfSize } from './decal'
import { apply3, rotationFromAngles } from './rotation'

function arrow(out: number[], o: Vec3, dir: Vec3, len: number): void {
  const tip = add(o, scale(dir, len))
  const side = normalize(Math.abs(dir[1]) < 0.9 ? cross(dir, [0, 1, 0]) : cross(dir, [1, 0, 0]))
  const back = sub(tip, scale(dir, len * 0.22)), w = len * 0.1
  out.push(...o, ...tip, ...tip, ...add(back, scale(side, w)), ...tip, ...sub(back, scale(side, w)))
}

/** A decal's projector: axes exactly as Core (dir = R*(0,-1,0), up = R*(0,0,-1), right = dir x up) and its size along them. */
function decalFrame(e: Entity): { o: Vec3; dir: Vec3; up: Vec3; right: Vec3; hw: number; hh: number; hd: number } {
  const o: Vec3 = e.origin ?? [0, 0, 0]
  const R = rotationFromAngles(e.angles ?? [0, 0, 0])
  const dir = apply3(R, [0, -1, 0]), up = apply3(R, [0, 0, -1])
  return { o, dir, up, right: normalize(cross(dir, up)), ...decalHalfSize(e) }
}

/** The 12 edges of a decal's projector box, as line endpoints. */
export function decalBoundsLines(e: Entity): number[] {
  const { o, dir, up, right, hw, hh, hd } = decalFrame(e)
  const c = (a: number, b: number, d: number): Vec3 => add(o, add(add(scale(right, a * hw), scale(up, b * hh)), scale(dir, d * hd)))
  const out: number[] = []
  const quad = [[-1, -1], [1, -1], [1, 1], [-1, 1]]
  for (const [a, b] of quad) out.push(...c(a, b, -1), ...c(a, b, 1)) // 4 edges along the depth
  for (const d of [-1, 1]) for (let i = 0; i < 4; i++) { // 4 on each cap
    const p = quad[i], q = quad[(i + 1) % 4]
    out.push(...c(p[0], p[1], d), ...c(q[0], q[1], d))
  }
  return out
}

export function entityAnnotations(entities: readonly Entity[]): number[] {
  const out: number[] = []
  for (const e of entities) {
    if (e.brushes?.length || e.classname !== 'decal') continue
    out.push(...decalBoundsLines(e))
    const { o, dir, hd } = decalFrame(e)
    arrow(out, o, dir, Math.max(hd * 2, 0.3))
  }
  return out
}
