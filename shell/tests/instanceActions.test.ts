import { readFileSync } from 'node:fs'
import { describe, expect, it } from 'vitest'
import { emptyDocument, MapDocument } from '../src/model/document'
import {
  deleteSelection, duplicateSelection, pivotOf, renameInstance, revertOverride, setInstanceVec, setOverrideField, setOverrideKeyvalue, spawnInstance, translateSelection,
} from '../src/state/actions'
import { selectionTargets, transformSelections } from '../src/model/transform'
import { makeApp } from './helpers'

function app(doc: MapDocument = emptyDocument()) {
  const a = makeApp()
  a.sync(doc)
  return a
}
const insts = (s: ReturnType<typeof app>['store']) => s.state.doc.prefabInstances

describe('prefab instance actions (N1 c/d)', () => {
  it('spawn gives unique generated names, selects the new instance, one undo step each', () => {
    const { store } = app()
    const a = spawnInstance(store, 'prefabs/door.map.json', [1, 0, 0])!
    const b = spawnInstance(store, 'prefabs/door.map.json', [2, 0, 0])!
    expect(insts(store).map((p) => p.name)).toEqual(['door', 'door2'])
    expect(store.state.selection).toEqual({ entityId: '', instanceId: b })
    store.undo()
    expect(insts(store).map((p) => p.id)).toEqual([a])
  })

  it('rename enforces uniqueness and legal characters, and says why', () => {
    const { store } = app()
    const a = spawnInstance(store, 'p/door.map.json', [0, 0, 0])!
    spawnInstance(store, 'p/door.map.json', [0, 0, 0])
    expect(renameInstance(store, a, 'door2')).toBe(false)
    expect(store.state.message).toMatch(/already called/)
    expect(renameInstance(store, a, 'a|b')).toBe(false)
    expect(renameInstance(store, a, 'front')).toBe(true)
    expect(insts(store)[0].name).toBe('front')
  })

  it('translate / delete / duplicate work on an instance selection', () => {
    const { store } = app()
    const id = spawnInstance(store, 'p/door.map.json', [1, 2, 3])!
    const sel = { entityId: '', instanceId: id }
    expect(pivotOf(store.state.doc, sel)).toEqual([1, 2, 3])
    store.edit('mv', (d) => translateSelection(d, sel, [1, 0, 0]))
    expect(insts(store)[0].origin).toEqual([2, 2, 3])
    duplicateSelection(store)
    expect(insts(store).length).toBe(2)
    expect(insts(store)[1].name).toBe('door2')
    deleteSelection(store)
    expect(insts(store).length).toBe(1)
  })

  it('rotating / scaling a selected instance writes angles / scale; grab only moves the origin', () => {
    const doc = emptyDocument()
    doc.prefabInstances = [{ id: 'i', file: 'p/d.map.json', origin: [0, 0, 0] }]
    const sels = [{ entityId: '', instanceId: 'i' }]
    const yaw90 = (p: [number, number, number]): [number, number, number] => [p[2], p[1], -p[0]] // Ry(90): (x,y,z) -> (z,y,-x)
    const r = transformSelections(doc, sels, yaw90, false, 90, true)
    expect(r.prefabInstances[0].angles![1]).toBeCloseTo(90, 6)
    const s = transformSelections(doc, sels, (p) => [p[0] * 2, p[1] * 2, p[2] * 2], false, 0, true)
    expect(s.prefabInstances[0].scale).toEqual([2, 2, 2])
    expect(selectionTargets(doc, sels).instances.has('i')).toBe(true)
  })

  it('setInstanceVec edits one axis; scale 0 is refused', () => {
    const { store } = app()
    const id = spawnInstance(store, 'p/d.map.json', [0, 0, 0])!
    setInstanceVec(store, id, 'angles', 1, 45)
    setInstanceVec(store, id, 'scale', 0, 2)
    expect(insts(store)[0].angles).toEqual([0, 45, 0])
    expect(insts(store)[0].scale).toEqual([2, 1, 1])
    expect(setInstanceVec(store, id, 'scale', 1, 0)).toBe(false)
    expect(store.state.message).toMatch(/Scale/)
  })

  it('overrides: set, revert one key, prune empties, and delete / restore an entity', () => {
    const { store } = app()
    const id = spawnInstance(store, 'p/d.map.json', [0, 0, 0])!
    setOverrideKeyvalue(store, id, 'lf/a', 'speed', '9')
    setOverrideKeyvalue(store, id, 'lf/a', 'wait', '2')
    expect(insts(store)[0].overrides!['lf/a'].keyvalues).toEqual({ speed: '9', wait: '2' })
    setOverrideKeyvalue(store, id, 'lf/a', 'speed', null)
    expect(insts(store)[0].overrides!['lf/a'].keyvalues).toEqual({ wait: '2' })
    setOverrideField(store, id, 'lf/a', 'deleted', true)
    expect(insts(store)[0].overrides!['lf/a'].deleted).toBe(true)
    setOverrideField(store, id, 'lf/a', 'deleted', false)
    setOverrideKeyvalue(store, id, 'lf/a', 'wait', null)
    expect(insts(store)[0].overrides).toBeUndefined() // nothing left -> no empty shell in the file
    setOverrideField(store, id, 'b', 'origin', [1, 2, 3])
    expect(insts(store)[0].overrides!.b.origin).toEqual([1, 2, 3])
    revertOverride(store, id, 'b')
    expect(insts(store)[0].overrides).toBeUndefined()
  })
})

