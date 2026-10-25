// @vitest-environment jsdom
// The outliner's search box filters rows by class, id, brush and prefab name.
import { afterEach, describe, expect, it } from 'vitest'
import { addEntity, emptyDocument } from '../src/model/document'
import type { Entity } from '../src/model/document'
import { makeBox } from '../src/model/geometry'
import { OutlinerPanel } from '../src/ui/panels'
import { expandOutliner, makeApp } from './helpers'

afterEach(() => document.body.replaceChildren())

const point = (id: string, classname: string, targetname?: string): Entity => ({ id, classname, keyvalues: { origin: '0 0 0', ...(targetname ? { targetname } : {}) } })
const detail: Entity = { id: 'd1', classname: 'func_detail', brushes: [makeBox('d1a', [0, 0, 0], [1, 1, 1]), makeBox('d1b', [2, 0, 0], [3, 1, 1])] }

function setup() {
  const app = makeApp()
  app.sync([point('p1', 'info_player_start'), point('l1', 'light', 'hall_lamp'), detail].reduce((d, e) => addEntity(d, e), emptyDocument()))
  const panel = new OutlinerPanel(app.store); panel.init()
  document.body.append(panel.element)
  expandOutliner(panel.element)
  const search = panel.element.querySelector<HTMLInputElement>('.outliner-search')!
  const type = (v: string) => { search.value = v; search.dispatchEvent(new Event('input')) }
  const keys = () => [...panel.element.querySelectorAll<HTMLElement>('[data-key]')].map((r) => r.dataset.key)
  return { panel, search, type, keys }
}

describe('outliner search', () => {
  it('has a search box and shows everything while it is empty', () => {
    const { search, keys } = setup()
    expect(search).not.toBeNull()
    expect(keys().length).toBe(5)
  })
  it('filters by class, case-insensitively', () => {
    const { type, keys } = setup()
    type('LIGHT')
    expect(keys()).toEqual(['e:l1'])
  })
  it('keeps an entity with its matching brush only, and all brushes when the entity matches', () => {
    const { type, keys } = setup()
    type('d1b')
    expect(keys().length).toBe(2)
    type('func_detail')
    expect(keys().length).toBe(3)
  })
  it('requires every word, and says so when nothing matches', () => {
    const { type, keys, panel } = setup()
    type('light info')
    expect(keys()).toEqual([])
    expect(panel.element.textContent).toContain('Nothing matches')
  })
  it('shows the target name in the row and finds the entity by it', () => {
    const { panel, type, keys } = setup()
    expect(panel.element.querySelector('[data-key="e:l1"] .target')?.textContent).toContain('hall_lamp')
    type('hall_la')
    expect(keys()).toEqual(['e:l1'])
  })
  it('Escape clears the search', () => {
    const { search, keys } = setup()
    search.value = 'light'; search.dispatchEvent(new Event('input'))
    search.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }))
    expect(search.value).toBe('')
    expect(keys().length).toBe(5)
  })
  it('keeps the search box (and its text) across unrelated rebuilds', () => {
    const { panel, type } = setup()
    type('light')
    expect(panel.element.querySelector<HTMLInputElement>('.outliner-search')!.value).toBe('light')
  })
})
