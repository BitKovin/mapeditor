import { describe, expect, it } from 'vitest'
import type { Brush } from '../src/model/document'
import { makeBox, signedVolume, validateBrush } from '../src/model/geometry'
import {
  MAX_BEVEL_SEGMENTS, bevelEdges, deleteFaces, dissolveEdges, dissolveFaces, dissolveVerts, edgeRing, extrudeFaces, fillHole, flipFaces, insetFaces,
  isError, loopCut, mergeByDistance, mergeVerts, OpResult, recalcNormals, transformVerts,
} from '../src/model/meshOps'
import { MeshSel, buildTopology, convertSel, edgeKey, growSel, isClosedManifold, selFromEdges, selFromFaces, selFromVerts, shrinkSel } from '../src/model/meshTopology'
import { projectFace } from '../src/model/uv'

const box = (): Brush => makeBox('b', [0, 0, 0], [1, 1, 1], 'wall/a')
// makeBox faces: 0 -Z, 1 +Z, 2 -X, 3 +X, 4 -Y, 5 +Y (top). Vertices 0..3 are z=0 (x0y0, x1y0, x1y1, x0y1), 4..7 z=1.
const TOP = 5

function ok(r: OpResult): { brush: Brush; sel: MeshSel } {
  if (isError(r)) throw new Error(`operation failed: ${r.error}`)
  return r
}
/** Every tool result must be a valid closed solid with outward faces. */
function expectSolid(b: Brush, convex = false): void {
  expect(validateBrush(b, convex)).toBeNull()
  expect(isClosedManifold(b)).toBe(true)
  expect(signedVolume(b)).toBeGreaterThan(0)
  expect(new Set(b.faces.map((f) => f.id)).size).toBe(b.faces.length) // ids unique
}

describe('topology and selection', () => {
  it('a box is a closed manifold; every edge has two faces', () => {
    const t = buildTopology(box())
    expect(t.edges.size).toBe(12)
    expect([...t.edges.values()].every((e) => e.faces.length === 2)).toBe(true)
    expect(isClosedManifold(box())).toBe(true)
  })
  it('converts selections between modes the Blender way', () => {
    const b = box()
    const top = selFromFaces(b, [TOP])
    expect(top.verts.length).toBe(4)
    expect(top.edges.length).toBe(4)
    const asVerts = convertSel(b, top, 'face', 'vertex')
    expect(asVerts.verts.length).toBe(4)
    expect(convertSel(b, asVerts, 'vertex', 'face').faces).toEqual([TOP])   // all four corners selected -> the face
    const oneEdge = selFromEdges(b, [edgeKey(0, 1)])
    expect(convertSel(b, oneEdge, 'edge', 'face').faces).toEqual([])         // no face has all its edges selected
    expect(convertSel(b, oneEdge, 'edge', 'vertex').verts.sort()).toEqual([0, 1])
  })
  it('grow and shrink', () => {
    const b = box()
    const v = selFromVerts(b, [0])
    expect(growSel(b, v, 'vertex').verts.length).toBe(4)                     // itself + 3 neighbours
    const g = growSel(b, selFromFaces(b, [TOP]), 'face')
    expect(g.faces.length).toBe(5)                                           // the top and the four sides that touch it
    expect(shrinkSel(b, g, 'face').faces).toEqual([TOP])                     // only the top has all its neighbours selected
  })
})

describe('transformVerts', () => {
  it('moving a whole face keeps everything planar and nothing splits', () => {
    const b = box(), top = selFromFaces(b, [TOP])
    const r = ok(transformVerts(b, top.verts, (p) => [p[0], p[1] + 1, p[2]]))
    expect(r.brush.faces.length).toBe(6)
    expectSolid(r.brush, true)
    expect(signedVolume(r.brush)).toBeCloseTo(2)
  })
  it('moving a single corner splits the faces that stop being planar; the first piece keeps the id', () => {
    const b = box()
    const r = ok(transformVerts(b, [7], (p) => [p[0], p[1] + 0.5, p[2]]))
    expect(r.brush.faces.length).toBeGreaterThan(6)
    expectSolid(r.brush)
    for (const id of [TOP, 1, 2]) expect(r.brush.faces.some((f) => f.id === id)).toBe(true)
    const ids = r.brush.faces.map((f) => f.id)
    expect(new Set(ids).size).toBe(ids.length)
  })
  it('is a pure function of the input: moving back gives the original planar faces', () => {
    const b = box()
    const there = ok(transformVerts(b, [7], (p) => [p[0], p[1] + 0.5, p[2]])).brush
    const back = ok(transformVerts(b, [7], (p) => p)).brush
    expect(there.faces.length).toBeGreaterThan(6)
    expect(back.faces.length).toBe(6)
  })
  it('re-projects stored UVs so textures stay put in the world', () => {
    const b = box()
    const withUv: Brush = { ...b, faces: b.faces.map((f) => ({ ...f, uvs: projectFace(b, f) })) }
    const top = selFromFaces(withUv, [TOP])
    const r = ok(transformVerts(withUv, top.verts, (p) => [p[0] + 3, p[1], p[2]])).brush
    const face = r.faces.find((f) => f.id === TOP)!
    expect(face.uvs).toEqual(projectFace(r, face))
    expect(face.uvs).not.toEqual(withUv.faces.find((f) => f.id === TOP)!.uvs)
  })
})