import { setEntityRef, setOverrideRef } from '../src/state/actions'
describe('references (N1 e)', () => {
  it('setEntityRef writes and clears refs on a document entity (no empty shell left)', () => {
    const doc = emptyDocument()
    doc.entities = [{ id: 'b', classname: 'func_button', origin: [0, 0, 0] }]
    const { store } = app(doc)
    setEntityRef(store, 'b', 'target', 'door_frame/d')
    expect(store.state.doc.entities[0].refs).toEqual({ target: 'door_frame/d' })
    setEntityRef(store, 'b', 'killtarget', 'x')
    setEntityRef(store, 'b', 'target', null)
    expect(store.state.doc.entities[0].refs).toEqual({ killtarget: 'x' })
    setEntityRef(store, 'b', 'killtarget', null)
    expect(store.state.doc.entities[0].refs).toBeUndefined()
  })
  it('setOverrideRef stores the override and reverting prunes the empty override', () => {
    const { store } = app()
    const id = spawnInstance(store, 'p/d.map.json', [0, 0, 0])!
    setOverrideRef(store, id, 'lf/btn', 'target', 'w')
    expect(insts(store)[0].overrides!['lf/btn'].refs).toEqual({ target: 'w' })
    setOverrideRef(store, id, 'lf/btn', 'target', null)
    expect(insts(store)[0].overrides).toBeUndefined()
  })
  it('the pipette hands the picked path to the armed field once, and Esc-style null cancels', () => {
    const { store } = app()
    const got: (string | null)[] = []
    store.setPipette((p) => got.push(p))
    expect(store.state.pipette).not.toBeNull()
    const cb = store.state.pipette!
    store.setPipette(null); cb('a/b')
    expect(got).toEqual(['a/b'])
    expect(store.state.pipette).toBeNull()
  })
})

