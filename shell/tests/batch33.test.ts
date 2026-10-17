// REQUIREMENTS §33: entity handles (H1 / H2), a texture replaces a texture group (H3), shape preview (H4), the sphere (H5), smooth edges (H6).
import { afterEach, describe, expect, it } from 'vitest'
import { findEntity } from '../src/model/document'
import { makeBox, validateBrush } from '../src/model/geometry'
import { anglesToward, angleHandle, draggedOrigin, hasAngleHandle } from '../src/model/entityHandles'
import { attachShape, regenerateShape, shapeOf } from '../src/model/parametric'
import { buildShape, defaultShapeParams, frameOf, shapePreviewLines } from '../src/model/shapes'
import { markSmooth, smoothEdgeKeys, smoothPairs } from '../src/model/smooth'
import { texEnv } from '../src/model/texprops'
import { rotationFromAngles, apply3 } from '../src/model/rotation'
import { planeThrough } from '../src/tools/boxTool'
import { addDrawnBrush, applyTexGroup, applyTexture } from '../src/state/actions'
import { makeApp } from './helpers'

afterEach(() => texEnv.setProvider(null))
const plane = planeThrough([0, 0, 0], [0, 1, 0])
const rect = { origin: [0, 0, 0] as [number, number, number], du: 4, dv: 2 }
const forward = (a: [number, number, number]) => apply3(rotationFromAngles(a), [0, 0, 1])
const near = (a: number[], b: number[], eps = 1e-6) => a.every((x, i) => Math.abs(x - b[i]) < eps)

describe('entity handles (H1 / H2)', () => {
  it('the angle handle turns the entity to look along the drag; yaw only keeps pitch and roll', () => {
    const a = anglesToward([10, 0, 5], [1, 0, 0], true)!
    expect(a).toEqual([10, 90, 5])
    const b = anglesToward(undefined, [0, -1, 1], false)!
    expect(near(forward(b), [0, -Math.SQRT1_2, Math.SQRT1_2])).toBe(true)
    const c = anglesToward(undefined, [1, 0, 0.2], true, 15)!
    expect(c[1]).toBe(75)
    expect(anglesToward(undefined, [0, 0, 0], true)).toBeNull()
  })
  it('the handle sits on the tip of the direction arrow; point entities get it, brush entities and decals do not', () => {
    const e = { id: 'p', classname: 'info_player_start', origin: [1, 2, 3] as [number, number, number], angles: [0, 90, 0] as [number, number, number] }
    const h = angleHandle(e, () => ({ min: [-0.5, -0.5, -0.5], max: [0.5, 0.5, 0.5] }))
    expect(near(h.base, [1, 2, 3])).toBe(true)
    expect(h.tip[0]).toBeGreaterThan(1.5)
    expect(near([h.tip[1], h.tip[2]], [2, 3])).toBe(true)
    expect(hasAngleHandle(e)).toBe(true)
    expect(hasAngleHandle({ id: 'd', classname: 'decal' })).toBe(false)
    expect(hasAngleHandle({ id: 'b', classname: 'func_door', brushes: [makeBox('b', [0, 0, 0], [1, 1, 1])] })).toBe(false)
  })
  it('the origin drag snaps the axes of the drag plane and keeps the third', () => {
    expect(draggedOrigin([0.3, 1.1, 0], [0.3, 1.1, 0], [2.2, 1.1, -0.9], [0, 1, 0], 0.5)).toEqual([2, 1.1, -1])
  })
})

describe('a texture replaces a texture group (H3)', () => {
  it('applying a texture removes the layout, also when it is the group fallback', () => {
    const app = makeApp(); app.sync(); addDrawnBrush(app.store, makeBox('b1', [0, 0, 0], [1, 1, 1]))
    const faces = () => findEntity(app.store.state.doc, 'e_worldspawn')!.brushes![0].faces
    applyTexGroup(app.store, 'bld/apartment', 'dev/wall')
    expect(faces().every((f) => f.layout)).toBe(true)
    applyTexture(app.store, 'dev/wall')
    expect(faces().some((f) => f.layout)).toBe(false)
    applyTexGroup(app.store, 'bld/apartment', 'dev/wall')
    applyTexture(app.store, 'dev/floor')
    expect(faces().every((f) => !f.layout && f.texture === 'dev/floor')).toBe(true)
  })
})

