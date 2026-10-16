// A face laid out by a texture group (REQUIREMENTS §20) as a FacePlan: the same interface repeat behaviour uses (model/uvmesh.ts), so every consumer (renderer,
// carved fragments, Lit views, stale overlay, relief placement, bake export) handles it without a special case.
//
// Layout space: the face's own frame, independent of its texture projection. x runs to the right as seen from the front, y runs up (world up projected into the
// face; for floors and ceilings world -Z, like Project Ortho), origin at the bottom-left of the face's extent. `layout.rotate` / `flipU` turn that frame.
//
// A cell samples its texture like a small face of its own: its UV is an affine map of (x, y) (fit / align / flip / rotate, see `cellMap`). A cell whose texture needs
// cuts (mirror, anchor, alternate texture, edge clamp: uvmesh `makePlan`) gets exactly that plan for the cell, so those behaviours keep working inside a layout.
import type { Brush, Face } from './document'
import { faceNormal } from './geometry'
import { Cell, Facing, LayoutResult, faceSeed, facingOf, solveLayout, tiltOf } from './layout'
import type { Vec3 } from './math'
import { FaceLayout, groupEnv, groupTextures } from './texgroup'
import { TexInfo, Vec2, edgeClamp, isPlainProps, naturalSize, texEnv } from './texprops'
import { Aff, Extent, FacePlan, Piece, UvAffine, lerpVec3, makePlan } from './uvmesh'

const dot = (a: Vec3, b: Vec3): number => a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
const cross = (a: Vec3, b: Vec3): Vec3 => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
const unit = (a: Vec3): Vec3 => { const l = Math.hypot(a[0], a[1], a[2]) || 1; return [a[0] / l, a[1] / l, a[2] / l] }
const neg = (a: Vec3): Vec3 => [-a[0], -a[1], -a[2]]

export interface Frame {
  ex: Vec3; ey: Vec3
  /** x = ex . p + dx, y = ey . p + dy */
  dx: number; dy: number
  /** extent of the face in the frame, metres */
  w: number; h: number
  normal: Vec3
  /** a point of the face and its layout coordinates (for the world height of a layout point) */
  q: Vec3; xq: number; yq: number
}

/** The layout frame of an authored face; null for a degenerate face. */
export function layoutFrame(b: Brush, f: Face, rotate = 0, flipU = false): Frame | null {
  const n = faceNormal(b, f)
  if (!(Math.hypot(n[0], n[1], n[2]) > 0.5) || f.loop.length < 3) return null
  const up: Vec3 = Math.abs(n[1]) > 0.999 ? [0, 0, -1] : [0, 1, 0]
  let ex = unit(cross(up, n)), ey = unit(cross(n, ex))
  for (let k = 0; k < rotate / 90; k++) { const t = ex; ex = ey; ey = neg(t) }
  if (flipU) ex = neg(ex)
  let xmin = Infinity, xmax = -Infinity, ymin = Infinity, ymax = -Infinity
  const pts = f.loop.map((i) => b.vertices[i])
  for (const p of pts) { const x = dot(ex, p), y = dot(ey, p); xmin = Math.min(xmin, x); xmax = Math.max(xmax, x); ymin = Math.min(ymin, y); ymax = Math.max(ymax, y) }
  return { ex, ey, dx: -xmin, dy: -ymin, w: xmax - xmin, h: ymax - ymin, normal: n, q: pts[0], xq: dot(ex, pts[0]) - xmin, yq: dot(ey, pts[0]) - ymin }
}

type Aff2 = [number, number, number]
/** u and v of a cell as affine functions of the layout coordinates: u = a[0] x + a[1] y + a[2]. */
export interface CellMap { u: Aff2; v: Aff2 }

/**
 * How a cell's texture sits in it. `tex` = natural size in metres of the sampled texture; `fw`, `fh` = the face (layout) size, used by origin 'face'.
 * v is in image convention (down); the texture's bottom-left corner goes to the origin's bottom-left, like a plain face.
 */