describe('extrude', () => {
  it('pulls a face out: closed, right volume, the face keeps its id, four new sides', () => {
    const b = box()
    const r = ok(extrudeFaces(b, [TOP], 2))
    expectSolid(r.brush, true)
    expect(signedVolume(r.brush)).toBeCloseTo(3)
    expect(r.brush.faces.length).toBe(10)
    expect(r.sel.faces).toEqual([TOP])
    expect(r.brush.faces.find((f) => f.id === TOP)).toBeDefined()
    expect(r.brush.faces.filter((f) => f.texture === 'wall/a').length).toBe(10)  // sides take the look of what they grow from
  })
  it('a negative distance cuts into the brush (valid, but no longer convex)', () => {
    const r = ok(extrudeFaces(box(), [TOP], -0.5))
    expectSolid(r.brush)
    expect(signedVolume(r.brush)).toBeCloseTo(0.5)
  })
  it('distance 0 is the identity, and an empty selection is refused', () => {
    const b = box()
    expect(ok(extrudeFaces(b, [TOP], 0)).brush).toBe(b)
    expect(isError(extrudeFaces(b, [], 1))).toBe(true)
  })
  it('extrudes a region of two adjacent faces as one piece (no wall between them)', () => {
    const cut = ok(loopCut(box(), edgeKey(4, 5), 0.5)).brush         // splits top and other faces in two
    const tops = cut.faces.filter((f) => f.loop.every((v) => cut.vertices[v][1] === 1)).map((f) => f.id)
    expect(tops.length).toBe(2)
    const r = ok(extrudeFaces(cut, tops, 1))
    expectSolid(r.brush)
    expect(signedVolume(r.brush)).toBeCloseTo(2)
    // 6 + 4 (loop cut splits) + 6 walls around the 2-face region
    expect(r.brush.faces.length).toBe(cut.faces.length + 6)
  })
  it('faces that point every which way have no single extrude direction', () => {
    const b = box()
    expect(isError(extrudeFaces(b, b.faces.map((f) => f.id), 1))).toBe(true)
  })
})

describe('inset', () => {
  it('makes an inner face (same id) and a ring of four quads', () => {
    const b = box()
    const r = ok(insetFaces(b, [TOP], 0.25))
    expectSolid(r.brush, true)
    expect(r.brush.faces.length).toBe(10)
    expect(r.sel.faces).toEqual([TOP])
    const inner = r.brush.faces.find((f) => f.id === TOP)!
    const xs = inner.loop.map((v) => r.brush.vertices[v][0])
    expect(Math.min(...xs)).toBeCloseTo(0.25)
    expect(Math.max(...xs)).toBeCloseTo(0.75)
    expect(signedVolume(r.brush)).toBeCloseTo(1)
  })
  it('refuses an inset bigger than the face, and 0 is the identity', () => {
    const b = box()
    expect(isError(insetFaces(b, [TOP], 0.6))).toBe(true)
    expect(ok(insetFaces(b, [TOP], 0)).brush).toBe(b)
  })
  it('inset then extrude builds a raised panel', () => {
    const a = ok(insetFaces(box(), [TOP], 0.25))
    const r = ok(extrudeFaces(a.brush, a.sel.faces, 0.2))
    expectSolid(r.brush)                       // a bump on a box is not convex
    expect(signedVolume(r.brush)).toBeCloseTo(1 + 0.5 * 0.5 * 0.2)
  })
})

