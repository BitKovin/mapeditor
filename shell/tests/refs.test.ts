import { describe, expect, it } from 'vitest'
import { emptyDocument, MapDocument } from '../src/model/document'
import { expandPrefabs } from '../src/model/expand'
import { chainNames, pathOfHit, refKeysOf, refTargets } from '../src/model/refs'

const ent = (id: string, name = '') => ({ id, classname: 'info_null', origin: [0, 0, 0] as [number, number, number], ...(name ? { keyvalues: { targetname: name } } : {}) })
const doc = (entities: any[], prefabInstances: any[] = []): MapDocument => ({ ...emptyDocument(), entities, prefabInstances })

describe('reference targets', () => {
  const lib = {
    'door.map.json': doc([ent('d', 'door')]),
    'room.map.json': doc([ent('r', 'light')], [{ id: 'di', file: 'door.map.json', name: 'inner' }]),
  }
  const root = doc([ent('w', 'switch')], [{ id: 'a', file: 'room.map.json' }, { id: 'b', file: 'room.map.json' }])
  const r = expandPrefabs(root, lib)

  it('lists own entities first, then prefab content grouped by instance chain with real names', () => {
    const t = refTargets(root, r.entities, lib)
    expect(t[0]).toMatchObject({ path: 'w', group: '', resolvedName: 'switch' })
    const deep = t.find((x) => x.path === 'b/di/d')!
    expect(deep).toMatchObject({ group: 'room2|inner', resolvedName: 'room2|inner|door' })
    expect(t.find((x) => x.path === 'a/r')!.resolvedName).toBe('room|light')
  })
  it('chainNames walks nested files and falls back to ids for unknown ones', () => {
    expect(chainNames(root, lib, ['a', 'di'])).toEqual(['room', 'inner'])
    expect(chainNames(root, lib, ['zz', 'q'])).toEqual(['zz', 'q'])
  })
  it('pathOfHit maps a picked entity object to its root-relative path', () => {
    expect(pathOfHit(root.entities[0], root, r.entities)).toBe('w')
    const hit = r.entities.find((x) => x.instancePath.join('/') === 'root/b/di')!
    expect(pathOfHit(hit.entity, root, r.entities)).toBe('b/di/d')
    expect(pathOfHit(ent('x'), root, r.entities)).toBeNull()
  })
  it('refKeysOf adds the class-defined target keys', () => {
    expect(refKeysOf(undefined)).toEqual(['target', 'killtarget'])
    expect(refKeysOf({ keys: [{ key: 'path', type: 'target_destination' }, { key: 'x', type: 'string' }] }, ['extra'])).toEqual(['target', 'killtarget', 'path', 'extra'])
  })
})
