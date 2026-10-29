import { describe, expect, it } from 'vitest'
import { LightvolStore, PROBE_BYTES } from '../src/render/lightvolStore'

/** A 2 x 1 x 1 grid, cell 2 m at the origin: probe 0 ambient 1 (valid), probe 1 ambient 0.5 (valid or not); direction up (lat 0). */
function store(secondValid = true): LightvolStore {
  const s = new LightvolStore()
  s.dims = [2, 1, 1]; s.cell = 2; s.origin = [0, 0, 0]
  s.bytes = new Uint8Array(2 * PROBE_BYTES)
  const set = (i: number, amb: number, valid: number) => { const b = i * PROBE_BYTES; for (let c = 0; c < 3; c++) s.bytes[b + c] = Math.round((amb / 2) * 255); s.bytes[b + 8] = valid }
  set(0, 1, 1); set(1, 0.5, secondValid ? 1 : 0)
  return s
}

describe('light volume sampling for models', () => {
  it('interpolates between the probes around a point', () => {
    const l = store().sampleAt([1, 0, 0])! // halfway between the two probes
    expect(l[0].s).toBe(0)
    expect(l[0].ambient[0]).toBeCloseTo(0.75, 1)
    expect(l[0].dir[1]).toBeCloseTo(1, 1) // lat 0 = straight up (no directed light: the fallback is up too)
  })
  it('leaves probes that were not baked out and renormalises', () => {
    const l = store(false).sampleAt([1.5, 0, 0])!
    expect(l[0].ambient[0]).toBeCloseTo(1, 1)
  })
  it('is null outside the grid or without probes', () => {
    expect(store().sampleAt([50, 50, 50])).toBeNull()
    expect(new LightvolStore().sampleAt([0, 0, 0])).toBeNull()
  })
})
