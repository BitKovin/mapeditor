// BSP export in the Shell (REQUIREMENTS §25 E9): the messages, the store state and the status bar text.
import { describe, expect, it } from 'vitest'
import { msg, parseInbound } from '../src/net/protocol'
import { progressMessage, resultMessage } from '../src/state/exportStatus'
import { makeApp } from './helpers'

const result = {
  type: 'export_result', ok: true, cancelled: false, error: '', warnings: ['a surface has more than 8 light styles; the rest are dropped'], bsp: 'maps/room.bsp', lightmaps: 'maps/room',
  seconds: 12.34, bakeSeconds: 11, device: 'cpu',
  stats: { surfaces: 120, droppedSurfaces: 4, brushes: 30, models: 2, leafs: 64, clusters: 40, lightmapPages: 2, gridPoints: 900, palette: 300, visCell: 4, visSeconds: 0.5 },
}

describe('export_bsp messages', () => {
  it('builds the request and parses progress and result (bad fields are defaulted, garbage is dropped)', () => {
    expect(JSON.parse(msg.exportBsp(7, 'maps/a.bsp'))).toEqual({ type: 'export_bsp', requestId: 7, path: 'maps/a.bsp', bake: true })
    expect(JSON.parse(msg.exportCancel(8))).toEqual({ type: 'export_cancel', requestId: 8 })
    expect(parseInbound(JSON.stringify({ type: 'export_progress', stage: 'visibility', fraction: 0.5 }))).toEqual({ type: 'export_progress', stage: 'visibility', fraction: 0.5 })
    expect(parseInbound(JSON.stringify({ type: 'export_progress', stage: 3 }))).toBeNull()
    const r = parseInbound(JSON.stringify(result))
    expect(r).toEqual(result)
    const partial = parseInbound(JSON.stringify({ type: 'export_result', ok: false, error: 'the map is not valid' }))
    expect(partial && partial.type === 'export_result' && partial.stats.surfaces).toBe(0)
  })
  it('status bar text', () => {
    expect(progressMessage({ type: 'export_progress', stage: 'baking lightmaps', fraction: 0.256 })).toBe('Exporting BSP: baking lightmaps 26%')
    const ok = resultMessage(result as never)
    expect(ok).toContain('Exported maps/room.bsp (lightmaps in maps/room/)')
    expect(ok).toContain('2 lightmap pages')
    expect(ok).toContain('40 vis cells of 4 m')
    expect(ok).toContain('1 warning')
    expect(resultMessage({ ...result, ok: false, error: 'boom' } as never)).toBe('BSP export failed: boom')
    expect(resultMessage({ ...result, cancelled: true } as never)).toBe('BSP export cancelled')
  })
  it('the store shows progress while the export runs and clears it with the result', () => {
    const app = makeApp(); app.sync()
    expect(app.store.state.exporting).toBeNull()
    app.store.onMessage({ type: 'export_progress', stage: 'cells', fraction: 0.3 })
    expect(app.store.state.exporting).toEqual({ stage: 'cells', fraction: 0.3 })
    expect(app.store.state.message).toBe('Exporting BSP: cells 30%')
    app.store.onMessage(parseInbound(JSON.stringify(result))!)
    expect(app.store.state.exporting).toBeNull()
    expect(app.store.state.message).toContain('Exported maps/room.bsp')
  })
})