import { clipSelection, flipSelection, hollowSelection, mergeSelection, rotateSelection90, snapSelection } from '../src/state/actions'
import { makeBox, brushBounds, signedVolume } from '../src/model/geometry'
describe('Radiant brush operations on a selection', () => {
  const world = (...boxes: ReturnType<typeof makeBox>[]) => {
    const d = emptyDocument()
    d.entities = [{ id: 'w', classname: 'worldspawn', brushes: boxes }]
    return d
  }
  const brushes = (s: ReturnType<typeof app>['store']) => s.state.doc.entities[0].brushes!
  it('clip keeps one side, split keeps both, and each is one undo step', () => {
    const { store } = app(world(makeBox('a', [0, 0, 0], [4, 4, 4])))
    store.select({ entityId: 'w', brushId: 'a' })
    expect(clipSelection(store, { n: [1, 0, 0], d: 1 }, 'back')).toBe(true)
    expect(brushBounds(brushes(store)[0])!.max[0]).toBeCloseTo(1)
    store.undo()
    expect(clipSelection(store, { n: [1, 0, 0], d: 1 }, 'both')).toBe(true)
    expect(brushes(store).length).toBe(2)
    expect(brushes(store).reduce((s, b) => s + signedVolume(b), 0)).toBeCloseTo(64)
    store.undo()
    expect(brushes(store).length).toBe(1)
    expect(clipSelection(store, { n: [1, 0, 0], d: 99 }, 'back')).toBe(false)
    expect(store.state.message).toMatch(/does not cut/)
  })
  it('hollow replaces the brush by walls; a too-thick wall is refused', () => {
    const { store } = app(world(makeBox('a', [0, 0, 0], [4, 4, 4])))
    store.select({ entityId: 'w', brushId: 'a' })
    expect(hollowSelection(store, 3)).toBe(false)
    expect(hollowSelection(store, 1)).toBe(true)
    expect(brushes(store).length).toBe(6)
    expect(brushes(store)[0].id).toBe('a')
  })
  it('merge welds touching boxes into one brush; refuses an L shape', () => {
    const { store } = app(world(makeBox('a', [0, 0, 0], [2, 2, 2]), makeBox('b', [2, 0, 0], [4, 2, 2]), makeBox('c', [0, 2, 0], [2, 4, 2])))
    store.selectMany([{ entityId: 'w', brushId: 'a' }, { entityId: 'w', brushId: 'b' }])
    expect(mergeSelection(store)).toBe(true)
    expect(brushes(store).map((b) => b.id)).toEqual(['a', 'c'])
    expect(signedVolume(brushes(store)[0])).toBeCloseTo(16)
    store.selectMany([{ entityId: 'w', brushId: 'a' }, { entityId: 'w', brushId: 'c' }])
    expect(mergeSelection(store)).toBe(false)
    expect(store.state.message).toMatch(/convex/)
  })
  it('merge accepts overlapping boxes whose hull equals their union, refuses overlap that leaves a gap', () => {
    const { store } = app(world(makeBox('a', [0, 0, 0], [3, 2, 2]), makeBox('b', [2, 0, 0], [5, 2, 2]), makeBox('c', [0, 0, 0], [4, 4, 2]), makeBox('d', [3, 3, 0], [6, 6, 2])))
    store.selectMany([{ entityId: 'w', brushId: 'a' }, { entityId: 'w', brushId: 'b' }])
    expect(mergeSelection(store)).toBe(true)
    expect(signedVolume(brushes(store)[0])).toBeCloseTo(20)
    store.selectMany([{ entityId: 'w', brushId: 'c' }, { entityId: 'w', brushId: 'd' }])
    expect(mergeSelection(store)).toBe(false)
  })
  it('2D resize drag scales the selected brushes between the fixed side and the dragged side, and is one undo step', async () => {
    const { resizeMap, screenAxis } = await import('../src/model/resize2d')
    const { OrthoCamera } = await import('../src/render/camera')
    const { transformSelections } = await import('../src/model/transform')
    const { store } = app(world(makeBox('a', [0, 0, 0], [4, 2, 2]), makeBox('b', [4, 0, 0], [8, 2, 2])))
    store.selectMany([{ entityId: 'w', brushId: 'a' }, { entityId: 'w', brushId: 'b' }])
    const b = new OrthoCamera('front').basis
    const map = resizeMap([0, 0, 0], [8, 2, 2], screenAxis(b.right), screenAxis(b.up), 1, 0, 8, 0, 1) // right side 8 -> 16
    expect(store.beginDrag('Resize')).toBe(true)
    store.dragUpdate((start) => transformSelections(start, [{ entityId: 'w', brushId: 'a' }, { entityId: 'w', brushId: 'b' }], map, false, 0, true))
    store.endDrag()
    expect(brushBounds(brushes(store)[0])!.max[0]).toBeCloseTo(8)
    expect(brushBounds(brushes(store)[1])).toEqual({ min: [8, 0, 0], max: [16, 2, 2] })
    store.undo()
    expect(brushBounds(brushes(store)[1])!.max[0]).toBeCloseTo(8)
  })
  it('flip mirrors about the selection centre and keeps the brush outward-wound; rotate 90 swaps extents', () => {
    const { store } = app(world(makeBox('a', [0, 0, 0], [4, 2, 2])))
    store.select({ entityId: 'w', brushId: 'a' })
    flipSelection(store, 0)
    expect(brushBounds(brushes(store)[0])).toEqual({ min: [0, 0, 0], max: [4, 2, 2] })
    expect(signedVolume(brushes(store)[0])).toBeGreaterThan(0)
    rotateSelection90(store, 1)
    const b = brushBounds(brushes(store)[0])!
    expect(b.max[0] - b.min[0]).toBeCloseTo(2); expect(b.max[2] - b.min[2]).toBeCloseTo(4)
  })
  it('snap moves vertices onto the grid and reports', () => {
    const { store } = app(world(makeBox('a', [0.1, 0, 0], [4.02, 4, 4])))
    store.select({ entityId: 'w', brushId: 'a' })
    expect(snapSelection(store, 0.5)).toBe(true)
    expect(brushBounds(brushes(store)[0])!.min[0]).toBe(0)
    expect(snapSelection(store, 0.5)).toBe(false)
    expect(store.state.message).toMatch(/Already/)
  })
})