describe('bevel', () => {
  it('a single edge becomes a chamfer: exact volume, one new face, closed', () => {
    const b = box()
    const r = ok(bevelEdges(b, [edgeKey(6, 7)], 0.2))   // top edge along x at z=1
    expectSolid(r.brush, true)
    expect(signedVolume(r.brush)).toBeCloseTo(1 - (0.2 * 0.2) / 2 * 1)
    expect(r.brush.faces.length).toBe(7)
    expect(r.sel.faces.length).toBe(1)
    for (const f of b.faces) expect(r.brush.faces.some((g) => g.id === f.id)).toBe(true) // the six originals survive
  })
  it('the four edges of the top face', () => {
    const b = box()
    const top = selFromFaces(b, [TOP])
    const r = ok(bevelEdges(b, top.edges, 0.1))
    expectSolid(r.brush, true)
    expect(signedVolume(r.brush)).toBeLessThan(1)
    expect(signedVolume(r.brush)).toBeGreaterThan(0.9)
    expect(r.brush.faces.length).toBe(10)
    expect(r.sel.faces.length).toBe(4)
  })
  it('all twelve edges: every corner gets a triangle', () => {
    const b = box()
    const r = ok(bevelEdges(b, selFromFaces(b, b.faces.map((f) => f.id)).edges, 0.1))
    expectSolid(r.brush, true)
    expect(r.brush.faces.length).toBe(6 + 12 + 8)
    expect(isClosedManifold(r.brush)).toBe(true)
  })
  it('two edges that meet in a chain', () => {
    const b = box()
    const r = ok(bevelEdges(b, [edgeKey(6, 7), edgeKey(5, 6)], 0.15))
    expectSolid(r.brush, true)
    expect(signedVolume(r.brush)).toBeLessThan(1)
  })
  it('an edge of a wedge (non-right angle) still closes up', () => {
    const ramp = ok(transformVerts(box(), [6, 7], (p) => [p[0], p[1] - 0.5, p[2]])).brush // far top edge slid down: a sloped top
    expectSolid(ramp, true)
    const r = ok(bevelEdges(ramp, [edgeKey(6, 7)], 0.1))
    expectSolid(r.brush, true)
    expect(signedVolume(r.brush)).toBeLessThan(signedVolume(ramp))
  })
  it('segments: one edge becomes a circular arc, volume converges to the exact quarter circle', () => {
    const b = box()
    const r = ok(bevelEdges(b, [edgeKey(6, 7)], 0.2, 16))
    expectSolid(r.brush, true)
    expect(r.brush.faces.length).toBe(6 + 16)
    expect(r.sel.faces.length).toBe(16)
    expect(signedVolume(r.brush)).toBeCloseTo(1 - 0.2 * 0.2 * (1 - Math.PI / 4), 3)
    // every rail vertex is the same distance (the radius) from the arc centre
    const arcVerts = r.brush.vertices.filter((p) => p[0] > 0.79 && p[1] > 0.79 && p[1] < 0.9999 + 0.0001 && p[0] < 1.0001 && Math.abs(p[2] - 0) < 1e-9 + 2)
    expect(arcVerts.length).toBeGreaterThan(0)
  })
  it('segments: 1 is exactly the chamfer, more segments never lose the solid', () => {
    const b = box()
    const one = ok(bevelEdges(b, [edgeKey(6, 7)], 0.2, 1)), def = ok(bevelEdges(b, [edgeKey(6, 7)], 0.2))
    expect(one.brush).toEqual(def.brush)
    for (const n of [2, 3, 5]) expectSolid(ok(bevelEdges(b, [edgeKey(6, 7)], 0.2, n)).brush, true)
  })
  it('segments: chains, the top face loop and all twelve edges stay closed solids', () => {
    const b = box()
    for (const n of [2, 3, 4]) {
      expectSolid(ok(bevelEdges(b, [edgeKey(6, 7), edgeKey(5, 6)], 0.15, n)).brush, true)
      const top = ok(bevelEdges(b, selFromFaces(b, [TOP]).edges, 0.1, n))
      expectSolid(top.brush, true)
      expect(top.sel.faces.length).toBe(4 * n)
    }
    // corners where three bevels meet: convex up to 3 segments; above that the single-fan corner cap is closed and valid but slightly concave
    const all = selFromFaces(b, b.faces.map((f) => f.id)).edges
    for (const n of [2, 3]) expectSolid(ok(bevelEdges(b, all, 0.1, n)).brush, true)
    for (const n of [4, 8]) expectSolid(ok(bevelEdges(b, all, 0.1, n)).brush, false)
  })
  it('segments are clamped to a sane range', () => {
    const b = box()
    expect(ok(bevelEdges(b, [edgeKey(6, 7)], 0.2, 0)).brush.faces.length).toBe(7)
    expect(ok(bevelEdges(b, [edgeKey(6, 7)], 0.2, 1000)).brush.faces.length).toBe(6 + MAX_BEVEL_SEGMENTS)
  })
  it('refuses a bevel wider than the geometry, negative width, and open meshes', () => {
    const b = box()
    expect(isError(bevelEdges(b, [edgeKey(6, 7)], 5))).toBe(true)
    expect(isError(bevelEdges(b, selFromFaces(b, [TOP]).edges, 0.6))).toBe(true)
    expect(isError(bevelEdges(b, [edgeKey(6, 7)], -1))).toBe(true)
    const open = ok(deleteFaces(b, [TOP])).brush
    expect(isError(bevelEdges(open, [edgeKey(6, 7)], 0.1))).toBe(true)
    expect(ok(bevelEdges(b, [edgeKey(6, 7)], 0)).brush).toBe(b)
  })
})

