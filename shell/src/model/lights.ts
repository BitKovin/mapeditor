// Modern light entities (REQUIREMENTS §12; core/lights.h is the baker's side of the same keys): reading their parameters like the baker does, and the
// viewport preview gizmos. Pure: gizmos are flat line endpoints (x,y,z, x,y,z, ...), so everything is testable without GL.
//
//   light_point        position, attenuation radius, source sphere
//   light_spot         position, direction, inner / outer cone, attenuation radius, source sphere
//   light_directional  direction only (the sun); angular diameter
//   light_rect         position, direction, width x height rectangle that emits from its front side, attenuation radius
//   light_sky          the upper hemisphere
//
// Defaults come from the game's .ent (the single source the editor shows; a Core test checks they equal the baker's); the constants below are only the
// fallback when the definitions are missing and equal the baker's.
import type { EntityClassDef } from '../net/protocol'
import type { Entity } from './document'
import { Vec3, add, cross, normalize, scale } from './math'
import { apply3, rotationFromAngles } from './rotation'

export type LightKind = 'point' | 'spot' | 'directional' | 'rect' | 'sky'
export const LIGHT_KINDS: Readonly<Record<string, LightKind>> = {
  light_point: 'point', light_spot: 'spot', light_directional: 'directional', light_rect: 'rect', light_sky: 'sky',
}
export const isModernLight = (classname: string | undefined): boolean => !!classname && classname in LIGHT_KINDS

export interface LightParams {
  kind: LightKind
  color: [number, number, number]
  brightness: number
  radius: number
  bounce: number
  castShadows: boolean
  style: number
  /** Sphere radius in metres (point, spot) or angular diameter in degrees (directional). 0 = the live bake's default softness. */
  sourceSize: number
  innerAngle: number
  outerAngle: number
  width: number
  height: number
}

const FALLBACK: Record<string, string> = {
  color: '1 1 1', brightness: '1', radius: '10', bounce: '1', cast_shadows: '1', style: '0', source_size: '0', inner_angle: '20', outer_angle: '35', width: '1', height: '1',
}

const num = (v: string | undefined): number | null => {
  if (v === undefined || v.trim() === '') return null
  const n = Number(v)
  return Number.isFinite(n) ? n : null
}

/** The light's parameters exactly as the baker parses them (same keys, same clamps), with the .ent's defaults for keys that are not set. */
export function lightParams(e: Entity, def?: EntityClassDef): LightParams | null {
  const kind = e.classname ? LIGHT_KINDS[e.classname] : undefined
  if (!kind) return null
  const raw = (key: string): string => e.keyvalues?.[key] ?? def?.keys.find((k) => k.key === key)?.default ?? FALLBACK[key]!
  const n = (key: string): number => num(e.keyvalues?.[key]) ?? num(def?.keys.find((k) => k.key === key)?.default) ?? Number(FALLBACK[key])
  let color: [number, number, number] = [1, 1, 1]
  const parts = raw('color').trim().split(/\s+/).map(Number)
  if (parts.length === 3 && parts.every(Number.isFinite)) color = [parts[0], parts[1], parts[2]]
  const outer = Math.min(89.9, Math.max(0.1, n('outer_angle')))
  let inner = Math.min(89, Math.max(0, n('inner_angle')))
  if (inner >= outer) inner = outer * 0.5
  return {
    kind, color,
    brightness: Math.max(0, n('brightness')), radius: Math.max(1e-3, n('radius')), bounce: Math.max(0, n('bounce')),
    castShadows: n('cast_shadows') !== 0, style: Math.min(63, Math.max(0, Math.trunc(n('style')))), sourceSize: Math.max(0, n('source_size')),
    innerAngle: inner, outerAngle: outer, width: Math.max(1e-3, n('width')), height: Math.max(1e-3, n('height')),
  }
}

/** Lights with a direction start pointing straight down (forward = R*(0,0,1); pitch 90 turns it to -Y), like a lamp placed on the ceiling. */
export const defaultLightAngles = (classname: string | undefined): Vec3 | undefined =>
  classname === 'light_spot' || classname === 'light_directional' || classname === 'light_rect' ? [90, 0, 0] : undefined

