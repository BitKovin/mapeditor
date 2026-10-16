// Hotspots (REQUIREMENTS §6 B5, Scythe-style): rectangles drawn on a texture (texture properties window); a face picks the one that fits its size and
// shows exactly that rectangle. Atlas-like textures (a trim sheet, a door + frame + panel sheet) can then be applied to faces of any size.
import type { Brush, Face } from './document'
import { Hotspot, TexInfo, Vec2, naturalSize } from './texprops'
import { Projection, defaultProjectionFor, faceBounds } from './uv'
import { edgeInsetTexels } from './uvmesh'

/** Size of the face along its texture axes (axial, unrotated) in metres. */
export function faceSize(b: Brush, f: Face): Vec2 {
  const base = defaultProjectionFor(f.texture)
  const { min, max } = faceBounds(b, f, { ...base, scale: [1, 1], shift: [0, 0], rotate: 0 })
  return [max[0] - min[0], max[1] - min[1]]
}

/** Natural size of a hotspot in metres (what it measures at the texture's pixels per meter). */
export const hotspotSize = (h: Hotspot, info: Pick<TexInfo, 'width' | 'height' | 'ppm'>): Vec2 => {
  const [nu, nv] = naturalSize(info)
  return [h.w * nu, h.h * nv]
}

/**
 * The best hotspot for a face of `face` metres: the least distortion first (aspect ratio of the face against the hotspot's), then the least change of size.
 * Returns the index, or -1 without hotspots. Ties go to the earlier one, so the choice is stable.
 */
export function chooseHotspot(hotspots: Hotspot[], info: Pick<TexInfo, 'width' | 'height' | 'ppm'>, face: Vec2): number {
  let best = -1, bestScore = Infinity
  const fw = Math.max(face[0], 1e-6), fh = Math.max(face[1], 1e-6)
  hotspots.forEach((h, i) => {
    const [hw, hh] = hotspotSize(h, info)
    const distortion = Math.abs(Math.log(fw / fh / (hw / hh)))
    const size = Math.abs(Math.log(Math.sqrt((fw * fh) / (hw * hh))))
    const score = 3 * distortion + size
    if (score < bestScore - 1e-12) { bestScore = score; best = i }
  })
  return best
}

/**
 * The projection that makes the face show exactly the hotspot: its edges on the face's edges (bottom-left origin kept: the hotspot's left / bottom on the
 * face's left / bottom). The rectangle is pulled in by the edge inset (about 1% of the texture, `edgeInsetTexels`) so the linear filter does not blend the
 * neighbouring part of the sheet in (atlas bleeding).
 */
export function hotspotProjection(b: Brush, f: Face, h: Hotspot, info: Pick<TexInfo, 'width' | 'height'>): Projection {
  const ix = edgeInsetTexels(info.width) / info.width, iy = edgeInsetTexels(info.height) / info.height
  const x0 = h.x + Math.min(ix, h.w / 4), x1 = h.x + h.w - Math.min(ix, h.w / 4)
  const y0 = h.y + Math.min(iy, h.h / 4), y1 = h.y + h.h - Math.min(iy, h.h / 4)
  const { min, max } = faceBounds(b, f, { scale: [1, 1], shift: [0, 0], rotate: 0 })
  const su = Math.max(max[0] - min[0], 1e-6) / (x1 - x0), sv = Math.max(max[1] - min[1], 1e-6) / (y1 - y0)
  return { scale: [su, sv], shift: [x0 - min[0] / su, y0 - min[1] / sv], rotate: 0 }
}

/** The hotspot (index) under an image point, the smallest one when they overlap; -1 when none. */
export function hotspotAt(hotspots: Hotspot[], x: number, y: number): number {
  let best = -1, bestArea = Infinity
  hotspots.forEach((h, i) => { if (x >= h.x && x <= h.x + h.w && y >= h.y && y <= h.y + h.h && h.w * h.h < bestArea) { best = i; bestArea = h.w * h.h } })
  return best
}

/** A rectangle from two corner points (any order), clamped into the texture, at least HOTSPOT_MIN in size. */
export function rectFromCorners(ax: number, ay: number, bx: number, by: number, min = 0.01): Hotspot {
  const cl = (v: number) => Math.min(1, Math.max(0, v))
  const x0 = cl(Math.min(ax, bx)), x1 = cl(Math.max(ax, bx)), y0 = cl(Math.min(ay, by)), y1 = cl(Math.max(ay, by))
  const w = Math.max(x1 - x0, min), h = Math.max(y1 - y0, min)
  return { x: Math.min(x0, 1 - w), y: Math.min(y0, 1 - h), w, h }
}

/** Moves a rectangle by (dx, dy), kept inside the texture. */
export const moveRect = (r: Hotspot, dx: number, dy: number): Hotspot => ({ ...r, x: Math.min(1 - r.w, Math.max(0, r.x + dx)), y: Math.min(1 - r.h, Math.max(0, r.y + dy)) })
