import { describe, expect, it } from 'vitest'
import { emptyDocument, MapDocument } from '../src/model/document'
import { addBoxTo } from '../src/state/actions'
import { extractPrefab, replaceWithInstance } from '../src/state/prefabize'

function level(): { doc: MapDocument; a: any; b: any } {
  const r1 = addBoxTo(emptyDocument(), [10, 0, 0], 2)
  const r2 = addBoxTo(r1.doc, [14, 0, 0], 2)
  const doc: MapDocument = { ...r2.doc, entities: [...r2.doc.entities, { id: 'l1', classname: 'light', origin: [12, 5, 0], refs: { target: 'l2', killtarget: 'gone' } }, { id: 'l2', classname: 'light', origin: [0, 0, 0] }] }
  return { doc, a: r1.sel, b: r2.sel }
}

describe('turn into prefab', () => {
  it('centres the prefab on the selection pivot and keeps untouched brushes out', () => {
    const { doc, a, b } = level()
    const ex = extractPrefab(doc, [a, { entityId: 'l1' }])!
    expect(ex.pivot[0]).toBeGreaterThan(10); expect(ex.pivot[0]).toBeLessThan(12.5)
    const world = ex.prefab.entities.find((e) => e.classname === 'worldspawn')!
    expect(world.brushes!.length).toBe(1)
    const xs = world.brushes![0].vertices.map((v) => v[0])
    expect((Math.min(...xs) + Math.max(...xs)) / 2).toBeCloseTo(10 - ex.pivot[0])
    const light = ex.prefab.entities.find((e) => e.id === 'l1')!
    expect(light.origin).toEqual([12 - ex.pivot[0], 5 - ex.pivot[1], 0 - ex.pivot[2]].map((v) => Math.round(v * 1000) / 1000))
    expect(light.refs).toBeUndefined() // targets left behind are dropped
    void b
  })

  it('keeps refs between selected entities', () => {
    const { doc } = level()
    const ex = extractPrefab(doc, [{ entityId: 'l1' }, { entityId: 'l2' }])!
    expect(ex.prefab.entities.find((e) => e.id === 'l1')!.refs).toEqual({ target: 'l2' })
  })

  it('swaps the selection for one instance at the pivot; the other brush stays', () => {
    const { doc, a, b } = level()
    const ex = extractPrefab(doc, [a])!
    const r = replaceWithInstance(doc, [a], 'prefabs/crate.map.json', ex.pivot)
    expect(r.doc.entities[0].brushes!.map((x) => x.id)).toEqual([b.brushId])
    expect(r.doc.prefabInstances).toEqual([{ id: r.instanceId, name: 'crate', file: 'prefabs/crate.map.json', origin: ex.pivot, angles: [0, 0, 0] }])
  })

  it('carries selected prefab instances into the prefab, relative to the pivot', () => {
    const doc: MapDocument = { ...emptyDocument(), prefabInstances: [{ id: 'p1', file: 'p/door.map.json', origin: [4, 0, 0] }] }
    const ex = extractPrefab(doc, [{ entityId: '', instanceId: 'p1' }])
    expect(ex).not.toBeNull()
    const r = replaceWithInstance(doc, [{ entityId: '', instanceId: 'p1' }], 'p/new.map.json', ex!.pivot)
    expect(r.doc.prefabInstances.map((p) => p.file)).toEqual(['p/new.map.json'])
  })

  it('returns null for nothing', () => {
    expect(extractPrefab(emptyDocument(), [])).toBeNull()
    expect(extractPrefab(emptyDocument(), [{ entityId: 'nope' }])).toBeNull()
  })
})