/** forward = R*(0,0,1), right = R*(1,0,0), up = R*(0,1,0): the baker's frame (core/lights.h). */
export function lightFrame(e: Entity): { origin: Vec3; forward: Vec3; right: Vec3; up: Vec3 } {
  const R = rotationFromAngles(e.angles ?? [0, 0, 0])
  return { origin: e.origin ?? [0, 0, 0], forward: normalize(apply3(R, [0, 0, 1])), right: normalize(apply3(R, [1, 0, 0])), up: normalize(apply3(R, [0, 1, 0])) }
}

// ---- line helpers --------------------------------------------------------------------------------------------------------------------

const SEG = 48
const at = (c: Vec3, a: Vec3, b: Vec3, x: number, y: number): Vec3 => add(c, add(scale(a, x), scale(b, y)))

/** A circle (or arc from `from` to `to` radians) around `c` in the plane spanned by the unit vectors a, b. */
function arc(out: number[], c: Vec3, a: Vec3, b: Vec3, r: number, from = 0, to = Math.PI * 2, seg = SEG): void {
  let prev = at(c, a, b, Math.cos(from) * r, Math.sin(from) * r)
  for (let i = 1; i <= seg; i++) {
    const t = from + ((to - from) * i) / seg
    const p = at(c, a, b, Math.cos(t) * r, Math.sin(t) * r)
    out.push(...prev, ...p)
    prev = p
  }
}
const seg = (out: number[], p: Vec3, q: Vec3): void => { out.push(...p, ...q) }
const X: Vec3 = [1, 0, 0], Y: Vec3 = [0, 1, 0], Z: Vec3 = [0, 0, 1]

function head(out: number[], tip: Vec3, dir: Vec3, len: number): void {
  const u = normalize(Math.abs(dir[1]) < 0.9 ? cross(dir, Y) : cross(dir, X)), v = cross(dir, u)
  const back = add(tip, scale(dir, -len))
  for (const s of [u, v, scale(u, -1), scale(v, -1)]) seg(out, tip, add(back, scale(s, len * 0.4)))
}

/** Three great circles = a wire sphere. */
function sphere(out: number[], c: Vec3, r: number): void {
  arc(out, c, X, Y, r); arc(out, c, Y, Z, r); arc(out, c, X, Z, r)
}

const DEG = Math.PI / 180

/** Size of the always-visible icon part, metres. */
const ICON = 0.35

/**
 * Preview gizmo of a light as line endpoints. Every light gets a compact icon (what it is, where it looks, its real rectangle / cone shape); a selected
 * light also shows its reach: the attenuation radius, the cones out to the radius, the source size.
 */
