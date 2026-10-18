import { describe, expect, it } from 'vitest'
import { emptyDocument, MapDocument } from '../src/model/document'
import { addBoxTo } from '../src/state/actions'
import { buildClipboard, parseClipboard, pasteInto, serializeClipboard, copySelection, pasteClipboard, cutSelection } from '../src/state/clipboard'
import { makeApp } from './helpers'

function docWithBox() {
  const r = addBoxTo(emptyDocument(), [0, 0, 0], 2)
  return { doc: r.doc, sel: r.sel }
}

describe('clipboard', () => {
  it('round-trips a brush through JSON and pastes it with fresh ids, offset', () => {
    const { doc, sel } = docWithBox()
    const p = buildClipboard(doc, [sel])!
    const back = parseClipboard(serializeClipboard(p))!
    const r = pasteInto(doc, back, [4, 0, 0])
    const brushes = r.doc.entities[0].brushes!
    expect(brushes.length).toBe(2)
    expect(brushes[1].id).not.toBe(brushes[0].id)
    expect(brushes[1].vertices[0][0]).toBeCloseTo(brushes[0].vertices[0][0] + 4)
    expect(r.sels).toEqual([{ entityId: r.doc.entities[0].id, brushId: brushes[1].id }])
  })

  it('copies whole entities and prefab instances; pasting makes new ones', () => {
    const doc: MapDocument = { ...emptyDocument(), entities: [{ id: 'e1', classname: 'light', origin: [1, 2, 3] }], prefabInstances: [{ id: 'p1', file: 'p/door.map.json', origin: [0, 0, 0] }] }
    const p = buildClipboard(doc, [{ entityId: 'e1' }, { entityId: '', instanceId: 'p1' }])!
    const r = pasteInto(doc, p, [1, 0, 0])
    expect(r.doc.entities.length).toBe(2)
    expect(r.doc.entities[1].origin).toEqual([2, 2, 3])
    expect(r.doc.prefabInstances.map((x) => x.name)).toEqual([undefined, 'door2'])
  })

  it('rejects text that is not editor clipboard JSON', () => {
    expect(parseClipboard('hello')).toBeNull()
    expect(parseClipboard('{"a":1}')).toBeNull()
    expect(parseClipboard(JSON.stringify({ format: 'level-editor/clipboard', version: 99, items: [] }))).toBeNull()
  })

  it('copy writes JSON to the OS clipboard; paste reads it back as one undo step; cut removes', async () => {
    let text = ''
    Object.defineProperty(globalThis, 'navigator', { configurable: true, value: { clipboard: { writeText: async (t: string) => { text = t }, readText: async () => text } } })
    const a = makeApp()
    const { doc, sel } = docWithBox()
    a.sync(doc)
    a.store.select(sel)
    expect(await copySelection(a.store)).toBe(true)
    expect(JSON.parse(text).format).toBe('level-editor/clipboard')
    expect(await pasteClipboard(a.store)).toBe(true)
    expect(a.store.state.doc.entities[0].brushes!.length).toBe(2)
    a.store.undo()
    expect(a.store.state.doc.entities[0].brushes!.length).toBe(1)
    a.store.select(sel)
    expect(await cutSelection(a.store)).toBe(true)
    expect(a.store.state.doc.entities[0].brushes!.length).toBe(0)
  })
})
