// @vitest-environment jsdom
// Details: every key the class defines is a typed field (target = entity picker), targetname comes first (REQUIREMENTS §17 #5, §24 C2).
import { describe, expect, it } from 'vitest'
import { addPointEntity, setEntityRef, setKeyvalue } from '../src/state/actions'
import type { EntityClassDef } from '../src/net/protocol'
import { InspectorPanel } from '../src/ui/panels'
import { fieldKind } from '../src/ui/properties'
import { makeApp } from './helpers'

const NAV: EntityClassDef = {
  name: 'nav_point', kind: 'point', color: [0, 0.5, 0], box: [-8, -8, 0, 8, 8, 32], description: '', flags: [],
  keys: [
    { key: 'angle', type: 'angle', name: 'Yaw Angle', description: '', default: '0' },
    { key: 'targetname', type: 'string', name: 'Name', description: '' },
    { key: 'waitTime', type: 'real', name: 'Wait Time', description: '', default: '0' },
    { key: 'target', type: 'target', name: 'Next nav point', description: '' },
    { key: 'loop', type: 'boolean', name: 'Loop', description: '' },
    { key: 'count', type: 'integer', name: 'Count', description: '' },
    { key: 'targetLocation', type: 'real3', name: 'Target Location', description: '', default: '0 0 0' },
    { key: 'classname', type: 'string', name: 'Classname', description: '', default: 'nav_point' },
  ],
}

const rows = (p: InspectorPanel) => [...p.element.querySelectorAll('label.field')] as HTMLElement[]
const row = (p: InspectorPanel, label: string) => rows(p).find((l) => l.querySelector('span')?.textContent === label)!
const refLabels = (p: InspectorPanel) => [...p.element.querySelectorAll('.field.ref')].map((l) => l.querySelector('span')!.textContent)