export function lightGizmo(e: Entity, p: LightParams, selected: boolean): number[] {
  const out: number[] = []
  const { origin: o, forward: f, right: r, up: u } = lightFrame(e)
  switch (p.kind) {
    case 'point': {
      // a diamond (octahedron) as the icon
      const s = ICON * 0.5
      for (const [a, b] of [[X, Y], [Y, Z], [X, Z]] as const) arc(out, o, a, b, s, 0, Math.PI * 2, 4)
      if (selected) {
        sphere(out, o, p.radius)
        if (p.sourceSize > 0) sphere(out, o, p.sourceSize)
      }
      break
    }
    case 'spot': {
      // the cone: icon length, or out to the radius when selected; inner and outer
      const len = selected ? p.radius : Math.min(p.radius, 1.2)
      const tipOf = (deg: number): { c: Vec3; rad: number } => ({ c: add(o, scale(f, len)), rad: len * Math.tan(deg * DEG) })
      const outer = tipOf(p.outerAngle), inner = tipOf(p.innerAngle)
      arc(out, outer.c, r, u, outer.rad)
      for (const [x, y] of [[1, 0], [-1, 0], [0, 1], [0, -1]] as const) seg(out, o, at(outer.c, r, u, x * outer.rad, y * outer.rad))
      arc(out, inner.c, r, u, inner.rad, 0, Math.PI * 2, 32)
      seg(out, o, outer.c)
      arc(out, o, X, Y, ICON * 0.25, 0, Math.PI * 2, 8); arc(out, o, Y, Z, ICON * 0.25, 0, Math.PI * 2, 8)
      if (selected) {
        // the reach: an arc of the attenuation sphere across the cone, in two planes
        const a = p.outerAngle * DEG
        arc(out, o, f, r, p.radius, -a, a, 16)
        arc(out, o, f, u, p.radius, -a, a, 16)
        if (p.sourceSize > 0) sphere(out, o, p.sourceSize)
      }
      break
    }
    case 'directional': {
      // parallel rays: a ring of arrows along the direction (the sun has no position and no range)
      const len = selected ? 4 : 1.6, ring = ICON
      seg(out, o, add(o, scale(f, len)))
      head(out, add(o, scale(f, len)), f, Math.min(0.4, len * 0.25))
      for (let i = 0; i < 8; i++) {
        const t = (i / 8) * Math.PI * 2
        const off = at(o, r, u, Math.cos(t) * ring, Math.sin(t) * ring)
        seg(out, off, add(off, scale(f, len * 0.75)))
      }
      arc(out, o, r, u, ring, 0, Math.PI * 2, 24)
      if (p.sourceSize > 0) { // angular diameter: a cone of that opening around the direction, at the arrow's end
        const rad = len * Math.tan((p.sourceSize / 2) * DEG), c = add(o, scale(f, len))
        arc(out, c, r, u, rad, 0, Math.PI * 2, 24)
        for (const [x, y] of [[1, 0], [-1, 0], [0, 1], [0, -1]] as const) seg(out, o, at(c, r, u, x * rad, y * rad))
      }
      break
    }
    case 'rect': {
      // the real emitting rectangle, its front (emitting) side marked by the direction arrow
      const hw = p.width / 2, hh = p.height / 2
      const corners: Vec3[] = [at(o, r, u, -hw, -hh), at(o, r, u, hw, -hh), at(o, r, u, hw, hh), at(o, r, u, -hw, hh)]
      for (let i = 0; i < 4; i++) seg(out, corners[i], corners[(i + 1) % 4])
      seg(out, corners[0], corners[2]); seg(out, corners[1], corners[3]) // the diagonals: an emitting panel, not a frame
      const len = Math.max(0.5, Math.min(p.width, p.height) * 0.5, selected ? 1 : 0)
      seg(out, o, add(o, scale(f, len)))
      head(out, add(o, scale(f, len)), f, Math.min(0.3, len * 0.3))
      if (selected) { // the reach: the attenuation sphere's front half in two planes, and the rays from the corners
        arc(out, o, r, f, p.radius, 0, Math.PI, 24)
        arc(out, o, u, f, p.radius, 0, Math.PI, 24)
        for (const c of corners) seg(out, c, add(c, scale(f, p.radius)))
      }
      break
    }
    case 'sky': {
      // a dome over the position: the upper hemisphere the light comes from
      const s = selected ? 2 : ICON * 1.4
      arc(out, o, X, Z, s, 0, Math.PI * 2, 32)                 // the horizon
      arc(out, o, X, Y, s, 0, Math.PI, 24); arc(out, o, Z, Y, s, 0, Math.PI, 24) // two meridians
      for (const [x, z] of [[1, 0], [-1, 0], [0, 1], [0, -1]] as const) {   // rays coming down onto the position
        const top = add(o, [x * s * 0.5, s * 0.87, z * s * 0.5])
        seg(out, top, add(o, [x * s * 0.12, s * 0.2, z * s * 0.12]))
      }
      head(out, add(o, [0, s * 0.05, 0]), [0, -1, 0], s * 0.25)
      seg(out, add(o, [0, s, 0]), add(o, [0, s * 0.1, 0]))
      break
    }
  }
  return out
}
