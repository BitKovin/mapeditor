import { describe, expect, it } from 'vitest'
import type { Brush } from '../src/model/document'
import { makeBox } from '../src/model/geometry'
import { deleteFaces, falloff, isError, loopCut, OpResult, proportionalVerts } from '../src/model/meshOps'
import { MeshSel, buildTopology, edgeKey, emptySel, parseEdge, selFromVerts } from '../src/model/meshTopology'
import { clickSelect, MeshPickAt, edgeLoop, edgeRingEdges, faceLoop, shortestEdgePath, shortestFacePath, shortestVertexPath } from '../src/model/meshSelect'

const box = (): Brush => makeBox('b', [0, 0, 0], [1, 1, 1], 'wall/a')
// makeBox: vertices 0..3 at z=0 (x0y0, x1y0, x1y1, x0y1), 4..7 at z=1; vertical edges 0-4, 1-5, 2-6, 3-7; faces 0 -Z, 1 +Z
function ok(r: OpResult): { brush: Brush; sel: MeshSel } {
  if (isError(r)) throw new Error(r.error)
  return r
}
/** A box cut once around its middle (z = 0.5): the cut's vertices have four edges, so loops run through them. */
const cutBox = (): { brush: Brush; sel: MeshSel } => ok(loopCut(box(), '0-4', 0.5))
const isChain = (edges: string[]): boolean => edges.every((k, i) => i === 0 || parseEdge(k).some((v) => parseEdge(edges[i - 1]).includes(v)))

describe('edge loop (Alt+click)', () => {
  it('stops at once on a box: every corner has three edges', () => {
    expect(edgeLoop(box(), '0-4')).toEqual(['0-4'])
  })
  it('runs all the way round through the vertices of a loop cut', () => {
    const { brush, sel } = cutBox()
    const loop = edgeLoop(brush, sel.edges[0])
    expect(new Set(loop)).toEqual(new Set(sel.edges)) // the four new edges
    expect(loop).toHaveLength(4); expect(isChain(loop)).toBe(true)
  })
  it('a vertical half edge continues through the cut vertex and stops at the box corners', () => {
    const { brush } = cutBox()
    const mid = brush.vertices.findIndex((p) => p[0] === 0 && p[1] === 0 && Math.abs(p[2] - 0.5) < 1e-9)
    const lower = buildTopology(brush).edges.has(edgeKey(0, mid)) ? edgeKey(0, mid) : ''
    expect(lower).not.toBe('')
    const loop = edgeLoop(brush, lower)
    expect(new Set(loop)).toEqual(new Set([edgeKey(0, mid), edgeKey(mid, 4)]))
  })
  it('follows an open border', () => {
    const open = ok(deleteFaces(box(), [1])).brush // no +Z face: the top rim is a border
    const rim = edgeLoop(open, '4-5')
    expect(rim).toHaveLength(4); expect(isChain(rim)).toBe(true)
    expect(rim.flatMap(parseEdge).every((v) => v >= 4)).toBe(true)
  })
  it('an unknown edge selects nothing', () => { expect(edgeLoop(box(), '0-6')).toEqual([]) })
})

describe('edge ring (Ctrl+Alt+click) and face loop', () => {
  it('ring of a vertical box edge = the four vertical edges', () => {
    expect(new Set(edgeRingEdges(box(), '0-4'))).toEqual(new Set(['0-4', '1-5', '2-6', '3-7']))
  })
  it('face loop across a vertical edge of a lower quad = the four lower side quads', () => {
    const { brush } = cutBox()
    const mid = brush.vertices.findIndex((p) => p[0] === 0 && p[1] === 0 && Math.abs(p[2] - 0.5) < 1e-9)
    const lowerQuad = brush.faces.find((f) => f.loop.includes(0) && f.loop.includes(mid))!
    const loop = faceLoop(brush, lowerQuad.id, edgeKey(0, mid))
    expect(loop).toHaveLength(4)
    for (const id of loop) expect(brush.faces.find((f) => f.id === id)!.loop.every((v) => brush.vertices[v][2] <= 0.5 + 1e-9)).toBe(true)
  })
  it('face loop across the cut edge goes over the bottom and the top: six faces', () => {
    const { brush, sel } = cutBox()
    const cut = sel.edges[0]
    const quad = brush.faces.find((f) => f.loop.some((v, i) => edgeKey(v, f.loop[(i + 1) % f.loop.length]) === cut))!
    expect(faceLoop(brush, quad.id, cut)).toHaveLength(6)
  })
})

describe('shortest path (Ctrl+click)', () => {
  it('vertex path between opposite corners of a box takes three edges', () => {
    const p = shortestVertexPath(box(), 0, 6)
    expect(p[0]).toBe(0); expect(p[p.length - 1]).toBe(6); expect(p).toHaveLength(4)
    for (let i = 1; i < p.length; i++) expect(buildTopology(box()).edges.has(edgeKey(p[i - 1], p[i]))).toBe(true)
  })
  it('vertex path prefers the shorter way by length', () => {
    const b = box(); b.vertices = b.vertices.map((p) => [p[0] * 10, p[1], p[2]]) // long in x
    const p = shortestVertexPath(b, 0, 7) // 0 = (0,0,0), 7 = (0,1,1): stay at x = 0
    expect(p.every((v) => b.vertices[v][0] === 0)).toBe(true)
  })
  it('edge path joins two edges with a connected chain', () => {
    const p = shortestEdgePath(box(), '0-1', '6-7')
    expect(p[0]).toBe('0-1'); expect(p[p.length - 1]).toBe('6-7'); expect(isChain(p)).toBe(true)
  })
  it('face path from bottom to top crosses one side face', () => {
    const p = shortestFacePath(box(), 0, 1)
    expect(p).toHaveLength(3); expect(p[0]).toBe(0); expect(p[2]).toBe(1)
  })
  it('same element = itself', () => {
    expect(shortestVertexPath(box(), 3, 3)).toEqual([3]); expect(shortestFacePath(box(), 2, 2)).toEqual([2]); expect(shortestEdgePath(box(), '0-1', '0-1')).toEqual(['0-1'])
  })
})

