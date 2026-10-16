import { describe, expect, it } from 'vitest'
import type { Brush, MapDocument } from '../src/model/document'
import { brushBounds, validateBrush } from '../src/model/geometry'
import { attachShape, mapFrame, reconcileShapes, regenerateShape, shapeOf } from '../src/model/parametric'
import { Turn, buildShape, defaultShapeParams, frameOf, turned } from '../src/model/shapes'
import { planeThrough } from '../src/tools/boxTool'
import { rotateAbout, transformSelections } from '../src/model/transform'
import { transformVerts, isError } from '../src/model/meshOps'

const plane = planeThrough([0, 0, 0], [0, 1, 0]) // the floor: u = z, v = x, height = +y
const rect = { origin: [0, 0, 0] as [number, number, number], du: 4, dv: 2 }
let n = 0
const id = () => `b${n++}`
/** A drawn shape in worldspawn, as the Box tool makes it. */
function drawn(kind: 'stairs' | 'wedge' | 'cylinder' | 'arch', over: Partial<ReturnType<typeof defaultShapeParams>> = {}, turn: Turn = 0): MapDocument {
  const p = { ...defaultShapeParams(), ...over, turn }
  const r = buildShape(kind, plane, rect, 3, 'dev/wall', p, id, turn)
  if ('error' in r) throw new Error(r.error)
  return { schemaVersion: '1', prefabInstances: [], entities: [{ id: 'w', classname: 'worldspawn', brushes: attachShape(r.brushes, kind, frameOf(plane, rect, 3), p, turn, 'g1') }] }
}
const brushes = (d: MapDocument, e = 'w'): Brush[] => d.entities.find((x) => x.id === e)!.brushes ?? []
const keep = (prev: MapDocument | null, next: MapDocument) => reconcileShapes(prev, next, () => 'fresh')
const ok = (r: { doc: MapDocument } | { error: string }): MapDocument => { if ('error' in r) throw new Error(r.error); return r.doc }

describe('placed shapes carry their generator', () => {
  it('every part of drawn stairs has the record; reconcile leaves a fresh shape alone', () => {
    const d = drawn('stairs', { steps: 5 })
    const parts = brushes(d).map(shapeOf)
    expect(parts.every((s) => s && s.group === 'g1' && s.kind === 'stairs' && s.parts === 5)).toBe(true)
    expect(parts.map((s) => s!.part)).toEqual([0, 1, 2, 3, 4])
    expect(parts[0]!.params).toEqual({ steps: 5 })
    expect(keep(null, d)).toBe(d)
  })
})

describe('editing a shape (Details)', () => {
  it('fewer steps: the first parts keep their ids, the rest go, every part valid; reconcile keeps the result', () => {
    const d = drawn('stairs', { steps: 6 }), ids = brushes(d).map((b) => b.id)
    const e = ok(regenerateShape(d, 'w', 'g1', { params: { steps: 3 } }, id))
    expect(brushes(e).map((b) => b.id)).toEqual(ids.slice(0, 3))
    expect(brushes(e).every((b) => validateBrush(b, true) === null && shapeOf(b)!.parts === 3)).toBe(true)
    expect(keep(d, e)).toBe(e)
  })
  it('more steps: new parts get new ids; the whole staircase still fills the same box', () => {
    const d = drawn('stairs', { steps: 2 })
    const e = ok(regenerateShape(d, 'w', 'g1', { params: { steps: 5 } }, () => 'new' + n++))
    expect(brushes(e)).toHaveLength(5); expect(brushes(e).filter((b) => b.id.startsWith('new'))).toHaveLength(3)
    const bb = brushes(e).map(brushBounds)
    expect(Math.max(...bb.map((b) => b!.max[1]))).toBeCloseTo(3, 9)
  })
  it('faces keep their texture by face id; other brushes of the entity are untouched and stay in place', () => {
    const d = drawn('wedge')
    const w = d.entities[0]
    const other: Brush = { id: 'x', vertices: [], faces: [] }
    w.brushes = [other, { ...w.brushes![0], faces: w.brushes![0].faces.map((f) => (f.id === 1 ? { ...f, texture: 'dev/red', uvs: f.loop.map(() => [0, 0] as [number, number]) } : f)) }]
    const e = ok(regenerateShape(d, 'w', 'g1', { size: { h: 5 } }, id))
    expect(brushes(e)[0]).toBe(other)
    const f1 = brushes(e)[1].faces.find((f) => f.id === 1)!
    expect(f1.texture).toBe('dev/red')
    expect(f1.uvs === undefined || f1.uvs.some((uv) => uv[0] !== 0 || uv[1] !== 0)).toBe(true) // stale stored UVs are not kept
    expect(Math.max(...brushes(e)[1].vertices.map((p) => p[1]))).toBeCloseTo(5, 9)
  })
  it('orientation: a wedge turned 90 degrees rises along the other side of the box', () => {
    const top = (d: MapDocument) => brushes(d)[0].vertices.filter((p) => p[1] > 2.999)
    const d0 = drawn('wedge')
    const d1 = ok(regenerateShape(d0, 'w', 'g1', { turn: 1 }, id))
    // turn 0: the high edge runs along v (x) at u = du (z = 4); turn 1: it runs along u (z) at one end of v
    expect(top(d0).every((p) => Math.abs(p[2] - 4) < 1e-9)).toBe(true)
    const xs = new Set(top(d1).map((p) => Math.round(p[0] * 1e6)))
    expect(xs.size).toBe(1); expect(new Set(top(d1).map((p) => Math.round(p[2] * 1e6))).size).toBe(2)
    expect(validateBrush(brushes(d1)[0], true)).toBeNull()
  })
  it('turned keeps the box: every turn covers the same corners', () => {
    const f = frameOf(plane, rect, 3)
    const corners = (t: Turn) => { const g = turned(f, t); return new Set([[0, 0], [1, 0], [0, 1], [1, 1]].map(([a, b]) => [0, 1, 2].map((k) => Math.round((g.o[k] + g.u[k] * g.du * a + g.v[k] * g.dv * b) * 1e6)).join())) }
    for (const t of [1, 2, 3] as Turn[]) expect(corners(t)).toEqual(corners(0))
  })
  it('a size of zero is refused', () => {
    expect('error' in regenerateShape(drawn('cylinder'), 'w', 'g1', { size: { du: 0 } }, id)).toBe(true)
  })
})

