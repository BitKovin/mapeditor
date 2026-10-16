import { describe, expect, it } from 'vitest'
import { MapDocument } from '../src/model/document'
import { PrefabPreviews } from '../src/state/prefabPreview'

const brush = (id: string) => ({ id, vertices: [[0, 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 1]] as [number, number, number][], faces: [] })
const doc = (entities: MapDocument['entities'], prefabInstances: MapDocument['prefabInstances'] = []): MapDocument => ({ schemaVersion: '0.2', entities, prefabInstances })
const flush = () => new Promise((r) => setTimeout(r, 0))

describe('PrefabPreviews', () => {
  const files: Record<string, MapDocument> = {
    'prefabs/a.map.json': doc([{ id: 'w', classname: 'worldspawn', brushes: [brush('b1')] }, { id: 'p', classname: 'light', origin: [1, 2, 3] }]),
    'prefabs/outer.map.json': doc([], [{ id: 'in', file: 'prefabs/a.map.json', origin: [10, 0, 0] }]),
    'prefabs/loop1.map.json': doc([{ id: 'x', classname: 'light', origin: [0, 0, 0] }], [{ id: 'l', file: 'prefabs/loop2.map.json' }]),
    'prefabs/loop2.map.json': doc([], [{ id: 'l', file: 'prefabs/loop1.map.json' }]),
  }
  const make = (known = {}) => {
    const reads: string[] = []
    const p = new PrefabPreviews(async (path) => { reads.push(path); const d = files[path]; if (!d) throw new Error(`no ${path}`); return d }, () => known)
    return { p, reads }
  }

  it('is null while loading, then returns the content at the origin, and reads each file once', async () => {
    const { p, reads } = make()
    let changes = 0; p.onChange(() => changes++)
    expect(p.entities('prefabs/a.map.json')).toBeNull()
    expect(p.entities('./prefabs/a.map.json')).toBeNull() // same file, no second read
    await flush()
    const e = p.entities('prefabs/a.map.json')!
    expect(e.map((x) => x.classname)).toEqual(['worldspawn', 'light'])
    expect(e[1].origin).toEqual([1, 2, 3])
    expect(reads).toEqual(['prefabs/a.map.json'])
    expect(changes).toBe(1)
    expect(p.entities('prefabs/a.map.json')).toBe(e) // cached object: GPU buffers keyed by it are reused
  })
  it('loads what a prefab nests and places it', async () => {
    const { p } = make()
    expect(p.entities('prefabs/outer.map.json')).toBeNull()
    await flush(); await flush() // outer, then its child
    const e = p.entities('prefabs/outer.map.json')!
    expect(e.find((x) => x.classname === 'light')!.origin).toEqual([11, 2, 3])
  })
  it('uses files the open map already has, without asking Core', () => {
    const { p, reads } = make({ 'prefabs/a.map.json': files['prefabs/a.map.json'] })
    expect(p.entities('prefabs/a.map.json')).not.toBeNull()
    expect(reads).toEqual([])
  })
  it('reports a file that cannot be read, and a cycle does not hang', async () => {
    const { p } = make()
    expect(p.entities('prefabs/missing.map.json')).toBeNull()
    await flush()
    expect(p.entities('prefabs/missing.map.json')).toBeNull()
    expect(p.error('prefabs/missing.map.json')).toMatch(/no prefabs\/missing/)
    p.entities('prefabs/loop1.map.json'); await flush(); await flush()
    expect(p.entities('prefabs/loop1.map.json')?.some((x) => x.classname === 'light')).toBe(true)
  })
  it('clear forgets everything', async () => {
    const { p, reads } = make()
    p.entities('prefabs/a.map.json'); await flush()
    p.clear()
    expect(p.entities('prefabs/a.map.json')).toBeNull()
    expect(reads).toHaveLength(2)
  })
})
