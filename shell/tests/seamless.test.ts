import { describe, expect, it } from 'vitest'
import { edgesWrap, lineDifference } from '../src/model/seamless'

const line = (n: number, f: (i: number) => [number, number, number]): number[] => Array.from({ length: n }, (_, i) => [...f(i), 255]).flat()
const noise = (seed: number) => { let s = seed; return () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 4294967296 } }

describe('seamless detection', () => {
  it('line difference is the mean absolute RGB difference', () => {
    expect(lineDifference(line(4, () => [10, 10, 10]), line(4, () => [20, 30, 10]))).toBeCloseTo(10, 9)
    expect(lineDifference([], [])).toBe(0)
  })
  it('a flat colour wraps', () => {
    const l = line(8, () => [90, 90, 90])
    expect(edgesWrap({ first: l, last: l, interior: [[l, l]] })).toBe(true)
  })
  it('a dark last row on a bright texture does not wrap (the black line)', () => {
    const r = noise(1)
    const bright = () => line(32, () => { const v = 120 + r() * 20; return [v, v, v] })
    expect(edgesWrap({ first: bright(), last: line(32, () => [20, 20, 20]), interior: [[bright(), bright()], [bright(), bright()]] })).toBe(false)
  })
  it('a smoothly continuing pattern wraps; the same pattern cut at a different phase does not', () => {
    const row = (y: number) => line(32, (x) => { const v = 128 + 100 * Math.sin((y / 16) * Math.PI * 2 + x * 0.1); return [v, v, v] })
    const N = 16
    const adj = [[row(3), row(4)], [row(8), row(9)]] as [number[], number[]][]
    expect(edgesWrap({ first: row(0), last: row(N - 1), interior: adj })).toBe(true)
    expect(edgesWrap({ first: row(0), last: row(N / 2 - 1), interior: adj })).toBe(false)
  })
  it('no interior information: assume it wraps', () => {
    expect(edgesWrap({ first: [1, 2, 3, 255], last: [200, 200, 200, 255], interior: [] })).toBe(true)
  })
})
