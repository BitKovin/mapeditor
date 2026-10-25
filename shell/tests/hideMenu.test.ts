// @vitest-environment jsdom
// The viewport's Hide button and its menu.
import { afterEach, describe, expect, it } from 'vitest'
import { addEntity, emptyDocument } from '../src/model/document'
import type { Entity } from '../src/model/document'
import { makeBox } from '../src/model/geometry'
import { buildHideMenu } from '../src/ui/hideMenu'
import { makeApp } from './helpers'

afterEach(() => document.body.replaceChildren())
const be = (id: string, classname: string): Entity => ({ id, classname, brushes: [makeBox(`${id}b`, [0, 0, 0], [1, 1, 1])] })

function setup() {
  const app = makeApp()
  app.sync([be('t1', 'trigger_hurt'), be('t2', 'trigger_hurt'), be('d1', 'func_detail')].reduce((d, e) => addEntity(d, e), emptyDocument()))
  const m = buildHideMenu(app.store)
  app.store.subscribe(() => m.refresh())  // the viewport does this for its header
  document.body.append(m.wrap)
  m.button.click()
  const rows = () => [...m.menu.querySelectorAll<HTMLLabelElement>('label.vp-menu-row')]
  const row = (text: string) => rows().find((r) => r.querySelector('.hide-pattern')?.textContent === text)!
  const box = (text: string) => row(text).querySelector<HTMLInputElement>('input')!
  /** the checkbox of a class in "Volumes in this map" (a hidden class is also listed among the rules) */
  const classBox = (text: string) => [...m.menu.querySelectorAll<HTMLLabelElement>('.hide-classes label')].find((r) => r.querySelector('.hide-pattern')?.textContent === text)!.querySelector<HTMLInputElement>('input')!
  const enabled = () => app.store.state.hideRules.filter((r) => r.enabled).map((r) => r.pattern)
  return { app, m, rows, row, box, classBox, enabled }
}

describe('Hide menu', () => {
  it('opens with the two presets off and the volume classes of the map, and the button shows nothing active', () => {
    const { m, row, rows } = setup()
    expect(m.menu.style.display).toBe('')
    expect(m.button.textContent).toBe('Hide ▾'); expect(m.button.classList.contains('on')).toBe(false)
    expect(rows().map((r) => r.querySelector('.hide-pattern')!.textContent)).toEqual(['trigger_*', 'editor_playable_area', 'trigger_hurt', 'func_detail'])
    expect(row('trigger_hurt').textContent).toContain('×2')
  })
  it('ticking a preset hides its class and the button counts the active rules', () => {
    const { app, m, box, enabled } = setup()
    box('trigger_*').click()
    expect(enabled()).toEqual(['trigger_*'])
    expect(m.button.textContent).toBe('Hide (1) ▾'); expect(m.button.classList.contains('on')).toBe(true)
    expect(box('trigger_hurt').checked).toBe(true); expect(box('trigger_hurt').disabled).toBe(true)  // covered by trigger_*: shown, not separately switchable
    expect(app.store.state.doc.entities).toHaveLength(3)  // still in the map
  })
  it('a custom rule is added with Enter (and the field stays usable), listed, removable', () => {
    const { app, m, row, enabled } = setup()
    const input = m.menu.querySelector<HTMLInputElement>('.hide-input')!
    input.value = 'Func_*'
    input.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))
    expect(input.value).toBe('')
    expect(enabled()).toEqual(['func_*'])
    expect(row('func_*')).toBeTruthy()
    row('func_*').querySelector<HTMLButtonElement>('.hide-x')!.click()
    expect(app.store.state.hideRules.map((r) => r.pattern)).toEqual(['trigger_*', 'editor_playable_area'])
  })
  it('a volume class of the map is hidden with its own checkbox and shown again by unticking it; presets are not removed', () => {
    const { app, classBox, enabled } = setup()
    classBox('func_detail').click()
    expect(enabled()).toEqual(['func_detail'])
    expect(app.store.state.hideRules.at(-1)).toMatchObject({ pattern: 'func_detail', custom: true })
    classBox('func_detail').click()
    expect(app.store.state.hideRules.map((r) => r.pattern)).toEqual(['trigger_*', 'editor_playable_area'])
  })
  it('"Show all classes" switches every rule off', () => {
    const { m, box, classBox, enabled } = setup()
    box('trigger_*').click(); classBox('func_detail').click()
    expect(enabled().length).toBe(2)
    ;[...m.menu.querySelectorAll<HTMLButtonElement>('.hide-foot button')][0].click()
    expect(enabled()).toEqual([])
    expect(m.button.classList.contains('on')).toBe(false)
  })
})
