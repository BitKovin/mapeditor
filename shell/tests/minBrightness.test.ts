import { describe, expect, it } from 'vitest'
import { minBrightness } from '../src/render/modelRenderer'

describe("the engine's minimum brightness (StaticMesh EnsureMinBrightness)", () => {
  it('leaves light that is bright enough alone', () => expect(minBrightness([0.5, 0.5, 0.5], 0.03)).toEqual([0.5, 0.5, 0.5]))
  it('black becomes grey at the minimum', () => expect(minBrightness([0, 0, 0], 0.03)).toEqual([0.03, 0.03, 0.03]))
  it('dim light is scaled up, hue kept, to the minimum luminance', () => {
    const c = minBrightness([0.01, 0.005, 0], 0.03)
    expect(c[0] / c[1]).toBeCloseTo(2, 5)
    expect(c[0] * 0.2126 + c[1] * 0.7152).toBeCloseTo(0.03, 5)
  })
})
