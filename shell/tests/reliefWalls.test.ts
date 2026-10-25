import { afterEach, describe, expect, it } from 'vitest'
import { faceNormal, makeBox } from '../src/model/geometry'
import { defaultProps, texEnv } from '../src/model/texprops'
import type { Vec3 } from '../src/model/math'
import type { Relief } from '../src/model/relief'
import { RELIEF_FLOATS, fanBorder, loopBorder, neighbourFn, pieceBorder, polygonBorders, reliefTriangle } from '../src/model/reliefApply'

type V2 = [number, number]
// a tile whose surface is tilted along x: 0.1 high at its left border, 0 at its right one (so repeats do not join), over the quad (0..w, 0..h), uv = (x, 1 - y)
const ramp: Relief = { vertices: [[0, 0, 0.1], [1, 0, 0], [1, 1, 0], [0, 1, 0.1]], faces: [[0, 1, 2, 3]] }
const flat: Relief = { vertices: [[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0]], faces: [[0, 1, 2, 3]] }
const raised: Relief = { vertices: [[0, 0, 0.2], [1, 0, 0.2], [1, 1, 0.2], [0, 1, 0.2]], faces: [[0, 1, 2, 3]] }

function tris(r: Relief, w: number, h: number, border: number[]): { p: Vec3; n: Vec3; uv: V2 }[] {
  const P: Vec3[] = [[0, 0, 0], [w, 0, 0], [w, h, 0], [0, h, 0]]
  const uv = (p: Vec3): V2 => [p[0], 1 - p[1]]
  const out: number[] = []
  const t: [Vec3, Vec3, Vec3][] = [[P[0], P[1], P[2]], [P[0], P[2], P[3]]]
  t.forEach((q, i) => { const g = reliefTriangle(q, [uv(q[0]), uv(q[1]), uv(q[2])], [0, 0, 1], r, undefined, border[i]); expect(g).not.toBeNull(); out.push(...g!) })
  const v: { p: Vec3; n: Vec3; uv: V2 }[] = []
  for (let i = 0; i < out.length; i += RELIEF_FLOATS) v.push({ p: [out[i], out[i + 1], out[i + 2]], n: [out[i + 3], out[i + 4], out[i + 5]], uv: [out[i + 6], out[i + 7]] })
  return v
}
const upright = (v: { p: Vec3; n: Vec3 }[]) => v.filter((x) => Math.abs(x.n[2]) < 1e-6)

