// Bridge Edge Loops (REQUIREMENTS §36 W2)
import { describe, expect, it } from 'vitest'
import { faceNormal, makeBox, signedVolume } from '../src/model/geometry'
import { bridgeLoops, deleteFaces, insetFaces, isError, isProperSolid } from '../src/model/meshOps'
import { emptySel } from '../src/model/meshTopology'
import type { Brush } from '../src/model/document'

const ok = (r: ReturnType<typeof bridgeLoops>): Brush => { if (isError(r)) throw new Error(r.error); return r.brush }
const faceAlong = (b: Brush, axis: number, sign: number) => b.faces.find((f) => faceNormal(b, f)[axis] * sign > 0.99)!.id

describe('bridge edge loops', () => {
  it('face mode: two inset faces on opposite sides become a tunnel through the brush', () => {
    let b = makeBox('b', [0, 0, 0], [2, 2, 2])
    const front = faceAlong(b, 2, 1), back = faceAlong(b, 2, -1)
    const ins = insetFaces(b, [front, back], 0.5)
    if (isError(ins)) throw new Error(ins.error)
    b = ins.brush
    const inner = ins.sel.faces
    expect(inner).toHaveLength(2)
    const r = bridgeLoops(b, { ...emptySel(), faces: inner }, 'face')
    const t = ok(r)
    expect(isProperSolid(t)).toBe(true)
    expect(signedVolume(t)).toBeCloseTo(8 - 1 * 1 * 2, 6) // the 1 x 1 tunnel, 2 long
    expect(t.faces.some((f) => inner.includes(f.id))).toBe(false)
    if (!isError(r)) expect(r.sel.faces).toHaveLength(4)
  })
  it('edge mode: two open holes are joined; unequal or single loops are refused', () => {
    let b = makeBox('b', [0, 0, 0], [2, 2, 2])
    const ins = insetFaces(b, [faceAlong(b, 0, 1), faceAlong(b, 0, -1)], 0.5)
    if (isError(ins)) throw new Error(ins.error)
    const holes = deleteFaces(ins.brush, ins.sel.faces)
    if (isError(holes)) throw new Error(holes.error)
    b = holes.brush
    const open: string[] = []
    for (const f of b.faces) f.loop.forEach((v, i) => { const w = f.loop[(i + 1) % f.loop.length]; if (!b.faces.some((g) => g.loop.some((x, j) => x === w && g.loop[(j + 1) % g.loop.length] === v))) open.push(v < w ? `${v}-${w}` : `${w}-${v}`) })
    expect(open).toHaveLength(8)
    const t = ok(bridgeLoops(b, { ...emptySel(), edges: open }, 'edge'))
    expect(isProperSolid(t)).toBe(true)
    expect(signedVolume(t)).toBeCloseTo(8 - 2, 6)
    expect(isError(bridgeLoops(b, { ...emptySel(), edges: open.slice(0, 4) }, 'edge'))).toBe(true)
    expect(isError(bridgeLoops(makeBox('c', [0, 0, 0], [1, 1, 1]), { ...emptySel(), faces: [0] }, 'face'))).toBe(true)
  })
})
