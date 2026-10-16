import { afterEach, describe, expect, it } from 'vitest'
import type { Vec3 } from '../src/model/math'
import { buildPreset, flatRelief } from '../src/model/relief'
import { MAX_RELIEF_CELLS, RELIEF_FLOATS, newBudget, reliefOf, reliefTriangle } from '../src/model/reliefApply'
import { defaultProps, texEnv } from '../src/model/texprops'

type V2 = [number, number]
// a wall quad on z = 0 facing +z, x right, y up; uv = (x / size, 1 - y / size): v runs down the picture like the real projection
function quad(size: number, w = 1, h = 1): { tris: [Vec3, Vec3, Vec3][]; uvs: [V2, V2, V2][] } {
  const P: Vec3[] = [[0, 0, 0], [w, 0, 0], [w, h, 0], [0, h, 0]]
  const uv = (p: Vec3): V2 => [p[0] / size, 1 - p[1] / size]
  return { tris: [[P[0], P[1], P[2]], [P[0], P[2], P[3]]], uvs: [[uv(P[0]), uv(P[1]), uv(P[2])], [uv(P[0]), uv(P[2]), uv(P[3])]] }
}
function verts(a: number[]): { p: Vec3; n: Vec3; uv: V2 }[] {
  const o: { p: Vec3; n: Vec3; uv: V2 }[] = []
  for (let i = 0; i < a.length; i += RELIEF_FLOATS) o.push({ p: [a[i], a[i + 1], a[i + 2]], n: [a[i + 3], a[i + 4], a[i + 5]], uv: [a[i + 6], a[i + 7]] })
  return o
}
function run(size: number, w: number, h: number, kind: Parameters<typeof buildPreset>[0], tile = size): number[] {
  const r = buildPreset(kind, tile, tile)
  const q = quad(size, w, h), out: number[] = []
  q.tris.forEach((t, i) => { const g = reliefTriangle(t, q.uvs[i], [0, 0, 1], r); expect(g).not.toBeNull(); out.push(...g!) })
  return out
}
const areaXY = (a: number[]): number => {
  const v = verts(a); let s = 0
  for (let i = 0; i < v.length; i += 3) s += ((v[i + 1].p[0] - v[i].p[0]) * (v[i + 2].p[1] - v[i].p[1]) - (v[i + 2].p[0] - v[i].p[0]) * (v[i + 1].p[1] - v[i].p[1])) / 2
  return s
}

