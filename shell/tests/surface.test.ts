import { afterEach, describe, expect, it } from 'vitest'
import { faceNormal, makeBox } from '../src/model/geometry'
import { defaultProps, texEnv } from '../src/model/texprops'
import { resetSurface, fitAxis, fitBoth, justify, matchGrid, mirrorAxis, projectAxial, shiftTexels, stretchOf, surfaceBasis, withShiftTexels, withStretch } from '../src/model/surface'
import { projectFace } from '../src/model/uv'

afterEach(() => texEnv.setProvider(null))
const load = () => texEnv.setProvider(() => ({ path: 't', width: 256, height: 128, ppm: 64, props: defaultProps() }))
const wall = () => {
  const b = makeBox('b', [0, 0, 0], [4, 3, 0.2], 't')
  return { b, f: b.faces.find((x) => faceNormal(b, x)[2] > 0.9)! }
}
const P = (o: Partial<{ scale: [number, number]; shift: [number, number]; rotate: number }> = {}) => ({ scale: [4, 2] as [number, number], shift: [0, 0] as [number, number], rotate: 0, ...o })

describe('surface inspector model', () => {
  it('basis: texels and natural size from the texture, 64 px / 1 m while it is not loaded', () => {
    expect(surfaceBasis(wall().f)).toEqual({ texels: [64, 64], natural: [1, 1] })
    load()
    expect(surfaceBasis(wall().f)).toEqual({ texels: [256, 128], natural: [4, 2] })
  })
  it('shift is shown in texels and set in texels', () => {
    const p = withShiftTexels(P(), 0, 64, [256, 128])
    expect(p.shift[0]).toBeCloseTo(0.25, 9)
    expect(shiftTexels(p, [256, 128])).toEqual([64, 0])
  })
  it('stretch is a multiplier of the natural size; zero is refused; mirror negates', () => {
    expect(stretchOf(P({ scale: [8, 1] }), [4, 2])).toEqual([2, 0.5])
    expect(withStretch(P(), 0, 2, [4, 2]).scale[0]).toBe(8)
    expect(withStretch(P(), 0, 0, [4, 2])).toEqual(P())
    expect(mirrorAxis(P(), 1).scale).toEqual([4, -2])
    expect(mirrorAxis(mirrorAxis(P(), 1), 1)).toEqual(P())
  })
  it('Fit / Width / Height: the texture covers the face the given number of times, edge to edge', () => {
    const { b, f } = wall()
    const p = fitBoth(b, f, P(), [1, 1])
    const uv = projectFace(b, f, p)
    const us = uv.map((q) => q[0]), vs = uv.map((q) => q[1])
    expect(Math.max(...us) - Math.min(...us)).toBeCloseTo(1, 6); expect(Math.min(...us)).toBeCloseTo(0, 6)
    expect(Math.max(...vs) - Math.min(...vs)).toBeCloseTo(1, 6); expect(Math.min(...vs)).toBeCloseTo(0, 6)
    const w = projectFace(b, f, fitAxis(b, f, P(), 0, 2))
    expect(Math.max(...w.map((q) => q[0])) - Math.min(...w.map((q) => q[0]))).toBeCloseTo(2, 6)
    expect(fitAxis(b, f, P(), 0).scale[1]).toBe(2) // the other axis is untouched
  })
  it('Fit keeps a mirrored axis mirrored and still covers the face', () => {
    const { b, f } = wall()
    const uv = projectFace(b, f, fitAxis(b, f, P({ scale: [-4, 2] }), 0, 1))
    const us = uv.map((q) => q[0])
    expect(Math.min(...us)).toBeCloseTo(0, 6); expect(Math.max(...us)).toBeCloseTo(1, 6)
  })
  it('Match Grid: whole grid cells per repeat, at least one, sign kept', () => {
    expect(matchGrid(P({ scale: [3.9, 0.3] }), 0.5).scale).toEqual([4, 0.5])
    expect(matchGrid(P({ scale: [-1.2, 2] }), 1).scale).toEqual([-1, 2])
    expect(matchGrid(P(), 0)).toEqual(P())
  })
  it('Project Axial clears rotation and shift, keeps the stretch', () => {
    expect(projectAxial(P({ shift: [0.3, 0.2], rotate: 30 }))).toEqual({ scale: [4, 2], shift: [0, 0], rotate: 0 })
  })

  it('Justify: each edge of the texture touches the same edge of the face; stretch kept; mirrored and rotated faces too', () => {
    const { b, f } = wall()
    const range = (p: ReturnType<typeof P>, k: 0 | 1) => { const v = projectFace(b, f, p).map((q) => q[k]); return [Math.min(...v), Math.max(...v)] }
    // 4 x 3 m face, 4 x 2 m repeats: u spans 1, v spans 1.5
    expect(range(justify(b, f, P({ shift: [0.3, 0.3] }), 'left'), 0)[0]).toBeCloseTo(0, 9)
    expect(range(justify(b, f, P(), 'right'), 0)[1]).toBeCloseTo(1, 9)
    expect(range(justify(b, f, P(), 'top'), 1)[0]).toBeCloseTo(0, 9)
    expect(range(justify(b, f, P(), 'bottom'), 1)[1]).toBeCloseTo(1, 9)
    const c = justify(b, f, P(), 'center')
    expect((range(c, 0)[0] + range(c, 0)[1]) / 2).toBeCloseTo(0.5, 9)
    expect((range(c, 1)[0] + range(c, 1)[1]) / 2).toBeCloseTo(0.5, 9)
    expect(c.scale).toEqual([4, 2])
    const m = justify(b, f, P({ scale: [-4, 2] }), 'left')
    expect(range(m, 0)[0]).toBeCloseTo(0, 9)
    const r = justify(b, f, P({ rotate: 30 }), 'top')
    expect(range(r, 1)[0]).toBeCloseTo(0, 9)
  })
})

