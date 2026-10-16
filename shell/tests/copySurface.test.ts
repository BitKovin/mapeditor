import { describe, expect, it } from 'vitest'
import { findEntity } from '../src/model/document'
import { makeBox } from '../src/model/geometry'
import { projectFace, projectionOf } from '../src/model/uv'
import { addDrawnBrush, copySurface, editProjection } from '../src/state/actions'
import { makeApp } from './helpers'

const E = 'e_worldspawn'
function setup() {
  const app = makeApp(); app.sync()
  addDrawnBrush(app.store, makeBox('b1', [0, 0, 0], [4, 2, 1], 'dev/wall'))
  addDrawnBrush(app.store, makeBox('b2', [10, 0, 0], [14, 2, 1], 'dev/floor'))
  const face = (b: string, id: number) => findEntity(app.store.state.doc, E)!.brushes!.find((x) => x.id === b)!.faces.find((f) => f.id === id)!
  return { ...app, face }
}

describe('copy UV and texture onto another face (right-click)', () => {
  it('copies texture and texturing parameters, recomputes the target uvs, one undo step', () => {
    const { store, face } = setup()
    store.select({ entityId: E, brushId: 'b1', faceId: 1 })
    editProjection(store, 'Scale', () => ({ scale: [3, 5], shift: [0.25, 0.5], rotate: 30 }))
    const n = store.state.history.length
    expect(copySurface(store, { entityId: E, brushId: 'b1', faceId: 1 }, { entityId: E, brushId: 'b2', faceId: 1 })).toBe(true)
    const t = face('b2', 1)
    expect(t.texture).toBe(face('b1', 1).texture)
    expect(projectionOf(t)).toEqual({ scale: [3, 5], shift: [0.25, 0.5], rotate: 30 })
    const brush = findEntity(store.state.doc, E)!.brushes!.find((x) => x.id === 'b2')!
    expect(t.uvs).toEqual(projectFace(brush, t, projectionOf(t)))
    expect(store.state.history).toHaveLength(n + 1)
    store.undo()
    expect(face('b2', 1).texture).toBe('dev/floor')
  })
  it('refuses the same face and missing faces; other faces of the target stay untouched', () => {
    const { store, face } = setup()
    const a = { entityId: E, brushId: 'b1', faceId: 1 }
    expect(copySurface(store, a, a)).toBe(false)
    expect(copySurface(store, a, { entityId: E, brushId: 'nope', faceId: 1 })).toBe(false)
    copySurface(store, a, { entityId: E, brushId: 'b2', faceId: 1 })
    expect(face('b2', 2).texture).toBe('dev/floor')
  })
})

import { flowTexture } from '../src/state/actions'
describe('flow texture action', () => {
  it('flows across a shared edge of one brush, one undo step; other brushes are refused with a message', () => {
    const { store, face } = setup()
    const n = store.state.history.length
    // faces 1 and 2 of a box share an edge in this test brush layout? find any two neighbours instead of assuming ids
    const b = findEntity(store.state.doc, E)!.brushes!.find((x) => x.id === 'b1')!
    const key = (f: { loop: number[] }) => f.loop
    const a = b.faces[0], nb = b.faces.find((f) => f.id !== a.id && key(f).filter((v) => key(a).includes(v)).length === 2)!
    expect(flowTexture(store, { entityId: E, brushId: 'b1', faceId: a.id }, { entityId: E, brushId: 'b1', faceId: nb.id })).toBe('')
    expect(store.state.history).toHaveLength(n + 1)
    expect(face('b1', nb.id).uvs).toBeTruthy()
    expect(flowTexture(store, { entityId: E, brushId: 'b1', faceId: a.id }, { entityId: E, brushId: 'b2', faceId: 1 })).toMatch(/same brush/)
    expect(flowTexture(store, { entityId: E, brushId: 'b1', faceId: a.id }, { entityId: E, brushId: 'b1', faceId: a.id })).toMatch(/different face/)
  })
})

import { liftMaterial } from '../src/state/actions'
describe('lift material', () => {
  it('returns the texture of the face, or null', () => {
    const { store } = setup()
    expect(liftMaterial(store.state.doc, E, 'b2', 1)).toBe('dev/floor')
    expect(liftMaterial(store.state.doc, E, 'b2', 99)).toBeNull()
    expect(liftMaterial(store.state.doc, 'nope', 'b2', 1)).toBeNull()
  })
})