describe('what a click selects (clickSelect)', () => {
  const none: MeshPickAt = { vertex: null, edge: null, face: null, faceEdge: null }
  const plain = { shift: false, ctrl: false, alt: false }
  it('nothing under the cursor = null (the viewport starts a box selection)', () => {
    expect(clickSelect(box(), 'vertex', none, plain, emptySel(), null)).toBeNull()
  })
  it('plain click selects the element and makes it active; Shift toggles', () => {
    const b = box()
    const r = clickSelect(b, 'vertex', { ...none, vertex: 2 }, plain, emptySel(), null)!
    expect(r.sel.verts).toEqual([2]); expect(r.active).toEqual({ mode: 'vertex', id: 2 })
    const r2 = clickSelect(b, 'vertex', { ...none, vertex: 5 }, { ...plain, shift: true }, r.sel, r.active)!
    expect(new Set(r2.sel.verts)).toEqual(new Set([2, 5]))
    const r3 = clickSelect(b, 'vertex', { ...none, vertex: 2 }, { ...plain, shift: true }, r2.sel, r2.active)!
    expect(r3.sel.verts).toEqual([5])
  })
  it('Ctrl+click adds the shortest path from the active element', () => {
    const b = box()
    const r = clickSelect(b, 'vertex', { ...none, vertex: 6 }, { ...plain, ctrl: true }, selFromVerts(b, [0]), { mode: 'vertex', id: 0 })!
    expect(r.sel.verts).toHaveLength(4); expect(r.sel.verts).toContain(0); expect(r.sel.verts).toContain(6)
    expect(r.active).toEqual({ mode: 'vertex', id: 6 })
  })
  it('Ctrl+click with no active element in this mode selects just the element (added)', () => {
    const b = box()
    const r = clickSelect(b, 'face', { ...none, face: 1 }, { ...plain, ctrl: true }, emptySel(), { mode: 'vertex', id: 0 })!
    expect(r.sel.faces).toEqual([1])
  })
  it('Alt+click in edge mode = loop, Ctrl+Alt = ring; vertex mode takes the loop vertices', () => {
    const b = box()
    expect(clickSelect(b, 'edge', { ...none, edge: '0-4' }, { ...plain, alt: true }, emptySel(), null)!.sel.edges).toEqual(['0-4'])
    expect(new Set(clickSelect(b, 'edge', { ...none, edge: '0-4' }, { ...plain, alt: true, ctrl: true }, emptySel(), null)!.sel.edges)).toEqual(new Set(['0-4', '1-5', '2-6', '3-7']))
    expect(clickSelect(b, 'vertex', { ...none, edge: '0-4' }, { ...plain, alt: true, ctrl: true }, emptySel(), null)!.sel.verts).toHaveLength(8)
  })
  it('loop by double-click (loop flag without Alt) in face mode; Shift adds it to the selection', () => {
    const { brush } = cutBox()
    const mid = brush.vertices.findIndex((p) => p[0] === 0 && p[1] === 0 && Math.abs(p[2] - 0.5) < 1e-9)
    const q = brush.faces.find((f) => f.loop.includes(0) && f.loop.includes(mid))!
    const at = { ...none, face: q.id, faceEdge: edgeKey(0, mid) }
    const r = clickSelect(brush, 'face', at, plain, emptySel(), null, true)!
    expect(r.sel.faces).toHaveLength(4)
    const r2 = clickSelect(brush, 'face', at, { ...plain, shift: true }, { verts: [], edges: [], faces: [0] }, null, true)!
    expect(r2.sel.faces).toHaveLength(5)
  })
})

describe('proportional editing (O)', () => {
  it('falloff: 1 at the selection, 0 at the radius, smooth in between', () => {
    expect(falloff(0, 2)).toBe(1); expect(falloff(2, 2)).toBe(0); expect(falloff(3, 2)).toBe(0); expect(falloff(1, 2)).toBeCloseTo(0.5, 12)
    expect(falloff(0.5, 0)).toBe(0)
  })
  it('moving the top of a cut box up: the middle ring follows half way, the bottom stays, the selection is unchanged', () => {
    const { brush } = cutBox()
    const top = brush.vertices.map((p, i) => (p[2] === 1 ? i : -1)).filter((i) => i >= 0)
    const r = proportionalVerts(brush, top, (p) => [p[0], p[1], p[2] + 1], 1)
    if (isError(r)) throw new Error(r.error)
    for (const [i, p] of brush.vertices.entries()) {
      const z = r.brush.vertices[i][2]
      if (p[2] === 1) expect(z).toBeCloseTo(2, 12)
      else if (p[2] === 0) expect(z).toBe(0)
      else expect(z).toBeCloseTo(0.5 + falloff(0.5, 1), 12) // the cut ring is 0.5 below the top
    }
    expect(new Set(r.sel.verts)).toEqual(new Set(top))
  })
  it('a radius too small to reach anything = a plain move', () => {
    const r = proportionalVerts(box(), [6], (p) => [p[0], p[1], p[2] + 0.25], 0.01)
    if (isError(r)) throw new Error(r.error)
    expect(r.brush.vertices.filter((p, i) => i !== 6 && p[2] > 1)).toHaveLength(0)
  })
})