describe('shapes (H4 / H5)', () => {
  it('the preview draws the shape itself: stairs have more edges than their box, every edge once', () => {
    const p = { ...defaultShapeParams(), steps: 4 }
    const lines = shapePreviewLines('stairs', plane, rect, 3, p)
    expect(lines.length / 6).toBeGreaterThan(12)
    const keys = new Set<string>()
    for (let i = 0; i < lines.length; i += 6) {
      const a = lines.slice(i, i + 3).map((x) => x.toFixed(4)).join(), b = lines.slice(i + 3, i + 6).map((x) => x.toFixed(4)).join()
      keys.add(a < b ? a + b : b + a)
    }
    expect(keys.size).toBe(lines.length / 6)
  })
  it('the sphere is one valid convex brush that fills its box, its edges all smooth; Details changes rings and segments', () => {
    const p = { ...defaultShapeParams(), segments: 8, rings: 4 }
    const r = buildShape('sphere', plane, rect, 3, 'dev/grey', p, () => 's1')
    if ('error' in r) throw new Error(r.error)
    expect(r.brushes).toHaveLength(1)
    const b = r.brushes[0]
    expect(validateBrush(b, true)).toBeNull()
    expect(b.faces).toHaveLength(8 * 4)
    const ys = b.vertices.map((v) => v[1])
    expect(Math.min(...ys)).toBeCloseTo(0); expect(Math.max(...ys)).toBeCloseTo(3)
    expect(smoothEdgeKeys(b).size).toBe(56) // every edge of the sphere (V - E + F = 2: 26 - 56 + 32)
    const doc = { schemaVersion: '0.2', prefabInstances: [], entities: [{ id: 'w', classname: 'worldspawn', brushes: attachShape(r.brushes, 'sphere', frameOf(plane, rect, 3), p, 0, 'g') }] }
    const next = regenerateShape(doc, 'w', 'g', { params: { rings: 6, segments: 10 } }, () => 'x')
    if ('error' in next) throw new Error(next.error)
    const nb = next.doc.entities[0].brushes![0]
    expect(nb.faces).toHaveLength(60)
    expect(shapeOf(nb)!.params).toMatchObject({ rings: 6, segments: 10 })
    expect(validateBrush(nb, true)).toBeNull()
  })
  it('a cylinder smooths its sides, not its caps; Smooth off leaves it faceted', () => {
    const r = buildShape('cylinder', plane, rect, 3, 'dev/grey', { ...defaultShapeParams(), sides: 6 }, () => 'c')
    if ('error' in r) throw new Error(r.error)
    expect(smoothPairs(r.brushes[0])).toHaveLength(6)
    const flat = buildShape('cylinder', plane, rect, 3, 'dev/grey', { ...defaultShapeParams(), sides: 6, smooth: false }, () => 'c')
    if ('error' in flat) throw new Error(flat.error)
    expect(flat.brushes[0].smooth).toBeUndefined()
  })
})

describe('smooth edges (H6)', () => {
  it('marks and clears edges as face pairs; ids survive vertex renumbering', () => {
    const b = makeBox('b', [0, 0, 0], [1, 1, 1])
    const edges = [...smoothEdgeKeys({ ...b, smooth: b.faces.flatMap((f, i) => b.faces.slice(i + 1).map((g) => [f.id, g.id])) })]
    expect(edges).toHaveLength(12) // every face pair that meets: the 12 edges of the box
    const one = markSmooth(b, [edges[0]], true)
    expect(smoothPairs(one)).toHaveLength(1)
    expect(markSmooth(one, [edges[0]], true)).toBe(one)
    expect(smoothEdgeKeys(one)).toEqual(new Set([edges[0]]))
    const cleared = markSmooth(one, [edges[0]], false)
    expect(cleared.smooth).toBeUndefined()
    // the same faces with their vertices listed in another order still name the same edge
    const perm = [7, 6, 5, 4, 3, 2, 1, 0]
    const moved = { ...one, vertices: perm.map((i) => one.vertices[i]), faces: one.faces.map((f) => ({ ...f, loop: f.loop.map((v) => perm.indexOf(v)) })) }
    const [k] = [...smoothEdgeKeys(moved)]
    const [x, y] = k.split('-').map(Number), [x0, y0] = edges[0].split('-').map(Number)
    expect(new Set([moved.vertices[x].join(), moved.vertices[y].join()])).toEqual(new Set([b.vertices[x0].join(), b.vertices[y0].join()]))
  })
})

describe('selection size (H7)', () => {
  it('one label per axis, outside the box; an orthographic view skips its depth axis; a face shows width and height', async () => {
    const { boxSizeLabels, faceSizeLabels } = await import('../src/model/sizeLabels')
    // a simple perspective-like projection: x right, y up, z towards the viewer (shifted so every point is distinct)
    const screen = (p: number[]) => ({ x: 400 + 160 * p[0] - 60 * p[2], y: 400 - 160 * p[1] + 40 * p[2] })
    const l3 = boxSizeLabels([0, 0, 0], [2, 1, 3], screen, () => 0.01)
    expect(l3.map((l) => l.text)).toEqual(['2 m', '1 m', '3 m'])
    for (const l of l3) expect(l.at.some((v, i) => v < [0, 0, 0][i] || v > [2, 1, 3][i])).toBe(true)
    // a far, small box: the numbers would cover each other, so fewer are shown
    const tiny = boxSizeLabels([0, 0, 0], [0.2, 0.2, 0.2], (p) => ({ x: 4 * p[0] - 1.5 * p[2], y: -4 * p[1] + p[2] }), () => 0.01)
    expect(tiny.length).toBe(1)
    const top = boxSizeLabels([0, 0, 0], [2, 1, 3], (p) => ({ x: 160 * p[0], y: 160 * p[2] }), () => 0.01, 1)
    expect(top.map((l) => l.text)).toEqual(['2 m', '3 m'])
    const b = makeBox('b', [0, 0, 0], [2, 1, 3])
    const f = b.faces.find((x) => x.loop.every((i) => b.vertices[i][1] === 1))!
    expect(faceSizeLabels(b, f, () => 0.01).map((l) => l.text).sort()).toEqual(['2 m', '3 m'])
  })
})