import { nudgeSelection } from '../src/state/actions'
describe('nudge', () => {
  it('moves brushes and instances together as one undo step', () => {
    const d = emptyDocument()
    d.entities = [{ id: 'w', classname: 'worldspawn', brushes: [makeBox('a', [0, 0, 0], [1, 1, 1])] }]
    d.prefabInstances = [{ id: 'i', file: 'p/x.map.json', origin: [5, 0, 0] }]
    const { store } = app(d)
    store.selectMany([{ entityId: 'w', brushId: 'a' }, { entityId: '', instanceId: 'i' }])
    expect(nudgeSelection(store, [0, 0.5, 0])).toBe(true)
    expect(store.state.doc.prefabInstances[0].origin).toEqual([5, 0.5, 0])
    expect(brushBounds(store.state.doc.entities[0].brushes![0])!.min[1]).toBeCloseTo(0.5)
    store.undo()
    expect(store.state.doc.prefabInstances[0].origin).toEqual([5, 0, 0])
  })
})

import { parseScaleVec, setEntityAngle, setEntityScale } from '../src/state/actions'
describe('entity angles and scale (DESIGN 18.3)', () => {
  it('angles edit one axis; modelscale_vec is written, merged and dropped at 1 1 1', () => {
    const d = emptyDocument()
    d.entities = [{ id: 'm', classname: 'misc_model', origin: [0, 0, 0] }]
    const { store } = app(d)
    setEntityAngle(store, 'm', 1, 90)
    expect(store.state.doc.entities[0].angles).toEqual([0, 90, 0])
    setEntityScale(store, 'm', 0, 2)
    expect(store.state.doc.entities[0].keyvalues!.modelscale_vec).toBe('2 1 1')
    setEntityScale(store, 'm', 0, 1)
    expect(store.state.doc.entities[0].keyvalues!.modelscale_vec).toBeUndefined()
    expect(setEntityScale(store, 'm', 1, 0)).toBe(false)
    expect(parseScaleVec('1 2')).toEqual([1, 1, 1])
    expect(parseScaleVec('1 2 4')).toEqual([1, 2, 4])
  })
})

