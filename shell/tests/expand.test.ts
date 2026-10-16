import { describe, expect, it } from 'vitest'
import { MapDocument, emptyDocument } from '../src/model/document'
import { makeBox, brushBounds } from '../src/model/geometry'
import { expandPrefabs, normalizePath } from '../src/model/expand'

const doc = (entities: MapDocument['entities'] = [], prefabInstances: MapDocument['prefabInstances'] = []): MapDocument =>
  ({ ...emptyDocument(), entities, prefabInstances })
const inst = (id: string, file: string, origin: [number, number, number] = [0, 0, 0]) => ({ id, file, origin })
const light = (id: string, origin: [number, number, number] = [0, 0, 0]) => ({ id, classname: 'light', origin })

describe('normalizePath', () => {
  it('collapses . and .. and backslashes', () => {
    expect(normalizePath('a/./b/../c')).toBe('a/c')
    expect(normalizePath('prefabs\\x.json')).toBe('prefabs/x.json')
    expect(normalizePath('../x')).toBe('../x')
  })
})

describe('expandPrefabs', () => {
  it('offsets entities and brushes of an instance', () => {
    const child = doc([{ id: 'f', classname: 'func_group', brushes: [makeBox('b', [-1, -1, -1], [1, 1, 1])] }])
    const r = expandPrefabs(doc([], [inst('pi', 'c.json', [10, 20, 30])]), { 'c.json': child })
    expect(r.problems).toEqual([])
    expect(r.entities).toHaveLength(1)
    expect(brushBounds(r.entities[0].entity.brushes![0])!.min).toEqual([9, 19, 29])
    expect(r.entities[0].instancePath).toEqual(['root', 'pi'])
  })
  it('accumulates offsets through three levels and allows diamonds', () => {
    const leaf = doc([light('l')])
    const mid = doc([], [inst('to_leaf', 'leaf.json', [1, 0, 0])])
    const root = doc([], [inst('a', 'mid.json', [10, 0, 0]), inst('b', 'mid.json', [20, 0, 0])])
    const r = expandPrefabs(root, { 'leaf.json': leaf, 'mid.json': mid })
    expect(r.entities.map((e) => e.entity.origin![0]).sort((x, y) => x - y)).toEqual([11, 21])
    expect(r.entities[0].instancePath).toEqual(['root', 'a', 'to_leaf'])
  })
  it('reports cycles (including through path aliases) and missing prefabs, without throwing', () => {
    const a = doc([], [inst('to_b', 'b.json')])
    const b = doc([], [inst('to_a', 'x/../a.json')])
    expect(expandPrefabs(doc([], [inst('r', 'a.json')]), { 'a.json': a, 'b.json': b }).problems.join()).toMatch(/cycle/)
    expect(expandPrefabs(doc([], [inst('r', 'nope.json')]), {}).problems.join()).toMatch(/not loaded/)
    expect(expandPrefabs(doc([], [inst('r', 'e.json')]), { 'e.json': { error: 'boom' } }).problems.join()).toMatch(/boom/)
  })
  it('detects a cycle back to the root document', () => {
    const p = doc([], [inst('back', 'maps/root.json')])
    expect(expandPrefabs(doc([], [inst('p', 'p.json')]), { 'p.json': p }, 'maps/root.json').problems.join()).toMatch(/cycle/)
  })
})

// ---- N1 (b): rotation, scale, overrides. Same cases as core/tests/test_prefab.cpp -- both sides must agree. ----
import { anglesFromRotation, rotationFromAngles } from '../src/model/expand'
import type { PrefabInstance } from '../src/model/document'
import { signedVolume } from '../src/model/geometry'

const near = (a: number, b: number, eps = 1e-9) => expect(Math.abs(a - b)).toBeLessThan(eps)
const pt = (id: string, origin: [number, number, number] = [0, 0, 0], extra: Record<string, unknown> = {}) => ({ id, classname: 'info_null', origin, ...extra })
const byId = (r: ReturnType<typeof expandPrefabs>, id: string) => r.entities.find((e) => e.sourceEntityId === id)?.entity

