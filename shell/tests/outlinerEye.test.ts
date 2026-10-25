// @vitest-environment jsdom
// The eye of every outliner row: hide / show entities, brushes and prefab instances (a view setting: not saved, not undone, still in the map).
import { afterEach, describe, expect, it } from 'vitest'
import { addEntity, emptyDocument } from '../src/model/document'
import type { Entity, MapDocument } from '../src/model/document'
import { makeBox } from '../src/model/geometry'
import { pick } from '../src/render/picking'
import { brushKey, entityKey, hiddenState, instanceKey, ruleMatcher, visibleEntities, visiblePrefabs } from '../src/model/visibility'
import { setHideRules, toggleHidden } from '../src/state/actions'
import { OutlinerPanel } from '../src/ui/panels'
import { expandOutliner, makeApp } from './helpers'

afterEach(() => document.body.replaceChildren())
const group: Entity = { id: 'g', classname: 'func_group', brushes: [makeBox('g1', [0, 0, 0], [1, 1, 1]), makeBox('g2', [2, 0, 0], [3, 1, 1])] }
const trig: Entity = { id: 't', classname: 'trigger_hurt', brushes: [makeBox('t1', [4, 0, 0], [5, 1, 1])] }
const point: Entity = { id: 'p', classname: 'info_player_start' }

function setup() {
  const app = makeApp()
  const doc: MapDocument = { ...[group, trig, point].reduce((d, e) => addEntity(d, e), emptyDocument()), prefabInstances: [{ id: 'pi', file: 'p.json', origin: [10, 0, 0] }] }
  app.sync(doc)
  app.sock().receive({ type: 'prefab_library', prefabs: { 'p.json': { ...emptyDocument(), entities: [{ id: 'pe', classname: 'light', origin: [1, 0, 0] as [number, number, number] }] } } })
  const panel = new OutlinerPanel(app.store); panel.init()
  document.body.append(panel.element)
  expandOutliner(panel.element)
  const row = (key: string) => panel.element.querySelector<HTMLElement>(`[data-key="${key}"]`)!
  const eye = (key: string) => row(key).querySelector<HTMLButtonElement>('button.eye')!
  return { app, panel, row, eye, st: () => app.store.state }
}

describe('model', () => {
  it('visiblePrefabs drops content of a hidden instance or class; nothing hidden = same array', () => {
    const a = { entity: { id: 'x', classname: 'light' }, instancePath: ['', 'i1'] }, b = { entity: { id: 'y', classname: 'trigger_once' }, instancePath: ['', 'i2'] }
    expect(visiblePrefabs([a, b], [], undefined)).toEqual([a, b])
    expect(visiblePrefabs([a, b], [instanceKey('i1')])).toEqual([b])
    expect(visiblePrefabs([a, b], [], [{ pattern: 'trigger_*', enabled: true }])).toEqual([a])
  })
  it('hiddenState: own key, class rule, or through the entity', () => {
    const none = ruleMatcher([]), trigs = ruleMatcher([{ pattern: 'trigger_*', enabled: true }])
    expect(hiddenState(new Set(), none, group)).toBe('no')
    expect(hiddenState(new Set([entityKey('g')]), none, group)).toBe('self')
    expect(hiddenState(new Set(), trigs, trig)).toBe('rule')
    expect(hiddenState(new Set([entityKey('g')]), none, group, 'g1')).toBe('parent')
    expect(hiddenState(new Set(), trigs, trig, 't1')).toBe('parent')
    expect(hiddenState(new Set([brushKey('g', 'g1')]), none, group, 'g1')).toBe('self')
    expect(hiddenState(new Set([brushKey('g', 'g1')]), none, group, 'g2')).toBe('no')
  })
})

