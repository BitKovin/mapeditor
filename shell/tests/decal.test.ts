import { describe, expect, it } from 'vitest'
import { anglesForDirection, decalDirection } from '../src/model/decal'
import { normalize } from '../src/model/math'

describe('decal orientation', () => {
  it('zero angles project straight down', () => {
    const d = decalDirection([0, 0, 0])
    expect(d[0]).toBeCloseTo(0); expect(d[1]).toBeCloseTo(-1); expect(d[2]).toBeCloseTo(0)
  })
  it('matches the conventions Core tests: pitch 90 projects along -Z, then yaw 90 along -X', () => {
    const a = decalDirection([90, 0, 0]); expect(a[2]).toBeCloseTo(-1)
    const b = decalDirection([90, 90, 0]); expect(b[0]).toBeCloseTo(-1)
  })
  it('anglesForDirection round-trips for arbitrary directions, including straight up and down', () => {
    for (const d of [[0, -1, 0], [0, 1, 0], [1, 0, 0], [-1, 0, 0], [0, 0, 1], [0, 0, -1], [0.3, -0.5, 0.8], [-0.7, 0.2, -0.1]] as [number, number, number][]) {
      const n = normalize(d), back = decalDirection(anglesForDirection(n))
      for (let i = 0; i < 3; i++) expect(back[i]).toBeCloseTo(n[i], 6)
    }
  })
})
