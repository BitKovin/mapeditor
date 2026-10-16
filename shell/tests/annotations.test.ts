import { describe, expect, it } from 'vitest'
import { entityAnnotations } from '../src/model/annotations'

const pts = (a: number[]) => { const o: number[][] = []; for (let i = 0; i < a.length; i += 3) o.push(a.slice(i, i + 3)); return o }

describe('entity annotations', () => {
  it('a decal is an oriented box (12 edges) plus an arrow, centred on its origin and sized by width/height/depth', () => {
    const a = entityAnnotations([{ id: 'd', classname: 'decal', origin: [1, 2, 3], keyvalues: { width: '4', height: '2', depth: '1' } }])
    expect(a.length / 6).toBe(12 + 3)
    const box = pts(a.slice(0, 72))
    const xs = box.map((p) => p[0]), ys = box.map((p) => p[1]), zs = box.map((p) => p[2])
    // angles 0: projects along -Y, width along X, height along Z, depth along Y
    expect(Math.min(...xs)).toBeCloseTo(-1); expect(Math.max(...xs)).toBeCloseTo(3)
    expect(Math.min(...ys)).toBeCloseTo(1.5); expect(Math.max(...ys)).toBeCloseTo(2.5)
    expect(Math.min(...zs)).toBeCloseTo(2); expect(Math.max(...zs)).toBeCloseTo(4)
  })
  it('the decal box turns with angles', () => {
    const a = entityAnnotations([{ id: 'd', classname: 'decal', origin: [0, 0, 0], angles: [0, 90, 0], keyvalues: { width: '4', height: '2', depth: '1' } }])
    const xs = pts(a.slice(0, 72)).map((p) => p[0]), zs = pts(a.slice(0, 72)).map((p) => p[2])
    expect(Math.max(...xs) - Math.min(...xs)).toBeCloseTo(2) // height now along X
    expect(Math.max(...zs) - Math.min(...zs)).toBeCloseTo(4) // width now along Z
  })
  it('point entities get no annotation here (their direction arrow is in arrows.ts)', () => {
    expect(entityAnnotations([{ id: 'p', classname: 'info_player_start', origin: [0, 0, 0], angles: [0, 90, 0] }])).toEqual([])
  })
  it('brush entities are skipped', () => {
    expect(entityAnnotations([{ id: 'b', classname: 'func_group', angles: [0, 90, 0], brushes: [{ id: 'x', vertices: [], faces: [] }] }])).toEqual([])
  })
})
