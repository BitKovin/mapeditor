import { describe, expect, it } from 'vitest'
import type { MapDocument } from '../src/model/document'
import { CORE_FLOATS, affectedTriangles, changedBoxes, changedLights, collectPredScene, mayShadow, predictorMesh, sameLight, staleBoxes, triInfo } from '../src/render/directLightPlan'
import { buildPlainMesh } from '../src/render/litStale'
import type { PredLight } from '../src/render/directLightPlan'
import { buildReceivers } from '../src/render/directLight'

const point = (id: string, pos: [number, number, number], extra: Partial<PredLight> = {}): PredLight => ({
  id, kind: 'point', position: pos, forward: [0, 0, 1], color: [10, 10, 10], radius: 20, cosInner: 1, cosOuter: 0.8, size: 0.25, castShadows: true, ...extra,
})
const sun = (id: string, forward: [number, number, number]): PredLight => point(id, [0, 0, 0], { kind: 'directional', forward, radius: 1e6, size: 0.5 })

/** a lit mesh of triangles: each triangle 3 vertices x 12 floats (position, normal, lightmap u, v, page, mask) */
function mesh(tris: { p: [number, number, number][]; uv?: [number, number][]; page?: number }[]): Float32Array {
  const out: number[] = []
  for (const t of tris)
    t.p.forEach((p, k) => { const uv = t.uv?.[k] ?? [0, 0]; out.push(p[0], p[1], p[2], 0, 1, 0, uv[0], uv[1], t.page ?? 0, 0, 0, 0) })
  return Float32Array.from(out)
}

describe('collectPredScene', () => {
  const world = (entities: any[]) => ({ doc: { entities } as unknown as MapDocument, prefabs: [] })
  it('takes the level brightness, the ambient and the softening defaults like the Core does', () => {
    const w = world([
      { id: 'w', classname: 'worldspawn', origin: [0, 0, 0], angles: [0, 0, 0], brushes: [], keyvalues: { _light_scale: '0.5', _ambient: '0.1 0.2 0.3' } },
      { id: 'p', classname: 'light_point', origin: [1, 2, 3], angles: [0, 0, 0], brushes: [], keyvalues: { brightness: '4', color: '1 0.5 0', radius: '12' } },
      { id: 's', classname: 'light_directional', origin: [0, 0, 0], angles: [90, 0, 0], brushes: [], keyvalues: { brightness: '2' } },
      { id: 'k', classname: 'light_sky', origin: [0, 0, 0], angles: [0, 0, 0], brushes: [], keyvalues: {} },
      { id: 'r', classname: 'light_rect', origin: [0, 0, 0], angles: [0, 0, 0], brushes: [], keyvalues: {} },
      { id: 'st', classname: 'light_point', origin: [0, 0, 0], angles: [0, 0, 0], brushes: [], keyvalues: { style: '3' } },
    ])
    const s = collectPredScene(w, [], { sourceSize: 0.25, sunSize: 0.5 })
    expect(s.ambient).toEqual([0.1, 0.2, 0.3])
    expect(s.lights.map((l) => l.id)).toEqual(['p', 's']) // sky and rect lights stay the Core's, a styled light is another layer
    const p = s.lights[0]
    expect(p.color).toEqual([2, 1, 0])
    expect(p.position).toEqual([1, 2, 3])
    expect(p.size).toBe(0.25)
    expect(s.lights[1].kind).toBe('directional')
    expect(s.lights[1].size).toBe(0.5)
    expect(s.lights[1].color).toEqual([1, 1, 1])
  })
  it('keeps an authored source size', () => {
    const w = world([{ id: 'p', classname: 'light_point', origin: [0, 0, 0], angles: [0, 0, 0], brushes: [], keyvalues: { source_size: '1.5' } }])
    expect(collectPredScene(w, [], { sourceSize: 0.25, sunSize: 0.5 }).lights[0].size).toBe(1.5)
  })
})

describe('changes', () => {
  it('finds added, removed and modified lights, both versions of a modified one', () => {
    const a = [point('a', [0, 0, 0]), point('b', [5, 0, 0])]
    const b = [point('a', [0, 0, 0]), point('b', [6, 0, 0]), point('c', [9, 0, 0])]
    const c = changedLights(a, b)
    expect(c.map((l) => `${l.id}@${l.position[0]}`).sort()).toEqual(['b@5', 'b@6', 'c@9'])
    expect(changedLights(a, a)).toEqual([])
    expect(sameLight(a[0], point('a', [0, 0, 0]))).toBe(true)
  })
  it('a brush that moved changes the boxes of its old pieces and its new place; one that did not move changes nothing', () => {
    const m = mesh([{ p: [[0, 0, 0], [1, 0, 0], [0, 0, 1]] }])
    const faces = [{ i: '', e: 'w', bi: '', b: 'b1', f: 0, t: 'x', n: 1 }]
    const analysis = (x: number) => ({ stale: new Set(['|b1']), overlay: [{ texture: 'x', vertexCount: 3, brush: '|b1', uv: [0, 0, 0, 0, 0, 0], core: [x, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, x + 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, x, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0] }] })
    const prev = staleBoxes(m, faces, analysis(5))
    expect(changedBoxes(prev, staleBoxes(m, faces, analysis(5)))).toEqual([])
    const moved = changedBoxes(prev, staleBoxes(m, faces, analysis(8)))
    expect(moved.length).toBe(3) // where the Core's piece is, the overlay before and the overlay after (the piece's box is the same in both states)
    expect(moved.some((b) => b.mn[0] === 8)).toBe(true)
    expect(moved.some((b) => b.mn[0] === 5)).toBe(true)
    expect(moved.some((b) => b.mn[0] === 0)).toBe(true)
    // back to where the Core has it: the old overlay place counts, the brush is not stale any more
    expect(changedBoxes(prev, staleBoxes(m, faces, null)).length).toBeGreaterThan(0)
  })
})

