// Right-click additions (REQUIREMENTS §24 C4): hide / unhide, group / ungroup, copy / paste UV.
import { describe, expect, it } from 'vitest'
import { classOf, findEntity } from '../src/model/document'
import { makeBox } from '../src/model/geometry'
import { brushKey, entityKey, hideKeys, visibleEntities } from '../src/model/visibility'
import { addDrawnBrushes, copyUv, groupSelection, hideSelection, pasteUv, unhideAll, ungroupSelection } from '../src/state/actions'
import { makeApp } from './helpers'

const setup = () => {
  const app = makeApp(); app.sync()
  addDrawnBrushes(app.store, [makeBox('b1', [0, 0, 0], [1, 1, 1], 'dev/wall'), makeBox('b2', [2, 0, 0], [3, 1, 1], 'dev/floor')])
  return app
}
const world = (app: ReturnType<typeof setup>) => app.store.state.doc.entities.find((e) => classOf(e) === 'worldspawn')!

describe('hide / unhide', () => {
  it('visibleEntities drops hidden brushes and entities, keeps the rest untouched', () => {
    const e = { id: 'w', classname: 'worldspawn', brushes: [makeBox('a', [0, 0, 0], [1, 1, 1]), makeBox('b', [0, 0, 0], [1, 1, 1])] }
    const p = { id: 'p', classname: 'info_notnull' }
    expect(visibleEntities([e, p], [])[0]).toBe(e)
    const v = visibleEntities([e, p], [brushKey('w', 'a'), entityKey('p')])
    expect(v).toHaveLength(1); expect(v[0].brushes!.map((b) => b.id)).toEqual(['b'])
    expect(visibleEntities([e], [brushKey('w', 'a'), brushKey('w', 'b')])).toHaveLength(0)
    expect(hideKeys([{ entityId: 'w', brushId: 'a' }, { entityId: 'p' }, { entityId: '', instanceId: 'i' }])).toEqual([brushKey('w', 'a'), entityKey('p'), 'i:i'])  // a prefab instance hides by its own key
  })
  it('Hide hides the selection (not saved, not an undo step), Unhide all brings it back', () => {
    const app = setup()
    const w = world(app)
    app.store.select({ entityId: w.id, brushId: 'b1' })
    const steps = app.store.state.history.length
    expect(hideSelection(app.store)).toBe(1)
    expect(app.store.state.hidden).toEqual([brushKey(w.id, 'b1')])
    expect(app.store.state.selection).toBeNull()
    expect(app.store.state.history.length).toBe(steps)
    expect(world(app).brushes!.map((b) => b.id)).toContain('b1')  // still in the map
    unhideAll(app.store)
    expect(app.store.state.hidden).toEqual([])
  })
})

describe('group / ungroup', () => {
  it('Group puts the selected brushes into a func_group; Ungroup moves them back into the world', () => {
    const app = setup()
    const w = world(app)
    app.store.selectMany([{ entityId: w.id, brushId: 'b1' }, { entityId: w.id, brushId: 'b2' }])
    expect(groupSelection(app.store)).toBe(true)
    const g = app.store.state.doc.entities.find((e) => classOf(e) === 'func_group')!
    expect(g.brushes!.map((b) => b.id).sort()).toEqual(['b1', 'b2'])
    app.store.select({ entityId: g.id, brushId: 'b1' })
    expect(ungroupSelection(app.store)).toBe(true)
    expect(app.store.state.doc.entities.some((e) => classOf(e) === 'func_group' && e.brushes?.length)).toBe(false)
    expect(world(app).brushes!.map((b) => b.id).sort()).toEqual(['b1', 'b2'])
  })
  it('Ungroup without a func_group selected says so', () => {
    const app = setup()
    app.store.select({ entityId: world(app).id, brushId: 'b1' })
    expect(ungroupSelection(app.store)).toBe(false)
  })
})

describe('copy / paste UV', () => {
  it('copies one face; pastes its texture and mapping onto every selected face as one undo step', () => {
    const app = setup()
    const w = world(app)
    const face0 = world(app).brushes!.find((b) => b.id === 'b1')!.faces[0].id
    expect(copyUv(app.store)).toBe(false)  // nothing selected
    app.store.select({ entityId: w.id, brushId: 'b1', faceId: face0 })
    expect(copyUv(app.store)).toBe(true)
    app.store.select({ entityId: w.id, brushId: 'b2' })
    const steps = app.store.state.history.length
    expect(pasteUv(app.store)).toBe(6)
    expect(app.store.state.history.length).toBe(steps + 1)
    const b2 = findEntity(app.store.state.doc, w.id)!.brushes!.find((b) => b.id === 'b2')!
    expect(b2.faces.every((f) => f.texture === 'dev/wall')).toBe(true)
  })
})