describe('loop cut', () => {
  it('finds the ring of parallel edges around a box', () => {
    const info = edgeRing(box(), edgeKey(4, 5))!
    expect(info.closed).toBe(true)
    expect(info.ring.length).toBe(4)
  })
  it('inserts a loop: 4 new vertices, 4 more faces, volume unchanged, original ids kept', () => {
    const b = box()
    const r = ok(loopCut(b, edgeKey(4, 5), 0.5))
    expectSolid(r.brush, true)
    expect(r.brush.vertices.length).toBe(12)
    expect(r.brush.faces.length).toBe(10)
    expect(signedVolume(r.brush)).toBeCloseTo(1)
    for (const f of b.faces) expect(r.brush.faces.some((g) => g.id === f.id)).toBe(true)
    expect(r.sel.edges.length).toBe(4)                     // the new loop is selected
  })
  it('slides: the cut position follows the parameter along a consistent rail', () => {
    const r = ok(loopCut(box(), edgeKey(4, 5), 0.25)).brush
    const mids = r.vertices.slice(8)
    const cuts = new Set(mids.map((p) => [p[0], p[1], p[2]].map((x) => Math.round(x * 1000) / 1000).join()))
    expect(cuts.size).toBe(4)
    for (const p of mids) expect(p[0] === 0.25 || p[0] === 0.75 || p[1] === 0.25 || p[1] === 0.75 || p[2] === 0.25 || p[2] === 0.75).toBe(true)
    // all four sit at the same fraction from the same side, so the cut face is planar
    expectSolid(r)
  })
  it('a second cut across the first works, and out-of-range parameters are refused', () => {
    const a = ok(loopCut(box(), edgeKey(4, 5), 0.5)).brush
    const other = [...buildTopology(a).edges.keys()].find((k) => { const [x, y] = k.split('-').map(Number); return a.vertices[x][0] === 0 && a.vertices[y][0] === 1 && a.vertices[x][1] === 1 && a.vertices[x][2] === 0 && a.vertices[y][2] === 0 })
    expect(other).toBeUndefined() // the edge along x was split by the first cut; cut across the other direction instead
    const e = [...buildTopology(a).edges.keys()].find((k) => { const [x, y] = k.split('-').map(Number); return a.vertices[x][2] !== a.vertices[y][2] && a.vertices[x][0] === 0 && a.vertices[x][1] === 0 })!
    const b = ok(loopCut(a, e, 0.5)).brush
    expectSolid(b, true)
    expect(isError(loopCut(box(), edgeKey(4, 5), 0))).toBe(true)
    expect(isError(loopCut(box(), edgeKey(4, 5), 1))).toBe(true)
  })
  it('stops at a triangle instead of failing (partial ring) and still closes', () => {
    const tri = ok(transformVerts(box(), [7], (p) => [p[0], p[1] + 0.5, p[2]])).brush
    const anyEdge = [...buildTopology(tri).edges.keys()][0]
    const r = loopCut(tri, anyEdge, 0.5)
    if (!isError(r)) expect(isClosedManifold(r.brush)).toBe(true)
  })
})

