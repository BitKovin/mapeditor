// @vitest-environment jsdom
// Level settings (REQUIREMENTS §22 P2): worldspawn keys, conversions, the action and the panel.
import { describe, expect, it } from 'vitest'
import { LEVEL_FIELDS, ambientKey, ambientOf, colorOfHex, hexOf, levelKeyFor, levelValue, worldspawnOf } from '../src/model/levelSettings'
import { addDrawnBrush, setLevelKey } from '../src/state/actions'
import { makeBox } from '../src/model/geometry'
import { LevelSettingsPanel } from '../src/ui/levelSettingsPanel'
import { makeApp } from './helpers'

const field = (key: string) => LEVEL_FIELDS.find((f) => f.key === key)!

describe('level settings model', () => {
  it('every key the Core reads has a field (core/lights.h levelSettingsOf)', () => {
    expect(LEVEL_FIELDS.map((f) => f.key).sort()).toEqual(['_ao_intensity', '_ao_power', '_ao_radius', '_bake_quality', '_cc_brightness', '_cc_exposure', '_light_scale', '_lightvol_grid', '_rays_per_probe', '_rays_per_texel', '_supersample'])
  })
  it('typed values are clamped; empty or the default removes the key; whole numbers where needed', () => {
    expect(levelKeyFor(field('_light_scale'), '2.5')).toBe('2.5')
    expect(levelKeyFor(field('_light_scale'), '1')).toBeNull()        // the default
    expect(levelKeyFor(field('_light_scale'), '')).toBeNull()
    expect(levelKeyFor(field('_ao_power'), '50')).toBe('10')          // clamped
    expect(levelKeyFor(field('_ao_intensity'), '3')).toBeNull()       // clamped to 1, which is the default
    expect(levelKeyFor(field('_supersample'), '3.7')).toBe('4')       // integer
    expect(levelKeyFor(field('_rays_per_texel'), '1')).toBe('1')      // no default: always stored
    expect(levelKeyFor(field('_bake_quality'), '2')).toBeNull()       // default 2
    expect(levelKeyFor(field('_cc_exposure'), 'abc')).toBeNull()
  })
  it('reads stored values; absent or broken = null', () => {
    const w = { id: 'w', classname: 'worldspawn', keyvalues: { _ao_radius: '1.5', _light_scale: 'x' } }
    expect(levelValue(w, field('_ao_radius'))).toBe(1.5)
    expect(levelValue(w, field('_light_scale'))).toBeNull()
    expect(levelValue(undefined, field('_ao_power'))).toBeNull()
  })
  it('ambient light: colour and intensity round trip through "r g b"', () => {
    const k = ambientKey([1, 0.5, 0], 0.2)
    expect(k).toBe('0.2 0.1 0')
    const back = ambientOf({ id: 'w', keyvalues: { _ambient: k! } })
    expect(back.intensity).toBeCloseTo(0.2, 12); expect(back.color).toEqual([1, 0.5, 0])
    expect(ambientKey([1, 1, 1], 0)).toBeNull()
    expect(ambientOf({ id: 'w', keyvalues: { _ambient: 'bad' } }).intensity).toBe(0)
    expect(hexOf([1, 0.5, 0])).toBe('#ff8000'); expect(colorOfHex('#ff8000')[1]).toBeCloseTo(128 / 255, 12)
  })
})

describe('level settings action and panel', () => {
  it('setLevelKey writes worldspawn keys as undoable steps and removes them with null', () => {
    const app = makeApp(); app.sync(); addDrawnBrush(app.store, makeBox('b1', [0, 0, 0], [1, 1, 1]))
    expect(setLevelKey(app.store, '_ao_radius', '1.5')).toBe(true)
    expect(worldspawnOf(app.store.state.doc)!.keyvalues!._ao_radius).toBe('1.5')
    expect(setLevelKey(app.store, '_ao_radius', '1.5')).toBe(false)  // no change, no step
    setLevelKey(app.store, '_ao_radius', null)
    expect(worldspawnOf(app.store.state.doc)!.keyvalues?._ao_radius).toBeUndefined()
    app.store.undo()
    expect(worldspawnOf(app.store.state.doc)!.keyvalues!._ao_radius).toBe('1.5')
  })
  it('the panel shows stored values, placeholders for defaults, and a change stores the key', () => {
    const app = makeApp(); app.sync(); addDrawnBrush(app.store, makeBox('b1', [0, 0, 0], [1, 1, 1]))
    setLevelKey(app.store, '_light_scale', '3')
    const p = new LevelSettingsPanel(app.store)
    p.init()
    const input = (label: string) => [...p.element.querySelectorAll('label.field')].find((l) => l.textContent!.startsWith(label))!.querySelector('input') as HTMLInputElement
    expect(input('General brightness').value).toBe('3')
    expect(input('Rays per lightmap pixel').placeholder).toBe('lighting.json')
    const ao = input('Radius (m)')
    ao.value = '0.75'; ao.dispatchEvent(new Event('change'))
    expect(worldspawnOf(app.store.state.doc)!.keyvalues!._ao_radius).toBe('0.75')
    expect(input('Radius (m)').value).toBe('0.75')  // the panel rebuilt with the new value
    p.dispose()
  })
})