import { moveBrushToDetail } from '../src/model/document'
describe('concave brushes: solids-only convex rule and auto func_detail (item 4)', () => {
  const lShape = () => {
    const v = JSON.parse(readFileSync(new URL('../../testdata/brush_vectors.json', import.meta.url), 'utf8'))
    return JSON.parse(JSON.stringify(v.cases.find((c: any) => c.name.startsWith('L prism where free')).brush))
  }

  it('a concave SUBTRACTIVE brush is allowed in worldspawn, a concave solid is not', () => {
    const d = emptyDocument()
    d.entities = [{ id: 'w', classname: 'worldspawn', brushes: [{ ...makeBox('a', [0, 0, 0], [1, 1, 1]) }] }]
    const { store } = app(d)
    const ent = store.state.doc.entities[0]
    expect(store.brushProblem(ent, { ...lShape(), op: 'subtract' })).toBeNull()
    expect(store.brushProblem(ent, { ...lShape(), op: 'add' })).toMatch(/convex/)
  })

  it('moveBrushToDetail creates the func_detail once and appends afterwards', () => {
    const d = emptyDocument()
    d.entities = [{ id: 'w', classname: 'worldspawn', brushes: [makeBox('a', [0, 0, 0], [1, 1, 1]), makeBox('b', [2, 0, 0], [3, 1, 1])] }]
    const one = moveBrushToDetail(d, 'w', 'a', d.entities[0].brushes![0], 'det')
    expect(one.entities.map((e) => e.classname)).toEqual(['worldspawn', 'func_detail'])
    expect(one.entities[0].brushes!.map((b) => b.id)).toEqual(['b'])
    const two = moveBrushToDetail(one, 'w', 'b', one.entities[0].brushes![0], 'det')
    expect(two.entities.length).toBe(2)
    expect(two.entities[1].brushes!.map((b) => b.id)).toEqual(['a', 'b'])
  })

  it('a mesh edit that makes a worldspawn brush concave moves it into a new func_detail (when auto-convert is on)', () => {
    const d = emptyDocument()
    d.entities = [{ id: 'w', classname: 'worldspawn', brushes: [makeBox('a', [0, 0, 0], [2, 2, 2])] }]
    const { store } = app(d)
    store.select({ entityId: 'w', brushId: 'a' })
    store.setEditMode('mesh')
    // an op whose result is a valid but CONCAVE brush (the shared L prism, under this brush's id)
    const dent = () => ({ brush: { ...lShape(), id: 'a' }, sel: { verts: [], edges: [], faces: [] } })
    store.setAutoDetail(false)
    const before = JSON.stringify(store.state.doc)
    expect(store.applyMeshOp('Move', dent)).toBe(false)
    expect(JSON.stringify(store.state.doc)).toBe(before)
    expect(store.state.message).toMatch(/convex/)
    store.setAutoDetail(true)
    expect(store.applyMeshOp('Move', dent)).toBe(true)
    const ents = store.state.doc.entities
    expect(ents.length).toBe(2)
    expect(ents[0].brushes!.length).toBe(0)
    expect(ents[1].classname).toBe('func_detail')
    expect(ents[1].brushes![0].id).toBe('a')
    expect(store.state.selection).toEqual({ entityId: ents[1].id, brushId: 'a' })
    expect(store.state.message).toMatch(/func_detail/)
    store.undo()
    expect(store.state.doc.entities.length).toBe(1)
    expect(store.state.doc.entities[0].brushes!.length).toBe(1)
  })
})

import { makeEntityFromSelection, moveSelectionToWorld } from '../src/state/actions'
describe('concave edit DURING a drag (selection follows the brush into func_detail)', () => {
  const lShape = () => {
    const v = JSON.parse(readFileSync(new URL('../../testdata/brush_vectors.json', import.meta.url), 'utf8'))
    return JSON.parse(JSON.stringify(v.cases.find((c: any) => c.name.startsWith('L prism where free')).brush))
  }
  it('the selection is kept (moved to the func_detail) at every step of a drag, and returns when the shape is convex again', () => {
    const d = emptyDocument()
    d.entities = [{ id: 'w', classname: 'worldspawn', brushes: [makeBox('a', [0, 0, 0], [2, 2, 2])] }]
    const { store } = app(d)
    store.select({ entityId: 'w', brushId: 'a' })
    store.setEditMode('mesh')
    expect(store.beginDrag('Move')).toBe(true)
    const concave = { ...lShape(), id: 'a' }
    store.dragUpdate((start) => moveBrushToDetail(start, 'w', 'a', concave, 'det'))
    // the brush is now in func_detail 'det': still selected, still in mesh mode (it used to be dropped here, so the next update reverted)
    expect(store.state.selection).toEqual({ entityId: 'det', brushId: 'a' })
    expect(store.state.editMode).toBe('mesh')
    expect(store.meshTarget()?.entity.id).toBe('det')
    store.dragUpdate((start) => moveBrushToDetail(start, 'w', 'a', concave, 'det'))
    expect(store.state.selection).toEqual({ entityId: 'det', brushId: 'a' })
    // back to a convex shape inside the same drag: the brush is in worldspawn again and the selection follows it back
    store.dragUpdate((start) => start)
    expect(store.state.selection).toEqual({ entityId: 'w', brushId: 'a' })
    store.dragUpdate((start) => moveBrushToDetail(start, 'w', 'a', concave, 'det'))
    store.endDrag()
    expect(store.state.doc.entities.map((e) => e.classname)).toEqual(['worldspawn', 'func_detail'])
    expect(store.state.selection).toEqual({ entityId: 'det', brushId: 'a' })
    store.undo()
    expect(store.state.doc.entities.length).toBe(1)
    expect(store.state.selection).toEqual({ entityId: 'w', brushId: 'a' })
  })
  it('multi-selected brushes follow too, and a deleted brush still drops out', () => {
    const d = emptyDocument()
    d.entities = [{ id: 'w', classname: 'worldspawn', brushes: [makeBox('a', [0, 0, 0], [1, 1, 1]), makeBox('b', [2, 0, 0], [3, 1, 1])] }]
    const { store } = app(d)
    store.selectMany([{ entityId: 'w', brushId: 'a' }, { entityId: 'w', brushId: 'b' }])
    store.edit('x', (doc) => moveBrushToDetail(doc, 'w', 'a', doc.entities[0].brushes![0], 'det'))
    expect(store.state.multi).toEqual([{ entityId: 'det', brushId: 'a' }, { entityId: 'w', brushId: 'b' }])
    store.edit('del', (doc) => ({ ...doc, entities: doc.entities.map((e) => ({ ...e, brushes: e.brushes?.filter((b) => b.id !== 'a') })) }))
    expect(store.state.multi).toEqual([{ entityId: 'w', brushId: 'b' }])
  })
})