describe('Details: typed fields and references', () => {
  it('a reference with several targets shows one picker each plus one to add another; picking and removing edit the list', () => {
    const app = makeApp(); app.sync()
    const spawn = (name: string, x: number) => { addPointEntity(app.store, 'info_x', [x, 0, 0], { targetname: name }); return app.store.state.selection!.entityId }
    const a = spawn('a', 0), b = spawn('b', 1), c = spawn('c', 2), d = spawn('d', 3)
    setEntityRef(app.store, c, 'target', [a, b])
    app.store.select({ entityId: c })
    const refOf = () => app.store.state.doc.entities.find((e) => e.id === c)!.refs!.target
    const p = new InspectorPanel(app.store); p.init()
    const selects = () => [...p.element.querySelectorAll('.field.ref select')] as HTMLSelectElement[]
    expect(selects().map((s) => s.value)).toEqual([a, b, ''])                // two targets and the "add" picker
    expect(p.element.querySelector('.refwarn')).not.toBeNull()                // the names a and b differ
    selects()[2].value = d; selects()[2].dispatchEvent(new Event('change'))
    expect(refOf()).toEqual([a, b, d])
    const q = new InspectorPanel(app.store); q.init()
    const buttons = () => [...q.element.querySelectorAll('.field.ref .refrow button')].filter((x) => x.textContent === '×') as HTMLElement[]
    expect(buttons()).toHaveLength(3)
    buttons()[0].click()
    expect(refOf()).toEqual([b, d])
    p.dispose(); q.dispose()
  })

  it('targetname is right after the id; an undefined class gets no target / killtarget pickers it did not ask for', () => {
    const app = makeApp(); app.sync()
    addPointEntity(app.store, 'info_notnull', [0, 0, 0], { targetname: 'door1', note: 'x' })
    const id = app.store.state.selection!.entityId
    const p = new InspectorPanel(app.store)
    p.init()
    const labels = rows(p).map((l) => l.querySelector('span')?.textContent)
    expect(labels.indexOf('targetname')).toBe(labels.indexOf('id') + 1)
    expect(labels.filter((l) => l === 'targetname')).toHaveLength(1)
    expect(refLabels(p)).toEqual([])
    expect(labels).toContain('note')
    const tn = row(p, 'targetname').querySelector('input')!
    expect(tn.value).toBe('door1')
    tn.value = 'door2'; tn.dispatchEvent(new Event('change'))
    expect(app.store.state.doc.entities.find((e) => e.id === id)!.keyvalues!.targetname).toBe('door2')
    p.dispose()
    // a reference that is set shows its picker; adding "killtarget" adds an empty picker row, not a text value
    setEntityRef(app.store, id, 'target', id)
    const q = new InspectorPanel(app.store); q.init()
    expect(refLabels(q)).toEqual(['target'])
    const add = q.element.querySelector('input[placeholder="new key"]') as HTMLInputElement
    add.value = 'killtarget'
    ;[...q.element.querySelectorAll('button')].find((b) => b.textContent === 'Add')!.click()
    expect(refLabels(q)).toEqual(['target', 'killtarget'])
    expect(app.store.state.doc.entities.find((e) => e.id === id)!.keyvalues?.killtarget).toBeUndefined()
    q.dispose()
  })

  it('a defined class: each key gets the widget of its type, in .ent order, the target picker among them', () => {
    const app = makeApp(); app.sync()
    app.store.onMessage({ type: 'entity_defs', classes: [NAV], warnings: [] } as never)
    addPointEntity(app.store, 'nav_point', [0, 0, 0], {})
    const id = app.store.state.selection!.entityId
    const kv = () => app.store.state.doc.entities.find((e) => e.id === id)!.keyvalues ?? {}
    const p = new InspectorPanel(app.store)
    p.init()
    const all = [...p.element.querySelectorAll('.section, label.field')]
    const start = all.findIndex((x) => x.textContent === 'Properties')
    const end = all.findIndex((x) => x.textContent === 'Other keyvalues')
    // angle -> the angles row, targetname -> top, classname -> the classname field
    expect(all.slice(start + 1, end).map((l) => l.querySelector('span')!.textContent)).toEqual(['Wait Time', 'Next nav point', 'Loop', 'Count', 'Target Location'])
    expect(row(p, 'Next nav point').classList.contains('ref')).toBe(true)
    expect(row(p, 'Loop').querySelector('input')!.type).toBe('checkbox')
    const wait = row(p, 'Wait Time').querySelector('input')!
    expect(wait.type).toBe('number'); expect(wait.step).toBe('any'); expect(wait.value).toBe('0')
    expect(row(p, 'Count').querySelector('input')!.step).toBe('1')
    expect(row(p, 'Target Location').querySelectorAll('input[type=number]')).toHaveLength(3)
    wait.value = '0.25'; wait.dispatchEvent(new Event('change'))
    expect(kv().waitTime).toBe('0.25')
    const count = row(p, 'Count').querySelector('input')!
    count.value = '2.6'; count.dispatchEvent(new Event('change'))
    expect(kv().count).toBe('3')
    const loc = row(p, 'Target Location').querySelectorAll('input')
    loc[2].value = '5'; loc[2].dispatchEvent(new Event('change'))
    expect(kv().targetLocation).toBe('0 0 5')
    const loop = row(p, 'Loop').querySelector('input')!
    loop.checked = true; loop.dispatchEvent(new Event('change'))
    expect(kv().loop).toBe('1')
    setKeyvalue(app.store, id, 'loop', null)
    expect(kv().loop).toBeUndefined()
    p.dispose()
  })

  it('fieldKind maps .ent types to widgets', () => {
    const k = (type: string, items?: { name: string; value: string }[]) => fieldKind({ type, items })
    expect(k('target')).toBe('ref'); expect(k('target_destination')).toBe('ref')
    expect(k('real')).toBe('real'); expect(k('angle')).toBe('real'); expect(k('integer')).toBe('integer')
    expect(k('boolean')).toBe('boolean'); expect(k('color')).toBe('color01'); expect(k('color1')).toBe('color01'); expect(k('color255')).toBe('color255')
    expect(k('real3')).toBe('vec3'); expect(k('angles')).toBe('vec3')
    expect(k('list', [{ name: 'a', value: '1' }])).toBe('list')
    expect(k('string')).toBe('text'); expect(k('model')).toBe('asset'); expect(k('asset')).toBe('asset'); expect(k('array')).toBe('text')
  })
})