describe('relief connecting walls', () => {
  it('closes the gap between repeats whose borders have different heights', () => {
    const walls = upright(tris(ramp, 3, 1, [0, 0]))
    expect(walls.length).toBeGreaterThan(0)
    expect(walls.length % 3).toBe(0)
    // two seams (x = 1, 2): the left repeat ends at 0, the right one starts 0.1 high; the wall looks at the lower side (-x ... the lower is the left)
    for (const x of walls) { expect([1, 2]).toContain(Math.round(x.p[0] * 1e6) / 1e6); expect(x.p[2]).toBeGreaterThanOrEqual(-1e-9); expect(x.p[2]).toBeLessThanOrEqual(0.1 + 1e-9); expect(x.n[0]).toBeCloseTo(-1, 6) }
    expect(Math.max(...walls.map((x) => x.p[2]))).toBeCloseTo(0.1, 6)
  })
  it('adds nothing where the repeats join', () => {
    expect(upright(tris(flat, 3, 2, [0, 0]))).toHaveLength(0)
    expect(upright(tris(raised, 3, 2, [0, 0]))).toHaveLength(0) // equal heights on both sides of every seam
  })
  it('closes the outline of the face down to the surface', () => {
    const open = tris(raised, 2, 1, [0, 0]), shut = tris(raised, 2, 1, [0b011, 0b110])
    expect(upright(open)).toHaveLength(0)
    const walls = upright(shut)
    expect(walls.length).toBeGreaterThan(0)
    // a wall stands on each of the four edges of the quad, from the surface (0) to the relief (0.2), looking out of the face
    for (const x of walls) { expect(x.p[2]).toBeGreaterThanOrEqual(-1e-9); expect(x.p[2]).toBeLessThanOrEqual(0.2 + 1e-9) }
    // the walls that look along `n` stand on the matching edge
    const on = (f: (p: Vec3) => boolean, n: Vec3) => { const s = walls.filter((x) => Math.abs(x.n[0] - n[0]) + Math.abs(x.n[1] - n[1]) < 1e-6); expect(s.length).toBeGreaterThan(0); for (const x of s) expect(f(x.p)).toBe(true) }
    on((p) => Math.abs(p[0]) < 1e-6, [-1, 0, 0])
    on((p) => Math.abs(p[0] - 2) < 1e-6, [1, 0, 0])
    on((p) => Math.abs(p[1]) < 1e-6, [0, -1, 0])
    on((p) => Math.abs(p[1] - 1) < 1e-6, [0, 1, 0])
    // the diagonal between the two triangles is not an outline: nothing stands on it
    expect(walls.every((x) => Math.abs(x.p[0]) < 1e-6 || Math.abs(x.p[0] - 2) < 1e-6 || Math.abs(x.p[1]) < 1e-6 || Math.abs(x.p[1] - 1) < 1e-6)).toBe(true)
  })
  it('every wall triangle has the winding of its normal', () => {
    const v = tris(ramp, 2, 1, [0b011, 0b110])
    for (let i = 0; i < v.length; i += 3) {
      if (Math.abs(v[i].n[2]) > 1e-6) continue
      const a = v[i].p, b = v[i + 1].p, c = v[i + 2].p, n = v[i].n
      const e1 = [b[0] - a[0], b[1] - a[1], b[2] - a[2]], e2 = [c[0] - a[0], c[1] - a[1], c[2] - a[2]]
      const cr = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
      expect(cr[0] * n[0] + cr[1] * n[1] + cr[2] * n[2]).toBeGreaterThan(0)
    }
  })
  it('border bits of fans, loops and pieces', () => {
    const e = [true, false, true, true]
    expect([1, 2].map((i) => fanBorder(i, 4, e))).toEqual([1, 6]) // fan (0,1,2): the polygon's edge 0; fan (0,2,3): the polygon's edges 2 and 3 (its edge 0 is the diagonal)
    expect(loopBorder([5, 6, 7, 8], [5, 6, 7])).toBe(0b011)
    expect(loopBorder([5, 6, 7, 8], [5, 7, 8])).toBe(0b110)
    expect(pieceBorder([{ w: [1, 0, 0] }, { w: [0, 1, 0] }, { w: [0.5, 0.25, 0.25] }], 0b001)).toBe(0b001) // the piece's edge 0 lies along the triangle's edge 0
    expect(pieceBorder([{ w: [1, 0, 0] }, { w: [0, 1, 0] }, { w: [0.5, 0.25, 0.25] }], 0)).toBe(0)
  })
  it('polygon outlines: the seam between two fragments is not an outline', () => {
    const A: Vec3[] = [[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0]], B: Vec3[] = [[1, 0, 0], [2, 0, 0], [2, 1, 0], [1, 1, 0]]
    const [a, b] = polygonBorders([A, B])
    expect(a).toEqual([true, false, true, true])
    expect(b).toEqual([true, true, true, false])
    // a neighbour that splits the shared edge in two still matches
    const C: Vec3[] = [[1, 0, 0], [2, 0, 0], [2, 0.5, 0], [1, 0.5, 0]], D: Vec3[] = [[1, 0.5, 0], [2, 0.5, 0], [2, 1, 0], [1, 1, 0]]
    expect(polygonBorders([A, C, D])[0]).toEqual([true, false, true, true])
  })
})

