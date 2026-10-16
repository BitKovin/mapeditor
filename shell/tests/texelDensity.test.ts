import { afterEach, describe, expect, it } from 'vitest'
import { makeBox } from '../src/model/geometry'
import { faceNormal } from '../src/model/geometry'
import { HEAT_BUCKETS, HEAT_RANGE, bucketColor, faceDensity, faceHeat, heatBucket, heatColor, heatLabel } from '../src/model/texelDensity'
import { defaultProps, texEnv } from '../src/model/texprops'

afterEach(() => texEnv.setProvider(null))
const info = (ppm = 64) => ({ path: 't', width: 256, height: 256, ppm, props: defaultProps() })
const wall = (scale: [number, number]) => {
  const b = makeBox('b', [0, 0, 0], [4, 4, 0.2], 't')
  const f = { ...b.faces.find((x) => faceNormal(b, x)[2] > 0.9)!, projection: { scale, shift: [0, 0], rotate: 0 } }
  return { b, f }
}

describe('texel density heat map', () => {
  it('density is texture pixels per metre of surface', () => {
    const { b, f } = wall([4, 4])
    const [du, dv] = faceDensity(b, f, 256, 256)
    expect(du).toBeCloseTo(64, 6); expect(dv).toBeCloseTo(64, 6)
  })
  it('0 octaves when the face shows the texture at its authored density', () => {
    const { b, f } = wall([4, 4])
    expect(faceHeat(b, f, info())).toBeCloseTo(0, 6)
  })
  it('stretched is negative, squeezed positive, by octaves; the worse axis wins', () => {
    expect(faceHeat(wall([8, 8]).b, wall([8, 8]).f, info())).toBeCloseTo(-1, 6)
    expect(faceHeat(wall([2, 2]).b, wall([2, 2]).f, info())).toBeCloseTo(1, 6)
    const { b, f } = wall([4, 16])
    expect(faceHeat(b, f, info())).toBeCloseTo(-2, 6) // v is 4x stretched
  })
  it('follows pixels per meter: doubling it makes the same face look stretched', () => {
    const { b, f } = wall([4, 4])
    expect(faceHeat(b, f, info(128))).toBeCloseTo(-1, 6)
  })
  it('unknown texture: no value', () => {
    const { b, f } = wall([4, 4])
    expect(faceHeat(b, f, undefined)).toBeNull()
  })
  it('colours: green as authored, red stretched, blue dense; buckets are symmetric', () => {
    const g = heatColor(0), r = heatColor(-HEAT_RANGE), bl = heatColor(HEAT_RANGE)
    expect(g[1]).toBeGreaterThan(g[0]); expect(g[1]).toBeGreaterThan(g[2])
    expect(r[0]).toBeGreaterThan(r[1]); expect(bl[2]).toBeGreaterThan(bl[0])
    expect(heatBucket(0)).toBe((HEAT_BUCKETS - 1) / 2)
    expect(heatBucket(-99)).toBe(0); expect(heatBucket(99)).toBe(HEAT_BUCKETS - 1)
    expect(bucketColor(0)).toEqual(heatColor(-HEAT_RANGE))
  })
  it('labels', () => {
    expect(heatLabel(0)).toBe('as authored')
    expect(heatLabel(-1)).toContain('stretched 2.0x')
    expect(heatLabel(1)).toContain('squeezed 2.0x')
  })
})

import { defaultProjectionFor, projectionOf } from '../src/model/uv'
describe('natural size default (pixels per meter)', () => {
  it('a face without a projection shows the texture at pixels / ppm; an explicit scale is kept; unloaded = 2 m', () => {
    const { b, f } = wall([4, 4])
    const bare = { ...f, projection: undefined }
    expect(projectionOf(bare).scale).toEqual([2, 2]) // not loaded yet
    texEnv.setProvider(() => info(64))
    expect(projectionOf(bare).scale).toEqual([4, 4]) // 256 px / 64 ppm
    texEnv.setProvider(() => info(128))
    expect(defaultProjectionFor('t').scale).toEqual([2, 2])
    expect(projectionOf(f).scale).toEqual([4, 4]) // explicit, old maps do not change
    void b
  })
})
