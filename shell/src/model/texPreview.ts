// The picture in the Texture Properties window: a flat wall (width x height metres, facing the viewer) textured with the DRAFT properties through the
// same plan the viewports use (model/uvmesh.ts), so what the window shows is what the level gets. Pure; the window only paints the pieces.
import type { Vec3 } from './math'
import { TexInfo, Vec2, naturalSize } from './texprops'
import { Extent, UvAffine, makePlan, retexture } from './uvmesh'

export interface PreviewPiece { texture: string; /** wall metres, y up */ poly: Vec2[]; /** per vertex, in repeats of `texture` */ uv: Vec2[]; }
export interface Preview { pieces: PreviewPiece[]; /** false = the face would need too many cells: drawn as a plain repeat, and the window says so */ cut: boolean; extent: Extent }

/** `scaleMul` stretches the texture on the wall (1 = natural size); it is what makes the heat map show something. */
export function buildPreview(draft: TexInfo, lookup: (path: string) => TexInfo | undefined, wallW: number, wallH: number, scaleMul = 1): Preview {
  const [nu, nv] = naturalSize(draft)
  const su = nu * scaleMul, sv = nv * scaleMul
  // image axes: u to the right, v downwards (v = 0 at the top of the wall's texture)
  const map: UvAffine = { u: [1 / su, 0, 0, 0], v: [0, -1 / sv, 0, wallH / sv] }
  const extent: Extent = { umin: 0, umax: wallW / su, vmin: 0, vmax: wallH / sv }
  const plan = makePlan({ texture: draft, info: lookup, map, extent })
  const corners: Vec3[] = [[0, 0, 0], [wallW, 0, 0], [wallW, wallH, 0], [0, wallH, 0]]
  const tris: [Vec3, Vec3, Vec3][] = [[corners[0], corners[1], corners[2]], [corners[0], corners[2], corners[3]]]
  if (!plan) {
    const uvOf = (p: Vec3): Vec2 => [p[0] / su, (wallH - p[1]) / sv]
    return { cut: false, extent, pieces: tris.map((t) => ({ texture: draft.path, poly: t.map((p) => [p[0], p[1]] as Vec2), uv: t.map(uvOf) })) }
  }
  const pieces: PreviewPiece[] = []
  for (const o of retexture(plan, tris)) {
    const t = tris[o.src]
    pieces.push({
      texture: o.texture,
      poly: o.verts.map((v) => [v.w[0] * t[0][0] + v.w[1] * t[1][0] + v.w[2] * t[2][0], v.w[0] * t[0][1] + v.w[1] * t[1][1] + v.w[2] * t[2][1]] as Vec2),
      uv: o.verts.map((v) => v.uv),
    })
  }
  return { cut: true, extent, pieces }
}

/** The affine map (a, b, c, d, e, f) of canvas.setTransform / pattern.setTransform taking texture PIXELS to canvas pixels, from three vertices of a piece. null when degenerate. */
export function pieceTransform(piece: PreviewPiece, toCanvas: (p: Vec2) => Vec2, texW: number, texH: number): [number, number, number, number, number, number] | null {
  const pts = piece.poly.map(toCanvas)
  const n = pts.length
  let best = -1, bi = 1, bj = 2
  for (let i = 1; i < n; i++) for (let j = i + 1; j < n; j++) {
    const a = Math.abs((piece.uv[i][0] - piece.uv[0][0]) * (piece.uv[j][1] - piece.uv[0][1]) - (piece.uv[j][0] - piece.uv[0][0]) * (piece.uv[i][1] - piece.uv[0][1]))
    if (a > best) { best = a; bi = i; bj = j }
  }
  if (best < 1e-12) return null
  const px = [0, bi, bj].map((k) => [piece.uv[k][0] * texW, piece.uv[k][1] * texH])
  const cv = [0, bi, bj].map((k) => pts[k])
  // solve [px py 1] * [a b; c d; e f] = [cx cy]
  const d = (px[1][0] - px[0][0]) * (px[2][1] - px[0][1]) - (px[2][0] - px[0][0]) * (px[1][1] - px[0][1])
  const solve = (k: 0 | 1): [number, number, number] => {
    const dx1 = cv[1][k] - cv[0][k], dx2 = cv[2][k] - cv[0][k]
    const A = (dx1 * (px[2][1] - px[0][1]) - dx2 * (px[1][1] - px[0][1])) / d
    const B = (dx2 * (px[1][0] - px[0][0]) - dx1 * (px[2][0] - px[0][0])) / d
    return [A, B, cv[0][k] - A * px[0][0] - B * px[0][1]]
  }
  const [a, c, e] = solve(0), [b, dd, f] = solve(1)
  return [a, b, c, dd, e, f]
}

/** Split line dragging: a pointer position along the texture view (0..1) becomes a `split`. */
export const splitFromPointer = (frac: number): number => Math.min(0.98, Math.max(0.02, frac))

/** Anchor side from the edge a click landed on (the nearest of the two edges of the axis, when within `reach` of it, as fractions 0..1). */
export function anchorFromEdgeClick(frac: number, reach = 0.08): 'start' | 'end' | null {
  if (frac <= reach) return 'start'
  if (frac >= 1 - reach) return 'end'
  return null
}

/** Dragging the alternate texture in the preview: a pointer movement of (dx, dy) wall metres moves its pattern by that many repeats of the alternate texture. */
export function dragAltOffset(offset: Vec2, dxMetres: number, dyMetres: number, alt: { width: number; height: number; ppm: number }, scaleMul = 1): Vec2 {
  const [nu, nv] = naturalSize(alt)
  // the pattern follows the pointer: moving right by dx shifts u backwards; moving up (dy > 0) shifts v (which grows downwards) the other way
  return [offset[0] - dxMetres / (nu * scaleMul), offset[1] + dyMetres / (nv * scaleMul)]
}