describe('outliner eye', () => {
  it('every entity, brush and prefab row has an eye', () => {
    const { row } = setup()
    for (const k of ['e:g', 'b:g|g1', 'b:g|g2', 'e:t', 'b:t|t1', 'e:p', 'i:pi']) expect(row(k).querySelector('button.eye'), k).toBeTruthy()
  })
  it('clicking the eye of a brush hides only that brush: dimmed row, gone from the views and picking, still in the map, no undo step; again shows it', () => {
    const { app, row, eye, st } = setup()
    const steps = st().history.length
    eye('b:g|g1').click()
    expect(st().hidden).toEqual(['b:g|g1'])
    expect(row('b:g|g1').classList.contains('hid')).toBe(true); expect(row('b:g|g2').classList.contains('hid')).toBe(false)
    expect(eye('b:g|g1').getAttribute('aria-pressed')).toBe('true')
    const shown = visibleEntities(st().doc.entities, st().hidden, st().hideRules)
    expect(shown.find((e) => e.id === 'g')!.brushes!.map((b) => b.id)).toEqual(['g2'])
    expect(pick({ origin: [0.5, 0.5, 9], dir: [0, 0, -1] }, shown)).toBeNull()
    expect(pick({ origin: [2.5, 0.5, 9], dir: [0, 0, -1] }, shown)?.brushId).toBe('g2')
    expect(st().history.length).toBe(steps); expect(st().doc.entities.find((e) => e.id === 'g')!.brushes).toHaveLength(2)
    expect(st().selection).toBeNull()
    eye('b:g|g1').click()
    expect(st().hidden).toEqual([]); expect(row('b:g|g1').classList.contains('hid')).toBe(false)
    void app
  })
  it('the eye does not select the row; hiding the selected thing deselects it', () => {
    const { app, eye, st } = setup()
    eye('e:t').click()
    expect(st().selection).toBeNull()
    app.store.select({ entityId: 'p' })
    eye('e:p').click()
    expect(st().selection).toBeNull(); expect(st().multi).toEqual([])
  })
  it('hiding an entity dims its brushes and locks their eyes; a point entity can be hidden too', () => {
    const { row, eye, st } = setup()
    eye('e:g').click()
    expect(st().hidden).toEqual(['e:g'])
    expect(row('b:g|g1').classList.contains('hid')).toBe(true)
    expect(eye('b:g|g1').classList.contains('locked')).toBe(true)
    eye('b:g|g1').click()  // locked: nothing changes
    expect(st().hidden).toEqual(['e:g'])
    eye('e:p').click()
    expect(st().hidden).toEqual(['e:g', 'e:p'])
  })
  it('hidden by a Hide rule: the row shows it and its eye is locked', () => {
    const { app, row, eye, st } = setup()
    setHideRules(app.store, [{ pattern: 'trigger_*', enabled: true }])
    expect(row('e:t').classList.contains('hid')).toBe(true)
    expect(eye('e:t').classList.contains('locked')).toBe(true)
    eye('e:t').click()
    expect(st().hidden).toEqual([])
    expect(eye('e:t').title).toMatch(/rule/)
  })
  it('a prefab instance hides with all its content', () => {
    const { app, row, eye, st } = setup()
    expect(visiblePrefabs(st().prefabEntities, st().hidden, st().hideRules)).toHaveLength(1)
    app.store.select({ entityId: '', instanceId: 'pi' })
    eye('i:pi').click()
    expect(st().hidden).toEqual(['i:pi'])
    expect(row('i:pi').classList.contains('hid')).toBe(true)
    expect(st().selection).toBeNull()
    expect(visiblePrefabs(st().prefabEntities, st().hidden, st().hideRules)).toHaveLength(0)
    expect(st().prefabEntities).toHaveLength(1)  // still expanded: still in the map and the export
    eye('i:pi').click()
    expect(visiblePrefabs(st().prefabEntities, st().hidden, st().hideRules)).toHaveLength(1)
  })
  it('a hidden row cannot be selected by clicking it (it says how to get it back); a shown one can', () => {
    const { app, row, eye, st } = setup()
    row('e:p').click()
    expect(st().selection).toEqual({ entityId: 'p' })
    toggleHidden(app.store, 'e:p')
    row('e:p').click()
    expect(st().selection).toBeNull(); expect(st().message).toMatch(/eye/)
    eye('e:p').click()
    row('e:p').click()
    expect(st().selection).toEqual({ entityId: 'p' })
  })
  it('Unhide all (right-click) brings every row back', () => {
    const { app, row, eye } = setup()
    eye('b:g|g2').click(); eye('i:pi').click()
    app.store.setHidden([])
    expect(row('b:g|g2').classList.contains('hid')).toBe(false); expect(row('i:pi').classList.contains('hid')).toBe(false)
  })
})

describe('hide groups shared through the game profile', () => {
  it('the profile list is the list: presets stay, new patterns start off, the switches we had are kept', async () => {
    const { rulesFromGroups, rulePatterns } = await import('../src/model/visibility')
    const mine = [{ pattern: 'trigger_*', enabled: true }, { pattern: 'editor_playable_area', enabled: false }, { pattern: 'mine_only', enabled: true, custom: true }]
    const next = rulesFromGroups(['func_clip', 'Trigger_*'], mine)
    expect(rulePatterns(next)).toEqual(['trigger_*', 'editor_playable_area', 'func_clip'])
    expect(next.map((r) => r.enabled)).toEqual([true, false, false])
    expect(next[2].custom).toBe(true)
    expect(rulesFromGroups([], []).length).toBe(2)  // an empty shared list still has the presets
  })
})
