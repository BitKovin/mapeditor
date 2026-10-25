// @vitest-environment jsdom
// Selecting something in the viewport scrolls the outliner to its row.
import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { addEntity, emptyDocument } from '../src/model/document'
import type { Entity } from '../src/model/document'
import { makeBox } from '../src/model/geometry'
import { OutlinerPanel } from '../src/ui/panels'
import { expandOutliner, makeApp } from './helpers'

// jsdom has no layout: rows sit 20px apart in a 100px tall panel scrolled to the top, and scrollIntoView is recorded
let revealed: string[] = []
let top = 0  // panel scroll offset
const realRect = Element.prototype.getBoundingClientRect
beforeEach(() => {
  revealed = []; top = 0
  Element.prototype.scrollIntoView = function (this: HTMLElement) { revealed.push(this.dataset.key ?? '?'); top = Number(this.dataset.y) - 40 }
  Element.prototype.getBoundingClientRect = function (this: HTMLElement) {
    if (this.classList.contains('panel')) return { top: 0, bottom: 100, left: 0, right: 100, width: 100, height: 100, x: 0, y: 0, toJSON() {} } as DOMRect
    const y = Number(this.dataset.y ?? 0) - top
    return { top: y, bottom: y + 20, left: 0, right: 100, width: 100, height: 20, x: 0, y, toJSON() {} } as DOMRect
  }
})
afterEach(() => { Element.prototype.getBoundingClientRect = realRect; document.body.replaceChildren() })

const wall = (n: number): Entity => ({ id: 'w', classname: 'worldspawn', brushes: Array.from({ length: n }, (_, i) => makeBox(`b${i}`, [i * 2, 0, 0], [i * 2 + 1, 1, 1])) })
const point = (id: string): Entity => ({ id, classname: 'info_player_start', keyvalues: { origin: '0 0 0' } })

function setup() {
  const app = makeApp()
  app.sync([wall(30), point('p1'), { id: 'd1', classname: 'func_detail', brushes: [makeBox('d1a', [0, 5, 0], [1, 6, 1]), makeBox('d1b', [2, 5, 0], [3, 6, 1])] }].reduce((d, e) => addEntity(d, e), emptyDocument()))
  const panel = new OutlinerPanel(app.store); panel.init()
  document.body.append(panel.element)
  expandOutliner(panel.element)
  const place = () => [...panel.element.querySelectorAll<HTMLElement>('[data-key]')].forEach((r, i) => { r.dataset.y = String(i * 20) })
  place()
  // rows are rebuilt on every change: give the new rows their positions before the panel looks at them
  const origReplace = panel.element.replaceChildren.bind(panel.element)
  panel.element.replaceChildren = (...n: (Node | string)[]) => { origReplace(...n); place() }
  return { app, panel }
}

describe('outliner follows the selection', () => {
  it('a brush picked in the viewport: its row is scrolled to', () => {
    const { app } = setup()
    app.store.select({ entityId: 'w', brushId: 'b25' })
    expect(revealed).toEqual(['b:w|b25'])
  })
  it('a point entity picked in the viewport: its row is scrolled to', () => {
    const { app } = setup()
    app.store.select({ entityId: 'p1' })
    expect(revealed).toEqual(['e:p1'])
  })
  it('a row that is already visible does not move the list', () => {
    const { app } = setup()
    app.store.select({ entityId: 'w', brushId: 'b1' })
    expect(revealed).toEqual([])
  })
  it('the primary selection of a multi-selection is the one scrolled to; a face selection goes to its brush', () => {
    const { app } = setup()
    app.store.selectMany([{ entityId: 'w', brushId: 'b2' }, { entityId: 'w', brushId: 'b27' }])
    expect(revealed).toEqual(['b:w|b27'])
    app.store.select({ entityId: 'w', brushId: 'b10', faceId: 2 })
    expect(revealed).toEqual(['b:w|b27', 'b:w|b10'])
  })
  it('an unrelated change (the same selection, a new entity) leaves the scroll position alone; clearing the selection too', () => {
    const { app } = setup()
    app.store.select({ entityId: 'w', brushId: 'b25' })
    top = 500  // the user scrolled somewhere else
    app.store.select(null)
    app.store.setMessage('hello')
    expect(revealed).toEqual(['b:w|b25'])
  })
  it('selecting the same thing again after it was deselected scrolls again', () => {
    const { app } = setup()
    app.store.select({ entityId: 'w', brushId: 'b25' }); top = 0
    app.store.select(null)
    app.store.select({ entityId: 'w', brushId: 'b25' })
    expect(revealed).toEqual(['b:w|b25', 'b:w|b25'])
  })
})
