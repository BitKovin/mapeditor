// @vitest-environment jsdom
import { afterEach, describe, expect, it, vi } from 'vitest'
import { findEntity } from '../src/model/document'
import { makeBox } from '../src/model/geometry'
import { groupEnv, parseGroup } from '../src/model/texgroup'
import { defaultProps, texEnv } from '../src/model/texprops'
import { addDrawnBrush, applyTexGroup } from '../src/state/actions'
import { APPLY_DELAY_MS } from '../src/ui/contentBrowser'
import { TexGroupCache } from '../src/state/texgroups'
import { TextureCache } from '../src/state/textures'
import { validGroupName } from '../src/ui/texGroupEditor'
import { TexGroupsPanel } from '../src/ui/texGroupsPanel'
import { TexturePanel } from '../src/ui/texturePanel'
import { makeApp } from './helpers'

afterEach(() => { texEnv.setProvider(null); groupEnv.setProvider(null); document.body.replaceChildren(); vi.useRealTimers() })

const raw = {
  format: 1, name: 'Block', description: 'A block', slots: { wall: 'dev/wall' }, fallback: 'wall',
  params: { floorHeight: { type: 'number', default: 3, min: 2, max: 6, step: 0.1, label: 'Floor height (m)' }, balconies: { type: 'bool', default: true, label: 'Balconies' }, style: { type: 'choice', options: ['a', 'b'], label: 'Style' } },
  root: { tex: 'wall' },
}

function setup(loaded = true) {
  const app = makeApp(); app.sync()
  addDrawnBrush(app.store, makeBox('b1', [0, 0, 0], [4, 2, 1], 'dev/wall'))
  app.store.select({ entityId: 'e_worldspawn', brushId: 'b1', faceId: 1 })
  const textures = new TextureCache(async () => { throw new Error('none') })
  texEnv.setProvider(() => ({ path: 'dev/wall', width: 64, height: 64, ppm: 64, props: defaultProps() }))
  const groups = new TexGroupCache(async (p) => { if (p === 'bld/block' && loaded) return raw; if (p === 'sets/walls') return { format: 1, name: 'Walls', kit: { kind: 'wall', middle: { pieces: ['dev/wall'] } } }; throw new Error('missing') }, () => textures.notify())
  const open = vi.fn()
  const panel = new TexturePanel(app.store, textures, () => {}, () => 0.5, () => null, () => {}, open)
  document.body.append(panel.element); panel.init()
  const face = () => findEntity(app.store.state.doc, 'e_worldspawn')!.brushes![0].faces.find((f) => f.id === 1)!
  const row = (label: string) => [...panel.element.querySelectorAll('.si-row')].find((r) => r.querySelector('.si-label')?.textContent === label) as HTMLElement | undefined
  const button = (text: string) => [...panel.element.querySelectorAll('button')].find((b) => b.textContent === text) as HTMLButtonElement | undefined
  return { app, panel, groups, textures, open, face, row, button }
}
const tick = () => new Promise((r) => setTimeout(r, 0))

describe('Layout section of the Surface Inspector', () => {
  it('is absent on a plain face and appears with the group parameters when a group is applied', async () => {
    const { app, groups, row, panel } = setup()
    expect(panel.element.textContent).not.toContain('Texture group layout')
    groups.group('bld/block'); await tick()
    applyTexGroup(app.store, 'bld/block')
    expect(panel.element.textContent).toContain('Texture group layout')
    expect(row('Floor height (m)')).toBeTruthy(); expect(row('Balconies')).toBeTruthy(); expect(row('Style')).toBeTruthy(); expect(row('Seed')).toBeTruthy()
  })

  it('parameter edits are stored as differences only; the default removes the key; seed and rotate are stored', async () => {
    const { app, groups, row, face, button } = setup()
    groups.group('bld/block'); await tick(); applyTexGroup(app.store, 'bld/block')
    const num = row('Floor height (m)')!.querySelector('input') as HTMLInputElement
    num.value = '4.5'; num.dispatchEvent(new Event('change'))
    expect(face().layout).toEqual({ group: 'bld/block', params: { floorHeight: 4.5 } })
    const num2 = row('Floor height (m)')!.querySelector('input') as HTMLInputElement
    num2.value = '3'; num2.dispatchEvent(new Event('change'))
    expect(face().layout).toEqual({ group: 'bld/block' })
    const bal = row('Balconies')!.querySelector('input') as HTMLInputElement; bal.checked = false; bal.dispatchEvent(new Event('change'))
    expect(face().layout).toEqual({ group: 'bld/block', params: { balconies: false } })
    const seed = row('Seed')!.querySelector('input') as HTMLInputElement; seed.value = '12'; seed.dispatchEvent(new Event('change'))
    expect((face().layout as { seed: number }).seed).toBe(12)
    const rot = row('Rotate / flip')!.querySelector('select') as HTMLSelectElement; rot.value = '90'; rot.dispatchEvent(new Event('change'))
    expect((face().layout as { rotate: number }).rotate).toBe(90)
    button('Clear layout')!.click()
    expect(face().layout).toBeUndefined()
  })

  it('a group that cannot be loaded says so, and Edit group opens it', async () => {
    const { app, groups, panel, open, button } = setup(false)
    groups.group('bld/block'); await tick(); applyTexGroup(app.store, 'bld/block')
    expect(panel.element.querySelector('.problems')?.textContent).toMatch(/not loaded/)
    button('Edit group...')!.click(); expect(open).toHaveBeenCalledWith('bld/block')
  })
})