export function cellMap(c: Cell, tex: Vec2, fw: number, fh: number): CellMap {
  const cw = c.x1 - c.x0, ch = c.y1 - c.y0
  const faceOrigin = c.origin === 'face' && c.fit === 'tile' && c.rotate === 0
  // local frame (a right, b up), its size (A, B)
  const local = (x: number, y: number): [number, number] => {
    if (faceOrigin) return [c.flipU ? fw - x : x, c.flipV ? fh - y : y]
    const px = c.flipU ? c.x1 - x : x - c.x0, py = c.flipV ? c.y1 - y : y - c.y0
    return c.rotate === 90 ? [py, cw - px] : c.rotate === 180 ? [cw - px, ch - py] : c.rotate === 270 ? [ch - py, px] : [px, py]
  }
  const swap = c.rotate === 90 || c.rotate === 270
  const A = faceOrigin ? fw : swap ? ch : cw, B = faceOrigin ? fh : swap ? cw : ch
  const [nw, nh] = tex
  let ew = nw, eh = nh
  if (c.fit === 'stretch') { ew = A / Math.max(c.cropU ?? 1, 1e-6); eh = B / Math.max(c.cropV ?? 1, 1e-6) } else if (c.fit === 'width') { ew = A; eh = (nh * A) / nw } else if (c.fit === 'height') { eh = B; ew = (nw * B) / nh }
  const fn = (x: number, y: number): [number, number] => {
    const [a, b] = local(x, y)
    const u = c.alignU === 'center' ? 0.5 + (a - A / 2) / ew : c.alignU === 'end' ? 1 + (a - A) / ew : a / ew
    const v = c.alignV === 'center' ? 0.5 - (b - B / 2) / eh : c.alignV === 'end' ? (B - b) / eh : 1 - b / eh
    return [u, v]
  }
  const o = fn(0, 0), px = fn(1, 0), py = fn(0, 1)
  return { u: [px[0] - o[0], py[0] - o[0], o[0]], v: [px[1] - o[1], py[1] - o[1], o[1]] }
}

const at = (m: Aff2, x: number, y: number): number => m[0] * x + m[1] * y + m[2]

interface CV { x: number; y: number; w: [number, number, number] }
/** Keeps x >= bound (axis 'x', ge) etc. New vertices lie exactly on the bound. */
function clip(poly: CV[], k: 'x' | 'y', bound: number, ge: boolean): CV[] {
  const out: CV[] = []
  const inside = (p: CV): boolean => (ge ? p[k] >= bound : p[k] <= bound)
  for (let i = 0; i < poly.length; i++) {
    const a = poly[i], b = poly[(i + 1) % poly.length]
    const ia = inside(a), ib = inside(b)
    if (ia) out.push(a)
    if (ia !== ib) {
      const t = (bound - a[k]) / (b[k] - a[k])
      const p: CV = { x: a.x + (b.x - a.x) * t, y: a.y + (b.y - a.y) * t, w: [a.w[0] + (b.w[0] - a.w[0]) * t, a.w[1] + (b.w[1] - a.w[1]) * t, a.w[2] + (b.w[2] - a.w[2]) * t] }
      p[k] = bound
      out.push(p)
    }
  }
  return out
}
const area = (p: CV[]): number => { let s = 0; for (let i = 0; i < p.length; i++) { const a = p[i], b = p[(i + 1) % p.length]; s += a.x * b.y - b.x * a.y } return Math.abs(s) / 2 }

interface Prepared { cell: Cell; info: TexInfo; map: CellMap; sub: FacePlan | null }

export interface LayoutPlanOut { plan: FacePlan | null; result: LayoutResult | null; why: string }