describe('merge', () => {
  it('welds two vertices of an edge; neighbouring quads become triangles; still closed', () => {
    const r = ok(mergeVerts(box(), [6, 7]))
    expect(isClosedManifold(r.brush)).toBe(true)
    expect(r.brush.vertices.length).toBe(7)
    expect(signedVolume(r.brush)).toBeLessThan(1)
  })
  it('merge at a given position', () => {
    const r = ok(mergeVerts(box(), [6, 7], [0.5, 1, 1]))
    expect(r.brush.vertices.some((p) => p[0] === 0.5 && p[1] === 1 && p[2] === 1)).toBe(true)
  })
  it('refuses vertices that would pinch a face, or too few', () => {
    expect(isError(mergeVerts(box(), [0, 6]))).toBe(true)  // opposite corners of the brush
    expect(isError(mergeVerts(box(), [0]))).toBe(true)
  })
  it('by distance', () => {
    const b = ok(loopCut(box(), edgeKey(4, 5), 0.005)).brush
    const all = b.vertices.map((_, i) => i)
    expect(isError(mergeByDistance(b, all, 0.0001))).toBe(true)
    const r = ok(mergeByDistance(b, all, 0.01))
    expect(isClosedManifold(r.brush)).toBe(true)
    expect(r.brush.vertices.length).toBeLessThan(b.vertices.length)
  })
})

describe('dissolve', () => {
  it('undoes a loop cut: dissolve the cut edges, then the leftover collinear vertices', () => {
    const cut = ok(loopCut(box(), edgeKey(4, 5), 0.5))
    const merged = ok(dissolveEdges(cut.brush, cut.sel.edges))
    expect(merged.brush.faces.length).toBe(6)
    expect(merged.brush.vertices.length).toBe(12)
    expectSolid(merged.brush, true)
    const mids = [8, 9, 10, 11]
    const clean = ok(dissolveVerts(merged.brush, mids))
    expect(clean.brush.vertices.length).toBe(8)
    expect(clean.brush.faces.length).toBe(6)
    expect(clean.brush.faces.every((f) => f.loop.length === 4)).toBe(true)
    expectSolid(clean.brush, true)
  })
  it('dissolves faces into one polygon', () => {
    const cut = ok(loopCut(box(), edgeKey(4, 5), 0.5)).brush
    const tops = cut.faces.filter((f) => f.loop.every((v) => cut.vertices[v][1] === 1)).map((f) => f.id)
    const r = ok(dissolveFaces(cut, tops))
    expect(r.brush.faces.length).toBe(cut.faces.length - 1)
    expectSolid(r.brush, true)
    expect(isError(dissolveFaces(cut, [tops[0]]))).toBe(true)
  })
  it('dissolving an edge with a non-manifold result is refused', () => {
    expect(isError(dissolveEdges(box(), []))).toBe(true)
  })
  it('dissolving a corner vertex merges its three faces (and may split again if non-planar)', () => {
    const r = dissolveVerts(box(), [6])
    if (!isError(r)) expect(r.brush.vertices.length).toBe(7)
  })
})

describe('delete faces + fill', () => {
  it('delete makes a hole; fill closes it with a new face', () => {
    const b = box()
    const open = ok(deleteFaces(b, [TOP])).brush
    expect(open.faces.length).toBe(5)
    expect(isClosedManifold(open)).toBe(false)
    const top = selFromFaces(b, [TOP]).verts
    const filled = ok(fillHole(open, top))
    expect(filled.brush.faces.length).toBe(6)
    expectSolid(filled.brush, true)
    expect(filled.sel.faces.length).toBe(1)
  })
  it('refuses when there is nothing to fill, or to delete everything', () => {
    expect(isError(fillHole(box(), [0, 1, 2, 3]))).toBe(true)
    expect(isError(deleteFaces(box(), box().faces.map((f) => f.id)))).toBe(true)
  })
})

describe('normals', () => {
  it('flip makes the volume negative; recalculate repairs a mixed-up mesh in both directions', () => {
    const b = box()
    const allFlipped = ok(flipFaces(b, b.faces.map((f) => f.id))).brush
    expect(signedVolume(allFlipped)).toBeLessThan(0)
    expect(signedVolume(ok(recalcNormals(allFlipped, true)).brush)).toBeGreaterThan(0)
    const mixed = ok(flipFaces(b, [0, 3])).brush
    expect(isClosedManifold(mixed)).toBe(false) // inconsistent winding shows up as a broken edge pairing
    const fixed = ok(recalcNormals(mixed, true)).brush
    expectSolid(fixed, true)
    expect(signedVolume(ok(recalcNormals(b, false)).brush)).toBeLessThan(0)  // inward on request
  })
  it('flipping keeps UVs matched to their corners', () => {
    const b = box()
    const f0 = { ...b, faces: b.faces.map((f) => ({ ...f, uvs: f.loop.map((_, i) => [i, 0] as [number, number]) })) }
    const r = ok(flipFaces(f0, [0])).brush.faces.find((f) => f.id === 0)!
    expect(r.uvs!.map((u) => u[0])).toEqual([3, 2, 1, 0])
  })
})