describe('Texture Groups panel', () => {
  const tab = (p: Element, label: string) => [...p.querySelectorAll('.gl-tabs button')].find((b) => b.textContent === label) as HTMLButtonElement
  const rows = (p: Element) => p.querySelectorAll('.gl-row')

  it('Simple lists texture sets, Advanced lists rule groups; each tab has its own New buttons', async () => {
    vi.useFakeTimers()
    const { app, groups, textures } = setup()
    const env = { list: async () => ['bld/block', 'floors/tile', 'sets/walls'], edit: vi.fn(), duplicate: vi.fn(async () => {}), remove: vi.fn(async () => {}) }
    const p = new TexGroupsPanel(app.store, textures, groups, env)
    document.body.append(p.element); p.init(); await p.reload(); await vi.advanceTimersByTimeAsync(5)
    expect(tab(p.element, 'Simple').classList.contains('active')).toBe(true)
    expect(rows(p.element)).toHaveLength(1); expect(p.element.textContent).toContain('Walls')
    const newBtn = (t: string) => [...p.element.querySelectorAll('.cb-bar button')].find((b) => b.textContent === t) as HTMLButtonElement
    newBtn('New wall set...').click(); newBtn('New 9-slice frame...').click()
    expect(env.edit).toHaveBeenCalledWith(null, 'wall'); expect(env.edit).toHaveBeenCalledWith(null, 'frame')
    tab(p.element, 'Advanced').click()
    expect(rows(p.element)).toHaveLength(2); expect(p.element.textContent).not.toContain('New wall set')
    ;[...p.element.querySelectorAll('.cb-bar button')].find((b) => b.textContent === 'New advanced group...')!.dispatchEvent(new Event('click'))
    expect(env.edit).toHaveBeenCalledWith(null, 'advanced')
    p.dispose()
  })

  it('filters by search, and a click applies the group (with its fallback texture) after the double-click delay; double-click edits', async () => {
    vi.useFakeTimers()
    const { app, groups, textures, face } = setup()
    const env = { list: async () => ['bld/block', 'floors/tile'], edit: vi.fn(), duplicate: vi.fn(async () => {}), remove: vi.fn(async () => {}) }
    const p = new TexGroupsPanel(app.store, textures, groups, env)
    document.body.append(p.element); p.init(); await p.reload(); await vi.advanceTimersByTimeAsync(5)
    tab(p.element, 'Advanced').click()
    expect(rows(p.element)).toHaveLength(2)
    const search = p.element.querySelector('input.search') as HTMLInputElement; search.value = 'floor'; search.dispatchEvent(new Event('input'))
    expect(rows(p.element)).toHaveLength(1)
    search.value = ''; search.dispatchEvent(new Event('input'))
    const row = rows(p.element)[0] as HTMLElement
    row.click(); expect(face().layout).toBeUndefined()
    vi.advanceTimersByTime(APPLY_DELAY_MS + 5)
    expect(face().layout).toEqual({ group: 'bld/block' })
    row.click(); row.dispatchEvent(new MouseEvent('dblclick', { bubbles: true })); vi.advanceTimersByTime(APPLY_DELAY_MS + 5)
    expect(env.edit).toHaveBeenCalledWith('bld/block')
    p.dispose()
  })

  it('says how to start on an empty tab', async () => {
    const { app, groups, textures } = setup()
    const p = new TexGroupsPanel(app.store, textures, groups, { list: async () => [], edit: () => {}, duplicate: async () => {}, remove: async () => {} })
    document.body.append(p.element); p.init(); await p.reload()
    expect(p.element.textContent).toMatch(/No texture sets yet/)
    tab(p.element, 'Advanced').click(); expect(p.element.textContent).toMatch(/No advanced groups/)
  })
})

describe('group names', () => {
  it('folders with / and plain characters only', () => {
    for (const ok of ['a', 'b/c', 'buildings/apartment_block-2']) expect(validGroupName(ok)).toBe(true)
    for (const bad of ['', '/a', 'a/', 'a//b', '../x', 'a b', 'a.b', 'a\\b', 'x'.repeat(121)]) expect(validGroupName(bad)).toBe(false)
    expect(parseGroup(raw).errors).toEqual([])
  })
})