/** Everything a plan needs of a face, so the editor and the tests can build one without a document. */
export function buildLayoutPlan(frame: Frame, cells: readonly Cell[], fallback: string, info: (path: string) => TexInfo | undefined): FacePlan | null {
  const prepared: Prepared[] = []
  const paths = new Set<string>([fallback])
  for (const cell of cells) {
    const ti = info(cell.texture)
    if (!ti || !(ti.width > 0 && ti.height > 0 && ti.ppm > 0)) return null // a texture is still loading: the face is drawn plainly until it is there
    paths.add(cell.texture)
    const map = cellMap(cell, naturalSize(ti), frame.w, frame.h)
    let sub: FacePlan | null = null
    if (!isPlainProps(ti.props) || edgeClamp(ti, 0) || edgeClamp(ti, 1)) {
      // the cell as a small face: its own world affine map and uv extent (the whole face for origin 'face', so anchoring means what it means on the face)
      const world = (m: Aff2): Aff => [m[0] * frame.ex[0] + m[1] * frame.ey[0], m[0] * frame.ex[1] + m[1] * frame.ey[1], m[0] * frame.ex[2] + m[1] * frame.ey[2], m[0] * frame.dx + m[1] * frame.dy + m[2]]
      const wm: UvAffine = { u: world(map.u), v: world(map.v) }
      const whole = cell.origin === 'face' && cell.fit === 'tile' && cell.rotate === 0
      const xs = whole ? [0, frame.w] : [cell.x0, cell.x1], ys = whole ? [0, frame.h] : [cell.y0, cell.y1]
      const ext: Extent = { umin: Infinity, umax: -Infinity, vmin: Infinity, vmax: -Infinity }
      for (const x of xs) for (const y of ys) { const u = at(map.u, x, y), v = at(map.v, x, y); ext.umin = Math.min(ext.umin, u); ext.umax = Math.max(ext.umax, u); ext.vmin = Math.min(ext.vmin, v); ext.vmax = Math.max(ext.vmax, v) }
      sub = makePlan({ texture: ti, info, map: wm, extent: ext })
      if (sub) for (const t of sub.textures) paths.add(t)
    }
    prepared.push({ cell, info: ti, map, sub })
  }
  if (prepared.length === 0) return null
  const toXY = (p: Vec3): [number, number] => [dot(frame.ex, p) + frame.dx, dot(frame.ey, p) + frame.dy]

  return {
    texture: fallback,
    textures: [...paths],
    pieces(tri) {
      const verts: CV[] = tri.map((p, i) => { const [x, y] = toXY(p); return { x, y, w: [i === 0 ? 1 : 0, i === 1 ? 1 : 0, i === 2 ? 1 : 0] as [number, number, number] } })
      const xmin = Math.min(...verts.map((v) => v.x)), xmax = Math.max(...verts.map((v) => v.x)), ymin = Math.min(...verts.map((v) => v.y)), ymax = Math.max(...verts.map((v) => v.y))
      const out: Piece[] = []
      for (const pc of prepared) {
        const c = pc.cell
        if (c.x1 < xmin - 1e-9 || c.x0 > xmax + 1e-9 || c.y1 < ymin - 1e-9 || c.y0 > ymax + 1e-9) continue
        const poly = clip(clip(clip(clip(verts, 'x', c.x0, true), 'x', c.x1, false), 'y', c.y0, true), 'y', c.y1, false)
        if (poly.length < 3 || area(poly) < 1e-10) continue
        if (!pc.sub) { out.push({ texture: c.texture, verts: poly.map((q) => ({ w: q.w, uv: [at(pc.map.u, q.x, q.y), at(pc.map.v, q.x, q.y)] as Vec2 })) }); continue }
        // a cell with its own cuts: plan each fan triangle of the clipped polygon, then express the weights against the source triangle
        for (let k = 1; k + 1 < poly.length; k++) {
          const sub = [poly[0], poly[k], poly[k + 1]]
          const world = sub.map((q) => lerpVec3(tri, q.w)) as [Vec3, Vec3, Vec3]
          for (const piece of pc.sub.pieces(world)) {
            out.push({
              texture: piece.texture,
              verts: piece.verts.map((v) => ({
                w: [0, 1, 2].map((m) => v.w[0] * sub[0].w[m] + v.w[1] * sub[1].w[m] + v.w[2] * sub[2].w[m]) as [number, number, number],
                uv: v.uv,
              })),
            })
          }
        }
      }
      return out
    },
  }
}

/** The layout plan of an authored face, or null (no group yet, a slot texture not loaded, a cap hit, degenerate face): the face is then drawn the plain way. */
export function layoutPlanFor(b: Brush, f: Face, layout: FaceLayout, info: (path: string) => TexInfo | undefined = (p) => texEnv.info(p)): LayoutPlanOut {
  const group = groupEnv.get(layout.group)
  if (!group) return { plan: null, result: null, why: 'group not loaded' }
  for (const t of groupTextures(group)) info(t) // starts loading every slot texture
  const frame = layoutFrame(b, f, layout.rotate, layout.flipU)
  if (!frame) return { plan: null, result: null, why: 'degenerate face' }
  const result = solveLayout({
    group, params: layout.params, w: frame.w, h: frame.h,
    facing: facingOf(frame.normal[1]) as Facing, tilt: tiltOf(frame.normal[1]),
    seed: layout.seed ?? faceSeed(b.id, f.id),
    worldY: (x, y) => frame.q[1] + frame.ex[1] * (x - frame.xq) + frame.ey[1] * (y - frame.yq),
    info, fallbackTexture: f.texture ?? 'dev/grey',
  })
  if (result.truncated || result.cells.length === 0) return { plan: null, result, why: result.truncated ? 'too many cells' : 'no cells' }
  const fb = group.fallback ? group.slots[group.fallback] : undefined
  const plan = buildLayoutPlan(frame, result.cells, fb ?? f.texture ?? 'dev/grey', info)
  return { plan, result, why: plan ? '' : 'texture not loaded' }
}
