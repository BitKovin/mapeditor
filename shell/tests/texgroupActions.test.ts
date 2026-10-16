// Texture group actions (REQUIREMENTS §20 T8): apply, parameters, seed, clear, undo; and the TexGroupCache.
import { afterEach, describe, expect, it } from 'vitest'
import { findEntity } from '../src/model/document'
import { makeBox } from '../src/model/geometry'
import { groupEnv } from '../src/model/texgroup'
import { texEnv } from '../src/model/texprops'
import { applyTexGroup, addDrawnBrush, clearFaceLayout, rerollLayoutSeed, selectedLayout, setFaceLayout } from '../src/state/actions'
import { TexGroupCache } from '../src/state/texgroups'
import { makeApp } from './helpers'

afterEach(() => { groupEnv.setProvider(null); texEnv.setProvider(null) })
const setup = () => { const app = makeApp(); app.sync(); addDrawnBrush(app.store, makeBox('b1', [0, 0, 0], [1, 1, 1])); return app }
const faces = (store: ReturnType<typeof setup>['store']) => findEntity(store.state.doc, 'e_worldspawn')!.brushes![0].faces

describe('texture group actions', () => {
  it('applies to every face of the selected brush, or to the one selected face; the fallback texture is set', () => {
    const { store } = setup()
    expect(applyTexGroup(store, 'bld/apartment', 'dev/wall')).toBe(6)
    expect(faces(store).every((f) => (f.layout as { group: string }).group === 'bld/apartment' && f.texture === 'dev/wall')).toBe(true)
    store.select({ entityId: 'e_worldspawn', brushId: 'b1', faceId: 2 })
    applyTexGroup(store, 'bld/other')
    expect(faces(store).map((f) => (f.layout as { group: string }).group)).toEqual(['bld/apartment', 'bld/apartment', 'bld/other', 'bld/apartment', 'bld/apartment', 'bld/apartment'])
  })

  it('parameters, seed, rotate and flip are per face and only differences are stored; null returns to the default', () => {
    const { store } = setup()
    applyTexGroup(store, 'g')
    setFaceLayout(store, { params: { floorHeight: 4, balconies: false }, seed: 9, rotate: 90, flipU: true })
    expect(faces(store)[0].layout).toEqual({ group: 'g', seed: 9, params: { floorHeight: 4, balconies: false }, rotate: 90, flipU: true })
    setFaceLayout(store, { params: { balconies: null }, seed: null, rotate: 0, flipU: false })
    expect(faces(store)[0].layout).toEqual({ group: 'g', params: { floorHeight: 4 } })
    setFaceLayout(store, { params: { floorHeight: null } })
    expect(faces(store)[0].layout).toEqual({ group: 'g' })
  })

  it('re-roll gives every face its own seed; clear removes the layout and keeps the texture; everything undoes', () => {
    const { store } = setup()
    applyTexGroup(store, 'g', 'dev/wall')
    rerollLayoutSeed(store)
    const seeds = faces(store).map((f) => (f.layout as { seed: number }).seed)
    expect(new Set(seeds).size).toBeGreaterThan(1)
    expect(selectedLayout(store)).toMatchObject({ count: 6, total: 6 })
    clearFaceLayout(store)
    expect(faces(store).every((f) => f.layout === undefined && f.texture === 'dev/wall')).toBe(true)
    expect(selectedLayout(store)).toBeNull()
    store.undo(); expect(faces(store).every((f) => f.layout !== undefined)).toBe(true)
    store.undo(); store.undo(); expect(faces(store).every((f) => f.layout === undefined)).toBe(true)
  })

  it('settings on faces without a layout change nothing (no undo step)', () => {
    const { store } = setup(); const n = store.state.history.length
    setFaceLayout(store, { params: { a: 1 } })
    rerollLayoutSeed(store)
    expect(faces(store).every((f) => f.layout === undefined)).toBe(true); expect(store.state.history).toHaveLength(n)
  })
})

describe('TexGroupCache', () => {
  const raw = { format: 1, slots: { a: 'dev/a' }, root: { tex: 'a' } }
  it('loads on first request, serves the group, bumps the environment, and survives a failed load', async () => {
    let changed = 0
    const c = new TexGroupCache(async (p) => { if (p === 'bad') throw new Error('nope'); return raw }, () => { changed++ })
    expect(groupEnv.get('g')).toBeUndefined() // loading
    await new Promise((r) => setTimeout(r, 0))
    expect(groupEnv.get('g')?.slots.a).toBe('dev/a'); expect(changed).toBe(1)
    const v1 = groupEnv.version('g')
    c.set('g', { ...raw, name: 'edited' })
    expect(groupEnv.get('g')?.name).toBe('edited'); expect(groupEnv.version('g')).not.toBe(v1)
    groupEnv.get('bad'); await new Promise((r) => setTimeout(r, 0))
    expect(c.state('bad')).toMatchObject({ status: 'missing', error: 'nope' }); expect(groupEnv.get('bad')).toBeUndefined()
    const rev = groupEnv.rev; c.remove('g'); expect(groupEnv.rev).toBeGreaterThan(rev)
  })
  it('a file without a usable root is kept (so the editor can show the errors) but gives no group', async () => {
    const c = new TexGroupCache(async () => ({ format: 1 }))
    expect(groupEnv.get('x')).toBeUndefined(); await new Promise((r) => setTimeout(r, 0))
    const st = c.state('x'); expect(st.status).toBe('ready'); if (st.status === 'ready') expect(st.parse.errors.length).toBeGreaterThan(0)
    expect(groupEnv.get('x')).toBeUndefined()
  })
})

describe('wall sets leave roofs and floors alone', () => {
  it('a whole brush: only the standing faces get a wall set; a face picked by hand always does; frames and advanced groups take everything', () => {
    const { store } = setup()
    expect(applyTexGroup(store, 'sets/wall', 'dev/wall', { wallsOnly: true })).toBe(4)
    const withLayout = () => faces(store).filter((f) => f.layout !== undefined).length
    expect(withLayout()).toBe(4)
    // the box's up and down faces are the ones without a layout
    const flat = faces(store).filter((f) => f.layout === undefined)
    expect(flat).toHaveLength(2)
    store.select({ entityId: 'e_worldspawn', brushId: 'b1', faceId: flat[0].id })
    expect(applyTexGroup(store, 'sets/wall', 'dev/wall', { wallsOnly: true })).toBe(1) // picked by hand
    expect(withLayout()).toBe(5)
    store.select({ entityId: 'e_worldspawn', brushId: 'b1' })
    expect(applyTexGroup(store, 'sets/frame')).toBe(6) // no restriction
    expect(withLayout()).toBe(6)
  })
})