describe('bottom-left origin', () => {
  it('Reset puts the texture bottom-left on the face bottom-left, whatever the face size', () => {
    const { b, f } = wall()
    const p = resetSurface(b, f)
    const uv = projectFace(b, f, p)
    expect(Math.min(...uv.map((q) => q[0]))).toBeCloseTo(0, 9)
    expect(Math.max(...uv.map((q) => q[1]))).toBeCloseTo(1, 9)
  })
  it('a fractional repeat count leaves the partial repeat at the TOP, not at the bottom', () => {
    const { b, f } = wall() // 4 x 3 m
    const uv = projectFace(b, f, fitAxis(b, f, P(), 1, 2.5))
    const vs = uv.map((q) => q[1])
    expect(Math.max(...vs)).toBeCloseTo(1, 9)        // bottom edge of the face = bottom edge of the texture
    expect(Math.min(...vs)).toBeCloseTo(1 - 2.5, 9)  // 2.5 repeats upwards
  })
})

import { alignBrush, withBottomLeftDefault } from '../src/model/surface'
describe('baked bottom-left default', () => {
  it('a new face of a known texture gets its projection at the face bottom-left; an unknown texture stays lazy; an explicit projection is kept', () => {
    const { b, f } = wall()
    expect(withBottomLeftDefault(b, f)).toBe(f) // texture not loaded
    load()
    const g = withBottomLeftDefault(b, f)
    expect((g.projection as { scale: number[] }).scale).toEqual([4, 2])
    const uv = g.uvs!
    expect(Math.min(...uv.map((q) => q[0]))).toBeCloseTo(0, 9); expect(Math.max(...uv.map((q) => q[1]))).toBeCloseTo(1, 9)
    const own = { ...f, projection: { scale: [1, 1] as [number, number], shift: [0.3, 0.3] as [number, number], rotate: 0 } }
    expect(withBottomLeftDefault(b, own)).toBe(own)
    expect(alignBrush(b).faces.every((x) => x.projection)).toBe(true)
  })
})