describe('brush <-> entity', () => {
  const base = () => {
    const d = emptyDocument()
    d.entities = [{ id: 'w', classname: 'worldspawn', brushes: [makeBox('a', [0, 0, 0], [1, 1, 1]), makeBox('b', [2, 0, 0], [3, 1, 1])] }]
    return d
  }
  it('Make entity moves the selected brushes into one new entity and the selection follows; one undo', () => {
    const { store } = app(base())
    store.selectMany([{ entityId: 'w', brushId: 'a' }, { entityId: 'w', brushId: 'b' }])
    expect(makeEntityFromSelection(store, 'func_group')).toBe(true)
    const ents = store.state.doc.entities
    expect(ents.length).toBe(2)
    expect(ents[0].brushes!.length).toBe(0)
    expect(ents[1].classname).toBe('func_group')
    expect(ents[1].brushes!.map((b) => b.id)).toEqual(['a', 'b'])
    expect(store.state.multi.every((m) => m.entityId === ents[1].id)).toBe(true)
    store.undo()
    expect(store.state.doc.entities.length).toBe(1)
    expect(store.state.doc.entities[0].brushes!.length).toBe(2)
  })
  it('the new entity origin is the centre of the bounding box of ALL selected brushes, not the world origin', () => {
    const d = emptyDocument()
    d.entities = [{ id: 'w', classname: 'worldspawn', brushes: [makeBox('a', [2, 0, 0], [4, 2, 2]), makeBox('b', [6, 0, 4], [10, 6, 8]), makeBox('c', [-50, -50, -50], [-49, -49, -49])] }]
    const { store } = app(d)
    store.selectMany([{ entityId: 'w', brushId: 'a' }, { entityId: 'w', brushId: 'b' }]) // c is not selected and must not count
    expect(makeEntityFromSelection(store, 'func_door')).toBe(true)
    const ent = store.state.doc.entities.find((e) => e.classname === 'func_door')!
    expect(ent.origin).toEqual([6, 3, 4]) // x (2..10), y (0..6), z (0..8)
    expect(ent.brushes!.map((b) => b.vertices.length)).toEqual([8, 8]) // brushes keep their world coordinates
    store.undo()
    expect(store.state.doc.entities.some((e) => e.classname === 'func_door')).toBe(false)
  })
  it('a single brush: origin = its own centre; subtractive brushes count too', () => {
    const d = emptyDocument()
    const cut = { ...makeBox('s', [0, 0, 0], [2, 2, 2]), op: 'subtract' as const }
    d.entities = [{ id: 'w', classname: 'worldspawn', brushes: [makeBox('a', [10, 10, 10], [12, 14, 16]), cut] }]
    const { store } = app(d)
    store.select({ entityId: 'w', brushId: 'a' })
    makeEntityFromSelection(store, 'func_group')
    expect(store.state.doc.entities.find((e) => e.classname === 'func_group')!.origin).toEqual([11, 12, 13])
    store.selectMany([{ entityId: 'w', brushId: 's' }])
    makeEntityFromSelection(store, 'func_detail')
    expect(store.state.doc.entities.find((e) => e.classname === 'func_detail')!.origin).toEqual([1, 1, 1])
  })
  it('concave edit auto-convert: the new func_detail is centred on its brush', () => {
    const d = emptyDocument()
    d.entities = [{ id: 'w', classname: 'worldspawn', brushes: [makeBox('a', [0, 0, 0], [2, 2, 2])] }]
    const moved = moveBrushToDetail(d, 'w', 'a', makeBox('a', [4, 4, 4], [8, 6, 10]), 'det')
    expect(moved.entities.find((e) => e.id === 'det')!.origin).toEqual([6, 5, 7])
  })
  it('Move to world brings brushes back, and refuses when they are already there', () => {
    const { store } = app(base())
    store.select({ entityId: 'w', brushId: 'a' })
    expect(moveSelectionToWorld(store)).toBe(false)
    expect(store.state.message).toMatch(/already/)
    makeEntityFromSelection(store, 'func_group')
    expect(moveSelectionToWorld(store)).toBe(true)
    expect(store.state.doc.entities[0].brushes!.map((b) => b.id).sort()).toEqual(['a', 'b'])
  })
  it('a concave solid cannot become func_group, but can become func_detail', () => {
    const v = JSON.parse(readFileSync(new URL('../../testdata/brush_vectors.json', import.meta.url), 'utf8'))
    const l = JSON.parse(JSON.stringify(v.cases.find((c: any) => c.name.startsWith('L prism where free')).brush))
    const d = emptyDocument()
    d.entities = [{ id: 'x', classname: 'func_detail', brushes: [l] }]
    const { store } = app(d)
    store.select({ entityId: 'x', brushId: l.id })
    expect(makeEntityFromSelection(store, 'func_group')).toBe(false)
    expect(store.state.message).toMatch(/convex/)
    expect(makeEntityFromSelection(store, 'func_detail')).toBe(true)
  })
})