describe('relief walls at a corner', () => {
  // two faces meet at a convex edge; both stand out 0.2: the wall bevels from one relief to the other instead of dropping to the surface
  const raisedBox = (neighbourDepth: number) => {
    const P: Vec3[] = [[0, 0, 0], [2, 0, 0], [2, 1, 0], [0, 1, 0]]
    const uv = (p: Vec3): V2 => [p[0], 1 - p[1]]
    const nbr = (a: Vec3, b: Vec3) => (Math.abs(a[0]) < 1e-9 && Math.abs(b[0]) < 1e-9
      ? { n: [-1, 0, 0] as Vec3, spans: [{ t0: 0, t1: 1, z0: neighbourDepth, z1: neighbourDepth }] } : null)
    const t: [Vec3, Vec3, Vec3][] = [[P[0], P[1], P[2]], [P[0], P[2], P[3]]]
    const out: number[] = []
    t.forEach((q, i) => out.push(...reliefTriangle(q, [uv(q[0]), uv(q[1]), uv(q[2])], [0, 0, 1], raised, undefined, i === 1 ? 0b100 : 0, nbr)!))
    const v: { p: Vec3; n: Vec3 }[] = []
    for (let i = 0; i < out.length; i += RELIEF_FLOATS) v.push({ p: [out[i], out[i + 1], out[i + 2]], n: [out[i + 3], out[i + 4], out[i + 5]] })
    return v.filter((x) => Math.abs(x.n[2]) < 0.99)
  }
  it('runs from the relief of the face to the relief of its neighbour', () => {
    const w = raisedBox(0.2)
    expect(w.length).toBeGreaterThan(0)
    // the edge x = 0: the face's relief is at z = 0.2 above (0, y, 0.2); the neighbour's, 0.2 out along -x, is at (-0.2, y, 0)
    expect(w.some((x) => Math.abs(x.p[0]) < 1e-6 && Math.abs(x.p[2] - 0.2) < 1e-6)).toBe(true)
    expect(w.some((x) => Math.abs(x.p[0] + 0.2) < 1e-6 && Math.abs(x.p[2]) < 1e-6)).toBe(true)
    for (const x of w) { expect(x.n[0]).toBeCloseTo(-Math.SQRT1_2, 6); expect(x.n[2]).toBeCloseTo(Math.SQRT1_2, 6) } // looks away from both faces
  })
  it('a neighbour without relief gives the plain wall down to the surface', () => {
    const w = raisedBox(0)
    expect(w.length).toBeGreaterThan(0)
    for (const x of w) { expect(x.p[0]).toBeCloseTo(0, 6); expect(x.n[0]).toBeCloseTo(-1, 6) }
  })
})

describe('neighbourFn', () => {
  const raisedRelief: Relief = { vertices: [[0, 0, 0.2], [1, 0, 0.2], [1, 1, 0.2], [0, 1, 0.2]], faces: [[0, 1, 2, 3]] }
  const setup = (texture: string) => {
    texEnv.setProvider((path) => (path === 't' ? { path, width: 64, height: 64, ppm: 64, props: { ...defaultProps(), relief: raisedRelief } } : path === 'plain' ? { path, width: 64, height: 64, ppm: 64, props: defaultProps() } : undefined))
    const b = makeBox('b', [0, 0, 0], [2, 1, 1], texture)
    return { b, top: b.faces.find((f) => faceNormal(b, f)[2] > 0.99)! }
  }
  afterEach(() => texEnv.setProvider(null))
  it('finds the face behind the edge and its relief height along it', () => {
    const { b, top } = setup('t')
    const got = neighbourFn(b, top)([0, 0, 1], [0, 1, 1])
    expect(got).not.toBeNull()
    expect(got!.n).toEqual([-1, 0, 0])
    expect(got!.spans.length).toBeGreaterThan(0)
    for (const s of got!.spans) { expect(s.z0).toBeCloseTo(0.2, 6); expect(s.z1).toBeCloseTo(0.2, 6) }
    // an edge that is not the top face's own edge between two faces of the brush, or half of one, is looked up too
    expect(neighbourFn(b, top)([0, 0.25, 1], [0, 0.75, 1])).not.toBeNull()
    expect(neighbourFn(b, top)([1, 0, 1], [1, 1, 1])).toBeNull() // the middle of the top face: no edge of the box
  })
  it('a neighbour without relief gives nothing (the wall goes down to the surface)', () => {
    const { b, top } = setup('t')
    for (const f of b.faces) if (f.id !== top.id) f.texture = 'plain'
    expect(neighbourFn(b, top)([0, 0, 1], [0, 1, 1])).toBeNull()
  })
})
