// @vitest-environment jsdom
// Entities fold their brushes (collapsed by default); a selected brush lights its collapsed entity; a rebuild keeps the scroll position.
import { afterEach, describe, expect, it } from 'vitest'
import { addEntity, emptyDocument } from '../src/model/document'
import type { Entity } from '../src/model/document'
import { makeBox } from '../src/model/geometry'
import { toggleHidden } from '../src/state/actions'
import { OutlinerPanel } from '../src/ui/panels'
import { makeApp } from './helpers'

afterEach(() => document.body.replaceChildren())
const detail: Entity = { id: 'd1', classname: 'func_detail', brushes: [makeBox('d1a', [0, 0, 0], [1, 1, 1]), makeBox('d1b', [2, 0, 0], [3, 1, 1])] }
const point: Entity = { id: 'p1', classname: 'info_player_start' }

function setup() {
  const app = makeApp()
  app.sync([detail, point].reduce((d, e) => addEntity(d, e), emptyDocument()))
  const panel = new OutlinerPanel(app.store); panel.init()
  document.body.append(panel.element)
  const keys = () => [...panel.element.querySelectorAll<HTMLElement>('[data-key]')].map((r) => r.dataset.key)
  const row = (k: string) => panel.element.querySelector<HTMLElement>(`[data-key="${k}"]`)!
  return { app, panel, keys, row }
}

describe('outliner fold', () => {
  it('starts collapsed: entities only, with the brush count; a point entity has no fold arrow', () => {
    const { keys, row } = setup()
    expect(keys()).toEqual(['e:d1', 'e:p1'])
    expect(row('e:d1').textContent).toContain('2 brushes')
    expect(row('e:d1').querySelector('button.twisty')).not.toBeNull()
    expect(row('e:p1').querySelector('button.twisty')).toBeNull()
  })
  it('the fold arrow opens and closes the brushes without selecting the entity', () => {
    const { app, keys, row } = setup()
    row('e:d1').querySelector<HTMLElement>('button.twisty')!.click()
    expect(keys().length).toBe(4)
    expect(app.store.state.selection).toBeNull()
    row('e:d1').querySelector<HTMLElement>('button.twisty')!.click()
    expect(keys()).toEqual(['e:d1', 'e:p1'])
  })
  it('a selected brush lights its collapsed entity and does not open it; open, the brush row is lit instead', () => {
    const { app, keys, row } = setup()
    app.store.select({ entityId: 'd1', brushId: 'd1b' })
    expect(keys()).toEqual(['e:d1', 'e:p1'])
    expect(row('e:d1').classList.contains('sel')).toBe(true)
    row('e:d1').querySelector<HTMLElement>('button.twisty')!.click()
    expect(row('e:d1').classList.contains('sel')).toBe(false)
    expect(row('b:d1|d1b').classList.contains('sel')).toBe(true)
  })
  it('hiding something keeps the scroll position of the list', () => {
    const { app, panel, row } = setup()
    panel.element.scrollTop = 37
    row('e:p1').querySelector<HTMLElement>('button.eye')!.click()
    expect(app.store.state.hidden.length).toBe(1)
    expect(panel.element.scrollTop).toBe(37)
    toggleHidden(app.store, 'e:p1')
    expect(panel.element.scrollTop).toBe(37)
  })
})