import { angleDelta, hitRing, ringAngle, ringBasis, ringLines } from '../src/render/gizmo'
describe('rotate gizmo maths', () => {
  const ray = (origin: [number, number, number], target: [number, number, number]) => {
    const d = [target[0] - origin[0], target[1] - origin[1], target[2] - origin[2]], l = Math.hypot(...d)
    return { origin, dir: [d[0] / l, d[1] / l, d[2] / l] as [number, number, number] }
  }
  it('ringAngle measures about the axis; angleDelta wraps; hitRing picks the ring the ray crosses', () => {
    const pivot: [number, number, number] = [0, 0, 0]
    // looking down -Y at the ring about Y (basis u=+Z, v=+X): a point on +X is 90 degrees
    expect(ringAngle(ray([0, 5, 0], [0, 0, 1]), pivot, 1)).toBeCloseTo(0)
    expect(ringAngle(ray([0, 5, 0], [1, 0, 0]), pivot, 1)).toBeCloseTo(Math.PI / 2)
    expect(angleDelta(3, -3)).toBeCloseTo(2 * Math.PI - 6)
    expect(hitRing(ray([0, 5, 0], [1, 0, 0]), pivot, 1, 0.1)).toBe(1)
    expect(hitRing(ray([0, 5, 0], [0.5, 0, 0]), pivot, 1, 0.1)).toBeNull()
    expect(ringLines(pivot, 1, 2, 8).length).toBe(8 * 6)
    expect(ringBasis(1)).toEqual([[0, 0, 1], [1, 0, 0]])
  })
})

