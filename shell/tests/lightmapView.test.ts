import { describe, expect, it } from 'vitest'
import { chartOf, chartsOf, densityOf, pagePicture, texelOf } from '../src/model/lightmapView'
import { makeBox, faceNormal } from '../src/model/geometry'
import type { LitFaceMap } from '../src/net/protocol'

const toHalf = (x: number): number => { // enough for the values used here: 0, 0.5, 1, 2
  const t: Record<number, number> = { 0: 0x0000, 0.5: 0x3800, 1: 0x3c00, 2: 0x4000 }
  return t[x]
}
// Core sends page units (0..1); with a 256 texel page this map puts x metres on texels with 16 texels per metre, 10 texels in from the page edge
const A = 256
const map = (over: Partial<LitFaceMap> = {}): LitFaceMap => ({ u: [16 / A, 0, 0, 10 / A], v: [0, 16 / A, 0, 20 / A], pg: 2, k: [0, 0, 0], ...over })

describe('lightmap view model', () => {
  it('texel of a world point, and density = length of the gradients', () => {
    expect(texelOf(map(), [2, 3, 5], A)).toEqual([42, 68])
    expect(densityOf(map(), A)).toEqual([16, 16]); expect(densityOf(map({ u: [3 / A, 4 / A, 0, 0] }), A)[0]).toBe(5)
  })
  it('chart of a face: polygon in texels, bounds, page, density and the face\'s own scale', () => {
    const b = makeBox('b', [0, 0, 0], [4, 2, 1], 'dev/wall'), f = { ...b.faces.find((x) => faceNormal(b, x)[2] > 0.99)!, lightmapScale: 2 }
    const c = chartOf(b, f, map(), A)
    expect(c.page).toBe(2); expect(c.polygon).toHaveLength(4); expect(c.min).toEqual([10, 20]); expect(c.max).toEqual([74, 52]); expect(c.scale).toBe(2); expect(c.density).toEqual([16, 16])
  })
  it('chartsOf skips faces without light data and unknown ids', () => {
    const b = makeBox('b', [0, 0, 0], [1, 1, 1], 'dev/wall')
    const out = chartsOf(b, [b.faces[0].id, b.faces[1].id, 999], (id) => (id === b.faces[0].id || id === 999 ? map() : undefined), A)
    expect(out.map((c) => c.faceId)).toEqual([b.faces[0].id])
  })
  it('page picture: tone mapped, empty texels transparent, downsampled', () => {
    const atlas = 4, page = new Uint16Array(atlas * atlas * 4)
    // texel (0,0) = linear 1.0 with alpha; texel (2,0) = 0.5 with alpha; the rest empty
    page.set([toHalf(1), toHalf(1), toHalf(1), toHalf(1)], 0); page.set([toHalf(0.5), toHalf(0.5), toHalf(0.5), toHalf(1)], 8)
    const img = pagePicture(page, atlas, 2) // every 2nd texel: pixel (0,0) <- texel (0,0), pixel (1,0) <- texel (2,0)
    expect(img[3]).toBe(255); expect(img[0]).toBe(Math.round(255 * Math.pow(0.5, 1 / 2.2)))
    expect(img[7]).toBe(255); expect(img[4]).toBe(Math.round(255 * Math.pow(0.5 / 1.5, 1 / 2.2)))
    expect(img[11]).toBe(0) // an empty texel
  })
})
