import { describe, expect, it } from 'vitest'
import { LightvolStore, PROBE_BYTES } from '../src/render/lightvolStore'
import { buildProbePreview, displayChannel, PREVIEW_STRIDE } from '../src/render/lightvolPreview'

const b64 = (bytes: number[]) => btoa(String.fromCharCode(...bytes))
/** A 2 x 1 x 2 grid: probes 0 and 3 baked (ambient 255 = 2.0 in the x2 range), probe 1 solid (invalid), probe 2 baked with a directed light up. */
function grid(): LightvolStore {
  const bytes = new Array(4 * PROBE_BYTES).fill(0)
  const set = (i: number, v: number[]) => v.forEach((x, k) => (bytes[i * PROBE_BYTES + k] = x))
  set(0, [128, 128, 128, 0, 0, 0, 0, 0, 1])
  set(2, [0, 0, 0, 255, 255, 255, 0, 0, 1])  // lat 0 = straight up (+Y), directed 2.0
  set(3, [64, 0, 0, 0, 0, 0, 0, 0, 1])
  const s = new LightvolStore()
  expect(s.applyGrid({ type: 'lightvol', rev: 1, origin: [10, 0, 20], cell: 2, dims: [2, 1, 2], probes: b64(bytes), layers: [] })).toBe(true)
  return s
}

describe('lightvol sample preview', () => {
  it('draws one sphere per baked probe at its grid position, skipping invalid probes', () => {
    const p = buildProbePreview(grid())
    expect(p.total).toBe(3)
    expect(p.shown).toBe(3)
    const V = p.tris.length / PREVIEW_STRIDE / 3  // vertices per sphere (80 triangles at level 1)
    expect(V).toBe(240)
    // the first sphere is centred on probe 0 = origin, the third (index 2) on origin + (0, 0, 1) cells
    const xs = [], zs = []
    for (let v = 0; v < V; v++) { xs.push(p.tris[v * PREVIEW_STRIDE]); zs.push(p.tris[v * PREVIEW_STRIDE + 2]) }
    expect((Math.min(...xs) + Math.max(...xs)) / 2).toBeCloseTo(10)
    expect((Math.min(...zs) + Math.max(...zs)) / 2).toBeCloseTo(20)
    const z2 = []
    for (let v = V; v < 2 * V; v++) z2.push(p.tris[v * PREVIEW_STRIDE + 2])
    expect((Math.min(...z2) + Math.max(...z2)) / 2).toBeCloseTo(22)  // probe 2 = (0, 0, 1): z = 20 + 1 * 2
  })

  it('shades a sphere smoothly by the light each normal receives and draws a line only where there is directed light', () => {
    const p = buildProbePreview(grid())
    const c0 = p.tris[3]  // red of the first vertex of probe 0: ambient 128 / 255 * 2 = 1.004 (no directed light, so every normal gets the same)
    expect(c0).toBeCloseTo(displayChannel(128 / 255 * 2, 1), 5)
    // vertices carry their unit normal; position = centre + normal * radius (cell 2 * 0.12)
    for (let v = 0; v < 240; v++) {
      const o = v * PREVIEW_STRIDE
      expect(Math.hypot(p.tris[o + 6], p.tris[o + 7], p.tris[o + 8])).toBeCloseTo(1, 5)
      expect(p.tris[o]).toBeCloseTo(10 + p.tris[o + 6] * 0.24, 5)
    }
    // probe 2 has a directed light straight up: its top is brighter than its bottom, with in-between values on the sides
    const green = (probe: number) => { const c: [number, number][] = []; for (let v = 0; v < 240; v++) { const o = (probe * 240 + v) * PREVIEW_STRIDE; c.push([p.tris[o + 7], p.tris[o + 4]]) } return c }
    const g = green(1), top = g.filter(([ny]) => ny > 0.99), bottom = g.filter(([ny]) => ny < -0.99)
    expect(top.length).toBeGreaterThan(0); expect(bottom.length).toBeGreaterThan(0)
    expect(top[0][1]).toBeGreaterThan(bottom[0][1])
    expect(g.some(([ny, c]) => Math.abs(ny) < 0.5 && c > bottom[0][1] && c < top[0][1])).toBe(true)
    // lines are not shaded (zero normal)
    expect(p.lines[6]).toBe(0)
    expect(p.lines.length).toBe(2 * PREVIEW_STRIDE)  // only probe 2 has directed light
    // the line leaves the probe straight up (+Y), by half a cell at full strength
    expect(p.lines[1]).toBeCloseTo(0)
    expect(p.lines[PREVIEW_STRIDE + 1]).toBeCloseTo(1, 1)
    expect(p.lines[PREVIEW_STRIDE]).toBeCloseTo(10, 1)
  })

  it('exposure brightens the display colour and thins out when there are more probes than the cap', () => {
    const dim = buildProbePreview(grid(), 0.25).tris[3], bright = buildProbePreview(grid(), 1).tris[3]
    expect(dim).toBeLessThan(bright)
    const p = buildProbePreview(grid(), 1, 2)
    expect(p.shown).toBe(2)
    expect(p.total).toBe(3)
  })

  it('a grid with more probes than the cap shows the ones around the center at the grid spacing, every one of them', () => {
    // 40 x 40 x 1 baked probes, 1 m apart, the cap draws 30: a ball of about 3 cells around the center
    const n = 40 * 40, bytes = new Uint8Array(n * PROBE_BYTES)
    for (let i = 0; i < n; i++) { bytes[i * PROBE_BYTES] = 128; bytes[i * PROBE_BYTES + 8] = 1 }
    const s = new LightvolStore()
    expect(s.applyGrid({ type: 'lightvol', rev: 1, origin: [0, 0, 0], cell: 1, dims: [40, 40, 1], probes: b64(Array.from(bytes)), layers: [] })).toBe(true)
    const p = buildProbePreview(s, 1, 30, [30, 5, 0])
    expect(p.shown).toBeGreaterThan(5)
    expect(p.shown).toBeLessThanOrEqual(30)
    expect(p.total).toBe(n)
    // every drawn sphere is centred on a whole grid position within a few cells of the center (not spread over the grid)
    const V = p.tris.length / PREVIEW_STRIDE / p.shown
    for (let k = 0; k < p.shown; k++) {
      let sx = 0, sy = 0
      for (let v = 0; v < V; v++) { sx += p.tris[(k * V + v) * PREVIEW_STRIDE]; sy += p.tris[(k * V + v) * PREVIEW_STRIDE + 1] }
      const cx = sx / V, cy = sy / V
      expect(Math.abs(cx - Math.round(cx))).toBeLessThan(0.02)
      expect(Math.hypot(cx - 30, cy - 5)).toBeLessThanOrEqual(3.01)
    }
    // a center elsewhere shows other probes
    const q = buildProbePreview(s, 1, 30, [5, 30, 0])
    expect(q.tris[0]).not.toBe(p.tris[0])
  })

  it('an empty store gives an empty preview', () => {
    const p = buildProbePreview(new LightvolStore())
    expect(p.shown).toBe(0)
    expect(p.tris.length).toBe(0)
  })
})
