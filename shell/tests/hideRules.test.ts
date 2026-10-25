// Hide by class (viewport > Hide): rule matching, what the views keep, picking, selection, persistence. A view setting: never in the map, never an undo step.
import { describe, expect, it } from 'vitest'
import { addEntity, emptyDocument } from '../src/model/document'
import type { Entity } from '../src/model/document'
import { makeBox } from '../src/model/geometry'
import { pick } from '../src/render/picking'
import { addRule, defaultHideRules, matchingRule, restoreHideRules, ruleMatcher, volumeClasses, visibleEntities } from '../src/model/visibility'
import type { HideRule } from '../src/model/visibility'
import { setHideRules } from '../src/state/actions'
import { makeApp } from './helpers'

const brushEnt = (id: string, classname: string, x: number): Entity => ({ id, classname, brushes: [makeBox(`${id}b`, [x, 0, 0], [x + 1, 1, 1])] })
const world: Entity = { id: 'w', classname: 'worldspawn', brushes: [makeBox('wb', [0, 0, 0], [1, 1, 1])] }
const trig = brushEnt('t1', 'trigger_hurt', 2), trig2 = brushEnt('t2', 'trigger_push', 4), area = brushEnt('a1', 'editor_playable_area', 6), detail = brushEnt('d1', 'func_detail', 8)
const point: Entity = { id: 'p1', classname: 'info_player_start' }
const on = (...patterns: string[]): HideRule[] => patterns.map((pattern) => ({ pattern, enabled: true }))

describe('rule matching', () => {
  it('`*` is a wildcard, a plain name is exact, case does not matter, regex characters are plain characters', () => {
    const m = ruleMatcher(on('trigger_*', 'editor_playable_area', 'a.b+c'))
    expect(['trigger_hurt', 'Trigger_Push', 'trigger_', 'editor_playable_area', 'EDITOR_PLAYABLE_AREA', 'a.b+c'].map(m)).toEqual(Array(6).fill(true))
    expect(['trigger', 'my_trigger_x', 'editor_playable_area2', 'editor_other', 'axb+c', 'func_detail', undefined].map(m)).toEqual(Array(7).fill(false))
  })
  it('disabled rules do nothing, empty patterns match nothing, worldspawn is never hidden by a rule', () => {
    expect(ruleMatcher([{ pattern: 'trigger_*', enabled: false }])('trigger_hurt')).toBe(false)
    expect(ruleMatcher(on('', '  '))('anything')).toBe(false)
    const all = ruleMatcher(on('*'))
    expect(all('worldspawn')).toBe(false); expect(all('func_detail')).toBe(true)
  })
  it('addRule normalizes, enables an existing rule instead of duplicating it, ignores empty input', () => {
    expect(addRule([], '  FOO_*  ')).toEqual([{ pattern: 'foo_*', enabled: true, custom: true }])
    const r = addRule(defaultHideRules(), 'Trigger_*')
    expect(r).toHaveLength(2); expect(r[0]).toMatchObject({ pattern: 'trigger_*', enabled: true })
    expect(addRule(r, '   ')).toEqual(r)
  })
  it('matchingRule names the rule that hides a class', () => {
    const r = [{ pattern: 'trigger_*', enabled: true }, { pattern: 'func_detail', enabled: false }]
    expect(matchingRule('trigger_hurt', r)?.pattern).toBe('trigger_*')
    expect(matchingRule('func_detail', r)).toBeUndefined()
  })
})

describe('what the views keep', () => {
  const all = [world, trig, trig2, area, detail, point]
  it('entities of a hidden class are left out; with no rule on, the very same array comes back', () => {
    expect(visibleEntities(all, [], defaultHideRules())).toBe(all)
    expect(visibleEntities(all, [], on('trigger_*', 'editor_playable_area')).map((e) => e.id)).toEqual(['w', 'd1', 'p1'])
  })
  it('rules and hidden keys add up', () => {
    expect(visibleEntities(all, ['e:d1'], on('trigger_*')).map((e) => e.id)).toEqual(['w', 'a1', 'p1'])
  })
  it('point entities are matched by class too', () => {
    expect(visibleEntities(all, [], on('info_*')).map((e) => e.id)).toEqual(['w', 't1', 't2', 'a1', 'd1'])
  })
  it('volumeClasses lists the brush entity classes with counts, triggers and editor volumes first, never worldspawn or point entities', () => {
    expect(volumeClasses([world, detail, trig, trig2, area, point, brushEnt('t3', 'trigger_hurt', 9)])).toEqual([
      { classname: 'trigger_hurt', count: 2 }, { classname: 'trigger_push', count: 1 }, { classname: 'editor_playable_area', count: 1 }, { classname: 'func_detail', count: 1 },
    ])
  })
})

