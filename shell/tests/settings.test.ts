import { describe, expect, it } from 'vitest'
import { emptyDocument } from '../src/model/document'
import { msg, parseInbound } from '../src/net/protocol'
import { addBox } from '../src/state/actions'
import { fileNameWithExt, joinPath, parentDir } from '../src/ui/fileDialog'
import { makeApp } from './helpers'

const classDef = (name: string, kind: 'point' | 'group', color: [number, number, number] = [1, 0, 0]) =>
  ({ name, kind, color, box: null, description: '', keys: [], flags: [] })

describe('protocol: M2.6 messages', () => {
  it('parses entity_defs, dropping malformed classes, and settings', () => {
    const m = parseInbound(JSON.stringify({ type: 'entity_defs', classes: [classDef('light', 'point'), { name: 5 }, { name: 'x', kind: 'weird' }], warnings: ['w'] }))
    expect(m).toMatchObject({ type: 'entity_defs', warnings: ['w'] })
    expect((m as any).classes.map((c: any) => c.name)).toEqual(['light'])
    expect(parseInbound('{"type":"entity_defs","classes":5}')).toBeNull()
    expect(parseInbound('{"type":"settings","assetsRoot":"C:\\\\a"}')).toMatchObject({ assetsRoot: 'C:\\a' })
    expect(parseInbound('{"type":"settings"}')).toBeNull()
  })
  it('builds the new requests', () => {
    expect(JSON.parse(msg.listDirectory(3, 'maps'))).toEqual({ type: 'list_directory', requestId: 3, path: 'maps' })
    expect(JSON.parse(msg.browseFilesystem(4))).toEqual({ type: 'browse_filesystem', requestId: 4 })
    expect(JSON.parse(msg.browseFilesystem(4, 'C:\\x'))).toMatchObject({ path: 'C:\\x' })
    expect(JSON.parse(msg.setAssetsRoot(5, '/a'))).toEqual({ type: 'set_assets_root', requestId: 5, path: '/a' })
  })
})

describe('Store: settings, entity definitions, dirty flag', () => {
  it('keeps the profile, definitions, marker colours and assets root sent by Core', () => {
    const { store, sock } = makeApp(); sock().open()
    sock().receive({ type: 'settings', assetsRoot: 'D:\\game\\assets' })
    sock().receive({ type: 'entity_defs', classes: [classDef('light', 'point', [1, 1, 0]), classDef('dark', 'point', [0, 0, 0]), classDef('func_door', 'group')], warnings: ['w1'] })
    expect(store.state.assetsRoot).toBe('D:\\game\\assets')
    expect(store.state.entityDefs.map((c) => c.name)).toEqual(['light', 'dark', 'func_door'])
    expect(store.state.entityWarnings).toEqual(['w1'])
    expect(store.entityColor('light')).toEqual([1, 1, 0])
    expect(store.entityColor('dark')![0]).toBeGreaterThan(0.35) // near-black markers are lifted so they stay visible
    expect(store.entityColor('unknown')).toBeUndefined()
    sock().receive({ type: 'entity_defs', classes: [], warnings: [] }) // e.g. the .ent file was removed
    expect(store.entityColor('light')).toBeUndefined()
  })

  it('is clean after loading, dirty after an edit, clean again after undo or save', () => {
    const { store, sock, sync } = makeApp(); sync()
    expect(store.state.dirty).toBe(false)
    addBox(store, [0, 1, 0])
    expect(store.state.dirty).toBe(true)
    store.undo()
    expect(store.state.dirty).toBe(false)          // back at the saved document
    store.redo()
    expect(store.state.dirty).toBe(true)
    store.onSaved('maps/a.map.json')
    expect(store.state.dirty).toBe(false)
    expect(store.state.path).toBe('maps/a.map.json')
    addBox(store, [3, 1, 0])
    expect(store.state.dirty).toBe(true)
    void sock
  })

  it('an edit made while a save was in flight stays dirty', () => {
    const { store, sync } = makeApp(); sync()
    addBox(store, [0, 1, 0])
    const snapshot = store.state.doc
    addBox(store, [4, 1, 0])                       // edited after the save request went out
    store.onSaved('m.map.json', snapshot)
    expect(store.state.dirty).toBe(true)
  })

  it('a document Core reports as dirty (edited from another window) is dirty here too', () => {
    const { store, sock } = makeApp(); sock().open()
    sock().receive({ type: 'document_sync', rev: 3, path: 'm.json', doc: emptyDocument(), dirty: true })
    expect(store.state.dirty).toBe(true)
    sock().receive({ type: 'document_sync', rev: 4, path: 'm.json', doc: emptyDocument(), dirty: false })
    expect(store.state.dirty).toBe(false)
  })
})

describe('connection: whenIdle', () => {
  it('resolves immediately when nothing is in flight, and after the ack otherwise', async () => {
    const { store, conn, sock, sync } = makeApp(); sync()
    await conn.whenIdle()
    addBox(store, [0, 1, 0])
    addBox(store, [3, 1, 0])                       // second edit waits in the pending slot
    let idle = false
    void conn.whenIdle().then(() => { idle = true })
    await Promise.resolve()
    expect(idle).toBe(false)
    sock().receive({ type: 'update_ack', rev: 2 })  // first update acked -> pending one is sent
    await Promise.resolve()
    expect(idle).toBe(false)
    sock().receive({ type: 'update_ack', rev: 3 })
    await Promise.resolve(); await Promise.resolve()
    expect(idle).toBe(true)
  })
  it('a rejected update also releases waiters', async () => {
    const { store, conn, sock, sync } = makeApp(); sync()
    addBox(store, [0, 1, 0])
    let idle = false
    void conn.whenIdle().then(() => { idle = true })
    sock().receive({ type: 'error', message: 'nope' })
    await Promise.resolve(); await Promise.resolve()
    expect(idle).toBe(true)
  })
})

describe('file dialog helpers', () => {
  it('joins and splits assets-relative paths', () => {
    expect(joinPath('', 'a.map.json')).toBe('a.map.json')
    expect(joinPath('maps/sub', 'a.map.json')).toBe('maps/sub/a.map.json')
    expect(joinPath('maps/', 'a')).toBe('maps/a')
    expect(parentDir('maps/sub')).toBe('maps')
    expect(parentDir('maps')).toBe('')
    expect(parentDir('')).toBe('')
  })
  it('adds the extension once and refuses names that could leave the folder', () => {
    expect(fileNameWithExt('level', '.map.json')).toBe('level.map.json')
    expect(fileNameWithExt('level.map.json', '.map.json')).toBe('level.map.json')
    expect(fileNameWithExt('LEVEL.MAP.JSON', '.map.json')).toBe('LEVEL.MAP.JSON')
    expect(fileNameWithExt('  spaced  ', '.map.json')).toBe('spaced.map.json')
    for (const bad of ['', '  ', '..', '.', '../x', 'a/b', 'a\\b', 'c:x', '.hidden', 'a*b', 'a?b']) expect(fileNameWithExt(bad, '.map.json')).toBeNull()
  })
})