describe('expandPrefabs: angles, scale, overrides', () => {
  it('yaw rotates origins about the instance origin and composes entity angles', () => {
    const c = doc([pt('p', [1, 0, 0], { angles: [0, 45, 0] })])
    const pi: PrefabInstance = { ...inst('i', 'c.json', [10, 0, 0]), angles: [0, 90, 0] }
    const e = byId(expandPrefabs(doc([], [pi]), { 'c.json': c }), 'p')!
    near(e.origin![0], 10); near(e.origin![2], -1)
    near(e.angles![1], 135); near(e.angles![0], 0)
  })
  it('angles <-> matrix round trip, including the pitch +-90 gimbal case', () => {
    for (const a of [[20, 30, 40], [-60, 170, -10], [0, 0, 0]] as [number, number, number][]) {
      const back = anglesFromRotation(rotationFromAngles(a))
      near(back[0], a[0], 1e-7); near(back[1], a[1], 1e-7); near(back[2], a[2], 1e-7)
    }
    const g = anglesFromRotation(rotationFromAngles([90, 30, 0]))
    near(g[0], 90, 1e-6); near(g[2], 0, 1e-6); near(g[1], 30, 1e-6)
  })
  it('nested rotation turns the child offset too', () => {
    const fs = { 'leaf.json': doc([pt('p')]), 'mid.json': doc([], [inst('to_leaf', 'leaf.json', [1, 0, 0])]) }
    const outer: PrefabInstance = { ...inst('outer', 'mid.json', [5, 0, 0]), angles: [0, 90, 0] }
    const e = byId(expandPrefabs(doc([], [outer]), fs), 'p')!
    near(e.origin![0], 5); near(e.origin![2], -1); near(e.angles![1], 90)
  })
  it('scale moves points and brush vertices, sets modelscale_vec on point entities only; a mirror stays outward-wound', () => {
    const c = doc([pt('p', [2, 0, 0]), { id: 'b', classname: 'func_detail', brushes: [makeBox('bb', [1, 1, 1], [2, 2, 2])] }])
    const pi: PrefabInstance = { ...inst('i', 'c.json'), scale: [2, 2, 2] }
    let r = expandPrefabs(doc([], [pi]), { 'c.json': c })
    near(byId(r, 'p')!.origin![0], 4)
    expect(byId(r, 'p')!.keyvalues!.modelscale_vec).toBe('2 2 2')
    const bb = brushBounds(byId(r, 'b')!.brushes![0])!
    near(bb.min[0], 2); near(bb.max[2], 4)
    expect(byId(r, 'b')!.keyvalues?.modelscale_vec).toBeUndefined()
    r = expandPrefabs(doc([], [{ ...pi, scale: [-1, 1, 1] }]), { 'c.json': c })
    expect(signedVolume(byId(r, 'b')!.brushes![0])).toBeGreaterThan(0)
  })
  it('overrides: keyvalues / origin / angles / scale / deleted, nested path, outer wins', () => {
    const leaf = doc([pt('a', [1, 0, 0], { keyvalues: { speed: '1' } }), pt('b')])
    const toLeaf: PrefabInstance = { ...inst('lf', 'leaf.json'), overrides: { a: { keyvalues: { speed: 'inner', note: 'inner' } } } }
    const mid = doc([], [toLeaf])
    const outer: PrefabInstance = {
      ...inst('o', 'mid.json'),
      overrides: { 'lf/a': { keyvalues: { speed: 'outer' }, origin: [7, 0, 0], angles: [0, 33, 0], scale: [3, 1, 1] }, 'lf/b': { deleted: true } },
    }
    const r = expandPrefabs(doc([], [outer]), { 'leaf.json': leaf, 'mid.json': mid })
    expect(byId(r, 'b')).toBeUndefined()
    const e = byId(r, 'a')!
    expect(e.keyvalues!.speed).toBe('outer'); expect(e.keyvalues!.note).toBe('inner')
    near(e.origin![0], 7); near(e.angles![1], 33); expect(e.keyvalues!.modelscale_vec).toBe('3 1 1')
  })
  it('pure translation still leaves brush UVs alone (old behaviour)', () => {
    const c = doc([{ id: 'b', classname: 'func_detail', brushes: [makeBox('bb', [0, 0, 0], [1, 1, 1])] }])
    const r = expandPrefabs(doc([], [inst('i', 'c.json', [5, 0, 0])]), { 'c.json': c })
    expect(r.entities[0].entity.brushes![0].faces[0].uvs).toEqual(c.entities[0].brushes![0].faces[0].uvs)
  })
})

describe('expandPrefabs: decals follow the instance scale (same cases as Core)', () => {
  it('stretches width / height / depth along the decal axes and sets no modelscale_vec', () => {
    const dec = { id: 'd', classname: 'decal', origin: [0, 0, 0] as [number, number, number], keyvalues: { width: '2', height: '3' } }
    const pi: PrefabInstance = { ...inst('i', 'c.json'), scale: [2, 4, 5] }
    let e = byId(expandPrefabs(doc([], [pi]), { 'c.json': doc([dec]) }), 'd')!
    near(Number(e.keyvalues!.width), 4); near(Number(e.keyvalues!.height), 15); near(Number(e.keyvalues!.depth), 2)
    expect(e.keyvalues!.modelscale_vec).toBeUndefined()
    e = byId(expandPrefabs(doc([], [pi]), { 'c.json': doc([{ ...dec, angles: [0, 90, 0] }]) }), 'd')!
    near(Number(e.keyvalues!.width), 10)
  })
})
