// @vitest-environment jsdom
// Details: references are ordinary keyvalues, targetname comes first (REQUIREMENTS §17 #5, §24 C2).
import { describe, expect, it } from 'vitest'
import { addPointEntity, setKeyvalue } from '../src/state/actions'
import { InspectorPanel } from '../src/ui/panels'
import { makeApp } from './helpers'

describe('Details: references and targetname', () => {
  it('no References section; target / killtarget rows sit under Keyvalues; targetname is right after the id and edits the key', () => {
    const app = makeApp(); app.sync()
    addPointEntity(app.store, 'info_notnull', [0, 0, 0], { targetname: 'door1', note: 'x' })
    const id = app.store.state.selection!.entityId
    const p = new InspectorPanel(app.store)
    p.init()
    const sections = [...p.element.querySelectorAll('.section')].map((s) => s.textContent)
    expect(sections).not.toContain('References')
    const labels = [...p.element.querySelectorAll('label.field')].map((l) => l.querySelector('span')?.textContent)
    expect(labels.indexOf('targetname')).toBe(labels.indexOf('id') + 1)
    const kv = sections.findIndex((t) => t === 'Keyvalues')
    expect(kv).toBeGreaterThanOrEqual(0)
    // the reference rows come after the Keyvalues heading, with their picker; targetname is not listed twice
    const all = [...p.element.querySelectorAll('.section, label.field')]
    const kvAt = all.findIndex((x) => x.textContent === 'Keyvalues')
    const targetAt = all.findIndex((x) => x.classList.contains('ref') && x.querySelector('span')?.textContent === 'target')
    expect(targetAt).toBeGreaterThan(kvAt)
    expect(labels.filter((l) => l === 'targetname')).toHaveLength(1)
    const tn = [...p.element.querySelectorAll('label.field')].find((l) => l.querySelector('span')?.textContent === 'targetname')!.querySelector('input')!
    expect(tn.value).toBe('door1')
    tn.value = 'door2'; tn.dispatchEvent(new Event('change'))
    expect(app.store.state.doc.entities.find((e) => e.id === id)!.keyvalues!.targetname).toBe('door2')
    setKeyvalue(app.store, id, 'targetname', null)
    expect(app.store.state.doc.entities.find((e) => e.id === id)!.keyvalues?.targetname).toBeUndefined()
    p.dispose()
  })
})
