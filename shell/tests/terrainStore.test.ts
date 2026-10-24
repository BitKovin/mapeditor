import { describe, expect, it } from 'vitest'
import { emptyDocument } from '../src/model/document'
import { TerrainStroke, terrainOf } from '../src/model/terrain'
import { addTerrain, deleteSelection } from '../src/state/actions'
import { makeApp } from './helpers'

function app() {
  const a = makeApp()
  a.sync(emptyDocument())
  let rev = 1
  ;(a as unknown as { ack: () => void }).ack = () => a.sock().receive({ type: 'update_ack', rev: ++rev })
  a.store.setTerrain({ create: { sizeX: 16, sizeZ: 16, cell: 1, tile: 8, thickness: 4 } })
  return a
}
const ack = (a: ReturnType<typeof app>) => (a as unknown as { ack: () => void }).ack()
const terrain = (a: ReturnType<typeof app>) => a.store.state.doc.entities.find((e) => e.terrain !== undefined)!

describe('the terrain in the store', () => {
  it('Create terrain: one undo step, selected, Terrain tool on, sent to Core', () => {
    const a = app()
    expect(addTerrain(a.store, [0, 0, 0])).toBe(true)
    const e = terrain(a)
    expect(e.classname).toBe('func_detail')
    expect(e.brushes).toHaveLength(4)
    expect(a.store.state.tool).toBe('terrain')
    expect(a.store.state.selection?.entityId).toBe(e.id)
    expect(a.store.state.history.at(-1)!.label).toBe('Add terrain')
    expect(a.sock().updates().at(-1).doc.entities[0].terrain).toEqual({ cols: 16, rows: 16, cell: 1, tile: 8 })
    a.store.undo()
    expect(a.store.state.doc.entities).toHaveLength(0)
    a.store.redo()
    expect(terrainOf(terrain(a))).not.toBeNull()
  })

  it('a refused size leaves the document alone and says why', () => {
    const a = app()
    a.store.setTerrain({ create: { sizeX: 5000 } })
    expect(addTerrain(a.store, [0, 0, 0])).toBe(false)
    expect(a.store.state.doc.entities).toHaveLength(0)
    expect(a.store.state.message).toMatch(/Too many cells/)
  })

  it('a stroke is ONE undo step; Core is sent the final document once (and not on every mouse move)', () => {
    const a = app()
    addTerrain(a.store, [0, 0, 0]); ack(a)
    const e0 = terrain(a), sent0 = a.sock().updates().length, hist0 = a.store.state.history.length
    const hf0 = terrainOf(e0)!
    const stroke = new TerrainStroke(e0, hf0, 'raise', { radius: 3, strength: 1, falloff: 0.5 })
    expect(a.store.beginDrag('Terrain: raise')).toBe(true)
    for (let x = -6; x <= 6; x += 0.5) { stroke.moveTo(x, 0); a.store.dragUpdate((start) => stroke.document(start), false) }
    // shown locally, nothing sent yet
    expect(Math.max(...terrainOf(terrain(a))!.h)).toBeGreaterThan(hf0.h[0])
    expect(a.sock().updates().length).toBe(sent0)
    expect(a.store.state.history.length).toBe(hist0)
    a.store.endDrag()
    expect(a.sock().updates().length).toBe(sent0 + 1)
    expect(a.store.state.history.length).toBe(hist0 + 1)
    expect(a.store.state.history.at(-1)!.label).toBe('Terrain: raise')
    // the terrain survived the store's reconcile pass and Core got the same heights
    const sent = a.sock().updates().at(-1).doc.entities[0]
    expect(sent.brushes.map((b: { id: string }) => b.id)).toEqual(terrain(a).brushes!.map((b) => b.id))
    expect(terrainOf(terrain(a))).not.toBeNull()
    const top = Math.max(...terrainOf(terrain(a))!.h)
    a.store.undo()
    expect(Math.max(...terrainOf(terrain(a))!.h)).toBe(Math.max(...hf0.h))
    a.store.redo()
    expect(Math.max(...terrainOf(terrain(a))!.h)).toBe(top)
  })

  it('submitCurrent sends the working document in between (the throttled updates of a long stroke)', () => {
    const a = app()
    addTerrain(a.store, [0, 0, 0]); ack(a)
    const e0 = terrain(a), stroke = new TerrainStroke(e0, terrainOf(e0)!, 'raise', { radius: 2, strength: 1, falloff: 0 })
    a.store.beginDrag('Terrain: raise')
    stroke.moveTo(0, 0)
    a.store.dragUpdate((start) => stroke.document(start), false)
    const n = a.sock().updates().length
    a.store.submitCurrent()
    expect(a.sock().updates().length).toBe(n + 1)
    a.store.submitCurrent() // nothing new to send
    expect(a.sock().updates().length).toBe(n + 1)
    a.store.endDrag()
    expect(a.sock().updates().length).toBe(n + 1) // endDrag has nothing unsent either
  })

  it('Esc: cancelling a stroke puts the ground back and adds no history', () => {
    const a = app()
    addTerrain(a.store, [0, 0, 0])
    const e0 = terrain(a), hist0 = a.store.state.history.length
    const stroke = new TerrainStroke(e0, terrainOf(e0)!, 'raise', { radius: 2, strength: 1, falloff: 0 })
    a.store.beginDrag('Terrain: raise')
    stroke.moveTo(0, 0)
    a.store.dragUpdate((start) => stroke.document(start), false)
    a.store.cancelDrag()
    expect(a.store.state.doc.entities[0]).toBe(e0)
    expect(a.store.state.history.length).toBe(hist0)
  })

  it('deleting the selected terrain removes every tile', () => {
    const a = app()
    addTerrain(a.store, [0, 0, 0])
    deleteSelection(a.store)
    expect(a.store.state.doc.entities).toHaveLength(0)
  })

  it('paint through the store changes only textures', () => {
    const a = app()
    addTerrain(a.store, [0, 0, 0])
    const e0 = terrain(a), stroke = new TerrainStroke(e0, terrainOf(e0)!, 'paint', { radius: 2, strength: 1, falloff: 0 }, { texture: 'dev/rock' })
    a.store.beginDrag('Terrain: paint')
    stroke.moveTo(-8 + 1, -8 + 1)
    a.store.dragUpdate((start) => stroke.document(start), false)
    a.store.endDrag()
    const e1 = terrain(a)
    expect(e1.brushes![0].faces.some((f) => f.texture === 'dev/rock')).toBe(true)
    expect(e1.brushes![0].vertices).toBe(e0.brushes![0].vertices)
    expect(e1.brushes![3]).toBe(e0.brushes![3])
  })
})
