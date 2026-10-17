// The game's export format (REQUIREMENTS §31 X2): from the profile message, the file name, the dialog filter.
import { describe, expect, it } from 'vitest'
import { RBSP_FORMAT, formatFilters, formatFromProfile, withFormatExt } from '../src/state/exportFormat'
import { Store } from '../src/state/store'

const OBJ = { id: 'obj', name: 'OBJ + JSON', extension: '.obj' }
const LIST = [{ id: 'rbsp', name: 'rBSP', extension: '.bsp' }, OBJ]

describe('export format', () => {
  it('is the one the profile message names, rBSP when the Core sends none or an unknown one', () => {
    expect(formatFromProfile({ exporter: 'obj', exporters: LIST })).toEqual(OBJ)
    expect(formatFromProfile({ exporter: 'rbsp', exporters: LIST })).toEqual(RBSP_FORMAT)
    expect(formatFromProfile({})).toEqual(RBSP_FORMAT)
    expect(formatFromProfile({ exporter: 'md3', exporters: LIST })).toEqual(RBSP_FORMAT)
    expect(formatFromProfile({ exporter: 'obj', exporters: [{ id: 'obj' }] })).toEqual(RBSP_FORMAT)  // malformed entries are skipped
  })
  it('gives the export file the format\'s extension', () => {
    expect(withFormatExt('maps/a.map.json', OBJ)).toBe('maps/a.obj')
    expect(withFormatExt('maps/a.bsp', OBJ)).toBe('maps/a.obj')
    expect(withFormatExt('maps/a', OBJ)).toBe('maps/a.obj')
    expect(withFormatExt('maps/a.obj', OBJ)).toBe('maps/a.obj')
    expect(withFormatExt('maps/a.obj', RBSP_FORMAT)).toBe('maps/a.bsp')
  })
  it('filters the save dialog by the format', () => {
    expect(formatFilters(OBJ)).toEqual([{ name: 'OBJ + JSON (*.obj)', pattern: '*.obj' }])
  })
  it('the store takes it from the profile message', () => {
    const store = new Store({ submitDocument() {}, updateInFlight: false, hasPendingUpdate: false, sendLighting() {} } as any)
    expect(store.state.exportFormat).toEqual(RBSP_FORMAT)
    store.onMessage({ type: 'profile', profile: { name: 'g', textureRoot: 'textures', textureExtensions: [], modelExtensions: [], entityDefinitions: '', mapRoot: 'maps', prefabRoot: 'prefabs' }, exporter: 'obj', exporters: LIST })
    expect(store.state.exportFormat).toEqual(OBJ)
  })
})