describe('hidden things are not picked', () => {
  const ray = (x: number) => ({ origin: [x + 0.5, 0.5, 10] as [number, number, number], dir: [0, 0, -1] as [number, number, number] })
  it('a ray through a trigger hits it until its class is hidden, then hits what is behind it (or nothing)', () => {
    const ents = [world, trig]
    expect(pick(ray(2), ents)?.entityId).toBe('t1')
    expect(pick(ray(2), visibleEntities(ents, [], on('trigger_*')))).toBeNull()
    expect(pick(ray(0), visibleEntities(ents, [], on('trigger_*')))?.entityId).toBe('w')
  })
  it('a cut wall whose subtractor is hidden is skipped (skipTri)', () => {
    const tri = { a: [0, 0, 0] as [number, number, number], b: [1, 0, 0] as [number, number, number], c: [0, 1, 0] as [number, number, number], normal: [0, 0, 1] as [number, number, number], entityId: 't1', instance: '', brushId: 't1b', faceId: 1 }
    const carved = new Map([['w/wb', { pickTris: [tri], textures: new Map(), edges: [] } as never]])
    const r = { origin: [0.2, 0.2, 5] as [number, number, number], dir: [0, 0, -1] as [number, number, number] }
    expect(pick(r, [world], { carved })?.entityId).toBe('t1')
    expect(pick(r, [world], { carved, skipTri: (t) => t.entityId === 't1' })).toBeNull()
  })
})

describe('persistence', () => {
  it('restoreHideRules: presets always present with their saved state, custom rules kept, junk dropped', () => {
    const got = restoreHideRules([{ pattern: 'trigger_*', enabled: true }, { pattern: ' FUNC_*', enabled: false }, { pattern: 'func_*', enabled: true }, { pattern: 5, enabled: true }, null, { pattern: '', enabled: true }, { pattern: 'x'.repeat(200), enabled: true }])
    expect(got.map((r) => [r.pattern, r.enabled, !!r.custom])).toEqual([['trigger_*', true, false], ['editor_playable_area', false, false], ['func_*', true, true]])
    expect(restoreHideRules('nope')).toEqual(defaultHideRules())
    expect(restoreHideRules(undefined)).toEqual(defaultHideRules())
  })
})

describe('store + selection', () => {
  const setup = () => {
    const app = makeApp()
    app.sync([world, trig, detail].reduce((d, e) => addEntity(d, e), emptyDocument()))
    return app
  }
  it('rules are a view setting: no undo step, the map keeps the entity, the selection drops what got hidden', () => {
    const app = setup(), st = () => app.store.state
    const steps = st().history.length
    app.store.selectMany([{ entityId: 't1', brushId: 't1b' }, { entityId: 'd1', brushId: 'd1b' }])
    setHideRules(app.store, addRule(st().hideRules, 'trigger_*'))
    expect(st().hideRules.find((r) => r.pattern === 'trigger_*')?.enabled).toBe(true)
    expect(st().multi.map((s) => s.entityId)).toEqual(['d1'])
    expect(st().history.length).toBe(steps)
    expect(st().doc.entities.some((e) => e.id === 't1')).toBe(true)
    setHideRules(app.store, addRule(st().hideRules, 'func_detail'))
    expect(st().selection).toBeNull(); expect(st().multi).toEqual([])
  })
  it('turning a rule off shows the entities again', () => {
    const app = setup(), st = () => app.store.state
    setHideRules(app.store, on('trigger_*'))
    expect(visibleEntities(st().doc.entities, st().hidden, st().hideRules).some((e) => e.id === 't1')).toBe(false)
    setHideRules(app.store, st().hideRules.map((r) => ({ ...r, enabled: false })))
    expect(visibleEntities(st().doc.entities, st().hidden, st().hideRules).some((e) => e.id === 't1')).toBe(true)
  })
})
