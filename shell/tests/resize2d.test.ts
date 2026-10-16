import { describe, expect, it } from 'vitest'
import { handles2D, pixelsToMetres, resizeMap, screenAxis } from '../src/model/resize2d'
import { OrthoCamera } from '../src/render/camera'
import type { Vec3 } from '../src/model/math'

const near = (a: number[], b: number[]) => a.forEach((x, i) => expect(Math.abs(x - b[i])).toBeLessThan(1e-9))
const axes = (c: OrthoCamera) => { const b = c.basis; return { r: screenAxis(b.right), u: screenAxis(b.up) } }
const min: Vec3 = [0, 0, 0], max: Vec3 = [4, 2, 6]

describe('resize2d', () => {
  it('screen axes of the three views', () => {
    expect(axes(new OrthoCamera('top'))).toEqual({ r: { axis: 0, sign: 1 }, u: { axis: 2, sign: -1 } })
    expect(axes(new OrthoCamera('front'))).toEqual({ r: { axis: 0, sign: 1 }, u: { axis: 1, sign: 1 } })
    expect(axes(new OrthoCamera('side'))).toEqual({ r: { axis: 2, sign: -1 }, u: { axis: 1, sign: 1 } })
  })
  it('eight handles on the rectangle; top view: up = -Z so the top side is the min-Z side', () => {
    const { r, u } = axes(new OrthoCamera('top'))
    const hs = handles2D(min, max, r, u)
    expect(hs.length).toBe(8)
    near(hs.find((x) => x.id === '1,1')!.pos, [4, 1, 0])   // right, top of screen = min z
    near(hs.find((x) => x.id === '-1,-1')!.pos, [0, 1, 6])
    near(hs.find((x) => x.id === '0,1')!.pos, [2, 1, 0])
  })
  it('right-side handle stretches along X from the fixed left side, snapped', () => {
    const { r, u } = axes(new OrthoCamera('front'))
    const m = resizeMap(min, max, r, u, 1, 0, 1.1, 0, 0.5)
    near(m([4, 1, 3]), [5, 1, 3]) // side lands on 5.0 (4 + 1.1 snapped to 5.0)
    near(m([2, 1, 3]), [2.5, 1, 3]) // x scaled 5/4 about 0
    near(m([0, 1, 3]), [0, 1, 3])
  })
  it('corner handle resizes two axes; top view maps screen up to -Z', () => {
    const { r, u } = axes(new OrthoCamera('top'))
    const m = resizeMap(min, max, r, u, 1, 1, 0, 1, 1) // drag top-right corner up by 1 m: min-Z side moves to -1
    near(m([4, 0, 0]), [4, 0, -1]); near(m([0, 0, 6]), [0, 0, 6]); near(m([2, 5, 3]), [2, 5, 2.5])
  })
  it('refuses to invert: the dragged side stops at minSize', () => {
    const { r, u } = axes(new OrthoCamera('front'))
    const m = resizeMap(min, max, r, u, -1, 0, 10, 0, 0, 0.25) // left side dragged far past the right side
    near(m([0, 0, 0]), [3.75, 0, 0]); near(m([4, 0, 0]), [4, 0, 0])
  })
  it('side view: screen right is -Z, so the right handle moves the min-Z side', () => {
    const { r, u } = axes(new OrthoCamera('side'))
    const m = resizeMap(min, max, r, u, 1, 0, 2, 0, 0)
    near(m([0, 0, 0]), [0, 0, -2]); near(m([0, 0, 6]), [0, 0, 6])
  })
  it('pixel deltas: screen y is flipped', () => { expect(pixelsToMetres(10, 20, 0.1)).toEqual([1, -2]) })
})
