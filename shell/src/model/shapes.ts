// Block-tool primitives beyond the box (DESIGN.md §7/§8). A shape is built inside the oriented box the person
// drew (rectangle on a plane, extruded by a height). Solids that are not convex (an arch, stairs) are built as
// several convex brushes, as level editors traditionally do, so they can live in worldspawn.
import type { Brush } from './document'
import { Vec3, add, scale } from './math'
import { signedVolume, validateBrush } from './geometry'
import { smoothBetween } from './smooth'
import type { DrawPlane, Rect } from '../tools/boxTool'

export type ShapeId = 'box' | 'cylinder' | 'wedge' | 'arch' | 'stairs' | 'sphere'
export interface ShapeParams {
  sides: number; steps: number; archSegments: number; archThickness: number
  /** sphere: faces around (4..32) and from pole to pole (2..16) */
  segments: number; rings: number
  /** cylinder sides / the whole sphere come out with their edges marked smooth (REQUIREMENTS §33 H6) */
  smooth: boolean
  /** orientation used when drawing */ turn: Turn
}
export const defaultShapeParams = (): ShapeParams => ({ sides: 12, steps: 8, archSegments: 8, archThickness: 0.5, segments: 12, rings: 6, smooth: true, turn: 0 })

/** Local frame of the drawn box: origin at its min corner, axes u (rect width), v (rect depth), n (height). Stored with parametric shapes. */
export interface Frame { o: Vec3; u: Vec3; v: Vec3; n: Vec3; du: number; dv: number; h: number }
/** Orientation of a shape in its box: quarter turns about the height axis (which way a ramp rises, stairs climb, an arch spans). */
export type Turn = 0 | 1 | 2 | 3

export function frameOf(plane: DrawPlane, rect: Rect, height: number): Frame {
  // negative extents flip the axis so the local box is always positive; handedness is fixed by the winding fix below
  const su = rect.du < 0 ? -1 : 1, sv = rect.dv < 0 ? -1 : 1, sh = height < 0 ? -1 : 1
  return { o: rect.origin, u: scale(plane.u, su), v: scale(plane.v, sv), n: scale(plane.normal, sh), du: Math.abs(rect.du), dv: Math.abs(rect.dv), h: Math.abs(height) }
}
/** The same box seen after `turn` quarter turns about its height axis: the shape's x axis then runs along v, -u or -v. Handedness is kept. */
export function turned(f: Frame, turn: Turn): Frame {
  const U = scale(f.u, f.du), V = scale(f.v, f.dv)
  switch (turn) {
    case 1: return { ...f, o: add(f.o, U), u: f.v, v: scale(f.u, -1), du: f.dv, dv: f.du }
    case 2: return { ...f, o: add(add(f.o, U), V), u: scale(f.u, -1), v: scale(f.v, -1) }
    case 3: return { ...f, o: add(f.o, V), u: scale(f.v, -1), v: f.u, du: f.dv, dv: f.du }
    default: return f
  }
}

const at = (f: Frame, x: number, y: number, z: number): Vec3 => add(add(add(f.o, scale(f.u, x)), scale(f.v, y)), scale(f.n, z))

/** Prism from a convex polygon profile (in the local x/z plane, listed CCW seen from +y) extruded over y in [y0, y1]. */
function prism(id: string, f: Frame, profile: [number, number][], y0: number, y1: number, texture: string): Brush {
  const n = profile.length
  const vertices: Vec3[] = [
    ...profile.map(([x, z]) => at(f, x, y0, z)),
    ...profile.map(([x, z]) => at(f, x, y1, z)),
  ]
  const faces = [
    { id: 0, loop: Array.from({ length: n }, (_, i) => i).reverse(), texture },                    // y0 side
    { id: 1, loop: Array.from({ length: n }, (_, i) => n + i), texture },                          // y1 side
    ...profile.map((_, i) => ({ id: 2 + i, loop: [i, (i + 1) % n, n + ((i + 1) % n), n + i], texture })),
  ]
  const b: Brush = { id, vertices, faces }
  // the frame may be left- or right-handed depending on the plane; flip everything if the volume came out negative
  return signedVolume(b) < 0 ? { ...b, faces: b.faces.map((fc) => ({ ...fc, loop: [...fc.loop].reverse() })) } : b
}

/**
 * A UV sphere filling the box (an ellipsoid when the box is not a cube): poles on the height axis, `segments` faces around, `rings` from pole to pole.
 * One convex brush: the quads between two rings are planar (symmetric trapezoids, also after the box's scaling), the caps are triangles.
 */
function sphere(id: string, f: Frame, segments: number, rings: number, texture: string): Brush {
  const rx = f.du / 2, ry = f.dv / 2, rz = f.h / 2
  const vertices: Vec3[] = [at(f, rx, ry, 0)]
  for (let k = 1; k < rings; k++) {
    const t = (Math.PI * k) / rings, s = Math.sin(t), z = rz * (1 - Math.cos(t))
    for (let j = 0; j < segments; j++) {
      const a = (2 * Math.PI * j) / segments
      vertices.push(at(f, rx + rx * s * Math.cos(a), ry + ry * s * Math.sin(a), z))
    }
  }
  const top = vertices.length
  vertices.push(at(f, rx, ry, f.h))
  const ring = (k: number, j: number) => 1 + (k - 1) * segments + (((j % segments) + segments) % segments)
  const faces: Brush['faces'] = []
  let fid = 0
  for (let j = 0; j < segments; j++) faces.push({ id: fid++, loop: [0, ring(1, j + 1), ring(1, j)], texture })
  for (let k = 1; k + 1 < rings; k++)
    for (let j = 0; j < segments; j++) faces.push({ id: fid++, loop: [ring(k, j), ring(k, j + 1), ring(k + 1, j + 1), ring(k + 1, j)], texture })
  for (let j = 0; j < segments; j++) faces.push({ id: fid++, loop: [top, ring(rings - 1, j), ring(rings - 1, j + 1)], texture })
  const b: Brush = { id, vertices, faces }
  return signedVolume(b) < 0 ? { ...b, faces: b.faces.map((fc) => ({ ...fc, loop: [...fc.loop].reverse() })) } : b
}

