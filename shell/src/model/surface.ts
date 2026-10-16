// The Surface Inspector's view of a face's projection (REQUIREMENTS §6, §19 B1). Pure functions over `Projection` (model/uv.ts):
//   shift   shown in TEXELS (repeats * texture size), like NetRadiant
//   stretch shown as a multiplier of the texture's natural size (1 = pixels / pixels per meter metres per repeat); negative = mirrored
// All functions return a new Projection; the actions apply them to every target face.
import type { Brush, Face } from './document'
import { faceNormal } from './geometry'
import type { Vec3 } from './math'
import { Projection, bottomLeft, projectFace, defaultProjectionFor, faceBounds, justify, projectionOf } from './uv'
import { Vec2, naturalSize, texEnv } from './texprops'

/** Texture size in texels and natural size in metres of a face's texture (64 x 64 px / 1 m while it is not loaded). */
export function surfaceBasis(f: Face): { texels: Vec2; natural: Vec2 } {
  const info = texEnv.info(f.texture ?? 'dev/grey')
  if (!info) return { texels: [64, 64], natural: [1, 1] }
  return { texels: [info.width, info.height], natural: naturalSize(info) }
}

export const shiftTexels = (p: Projection, texels: Vec2): Vec2 => [p.shift[0] * texels[0], p.shift[1] * texels[1]]
export const stretchOf = (p: Projection, natural: Vec2): Vec2 => [p.scale[0] / natural[0], p.scale[1] / natural[1]]

export const withShiftTexels = (p: Projection, axis: 0 | 1, texels: number, size: Vec2): Projection => {
  const shift: Vec2 = [p.shift[0], p.shift[1]]
  shift[axis] = texels / size[axis]
  return { ...p, shift }
}

/** Sets one axis' stretch; 0 is refused (it would collapse the texture). */
export const withStretch = (p: Projection, axis: 0 | 1, stretch: number, natural: Vec2): Projection => {
  if (!Number.isFinite(stretch) || Math.abs(stretch) < 1e-4) return p
  const scale: Vec2 = [p.scale[0], p.scale[1]]
  scale[axis] = stretch * natural[axis]
  return { ...p, scale }
}

/** The mirror button: negate one axis' scale. */
export const mirrorAxis = (p: Projection, axis: 0 | 1): Projection => {
  const scale: Vec2 = [p.scale[0], p.scale[1]]
  scale[axis] = -scale[axis]
  return { ...p, scale }
}

/**
 * Texturing > Width / Height / Fit: stretch the texture so `count` repeats cover the face along the axis (other axis and rotation kept).
 * The origin is the face's BOTTOM-LEFT corner: horizontally the texture starts at the left edge, vertically its bottom edge sits on the face's bottom edge,
 * so a fractional count leaves the partial repeat at the top / right (wallpaper above a skirting board, not under it).
 */
export function fitAxis(b: Brush, f: Face, p: Projection, axis: 0 | 1, count = 1): Projection {
  const { min, max } = faceBounds(b, f, p)
  const extent = Math.max(max[axis] - min[axis], 1e-6), k = Math.max(count, 1e-3)
  const sign = p.scale[axis] < 0 ? -1 : 1
  const scale: Vec2 = [p.scale[0], p.scale[1]], shift: Vec2 = [p.shift[0], p.shift[1]]
  scale[axis] = (sign * extent) / k
  const lo = Math.min(min[axis] / scale[axis], max[axis] / scale[axis]), hi = Math.max(min[axis] / scale[axis], max[axis] / scale[axis])
  shift[axis] = axis === 0 ? -lo : 1 - hi // u: left edge = texture start; v grows downwards: the face's bottom (largest v) = texture bottom (1)
  return { ...p, scale, shift }
}
export const fitBoth = (b: Brush, f: Face, p: Projection, counts: Vec2): Projection => fitAxis(b, f, fitAxis(b, f, p, 0, counts[0]), 1, counts[1])

/** Match Grid: every repeat is a whole number of grid cells wide (at least one). */
export function matchGrid(p: Projection, grid: number): Projection {
  if (!(grid > 0)) return p
  const snap = (v: number): number => Math.sign(v || 1) * Math.max(1, Math.round(Math.abs(v) / grid)) * grid
  return { ...p, scale: [snap(p.scale[0]), snap(p.scale[1])] }
}