import { nudgeTexture } from '../src/state/actions'
import { projectionOf } from '../src/model/uv'
describe('texture keys (Radiant)', () => {
  const setup = () => {
    const d = emptyDocument()
    d.entities = [{ id: 'w', classname: 'worldspawn', brushes: [makeBox('a', [0, 0, 0], [2, 2, 2])] }]
    const a = app(d)
    return a.store
  }
  const faces = (s: ReturnType<typeof setup>) => s.state.doc.entities[0].brushes![0].faces
  it('does nothing when no face / brush is selected', () => {
    expect(nudgeTexture(setup(), 'shift', 'left')).toBe(false)
  })
  it('shift: arrows move U / V by the step, on every face of a selected brush; Alt is 8x finer; one undo step', () => {
    const s = setup()
    s.select({ entityId: 'w', brushId: 'a' })
    expect(nudgeTexture(s, 'shift', 'left')).toBe(true)
    expect(faces(s).every((f) => projectionOf(f).shift[0] === 0.125 && projectionOf(f).shift[1] === 0)).toBe(true)
    nudgeTexture(s, 'shift', 'down')
    expect(projectionOf(faces(s)[0]).shift).toEqual([0.125, -0.125])
    nudgeTexture(s, 'shift', 'right', true)
    expect(projectionOf(faces(s)[0]).shift[0]).toBeCloseTo(0.125 - 0.125 / 8)
    expect(faces(s)[0].uvs).toBeTruthy() // uvs are rewritten so Core / export see the result
    s.undo()
    expect(projectionOf(faces(s)[0]).shift[0]).toBeCloseTo(0.125)
  })
  it('a selected face is the only one that changes', () => {
    const s = setup()
    s.select({ entityId: 'w', brushId: 'a', faceId: 3 })
    nudgeTexture(s, 'shift', 'up')
    faces(s).forEach((f) => expect(projectionOf(f).shift[1]).toBe(f.id === 3 ? 0.125 : 0))
  })
  it('scale: right/up grow, left/down shrink by the scale step, per axis; rotate adds / subtracts the step', () => {
    const s = setup()
    s.select({ entityId: 'w', brushId: 'a', faceId: 1 })
    const f = () => projectionOf(faces(s).find((x) => x.id === 1)!)
    nudgeTexture(s, 'scale', 'right')
    expect(f().scale[0]).toBeCloseTo(2 * 1.1); expect(f().scale[1]).toBe(2)
    nudgeTexture(s, 'scale', 'left')
    expect(f().scale[0]).toBeCloseTo(2)
    nudgeTexture(s, 'scale', 'up')
    expect(f().scale[1]).toBeCloseTo(2.2)
    nudgeTexture(s, 'rotate', 'rotcw')
    expect(f().rotate).toBe(15)
    nudgeTexture(s, 'rotate', 'rotccw', true)
    expect(f().rotate).toBeCloseTo(15 - 15 / 8)
  })
  it('step settings change the step; invalid values are ignored', () => {
    const s = setup()
    s.select({ entityId: 'w', brushId: 'a', faceId: 1 })
    s.setTexStep('shift', 0.5); s.setTexStep('shift', -1); s.setTexStep('shift', NaN)
    nudgeTexture(s, 'shift', 'left')
    expect(projectionOf(faces(s).find((x) => x.id === 1)!).shift[0]).toBe(0.5)
  })
})

import { invertSelectionItems, selectAllItems, selectableItems } from '../src/state/actions'
describe('select all / invert (object mode)', () => {
  const setup = () => {
    const d = emptyDocument()
    d.entities = [
      { id: 'w', classname: 'worldspawn', brushes: [makeBox('a', [0, 0, 0], [1, 1, 1]), makeBox('b', [2, 0, 0], [3, 1, 1])] },
      { id: 'g', classname: 'func_group', brushes: [makeBox('c', [5, 0, 0], [6, 1, 1])] },
      { id: 'p', classname: 'info_null', origin: [0, 0, 0] },
    ]
    d.prefabInstances = [{ id: 'i', file: 'p/x.map.json' }]
    return app(d).store
  }
  it('selectable items: every brush, point entity and instance (not worldspawn itself)', () => {
    expect(selectableItems(setup().state.doc).map((s) => s.brushId ?? s.entityId ?? s.instanceId).sort()).toEqual(['', 'a', 'b', 'c', 'p'].sort())
  })
  it('select all selects them all; invert selects the rest, and invert of everything is empty', () => {
    const s = setup()
    expect(selectAllItems(s)).toBe(true)
    expect(s.state.multi.length).toBe(5)
    expect(invertSelectionItems(s)).toBe(false)
    expect(s.state.multi.length).toBe(0)
    expect(s.state.message).toMatch(/Everything was selected/)
    s.select({ entityId: 'w', brushId: 'a' })
    expect(invertSelectionItems(s)).toBe(true)
    expect(s.state.multi.map((m) => m.brushId ?? m.entityId ?? m.instanceId).sort()).toEqual(['', 'b', 'c', 'p'].sort())
  })
  it('a selected whole brush-entity counts as all of its brushes', () => {
    const s = setup()
    s.select({ entityId: 'g' })
    invertSelectionItems(s)
    expect(s.state.multi.some((m) => m.brushId === 'c')).toBe(false)
    expect(s.state.multi.length).toBe(4)
  })
})
