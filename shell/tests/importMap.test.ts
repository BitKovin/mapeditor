import { describe, expect, it } from 'vitest'
import { msg } from '../src/net/protocol'

describe('import_map message', () => {
  it('asks Core to convert a .map; overwrite is only sent when set', () => {
    expect(JSON.parse(msg.importMap(3, 'maps/a.map'))).toEqual({ type: 'import_map', requestId: 3, path: 'maps/a.map' })
    expect(JSON.parse(msg.importMap(4, 'maps/a.map', true))).toEqual({ type: 'import_map', requestId: 4, path: 'maps/a.map', overwrite: true })
  })
})