describe('predictorMesh', () => {
  it('equals the plain mesh the Lighting-only view draws: the Core mesh without the stale brushes, then their current faces', () => {
    const m = mesh([
      { p: [[0, 0, 0], [1, 0, 0], [0, 0, 1]] }, { p: [[0, 1, 0], [1, 1, 0], [0, 1, 1]] }, { p: [[5, 0, 0], [6, 0, 0], [5, 0, 1]] }, { p: [[7, 0, 0], [8, 0, 0], [7, 0, 1]] },
    ])
    const faces = [
      { i: '', e: 'w', bi: '', b: 'a', f: 0, t: 'x', n: 1 }, { i: '', e: 'w', bi: '', b: 'b', f: 0, t: 'x', n: 2 }, { i: '', e: 'w', bi: '', b: 'c', f: 0, t: 'x', n: 1 },
    ]
    const overlay = [{ texture: 'x', vertexCount: 3, brush: '|b', uv: [0, 0, 0, 0, 0, 0], core: Array.from({ length: 36 }, (_, i) => 100 + i) }]
    const a = { stale: new Set(['|b']), overlay }
    const got = predictorMesh(m, faces, a)
    expect(Array.from(got.mesh)).toEqual(Array.from(buildPlainMesh(m, faces, a)))
    expect(got.overlayTriangles).toBe(1)
    expect(predictorMesh(m, faces, null).mesh).toBe(m)
  })
})

describe('shadow reach', () => {
  it('a point light is shadowed by a box on the segment, not by one behind the light or far beside it', () => {
    const l = point('a', [0, 5, 0])
    expect(mayShadow(l, 0, 2.5, 0, 0.5, 0, 0, 0, 0.1)).toBe(true) // between light and receiver
    expect(mayShadow(l, 0, 9, 0, 0.5, 0, 0, 0, 0.1)).toBe(false)  // behind the light
    expect(mayShadow(l, 8, 2.5, 0, 0.5, 0, 0, 0, 0.1)).toBe(false) // beside the segment
    expect(mayShadow(l, 0, -3, 0, 0.5, 0, 0, 0, 0.1)).toBe(false)  // behind the receiver
  })
  it('a sun is shadowed by what is above the receiver along its direction', () => {
    const s = sun('s', [0, -1, 0])
    expect(mayShadow(s, 0, 40, 0, 1, 0, 0, 0, 0.1)).toBe(true)
    expect(mayShadow(s, 3, 40, 0, 1, 0, 0, 0, 0.1)).toBe(false)
    expect(mayShadow(s, 0, -4, 0, 1, 0, 0, 0, 0.1)).toBe(false)
  })
  it('affected triangles: those in the shadow of a changed box, those in reach of a changed light, and every forced one', () => {
    const m = mesh([
      { p: [[0, 0, 0], [1, 0, 0], [0, 0, 1]] },        // under the box
      { p: [[30, 0, 0], [31, 0, 0], [30, 0, 1]] },     // far beside it
      { p: [[0, 0, 40], [1, 0, 40], [0, 0, 41]] },     // out of reach of the point light, in the sun's shadow of nothing
      { p: [[50, 5, 5], [51, 5, 5], [50, 5, 6]] },     // overlay
    ])
    const info = triInfo(m)
    const light = point('l', [0, 6, 0], { radius: 10 })
    const box = { mn: [-0.5, 2, -0.5] as [number, number, number], mx: [0.5, 3, 0.5] as [number, number, number] }
    const hit = affectedTriangles(info, { boxes: [box], lights: [] }, [light], 3)
    expect([...hit]).toEqual([0, 3])
    const moved = affectedTriangles(info, { boxes: [], lights: [light] }, [light], 99)
    expect([...moved]).toEqual([0]) // only the first is within 10 m of the light
  })
})

describe('buildReceivers', () => {
  it('lays out position, normal and lightmap uv of every vertex, and the rectangle they cover', () => {
    const atlas = 64
    const m = mesh([{ p: [[0, 0, 0], [8, 0, 0], [0, 0, 8]], uv: [[10 / atlas, 10 / atlas], [18 / atlas, 10 / atlas], [10 / atlas, 18 / atlas]], page: 2 }])
    const r = buildReceivers(m, [0], atlas)
    expect(r.count).toBe(3)
    const v = (i: number) => Array.from(r.data.subarray(i * 8, i * 8 + 8))
    expect(v(0).slice(0, 3)).toEqual([0, 0, 0])
    expect(v(1).slice(0, 3)).toEqual([8, 0, 0])
    expect(v(0).slice(3, 6)).toEqual([0, 1, 0])
    expect(v(0)[6]).toBeCloseTo(10 / atlas)
    expect(v(2)[7]).toBeCloseTo(18 / atlas)
    // the rectangle covers the triangle with a margin
    expect(r.rect[0]).toBeLessThanOrEqual(7)
    expect(r.rect[0] + r.rect[2]).toBeGreaterThanOrEqual(21)
  })
  it('skips a triangle with no area in the lightmap', () => {
    const m = mesh([{ p: [[0, 0, 0], [1, 0, 0], [0, 0, 1]], uv: [[0.1, 0.1], [0.1, 0.1], [0.1, 0.1]] }])
    expect(buildReceivers(m, [0], 64).count).toBe(0)
  })
  it('the vertex stride matches the Core mesh', () => { expect(CORE_FLOATS).toBe(12) })
})
