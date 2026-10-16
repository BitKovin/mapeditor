import { describe, expect, it } from 'vitest'
import { LIT_VERTEX_FLOATS } from '../src/render/lightmapStore'
import { DENSITY_STRIDE, buildDensityMesh, densityColor, densityLegend, triangleDensity } from '../src/render/lightmapDensity'

/** One triangle per entry: three vertices [x, y, z, u, v] (u, v as fractions of the atlas). */
function mesh(tris: number[][][]): Float32Array {
  const m = new Float32Array(tris.length * 3 * LIT_VERTEX_FLOATS)
  tris.forEach((t, i) => t.forEach((v, k) => {
    const o = (i * 3 + k) * LIT_VERTEX_FLOATS
    m[o] = v[0]; m[o + 1] = v[1]; m[o + 2] = v[2]; m[o + 6] = v[3]; m[o + 7] = v[4]
  }))
  return m
}

describe('lightmap density overlay', () => {
  const atlas = 128
  // a 4 m x 4 m floor triangle mapped to 32 x 32 texels: 8 texels per metre
  const dense = [[0, 0, 0, 0, 0], [4, 0, 0, 32 / atlas, 0], [0, 0, 4, 0, 32 / atlas]]
  // the same size at 2 texels per metre
  const coarse = [[10, 0, 0, 0.5, 0.5], [14, 0, 0, 0.5 + 8 / atlas, 0.5], [10, 0, 4, 0.5, 0.5 + 8 / atlas]]

  it('measures texels per metre of a triangle', () => {
    const m = mesh([dense, coarse])
    expect(triangleDensity(m, 0, atlas)).toBeCloseTo(8, 5)
    expect(triangleDensity(m, 1, atlas)).toBeCloseTo(2, 5)
  })

  it('a degenerate triangle has no density', () => {
    const p = [1, 1, 1, 0.1, 0.1]
    expect(triangleDensity(mesh([[p, p, p]]), 0, atlas)).toBe(0)
  })

  it('builds the overlay buffer: position, lightmap texel position, density per vertex, and the range', () => {
    const d = buildDensityMesh(mesh([dense, coarse]), 2, atlas)
    expect(d.data.length).toBe(2 * 3 * DENSITY_STRIDE)
    expect(d.min).toBeCloseTo(2, 5)
    expect(d.max).toBeCloseTo(8, 5)
    expect(Array.from(d.data.slice(DENSITY_STRIDE, DENSITY_STRIDE + 6))).toEqual([4, 0, 0, 32, 0, expect.closeTo(8, 4)])
    expect(d.data[(3 + 1) * DENSITY_STRIDE + 3]).toBeCloseTo(0.5 * atlas + 8, 3)
    const empty = buildDensityMesh(new Float32Array(0), 0, atlas)
    expect([empty.min, empty.max]).toEqual([0, 0])
  })

  it('the colour ramp runs from blue over green to red and is grey for no density', () => {
    const lo = densityColor(1), mid = densityColor(8), hi = densityColor(64), beyond = densityColor(1000)
    expect(lo[2]).toBeGreaterThan(lo[0])           // blue
    expect(mid[1]).toBeGreaterThan(mid[0])          // green
    expect(mid[1]).toBeGreaterThan(mid[2])
    expect(hi[0]).toBeGreaterThan(hi[2])            // red
    expect(beyond).toEqual(hi)
    expect(densityColor(0.01)).toEqual(lo)
    expect(densityColor(0)).toEqual([0.5, 0.5, 0.5])
  })

  it('the legend names the range', () => {
    expect(densityLegend(2, 8)).toContain('2.0 to 8.0')
    expect(densityLegend(0, 0)).toContain('no lightmapped')
  })
})
