import { describe, expect, it } from 'vitest'
import { Store } from '../src/state/store'
import { LIT_VERTEX_FLOATS } from '../src/render/lightmapStore'

function rig() {
  const sent: any[] = []
  const conn = { submitDocument() {}, updateInFlight: false, hasPendingUpdate: false, sendLighting: (t: string) => sent.push(JSON.parse(t)) }
  return { store: new Store(conn as any), sent }
}
const b64 = (bytes: Uint8Array) => btoa(String.fromCharCode(...bytes))
const mesh = (rev: number) => ({ type: 'lit_mesh' as const, rev, atlas: 8, pageCount: 1, triangleCount: 1, keepPages: false, vertices: b64(new Uint8Array(new Float32Array(3 * LIT_VERTEX_FLOATS).buffer)) })

describe('store: live lighting', () => {
  it('asks Core for lighting when the first viewport wants it and stops when the last one leaves', () => {
    const { store, sent } = rig()
    const a = {}, b = {}
    store.setLightingWanted(a, true)
    store.setLightingWanted(b, true)   // a second viewport changes nothing
    store.setLightingWanted(a, false)  // one still wants it
    expect(sent).toEqual([{ type: 'lighting', enabled: true }])
    store.setLightingWanted(b, false)
    expect(sent[1]).toEqual({ type: 'lighting', enabled: false })
    expect(sent.length).toBe(2)
  })

  it('re-sends the wish after a reconnect', () => {
    const { store, sent } = rig()
    store.setLightingWanted({}, true)
    sent.length = 0
    store.setStatus('disconnected')
    store.setStatus('connected')
    expect(sent).toEqual([{ type: 'lighting', enabled: true }])
  })

  it('reports the camera only while lighting is wanted and only when it moved or a second passed', () => {
    const { store, sent } = rig()
    store.reportCamera([0, 0, 0], 0)
    expect(sent).toEqual([])
    store.setLightingWanted({}, true)
    sent.length = 0
    store.reportCamera([0, 0, 0], 1000)
    store.reportCamera([0.2, 0, 0], 1100)   // too close, too soon
    store.reportCamera([5, 0, 0], 1200)     // moved
    store.reportCamera([5, 0, 0.1], 2500)   // a second passed
    expect(sent).toEqual([{ type: 'lighting', camera: [0, 0, 0] }, { type: 'lighting', camera: [5, 0, 0] }, { type: 'lighting', camera: [5, 0, 0.1] }])
  })

  it('keeps the lit mesh and pages from Core messages and drops them when lighting is switched off', () => {
    const { store } = rig()
    let notified = 0
    store.subscribe(() => notified++)
    store.onMessage(mesh(2))
    expect(store.lightmap.ready).toBe(true)
    store.onMessage({ type: 'lightmap_tiles', rev: 1, atlas: 8, tiles: [{ page: 0, x: 0, y: 0, w: 1, h: 1, rgba: b64(new Uint8Array(new Uint16Array([0x3c00, 0x4000, 0x4200, 0x3c00]).buffer)) }] })
    expect(store.lightmap.dirtySince(0).rects.length).toBe(0)  // another revision: ignored
    store.onMessage({ type: 'lightmap_tiles', rev: 2, atlas: 8, tiles: [{ page: 0, x: 0, y: 0, w: 1, h: 1, rgba: b64(new Uint8Array(new Uint16Array([0x3c00, 0x4000, 0x4200, 0x3c00]).buffer)) }] })
    expect(store.lightmap.pages[0][0]).toBe(0x3c00)
    store.onMessage({ type: 'lighting_status', enabled: false, rev: 2, chunks: 0, stale: 0, minPasses: 0, maxPasses: 4, converged: false, texels: 0, pages: 0 })
    expect(store.lightmap.ready).toBe(false)
    expect(notified).toBeGreaterThan(2)
  })
})

import { makeBox } from '../src/model/geometry'
import { emptyDocument } from '../src/model/document'
import { buildPreset } from '../src/model/relief'
import { defaultProps, texEnv } from '../src/model/texprops'

describe('store: relief geometry for the bake', () => {
  const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms))
  const withRelief = (): void => texEnv.setProvider((p) => (p === 'wall/relief' ? { path: p, width: 64, height: 64, ppm: 64, props: { ...defaultProps(), relief: buildPreset('recess', 1, 1) } } : undefined))
  const addWall = (store: Store): void => { store.edit('wall', (d) => ({ ...d, entities: [{ id: 'w', classname: 'worldspawn', keyvalues: {}, brushes: [makeBox('a', [0, 0, 0], [2, 2, 2], 'wall/relief')] } as never] })) }

  it('sends the relief once it settles, only while lighting is wanted, and not again when nothing changed', async () => {
    withRelief()
    try {
      const { store, sent } = rig()
      store.setStatus('connected')
      store.onMessage({ type: 'document_sync', rev: 1, path: 'maps/t.json', doc: emptyDocument(), dirty: false } as never)
      addWall(store)
      store.syncRelief()
      await sleep(250)
      expect(sent.filter((m) => m.type === 'relief_geometry')).toHaveLength(0) // lighting is not wanted: nothing to bake
      store.setLightingWanted({}, true)
      store.syncRelief()
      store.syncRelief() // every frame calls it; the second is free
      await sleep(250)
      const first = sent.filter((m) => m.type === 'relief_geometry')
      expect(first).toHaveLength(1)
      expect(first[0].faces).toHaveLength(6)
      store.syncRelief()
      await sleep(250)
      expect(sent.filter((m) => m.type === 'relief_geometry')).toHaveLength(1)
      // a relief edit (new texture environment revision) is sent again
      texEnv.setProvider((p) => (p === 'wall/relief' ? { path: p, width: 64, height: 64, ppm: 64, props: { ...defaultProps(), relief: buildPreset('raise', 1, 1) } } : undefined))
      store.syncRelief()
      await sleep(250)
      expect(sent.filter((m) => m.type === 'relief_geometry')).toHaveLength(2)
      // a new connection forgets what Core holds: it is sent again
      store.setStatus('disconnected'); store.setStatus('connected')
      store.syncRelief()
      await sleep(250)
      expect(sent.filter((m) => m.type === 'relief_geometry')).toHaveLength(3)
    } finally { texEnv.setProvider(null) }
  })
})
