import { describe, expect, it } from 'vitest'
import type { Brush } from '../src/model/document'
import { makeBox, signedVolume, validateBrush } from '../src/model/geometry'
import { extrudeFaces, isError, mirrorVerts, OpResult } from '../src/model/meshOps'
import { MeshSel, edgeKey, isClosedManifold } from '../src/model/meshTopology'
import { connectVerts, knifeCut, knifePos, splitFace, subdivideEdges } from '../src/model/meshCut'
import { faceUvAt } from '../src/model/uvmesh'

const box = (): Brush => makeBox('b', [0, 0, 0], [2, 1, 1], 'wall/a')
// makeBox: vertices 0..3 at z=0 (x0y0, x1y0, x1y1, x0y1), 4..7 at z=1; faces 0 -Z (0,1,2,3 in some order), 1 +Z
function ok(r: OpResult): { brush: Brush; sel: MeshSel } {
  if (isError(r)) throw new Error(r.error)
  return r
}
function expectSolid(b: Brush): void {
  expect(validateBrush(b, true)).toBeNull() // still convex
  expect(isClosedManifold(b)).toBe(true)
  expect(signedVolume(b)).toBeCloseTo(2, 9) // nothing moved
  expect(new Set(b.faces.map((f) => f.id)).size).toBe(b.faces.length)
}
/** The UV of world point p on a face, through the face's affine UV map. */
function uvAt(b: Brush, faceId: number, p: [number, number, number]): [number, number] {
  return faceUvAt(b, b.faces.find((x) => x.id === faceId)!)(p)
}

describe('splitFace', () => {
  it('cuts a quad along its diagonal; skips chords that are already edges', () => {
    let id = 10
    const f = { id: 3, loop: [0, 1, 2, 3], texture: 't' } as never
    const r = splitFace(f, [[0, 1], [0, 2]], () => id++)
    expect(r.pieces.map((p) => p.loop)).toEqual([[0, 1, 2], [2, 3, 0]]); expect(r.pieces[0].id).toBe(3); expect(r.pieces[1].id).toBe(10)
    expect(r.made).toEqual([[0, 2]])
  })
})

describe('connect vertices (J)', () => {
  it('two opposite corners of the bottom face: the face becomes two triangles, the brush stays a convex solid', () => {
    const b = box()
    const r = ok(connectVerts(b, [0, 2]))
    expectSolid(r.brush)
    expect(r.brush.faces).toHaveLength(7)
    expect(r.sel.edges).toEqual([edgeKey(0, 2)])
  })
  it('the pieces keep the texture where it was', () => {
    const b = box(), bottom = b.faces.find((f) => f.loop.every((v) => b.vertices[v][2] === 0))!
    const before = uvAt(b, bottom.id, [2, 1, 0])
    const r = ok(connectVerts(b, [0, 2]))
    const piece = r.brush.faces.filter((f) => f.loop.every((v) => r.brush.vertices[v][2] === 0)).find((f) => f.loop.some((v) => r.brush.vertices[v][0] === 2 && r.brush.vertices[v][1] === 1))!
    const after = uvAt(r.brush, piece.id, [2, 1, 0])
    expect(after[0]).toBeCloseTo(before[0], 9); expect(after[1]).toBeCloseTo(before[1], 9)
  })
  it('neighbours or no shared face: refused with a reason', () => {
    expect(isError(connectVerts(box(), [0, 1]))).toBe(true)
    expect(isError(connectVerts(box(), [0, 6]))).toBe(true) // opposite corners of the box share no face
    expect(isError(connectVerts(box(), [0]))).toBe(true)
  })
})

describe('subdivide / connect edges', () => {
  it('one edge: both faces on it get the new vertex, nothing else changes', () => {
    const r = ok(subdivideEdges(box(), ['0-1']))
    expectSolid(r.brush)
    expect(r.brush.vertices).toHaveLength(9); expect(r.brush.vertices[8]).toEqual([1, 0, 0])
    expect(r.brush.faces).toHaveLength(6)
    expect(r.sel.edges).toHaveLength(2) // the two halves
  })
  it('two opposite edges of a face: the face is cut across (Scythe connect edges); 3 cuts give 4 strips', () => {
    const top = (b: Brush) => b.faces.filter((f) => f.loop.every((v) => b.vertices[v][2] === 1))
    const r = ok(subdivideEdges(box(), ['4-5', '6-7']))
    expectSolid(r.brush); expect(top(r.brush)).toHaveLength(2); expect(r.sel.edges).toHaveLength(1)
    const r3 = ok(subdivideEdges(box(), ['4-5', '6-7'], 3))
    expectSolid(r3.brush); expect(top(r3.brush)).toHaveLength(4); expect(r3.sel.edges).toHaveLength(3)
  })
  it('all four edges of a face: midpoints joined around a middle face (4 corners + 1)', () => {
    const r = ok(subdivideEdges(box(), ['4-5', '5-6', '6-7', '4-7']))
    expectSolid(r.brush)
    expect(r.brush.faces.filter((f) => f.loop.every((v) => r.brush.vertices[v][2] === 1))).toHaveLength(5)
  })
  it('nothing selected is refused', () => { expect(isError(subdivideEdges(box(), []))).toBe(true) })
})

