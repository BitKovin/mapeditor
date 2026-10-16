import { describe, expect, it } from 'vitest'
import { FlyCamera, OrbitCamera, OrthoCamera } from '../src/render/camera'

const project = (c: OrthoCamera, p: [number, number, number], aspect = 2) => {
  const m = c.viewProj(aspect)
  return [m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13]]
}
const near = (a: number, b: number) => expect(Math.abs(a - b)).toBeLessThan(1e-6)

describe('OrthoCamera', () => {
  it('top view: +X is right, +Z is down, height is ignored', () => {
    const c = new OrthoCamera('top'); c.halfHeight = 4
    const [x, y] = project(c, [2, 100, 0]); near(x, 2 / 8); near(y, 0)
    const [, y2] = project(c, [0, 0, 2]); near(y2, -0.5)
  })
  it('front view: +X right, +Y up; side view: -Z right, +Y up', () => {
    const f = new OrthoCamera('front'); f.halfHeight = 2
    const [x, y] = project(f, [1, 1, 50]); near(x, 0.25); near(y, 0.5)
    const s = new OrthoCamera('side'); s.halfHeight = 2
    const [sx, sy] = project(s, [50, 1, -1]); near(sx, 0.25); near(sy, 0.5)
  })
  it('rays are parallel to the view direction and pass through the clicked point', () => {
    const c = new OrthoCamera('top'); c.halfHeight = 5
    const r = c.rayFromNdc(0.2, 0.5, 2) // x = 0.2*10 = 2, screen up = -Z -> z = -2.5
    near(r.dir[1], -1)
    const t = (3 - r.origin[1]) / r.dir[1]
    near(r.origin[0] + r.dir[0] * t, 2); near(r.origin[2] + r.dir[2] * t, -2.5)
  })
  it('pan moves the target so the content follows the cursor; zoom scales halfHeight', () => {
    const c = new OrthoCamera('front'); c.halfHeight = 5
    c.pan(100, 0, 500) // 500 px = 10 m, so 100 px = 2 m; the scene follows the cursor, so the target moves left
    near(c.target[0], -2)
    const h = c.halfHeight; c.zoom(100); expect(c.halfHeight).toBeGreaterThan(h)
  })
  it('frame fits the bounds; distanceTo is constant (gizmo size) and switching to perspective keeps the scale', () => {
    const c = new OrthoCamera('top'); c.aspect = 2
    c.frame([0, 0, 0], [20, 5, 10])
    near(c.target[0], 10); near(c.halfHeight, Math.max(5, 10 / 2) * 1.2)
    expect(c.distanceTo([1, 2, 3])).toBe(c.distanceTo([-9, 0, 9]))
    const fly = new FlyCamera(); fly.setFrom(c)
    near(fly.focusDistance, c.equivDistance)
    const orb = new OrbitCamera(); orb.setFrom(c)
    near(orb.distance, c.equivDistance); near(orb.pitch, 1.5) // clamped straight-down pitch
    const back = new OrthoCamera('front'); back.setFrom(orb)
    near(back.halfHeight, c.halfHeight)
  })
})

import { orthoGrid } from '../src/render/orthoGrid'
describe('orthoGrid', () => {
  it('lies in the view plane, covers the view and coarsens when zoomed out', () => {
    const c = new OrthoCamera('top'); c.halfHeight = 5
    const g = orthoGrid(c, 1, 500, 2)
    expect(g.step).toBe(1)
    for (let i = 1; i < g.minor.length; i += 3) expect(g.minor[i]).toBe(0) // y = 0 plane
    expect(g.axisH.length).toBe(6); expect(g.axisV.length).toBe(6)
    expect(g.major.length).toBeGreaterThan(0)
    c.halfHeight = 2000
    expect(orthoGrid(c, 1, 500, 2).step).toBeGreaterThan(1)
    const f = new OrthoCamera('front'); f.halfHeight = 5
    const gf = orthoGrid(f, 1, 500, 2)
    for (let i = 2; i < gf.minor.length; i += 3) expect(gf.minor[i]).toBe(0) // z = 0 plane
  })
})