import { projectCam, projectOrtho, withShear } from '../src/model/surface'
import { projectionOf, placedCoords } from '../src/model/uv'
describe('projection kinds and shear (B4)', () => {
  const spread = (uv: [number, number][], k: 0 | 1) => Math.max(...uv.map((q) => q[k])) - Math.min(...uv.map((q) => q[k]))

  it('Ortho equals Axial (up to the shift) on an axis-aligned wall, floor and ceiling', () => {
    const b = makeBox('b', [0, 0, 0], [4, 3, 2], 't')
    for (const f of b.faces) {
      const axial = projectFace(b, f, P({ scale: [2, 2] }))
      const o = projectOrtho(b, f, P({ scale: [2, 2] }))
      const ortho = projectFace(b, f, o)
      for (let i = 1; i < axial.length; i++) {
        expect(ortho[i][0] - ortho[0][0]).toBeCloseTo(axial[i][0] - axial[0][0], 9)
        expect(ortho[i][1] - ortho[0][1]).toBeCloseTo(axial[i][1] - axial[0][1], 9)
      }
      expect(o.kind).toBe('ortho')
    }
  })
  it('on a sloped face Ortho does not stretch: v measures the length along the slope', () => {
    const b0 = makeBox('b', [0, 0, 0], [4, 2, 1], 't')
    const b = { ...b0, vertices: b0.vertices.map((v) => (v[1] > 1 && v[2] > 0.5 ? ([v[0], v[1], v[2] + 2] as [number, number, number]) : v)) }
    const f = b.faces.find((x) => { const n = faceNormal(b, x); return n[2] > 0.5 && n[1] < -0.2 || (n[2] > 0.5 && Math.abs(n[1]) > 0.2) })!
    const n = faceNormal(b, f)
    expect(Math.abs(n[1])).toBeGreaterThan(0.2) // really sloped
    const ortho = projectFace(b, f, projectOrtho(b, f, P({ scale: [1, 1] })))
    const axial = projectFace(b, f, P({ scale: [1, 1] }))
    const slope = Math.hypot(2, 2)
    expect(spread(ortho, 1)).toBeCloseTo(slope, 6)
    expect(spread(axial, 1)).toBeLessThan(slope - 0.1) // axial compresses it
    expect(spread(ortho, 0)).toBeCloseTo(4, 6)
  })
  it('Cam freezes the camera axes in the face plane and refuses a degenerate view', () => {
    const { b, f } = wall()
    const c = projectCam(b, f, P({ scale: [1, 1] }), { right: [0, 0, 1], up: [0, 1, 0] })
    expect(c).toBeNull() // right axis along the wall normal
    const r = projectCam(b, f, P({ scale: [1, 1] }), { right: [1, 0, 0], up: [0, 1, 0] })!
    expect(r.kind).toBe('cam')
    const uv = projectFace(b, f, r), ax = projectFace(b, f, P({ scale: [1, 1] }))
    expect(uv[1][0] - uv[0][0]).toBeCloseTo(ax[1][0] - ax[0][0], 9)
    // a camera rolled 90 degrees turns the texture
    const rolled = projectCam(b, f, P({ scale: [1, 1] }), { right: [0, 1, 0], up: [-1, 0, 0] })!
    const u2 = projectFace(b, f, rolled)
    expect(u2[1][0] - u2[0][0]).toBeCloseTo(0, 9)
  })
  it('stored kind and basis survive projectionOf; a broken basis falls back to axial', () => {
    const { f } = wall()
    const p = projectionOf({ ...f, projection: { scale: [1, 1], shift: [0, 0], rotate: 0, kind: 'ortho', basis: [[1, 0, 0], [0, -1, 0]] } as never })
    expect(p.kind).toBe('ortho'); expect(p.basis).toEqual([[1, 0, 0], [0, -1, 0]])
    expect(projectionOf({ ...f, projection: { scale: [1, 1], shift: [0, 0], rotate: 0, kind: 'ortho', basis: [[1, 0, 0], [2, 0, 0]] } as never }).kind).toBeUndefined()
    expect(projectionOf({ ...f, projection: { scale: [1, 1], shift: [0, 0], rotate: 0, kind: 'cam' } as never }).kind).toBeUndefined()
  })
  it('shear slants the texture: u += shear * v after the rotation; justify and fit still touch the edges', () => {
    const { b, f } = wall()
    const p = withShear(P({ scale: [1, 1] }), 0.5)
    const n = faceNormal(b, f)
    const [a0, c0] = placedCoords(n, b.vertices[f.loop[0]], P({ scale: [1, 1] }))
    const [a1, c1] = placedCoords(n, b.vertices[f.loop[0]], p)
    expect(a1).toBeCloseTo(a0 + 0.5 * c0, 9); expect(c1).toBeCloseTo(c0, 9)
    const j = justify(b, f, p, 'left')
    expect(Math.min(...projectFace(b, f, j).map((q) => q[0]))).toBeCloseTo(0, 9)
    const fit = fitBoth(b, f, p, [1, 1])
    const uv = projectFace(b, f, fit)
    expect(spread(uv, 0)).toBeCloseTo(1, 6); expect(spread(uv, 1)).toBeCloseTo(1, 6)
    expect(withShear(p, 0).shear).toBeUndefined()
  })
})
