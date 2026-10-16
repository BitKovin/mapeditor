import { describe, expect, it } from 'vitest'
import { clipBy, clipBrush, hollowBrush, hullBrush, planeFromPoints, snapBrush } from '../src/model/brushOps'
import { brushBounds, makeBox, signedVolume } from '../src/model/geometry'
import { isProperSolid } from '../src/model/meshOps'

const box = () => makeBox('b', [0, 0, 0], [4, 4, 4])
const near = (a: number, b: number, e = 1e-9) => expect(Math.abs(a - b)).toBeLessThan(e)

describe('clipBrush', () => {
  it('cuts a box in half with an axis plane; the cap uses a new face id', () => {
    const r = clipBrush(box(), { n: [1, 0, 0], d: 1 })!
    near(signedVolume(r), 16); expect(isProperSolid(r)).toBe(true)
    expect(r.faces.length).toBe(6)
    expect(new Set(r.faces.map((f) => f.id)).size).toBe(6)
    near(brushBounds(r)!.max[0], 1)
  })
  it('a diagonal cut makes a wedge with the right volume', () => {
    const r = clipBrush(box(), { n: [0.7071067811865476, 0.7071067811865476, 0], d: 0.7071067811865476 * 4 })!
    near(signedVolume(r), 32, 1e-9); expect(isProperSolid(r)).toBe(true)
  })
  it('returns the brush untouched when the plane misses it, null when nothing is left', () => {
    const b = box()
    expect(clipBrush(b, { n: [1, 0, 0], d: 9 })).toBe(b)
    expect(clipBrush(b, { n: [1, 0, 0], d: -1 })).toBeNull()
  })
  it('keeps uvs continuous across the cut', () => {
    const b = box()
    b.faces[5] = { ...b.faces[5], uvs: b.faces[5].loop.map((i) => [b.vertices[i][0], b.vertices[i][2]] as [number, number]) } // +Y: u=x, v=z
    const r = clipBrush(b, { n: [1, 0, 0], d: 1.5 })!
    const f = r.faces.find((x) => x.uvs)!
    f.uvs!.forEach((uv, k) => { const p = r.vertices[f.loop[k]]; near(uv[0], p[0]); near(uv[1], p[2]) })
  })
  it('split keeps both halves and they add up', () => {
    const { back, front } = clipBy(box(), { n: [0, 0, 1], d: 1 }, 'both')
    near(signedVolume(back!) + signedVolume(front!), 64)
    near(signedVolume(back!), 16)
    expect(Object.keys(clipBy(box(), { n: [0, 0, 1], d: 1 }, 'front'))).toEqual(['front'])
  })
  it('planeFromPoints orients by the winding and rejects collinear points', () => {
    expect(planeFromPoints([0, 0, 0], [1, 0, 0], [0, 1, 0])!.n).toEqual([0, 0, 1])
    expect(planeFromPoints([0, 0, 0], [1, 0, 0], [2, 0, 0])).toBeNull()
  })
})

describe('hollowBrush', () => {
  it('a box becomes 6 non-overlapping walls whose volume is the shell', () => {
    const w = hollowBrush(box(), 1)!
    expect(w.length).toBe(6)
    near(w.reduce((s, x) => s + signedVolume(x), 0), 64 - 8, 1e-9)
    w.forEach((x) => expect(isProperSolid(x)).toBe(true))
  })
  it('refuses a wall as thick as the brush', () => { expect(hollowBrush(box(), 2)).toBeNull(); expect(hollowBrush(box(), 0)).toBeNull() })
  it('works on a wedge', () => {
    const wedge = clipBrush(box(), { n: [0.7071067811865476, 0.7071067811865476, 0], d: 0.7071067811865476 * 4 })!
    const w = hollowBrush(wedge, 0.5)!
    expect(w.length).toBeGreaterThanOrEqual(4)
    expect(w.reduce((s, x) => s + signedVolume(x), 0)).toBeLessThan(signedVolume(wedge))
  })
})

describe('hullBrush (merge)', () => {
  it('two touching boxes merge into one box', () => {
    const a = makeBox('a', [0, 0, 0], [2, 2, 2]), c = makeBox('c', [2, 0, 0], [4, 2, 2])
    const h = hullBrush([...a.vertices, ...c.vertices], 'dev/grey')!
    near(signedVolume(h), 16); expect(h.faces.length).toBe(6); expect(isProperSolid(h)).toBe(true)
  })
  it('refuses fewer than 4 points or a flat set', () => {
    expect(hullBrush([[0, 0, 0], [1, 0, 0], [0, 1, 0]])).toBeNull()
    expect(hullBrush([[0, 0, 0], [1, 0, 0], [0, 1, 0], [1, 1, 0]])).toBeNull()
  })
})

describe('snapBrush', () => {
  it('snaps a box to the grid', () => {
    const b = makeBox('b', [0.13, 0.02, -0.1], [3.9, 4.05, 4.2])
    const r = snapBrush(b, 0.5)!
    expect(r.vertices.every((v) => v.every((x) => Math.abs(x / 0.5 - Math.round(x / 0.5)) < 1e-9))).toBe(true)
  })
  it('refuses a snap that would collapse the brush', () => {
    expect(snapBrush(makeBox('b', [0, 0, 0], [0.1, 4, 4]), 1)).toBeNull()
  })
})

import { unionVolume } from '../src/model/brushOps'
import { makeBox as mb } from '../src/model/geometry'
describe('unionVolume', () => {
  it('counts overlap once', () => {
    expect(unionVolume([mb('a', [0, 0, 0], [2, 2, 2]), mb('b', [1, 0, 0], [3, 2, 2])])).toBeCloseTo(12)
    expect(unionVolume([mb('a', [0, 0, 0], [2, 2, 2]), mb('b', [1, 0, 0], [3, 2, 2]), mb('c', [1, 1, 0], [2, 3, 2])])).toBeCloseTo(14) // 12 + 4 - 2 - 2 + 2
  })
})