export type ShapeOutcome = { brushes: Brush[] } | { error: string }

/** All shapes fill the drawn box exactly. Returns brushes with fresh ids from `newId` (no `shape` record: see model/parametric.ts). */
export function buildShape(shape: ShapeId, plane: DrawPlane, rect: Rect, height: number, texture: string, params: ShapeParams, newId: () => string, turn: Turn = 0): ShapeOutcome {
  return buildInFrame(shape, turned(frameOf(plane, rect, height), turn), texture, params, newId)
}

/** The shape built in a box given as a frame (already turned). Parts come out in a fixed order, so part i of a shape is always the same piece. */
export function buildInFrame(shape: ShapeId, f: Frame, texture: string, params: ShapeParams, newId: () => string): ShapeOutcome {
  if (f.du < 1e-4 || f.dv < 1e-4 || f.h < 1e-4) return { error: 'Shape is too thin' }
  let out: Brush[] = []
  switch (shape) {
    case 'box': {
      const [x, y, z] = [f.du, f.dv, f.h]
      out = [prism(newId(), f, [[0, 0], [x, 0], [x, z], [0, z]], 0, y, texture)]
      break
    }
    case 'cylinder': {
      const n = Math.max(3, Math.min(64, Math.round(params.sides)))
      const profile: [number, number][] = Array.from({ length: n }, (_, i) => {
        const a = (i / n) * Math.PI * 2
        return [f.du / 2 + (Math.cos(a) * f.du) / 2, f.h / 2 + (Math.sin(a) * f.h) / 2] as [number, number]
      })
      const b = prism(newId(), f, profile, 0, f.dv, texture)
      // faces 2.. are the sides (prism): smooth between them, the caps stay sharp
      out = [params.smooth === false ? b : smoothBetween(b, b.faces.filter((fc) => fc.id >= 2).map((fc) => fc.id))]
      break
    }
    case 'sphere': {
      const b = sphere(newId(), f, Math.max(4, Math.min(32, Math.round(params.segments ?? 12))), Math.max(2, Math.min(16, Math.round(params.rings ?? 6))), texture)
      out = [params.smooth === false ? b : smoothBetween(b, b.faces.map((fc) => fc.id))]
      break
    }
    case 'wedge':
      // a ramp: full height at u = du, zero at u = 0 (profile in the u/height plane)
      out = [prism(newId(), f, [[0, 0], [f.du, 0], [f.du, f.h]], 0, f.dv, texture)]
      break
    case 'stairs': {
      const n = Math.max(2, Math.min(64, Math.round(params.steps)))
      // each step is a box from the floor up to its own height, so every brush stays convex
      for (let i = 0; i < n; i++) {
        const x0 = (i / n) * f.du, x1 = ((i + 1) / n) * f.du, top = ((i + 1) / n) * f.h
        out.push(prism(newId(), f, [[x0, 0], [x1, 0], [x1, top], [x0, top]], 0, f.dv, texture))
      }
      break
    }
    case 'arch': {
      const n = Math.max(3, Math.min(64, Math.round(params.archSegments)))
      const t = params.archThickness
      if (t <= 1e-3 || t >= Math.min(f.du / 2, f.h) - 1e-3) return { error: 'Arch thickness must be smaller than half its width and its height' }
      const rx = f.du / 2, rz = f.h            // outer half-ellipse; the flat side sits on the bottom of the box
      const pt = (a: number, ox: number, oz: number): [number, number] => [rx + Math.cos(a) * ox, Math.sin(a) * oz]
      for (let i = 0; i < n; i++) {
        const a0 = Math.PI - (i / n) * Math.PI, a1 = Math.PI - ((i + 1) / n) * Math.PI
        const profile = [pt(a0, rx, rz), pt(a0, rx - t, rz - t), pt(a1, rx - t, rz - t), pt(a1, rx, rz)]
        // outer0 -> inner0 -> inner1 -> outer1 runs clockwise for the left-to-right sweep: reverse for CCW
        out.push(prism(newId(), f, profile.reverse(), 0, f.dv, texture))
      }
      break
    }
  }
  for (const b of out) {
    const problem = validateBrush(b, true)
    if (problem) return { error: `${shape}: ${problem}` }
  }
  return { brushes: out }
}

/** Edges of a shape being drawn (REQUIREMENTS §33 H4): flat line endpoints of every brush edge, each shared edge once; [] when it cannot be built. */
export function shapePreviewLines(shape: ShapeId, plane: DrawPlane, rect: Rect, height: number, params: ShapeParams, turn: Turn = 0): number[] {
  const r = buildShape(shape, plane, rect, height, '', params, () => 'p', turn)
  if ('error' in r) return []
  const out: number[] = [], seen = new Set<string>()
  const key = (p: Vec3) => p.map((x) => x.toFixed(5)).join(',')
  for (const b of r.brushes)
    for (const f of b.faces)
      for (let i = 0; i < f.loop.length; i++) {
        const p = b.vertices[f.loop[i]], q = b.vertices[f.loop[(i + 1) % f.loop.length]]
        const a = key(p), c = key(q), k = a < c ? `${a}|${c}` : `${c}|${a}`
        if (seen.has(k)) continue
        seen.add(k)
        out.push(...p, ...q)
      }
  return out
}
