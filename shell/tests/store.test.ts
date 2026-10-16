import { describe, expect, it } from 'vitest'
import { emptyDocument, findEntity, MapDocument } from '../src/model/document'
import { addBox, addBoxTo, addPointEntity, deleteSelection, pivotOf, setBrushOp, setCarve, setClassname, setKeyvalue, translateSelection } from '../src/state/actions'
import { makeApp } from './helpers'
import { makeBox } from '../src/model/geometry'

describe('Store: editing rules', () => {
  it('is read-only until Core has synced, and while disconnected', () => {
    const { store, sock } = makeApp()
    addBox(store, [0, 0, 0])
    expect(store.state.doc.entities).toHaveLength(0)
    expect(store.state.message).toMatch(/not connected/i)
    sock().open() // connected but no document yet
    addBox(store, [0, 0, 0])
    expect(store.state.doc.entities).toHaveLength(0)
    sock().receive({ type: 'document_sync', rev: 1, path: 'm.json', doc: emptyDocument() })
    addBox(store, [0, 0, 0])
    expect(store.state.doc.entities).toHaveLength(1)
    sock().close()
    expect(store.canEdit).toBe(false)
  })

  it('applies edits locally at once, sends them to Core, and supports undo/redo', () => {
    const { store, sock, sync } = makeApp(); sync()
    addBox(store, [0, 1, 0])
    expect(store.state.doc.entities[0].classname).toBe('worldspawn')
    expect(store.state.selection).toMatchObject({ entityId: 'e_worldspawn' })
    expect(sock().updates()).toHaveLength(1)

    store.undo()
    expect(store.state.doc.entities).toHaveLength(0)
    store.redo()
    expect(store.state.doc.entities).toHaveLength(1)
    expect(store.state.history.map((h) => h.label)).toEqual(['Loaded', 'Add box'])
  })

  it('a new edit after undo discards the redo branch', () => {
    const { store, sync } = makeApp(); sync()
    addBox(store, [0, 0, 0]); addBox(store, [3, 0, 0])
    store.undo()
    addPointEntity(store, 'light', [1, 1, 1])
    expect(store.state.history.map((h) => h.label)).toEqual(['Loaded', 'Add box', 'Add light'])
    store.redo()
    expect(store.state.historyIndex).toBe(2)
  })

  it('a drag shows and sends every step but creates ONE history entry', () => {
    const { store, sock, sync } = makeApp(); sync()
    addBox(store, [0, 0, 0])
    const sel = store.state.selection!
    const before = store.state.history.length
    expect(store.beginDrag('Move')).toBe(true)
    for (let i = 1; i <= 5; i++) store.dragUpdate((start) => translateSelection(start, sel, [i, 0, 0]))
    store.endDrag()
    expect(store.state.history).toHaveLength(before + 1)
    const b = findEntity(store.state.doc, sel.entityId)!.brushes![0]
    expect(b.vertices[0][0]).toBeCloseTo(4.5) // total = 5, not 1+2+3+4+5 (steps are relative to the drag start)
    expect(sock().updates().length).toBeGreaterThan(0)
    store.undo()
    expect(findEntity(store.state.doc, sel.entityId)!.brushes![0].vertices[0][0]).toBeCloseTo(-0.5)
  })

  it('a rejected update rolls back to the last document Core accepted', () => {
    const { store, sock, sync } = makeApp(); sync()
    addBox(store, [0, 0, 0])
    sock().receive({ type: 'update_ack', rev: 2 })
    addBox(store, [5, 0, 0]) // this one Core will refuse
    expect(store.state.doc.entities[0].brushes).toHaveLength(2)
    sock().receive({ type: 'error', message: 'worldspawn brushes must be convex' })
    expect(store.state.doc.entities[0].brushes).toHaveLength(1)
    expect(store.state.message).toContain('convex')
    expect(store.state.history.map((h) => h.label)).toEqual(['Loaded', 'Add box'])
  })

  it('adopts other windows\' documents when idle, but ignores them while our update is in flight', () => {
    const { store, sock, sync } = makeApp(); sync()
    const remote: MapDocument = { ...emptyDocument(), entities: [{ id: 'remote', classname: 'light' }] }
    sock().receive({ type: 'document_sync', rev: 5, path: 'm.json', doc: remote })
    expect(store.state.doc).toEqual(remote)
    expect(store.state.history).toHaveLength(1) // a remote change is a new baseline, not an undo step

    addBox(store, [0, 0, 0]) // now an update is in flight
    const mine = store.state.doc
    sock().receive({ type: 'document_sync', rev: 6, path: 'm.json', doc: emptyDocument() })
    expect(store.state.doc).toBe(mine)
  })

  it('keeps the selection only while its entity/brush still exists', () => {
    const { store, sync } = makeApp(); sync()
    addBox(store, [0, 0, 0])
    expect(store.state.selection).not.toBeNull()
    store.undo()
    expect(store.state.selection).toBeNull()
  })

  it('expands prefab instances from the library, and only recomputes when they change', () => {
    const { store, sock, sync } = makeApp()
    const doc: MapDocument = { ...emptyDocument(), prefabInstances: [{ id: 'pi', file: 'p.json', origin: [10, 0, 0] }] }
    sync(doc)
    const lib = { 'p.json': { ...emptyDocument(), entities: [{ id: 'e', classname: 'light', origin: [1, 0, 0] as [number, number, number] }] } }
    sock().receive({ type: 'prefab_library', prefabs: lib })
    expect(store.state.prefabEntities[0].entity.origin).toEqual([11, 0, 0])
    const before = store.state.prefabEntities
    addBox(store, [0, 0, 0]) // unrelated edit
    expect(store.state.prefabEntities).toBe(before)
  })
})