describe('what keeps a shape and what turns it into brushes', () => {
  it('rotating the shape keeps it parametric: the box turns with it and still matches', () => {
    const d = drawn('arch')
    const sels = brushes(d).map((b) => ({ entityId: 'w', brushId: b.id }))
    const moved = transformSelections(d, sels, rotateAbout([1, 0, 1], [0, 1, 0], 0.7), false, 40, true)
    const r = keep(d, moved)
    expect(brushes(r).every((b) => shapeOf(b)?.group === 'g1')).toBe(true)
    // and regenerating the rotated shape gives the rotated geometry back
    const again = ok(regenerateShape(r, 'w', 'g1', {}, id))
    brushes(again).forEach((b, i) => b.vertices.forEach((p, k) => p.forEach((c, j) => expect(c).toBeCloseTo(brushes(r)[i].vertices[k][j], 9))))
  })
  it('mapFrame of an affine map moves every point of the box like the map', () => {
    const f = frameOf(plane, rect, 3), map = (p: [number, number, number]): [number, number, number] => [2 * p[0] + 1, p[1] - 3, p[2] + 0.5 * p[0]]
    const g = mapFrame(f, map as never)
    const pt = (fr: typeof f) => [0, 1, 2].map((k) => fr.o[k] + fr.u[k] * fr.du * 0.3 + fr.v[k] * fr.dv * 0.6 + fr.n[k] * fr.h * 0.9)
    const want = map(pt(f) as never)
    pt(g).forEach((c, k) => expect(c).toBeCloseTo(want[k], 9))
  })
  it('a mesh edit of one part turns the whole shape into ordinary brushes', () => {
    const d = drawn('stairs', { steps: 3 })
    const b0 = brushes(d)[0], r = transformVerts(b0, [0], (p) => [p[0], p[1] + 0.1, p[2]])
    if (isError(r)) throw new Error(r.error)
    const edited = { ...d, entities: [{ ...d.entities[0], brushes: [r.brush, ...brushes(d).slice(1)] }] }
    expect(brushes(keep(d, edited)).every((b) => b.shape === undefined)).toBe(true)
  })
  it('moving one part on its own turns the shape into brushes; moving all parts keeps it', () => {
    const d = drawn('stairs', { steps: 3 }), b = brushes(d)
    const one = transformSelections(d, [{ entityId: 'w', brushId: b[1].id }], (p) => [p[0] + 1, p[1], p[2]], false, 0, true)
    expect(brushes(keep(d, one)).every((x) => x.shape === undefined)).toBe(true)
    const all = transformSelections(d, b.map((x) => ({ entityId: 'w', brushId: x.id })), (p) => [p[0] + 1, p[1], p[2]], false, 0, true)
    expect(brushes(keep(d, all)).every((x) => shapeOf(x)?.group === 'g1')).toBe(true)
  })
  it('a duplicate becomes its own shape; the original keeps its group', () => {
    const d = drawn('wedge'), b = brushes(d)[0]
    const dup = { ...d, entities: [{ ...d.entities[0], brushes: [b, { ...b, id: 'copy' }] }] }
    const r = brushes(keep(d, dup))
    expect(shapeOf(r[0])!.group).toBe('g1'); expect(shapeOf(r[1])!.group).toBe('fresh')
  })
  it('a part moved to another entity: neither half is a shape any more', () => {
    const d = drawn('stairs', { steps: 3 }), [a, b, c] = brushes(d)
    const split = { ...d, entities: [{ ...d.entities[0], brushes: [a, b] }, { id: 'g', classname: 'func_group', brushes: [c] }] }
    const r = keep(d, split)
    expect([...brushes(r), ...brushes(r, 'g')].every((x) => x.shape === undefined)).toBe(true)
  })
  it('a damaged record is dropped', () => {
    const d = drawn('wedge')
    const bad = { ...d, entities: [{ ...d.entities[0], brushes: [{ ...brushes(d)[0], shape: { group: 'g1', kind: 'teapot' } }] }] }
    expect(brushes(keep(d, bad))[0].shape).toBeUndefined()
  })
})