const unit = (v: Vec3): Vec3 | null => { const l = Math.hypot(v[0], v[1], v[2]); return l < 1e-9 ? null : [v[0] / l, v[1] / l, v[2] / l] }
const crossV = (a: Vec3, b: Vec3): Vec3 => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
const dotV = (a: Vec3, b: Vec3): number => a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
const inPlane = (v: Vec3, n: Vec3): Vec3 | null => unit([v[0] - n[0] * dotV(v, n), v[1] - n[1] * dotV(v, n), v[2] - n[2] * dotV(v, n)])

/** The new kind keeps stretch and mirroring; rotation, shift and shear restart (they were relative to the old axes). */
const rekind = (p: Projection, kind: 'ortho' | 'cam', basis: [Vec3, Vec3]): Projection => ({ scale: [p.scale[0], p.scale[1]], shift: [0, 0], rotate: 0, kind, basis })

/**
 * Project > Ortho: the texture axes lie IN the face: v runs down the face's steepest slope (world up projected into the plane; for floors and ceilings the
 * world -Z direction), u across it, readable from the front. On axis-aligned faces it equals Axial; on sloped faces it does not stretch.
 */
export function projectOrtho(b: Brush, f: Face, p: Projection): Projection {
  const n = faceNormal(b, f)
  const up: Vec3 = Math.abs(n[1]) > 0.999 ? [0, 0, -1] : [0, 1, 0]
  const right = unit(crossV(up, n))!
  const upInPlane = unit(crossV(n, right))!
  return bottomLeft(b, f, rekind(p, 'ortho', [right, [-upInPlane[0], -upInPlane[1], -upInPlane[2]]]))
}

/** Project > Cam: the texture axes are the camera's right / up projected into the face, frozen now. null when the camera looks along the face (degenerate). */
export function projectCam(b: Brush, f: Face, p: Projection, cam: { right: Vec3; up: Vec3 }): Projection | null {
  const n = faceNormal(b, f)
  const r = inPlane(cam.right, n), u = inPlane(cam.up, n)
  if (!r || !u || Math.abs(dotV(r, u)) > 0.999) return null
  return bottomLeft(b, f, rekind(p, 'cam', [r, [-u[0], -u[1], -u[2]]]))
}

export const withShear = (p: Projection, shear: number): Projection => {
  const { shear: _drop, ...rest } = p
  return Number.isFinite(shear) && Math.abs(shear) > 1e-9 ? { ...rest, shear } : rest
}

/** Project > Axial: the standard projection along the dominant axis again (rotation and shift cleared), the face's current stretch kept. */
export const projectAxial = (p: Projection): Projection => ({ scale: [p.scale[0], p.scale[1]], shift: [0, 0], rotate: 0 })

/** Reset: the texture at its natural size, unrotated, its bottom-left corner on the face's bottom-left corner. */
export const resetSurface = (b: Brush, f: Face): Projection => bottomLeft(b, f, defaultProjectionFor(f.texture))


/** Pixels per metre the face shows along u and v (texel density of the surface). */
export { faceDensity } from './texelDensity'
export { projectionOf, justify, bottomLeft, faceBounds }
export type { JustifyEdge } from './uv'

/**
 * A face that has no projection yet, textured with a texture whose size is known, gets one: natural size, bottom-left corner on the face's bottom-left
 * corner (REQUIREMENTS §19: UVs start at the bottom left of a wall). Baked at creation / first texturing so that UV lock keeps working afterwards
 * (a face-relative default computed at draw time would move with the brush). Unknown size (texture not loaded): left lazy (world-aligned 2 m).
 */
export function withBottomLeftDefault(b: Brush, f: Face): Face {
  if (f.projection || !texEnv.info(f.texture ?? 'dev/grey')) return f
  const p = bottomLeft(b, f, defaultProjectionFor(f.texture))
  return { ...f, projection: p, uvs: projectFace(b, f, p) }
}
export const alignBrush = (b: Brush): Brush => ({ ...b, faces: b.faces.map((f) => withBottomLeftDefault(b, f)) })