describe('actions', () => {
  it('addBoxTo makes a valid convex brush in worldspawn', () => {
    const r = addBoxTo(emptyDocument(), [1, 2, 3], 2)
    const b = r.doc.entities[0].brushes![0]
    expect(b.vertices[0]).toEqual([0, 1, 2])
    expect(r.sel.brushId).toBe(b.id)
  })

  it('classname changes are refused when they would break the convexity rule', () => {
    const { store, sync } = makeApp()
    const lPrism = { ...makeBox('l', [0, 0, 0], [1, 1, 1]) }
    // L-shaped brush taken from a func_detail: allowed there, not in worldspawn/func_group
    const l = JSON.parse(JSON.stringify(require_l()))
    sync({ ...emptyDocument(), entities: [{ id: 'd', classname: 'func_detail', brushes: [l] }] })
    setClassname(store, 'd', 'func_group')
    expect(store.state.message).toMatch(/convex/)
    expect(findEntity(store.state.doc, 'd')!.classname).toBe('func_detail')
    setClassname(store, 'd', 'func_door')
    expect(findEntity(store.state.doc, 'd')!.classname).toBe('func_door')
    expect(lPrism.id).toBe('l')
  })

  it('keyvalue / op / carve edits are undoable single steps and no-ops change nothing', () => {
    const { store, sync } = makeApp(); sync()
    addBox(store, [0, 0, 0])
    const { entityId, brushId } = store.state.selection!
    const n = store.state.history.length
    setBrushOp(store, entityId, brushId!, 'add') // already add
    expect(store.state.history).toHaveLength(n)
    setBrushOp(store, entityId, brushId!, 'subtract')
    setCarve(store, entityId, brushId!, 'nonStructural', true)
    setKeyvalue(store, entityId, 'note', 'hi')
    const b = findEntity(store.state.doc, entityId)!.brushes![0]
    expect(b.op).toBe('subtract')
    expect(b.carve).toEqual({ nonStructural: true })
    expect(findEntity(store.state.doc, entityId)!.keyvalues).toEqual({ note: 'hi' })
    setKeyvalue(store, entityId, 'note', null)
    expect(findEntity(store.state.doc, entityId)!.keyvalues).toEqual({})
  })

  it('deleteSelection removes a brush, or a whole entity', () => {
    const { store, sync } = makeApp(); sync()
    addBox(store, [0, 0, 0])
    deleteSelection(store)
    expect(store.state.doc.entities[0].brushes).toHaveLength(0)
    addPointEntity(store, 'light', [0, 1, 0])
    deleteSelection(store)
    expect(store.state.doc.entities.map((e) => e.id)).toEqual(['e_worldspawn'])
  })

  it('pivot: origin for dynamic/point entities, bounds centre for structural brushes', () => {
    const doc: MapDocument = {
      ...emptyDocument(),
      entities: [
        { id: 'w', classname: 'worldspawn', brushes: [makeBox('b', [0, 0, 0], [2, 4, 6])] },
        { id: 'd', classname: 'func_door', origin: [9, 9, 9], brushes: [makeBox('b2', [0, 0, 0], [1, 1, 1])] },
        { id: 'p', classname: 'light', origin: [1, 2, 3] },
      ],
    }
    expect(pivotOf(doc, { entityId: 'w', brushId: 'b' })).toEqual([1, 2, 3])
    expect(pivotOf(doc, { entityId: 'd' })).toEqual([9, 9, 9])
    expect(pivotOf(doc, { entityId: 'p' })).toEqual([1, 2, 3])
    expect(pivotOf(doc, { entityId: 'w' })).toEqual([1, 2, 3]) // structural: centre of its brushes
  })
})

import { readFileSync } from 'node:fs'
function require_l() {
  const v = JSON.parse(readFileSync(new URL('../../testdata/brush_vectors.json', import.meta.url), 'utf8'))
  return v.cases.find((c: any) => c.name.startsWith('L prism where free')).brush
}