describe('relief on a face', () => {
  it('a recessed panel sinks into the wall and still covers the whole face', () => {
    const out = run(1, 1, 1, 'recess')
    const v = verts(out)
    expect(v.length % 3).toBe(0)
    expect(Math.min(...v.map((x) => x.p[2]))).toBeLessThan(-0.01)
    expect(Math.max(...v.map((x) => x.p[2]))).toBeLessThanOrEqual(1e-9)
    // seen from the front, the front-facing triangles tile the face exactly once (walls have no area from the front)
    expect(areaXY(out)).toBeCloseTo(1, 5)
    for (const x of v) expect(Math.hypot(...x.n)).toBeCloseTo(1, 5)
  })
  it('the floor of the recess faces out, its walls face into the pit', () => {
    const v = verts(run(1, 1, 1, 'recess'))
    const floor = v.filter((x) => x.p[2] < -0.01 && x.n[2] > 0.99)
    expect(floor.length).toBeGreaterThan(0)
    // a wall vertex on the left side of the pit (x ~ 0.12) must have a normal pointing to +x, on the right side to -x
    const xwalls = v.filter((x) => Math.abs(x.n[2]) < 1e-6 && Math.abs(x.n[1]) < 1e-6 && x.p[2] < -1e-3)
    const left = xwalls.filter((x) => Math.abs(x.p[0] - 0.12) < 1e-6), right = xwalls.filter((x) => Math.abs(x.p[0] - 0.88) < 1e-6)
    expect(left.length).toBeGreaterThan(0); expect(right.length).toBeGreaterThan(0)
    expect(left.every((x) => x.n[0] > 0.99)).toBe(true)
    expect(right.every((x) => x.n[0] < -0.99)).toBe(true)
    const ywalls = v.filter((x) => Math.abs(x.n[2]) < 1e-6 && Math.abs(x.n[0]) < 1e-6 && x.p[2] < -1e-3)
    expect(ywalls.filter((x) => Math.abs(x.p[1] - 0.12) < 1e-6).every((x) => x.n[1] > 0.99)).toBe(true)
    expect(ywalls.filter((x) => Math.abs(x.p[1] - 0.88) < 1e-6).every((x) => x.n[1] < -0.99)).toBe(true)
  })
  it('a raised panel stands out along the normal', () => {
    const v = verts(run(1, 1, 1, 'raise'))
    expect(Math.max(...v.map((x) => x.p[2]))).toBeGreaterThan(0.01)
    expect(Math.min(...v.map((x) => x.p[2]))).toBeGreaterThanOrEqual(-1e-9)
  })
  it('repeats once per UV repeat and is cut at the face border', () => {
    const one = run(1, 1, 1, 'recess'), many = run(0.5, 1.25, 1, 'recess', 0.5)
    expect(areaXY(many)).toBeCloseTo(1.25, 5)
    expect(many.length).toBeGreaterThan(one.length)
    const v = verts(many)
    for (const x of v) { expect(x.p[0]).toBeGreaterThanOrEqual(-1e-9); expect(x.p[0]).toBeLessThanOrEqual(1.25 + 1e-9); expect(x.p[1]).toBeGreaterThanOrEqual(-1e-9); expect(x.p[1]).toBeLessThanOrEqual(1 + 1e-9) }
  })
  it('every vertex carries the picture uv (position / tile) for the front faces', () => {
    const v = verts(run(1, 1, 1, 'recess')).filter((x) => x.n[2] > 0.99)
    for (const x of v) { expect(x.uv[0]).toBeCloseTo(x.p[0], 5); expect(x.uv[1]).toBeCloseTo(1 - x.p[1], 5) }
  })
  it('a flat relief, degenerate uv, too many repeats and an empty budget stay flat (null)', () => {
    const q = quad(1)
    const flat = reliefTriangle(q.tris[0], q.uvs[0], [0, 0, 1], flatRelief())
    expect(flat).not.toBeNull() // a flat relief is legal input: it just reproduces the face
    expect(areaXY(flat!)).toBeCloseTo(0.5, 6)
    expect(reliefTriangle(q.tris[0], [[0, 0], [1, 1], [2, 2]], [0, 0, 1], buildPreset('recess', 1, 1))).toBeNull()
    const huge = quad(1 / Math.sqrt(MAX_RELIEF_CELLS * 4), 1, 1)
    expect(reliefTriangle(huge.tris[0], huge.uvs[0], [0, 0, 1], buildPreset('recess', 1, 1))).toBeNull()
    expect(reliefTriangle(q.tris[0], q.uvs[0], [0, 0, 1], buildPreset('window', 1, 1), newBudget(1))).toBeNull()
  })
  it('a mirrored projection keeps the walls facing into the pit and the floor facing out', () => {
    const r = buildPreset('recess', 1, 1)
    const P: Vec3[] = [[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0]]
    // u runs the other way (a mirrored texture): the UV handedness flips, the wall faces must not
    const tris: [number, number, number][] = [[0, 1, 2], [0, 2, 3]]
    for (const mirror of [false, true]) {
      const uv = (p: Vec3): V2 => [mirror ? 1 - p[0] : p[0], 1 - p[1]]
      const v = tris.flatMap((t) => verts(reliefTriangle([P[t[0]], P[t[1]], P[t[2]]], [uv(P[t[0]]), uv(P[t[1]]), uv(P[t[2]])], [0, 0, 1], r)!))
      const xw = v.filter((x) => Math.abs(x.n[2]) < 1e-6 && Math.abs(x.n[1]) < 1e-6 && x.p[2] < -1e-3)
      expect(xw.length).toBeGreaterThan(0)
      expect(xw.filter((x) => Math.abs(x.p[0] - 0.12) < 1e-6).every((x) => x.n[0] > 0.99)).toBe(true)
      expect(v.filter((x) => x.p[2] < -0.07 && Math.abs(x.n[2]) > 0.5).every((x) => x.n[2] > 0.99)).toBe(true)
    }
  })
  it('relief of a texture comes from the texture environment and ignores flat ones', () => {
    texEnv.setProvider((path) => (path === 'a' ? { path, width: 64, height: 64, ppm: 64, props: { ...defaultProps(), relief: buildPreset('window', 1, 1) } } : path === 'b' ? { path, width: 64, height: 64, ppm: 64, props: { ...defaultProps(), relief: flatRelief() } } : undefined))
    expect(reliefOf('a')).not.toBeNull()
    expect(reliefOf('b')).toBeNull()
    expect(reliefOf('missing')).toBeNull()
  })
  afterEach(() => texEnv.setProvider(null))
})