describe('mirror (Ctrl+M, then X / Y / Z)', () => {
  // a box with its top +x edge pulled out (a slanted, still planar side), so a mirror is visible
  const lopsided = (): Brush => { const b = box(); b.vertices = b.vertices.map((p, i) => (i === 5 || i === 6 ? [p[0] + 1, p[1], p[2]] : p)); return b }
  it('whole brush: a true mirror about its centre, still a closed solid facing out, same volume', () => {
    const b = lopsided(), all = b.vertices.map((_, i) => i)
    const r = ok(mirrorVerts(b, all, 0))
    expect(isClosedManifold(r.brush)).toBe(true)
    expect(signedVolume(r.brush)).toBeCloseTo(signedVolume(b), 9)
    const cx = b.vertices.reduce((s, p) => s + p[0], 0) / b.vertices.length
    expect(r.brush.vertices[6][0]).toBeCloseTo(2 * cx - b.vertices[6][0], 9)
  })
  it('whole brush with UV lock: the texture is mirrored with the geometry (a point keeps its UV at its mirrored place)', () => {
    const b = lopsided(), all = b.vertices.map((_, i) => i)
    const top = b.faces.find((f) => f.loop.every((v) => b.vertices[v][2] === 1))!
    const before = uvAt(b, top.id, b.vertices[6] as [number, number, number])
    const r = ok(mirrorVerts(b, all, 0, true))
    const after = uvAt(r.brush, top.id, r.brush.vertices[6] as [number, number, number])
    expect(after[0]).toBeCloseTo(before[0], 9); expect(after[1]).toBeCloseTo(before[1], 9)
  })
  it('part of a brush mirrored onto itself is refused when it would turn the brush inside out', () => {
    const r = mirrorVerts(box(), [0, 1, 4, 5], 0) // the y=0 side, mirrored in x about its own centre: its corners swap
    expect(isError(r)).toBe(true)
  })
  it('a partial mirror that keeps the brush valid is applied', () => {
    const b = lopsided()
    const r = ok(mirrorVerts(b, [6], 0)) // one vertex about itself: nothing moves
    expect(r.brush.vertices[6]).toEqual(b.vertices[6])
    expect(isError(mirrorVerts(b, [], 1))).toBe(true)
  })
})

describe('knife (K)', () => {
  const mid = (edge: string) => ({ edge, t: 0.5 })
  it('from the middle of a bottom edge to the middle of the top edge above it: that side is cut in two', () => {
    const r = ok(knifeCut(box(), [mid('0-1'), mid('4-5')]))
    expectSolid(r.brush)
    expect(r.brush.vertices).toHaveLength(10); expect(r.brush.faces).toHaveLength(7); expect(r.sel.edges).toHaveLength(1)
    const [a, c] = r.sel.edges[0].split('-').map(Number)
    expect(r.brush.vertices[a][0]).toBeCloseTo(1, 12); expect(r.brush.vertices[c][0]).toBeCloseTo(1, 12)
  })
  it('a closed cut all the way round splits four faces', () => {
    const r = ok(knifeCut(box(), [mid('0-1'), mid('4-5'), mid('6-7'), mid('2-3'), mid('0-1')]))
    expectSolid(r.brush)
    expect(r.brush.faces).toHaveLength(10); expect(r.sel.edges).toHaveLength(4)
  })
  it('vertex to vertex across a face, and positions along an edge', () => {
    const r = ok(knifeCut(box(), [{ v: 0 }, { v: 2 }]))
    expectSolid(r.brush); expect(r.brush.faces).toHaveLength(7)
    expect(knifePos(box(), { edge: '0-1', t: 0.25 })).toEqual([0.5, 0, 0])
    expect(knifePos(box(), { edge: '0-1', t: 0 })).toEqual([0, 0, 0])
  })
  it('points with no face in common cut nothing and are refused; one point is not a cut', () => {
    expect(isError(knifeCut(box(), [{ v: 0 }, { v: 6 }]))).toBe(true)
    expect(isError(knifeCut(box(), [mid('0-1')]))).toBe(true)
  })
})

describe('Shift+drag extrude along a gizmo axis (extrudeFaces with an offset vector)', () => {
  const topId = (b: Brush) => b.faces.find((f) => f.loop.every((v) => b.vertices[v][2] === 1))!.id
  it('along the normal = the plain extrude', () => {
    const b = box()
    const a = ok(extrudeFaces(b, [topId(b)], 0, [0, 0, 0.5])), n = ok(extrudeFaces(b, [topId(b)], 0.5))
    expect(a.brush.vertices).toEqual(n.brush.vertices); expect(a.brush.faces.map((f) => f.loop)).toEqual(n.brush.faces.map((f) => f.loop))
  })
  it('a slanted offset gives a sheared prism: closed, volume grows by area x height', () => {
    const b = box()
    const r = ok(extrudeFaces(b, [topId(b)], 0, [0.5, 0, 1]))
    expect(isClosedManifold(r.brush)).toBe(true)
    expect(signedVolume(r.brush)).toBeCloseTo(2 + 2 * 1, 9)
    expect(r.sel.faces).toEqual([topId(b)])
  })
  it('a zero offset changes nothing', () => {
    const b = box()
    expect(ok(extrudeFaces(b, [topId(b)], 0, [0, 0, 0])).brush).toBe(b)
  })
})
