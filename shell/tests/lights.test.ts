import { describe, expect, it } from 'vitest'
import { defaultLightAngles, isModernLight, lightFrame, lightGizmo, lightParams } from '../src/model/lights'
import { clampValue, colorToHex, currentValue, formatColor, hexToColor, isChecked, isDefault, parseColor1, propertyKeys } from '../src/ui/properties'
import type { EntityClassDef, EntityKeyDef } from '../src/net/protocol'
import type { Entity } from '../src/model/document'

const key = (k: string, type: string, def: string, extra: Partial<EntityKeyDef> = {}): EntityKeyDef => ({ key: k, type, name: k, description: '', default: def, ...extra })
const classDef = (name: string, keys: EntityKeyDef[]): EntityClassDef => ({ name, kind: 'point', color: [1, 1, 1], box: null, description: '', keys, flags: [] })
const ent = (classname: string, keyvalues: Record<string, string> = {}, extra: Partial<Entity> = {}): Entity => ({ id: 'l', classname, origin: [0, 0, 0], angles: [0, 0, 0], keyvalues, brushes: [], ...extra }) as Entity

// every segment of a flat line list
const segments = (l: number[]): [number[], number[]][] => { const out: [number[], number[]][] = []; for (let i = 0; i < l.length; i += 6) out.push([l.slice(i, i + 3), l.slice(i + 3, i + 6)]); return out }
const dist = (a: number[], b: number[]): number => Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2])
const maxDistFrom = (l: number[], c: number[]): number => Math.max(...segments(l).flatMap(([a, b]) => [dist(a, c), dist(b, c)]))

describe('light parameters (the baker\'s keys, clamps and defaults)', () => {
  it('knows the modern classes only', () => {
    expect(isModernLight('light_spot')).toBe(true)
    expect(isModernLight('light')).toBe(false)
    expect(isModernLight(undefined)).toBe(false)
    expect(lightParams(ent('light'))).toBeNull()
    expect(lightParams(ent('func_door'))).toBeNull()
  })
  it('uses the .ent defaults for keys that are not set, the entity\'s values otherwise', () => {
    const def = classDef('light_point', [key('radius', 'real', '7'), key('brightness', 'real', '2'), key('color', 'color1', '0.5 1 1')])
    const p = lightParams(ent('light_point', { brightness: '3' }), def)!
    expect(p.kind).toBe('point')
    expect(p.radius).toBe(7)
    expect(p.brightness).toBe(3)
    expect(p.color).toEqual([0.5, 1, 1])
    expect(p.castShadows).toBe(true)     // the built-in fallback for a key the definition does not list
    expect(lightParams(ent('light_point'))!.radius).toBe(10) // no definitions at all: the baker's defaults
  })
  it('clamps like the baker: inner < outer, ranges, style 0..63, shadows flag', () => {
    const p = lightParams(ent('light_spot', { inner_angle: '60', outer_angle: '40', radius: '-3', brightness: '-1', style: '99', cast_shadows: '0', width: '0', source_size: '-2' }))!
    expect(p.outerAngle).toBe(40)
    expect(p.innerAngle).toBe(20)          // inner >= outer: half of outer
    expect(p.radius).toBeCloseTo(1e-3)
    expect(p.brightness).toBe(0)
    expect(p.style).toBe(63)
    expect(p.castShadows).toBe(false)
    expect(p.width).toBeCloseTo(1e-3)
    expect(p.sourceSize).toBe(0)
    expect(lightParams(ent('light_spot', { outer_angle: '200' }))!.outerAngle).toBe(89.9)
  })
  it('a bad colour falls back to white', () => { expect(lightParams(ent('light_point', { color: 'red' }))!.color).toEqual([1, 1, 1]) })
  it('the frame is the baker\'s: forward = R*(0,0,1); pitch 90 points straight down (the default of spot / sun / rect lights)', () => {
    const f = lightFrame(ent('light_spot', {}, { angles: [90, 0, 0] }))
    expect(f.forward[1]).toBeCloseTo(-1)
    expect(Math.hypot(f.forward[0], f.forward[2])).toBeCloseTo(0)
    expect(lightFrame(ent('light_spot')).forward).toEqual([0, 0, 1])
    expect(defaultLightAngles('light_spot')).toEqual([90, 0, 0])
    expect(defaultLightAngles('light_directional')).toEqual([90, 0, 0])
    expect(defaultLightAngles('light_rect')).toEqual([90, 0, 0])
    expect(defaultLightAngles('light_point')).toBeUndefined()
    expect(defaultLightAngles('light_sky')).toBeUndefined()
  })
})

describe('light preview gizmos', () => {
  const o = [1, 2, 3]
  const at = (cls: string, kv: Record<string, string> = {}, angles: [number, number, number] = [0, 0, 0]) => ent(cls, kv, { origin: o as any, angles })
  const gizmo = (e: Entity, selected: boolean) => lightGizmo(e, lightParams(e)!, selected)

  it('every kind draws something, always whole segments of finite numbers, centred on the light', () => {
    for (const cls of ['light_point', 'light_spot', 'light_directional', 'light_rect', 'light_sky']) {
      for (const sel of [false, true]) {
        const l = gizmo(at(cls), sel)
        expect(l.length).toBeGreaterThan(0)
        expect(l.length % 6).toBe(0)
        expect(l.every(Number.isFinite)).toBe(true)
      }
    }
  })
  it('a point light: compact diamond, and when selected the attenuation radius and the source sphere', () => {
    const icon = gizmo(at('light_point', { radius: '5', source_size: '0.4' }), false)
    expect(maxDistFrom(icon, o)).toBeLessThan(0.3)
    const sel = gizmo(at('light_point', { radius: '5', source_size: '0.4' }), true)
    expect(maxDistFrom(sel, o)).toBeCloseTo(5, 5)
    const radii = new Set(segments(sel).map(([a]) => +dist(a, o).toFixed(3)))
    expect(radii.has(5)).toBe(true)
    expect(radii.has(0.4)).toBe(true)  // the source sphere
    expect(sel.length).toBeGreaterThan(icon.length)
  })
  it('a spot light: the cone opens by the outer angle along its direction, to the radius when selected, with the inner cone inside', () => {
    const e = at('light_spot', { radius: '6', inner_angle: '10', outer_angle: '30' }, [90, 0, 0]) // looking down
    const sel = gizmo(e, true)
    // the outer circle sits at distance 6 below the light with radius 6 * tan(30deg)
    const base = [1, 2 - 6, 3]
    const circle = segments(sel).filter(([a, b]) => Math.abs(a[1] - base[1]) < 1e-6 && Math.abs(b[1] - base[1]) < 1e-6)
    expect(circle.length).toBeGreaterThan(30)
    const radii = circle.map(([a]) => +Math.hypot(a[0] - 1, a[2] - 3).toFixed(4))
    expect(Math.max(...radii)).toBeCloseTo(6 * Math.tan(30 * Math.PI / 180), 3)
    expect(Math.min(...radii)).toBeCloseTo(6 * Math.tan(10 * Math.PI / 180), 3) // the inner cone's circle at the same distance
    // the apex is the light: spokes start at its origin
    expect(segments(sel).some(([a]) => dist(a, o) < 1e-9)).toBe(true)
    // unselected: a short cone only (<= 1.2 m)
    expect(Math.max(...segments(gizmo(e, false)).flatMap(([a, b]) => [Math.abs(a[1] - 2), Math.abs(b[1] - 2)]))).toBeLessThanOrEqual(1.2 + 1e-9)
    // the direction follows the angles: pitch 0 looks along +z
    const zs = segments(gizmo(at('light_spot', { radius: '6' }, [0, 0, 0]), true)).flatMap(([a, b]) => [a[2], b[2]])
    expect(Math.max(...zs)).toBeCloseTo(3 + 6, 5)
  })
  it('a directional light: arrows along the direction, longer when selected, angular size as a cone', () => {
    const e = at('light_directional', { source_size: '10' }, [90, 0, 0])
    const sel = gizmo(e, true), icon = gizmo(e, false)
    const lowest = (l: number[]) => Math.min(...segments(l).flatMap(([a, b]) => [a[1], b[1]]))
    expect(lowest(icon)).toBeCloseTo(2 - 1.6, 5)
    expect(lowest(sel)).toBeCloseTo(2 - 4, 5)
    // the cone of the angular diameter: radius = 4 * tan(5deg) at the far end
    const far = segments(sel).filter(([a, b]) => Math.abs(a[1] - (2 - 4)) < 1e-6 && Math.abs(b[1] - (2 - 4)) < 1e-6)
    expect(Math.max(...far.map(([a]) => Math.hypot(a[0] - 1, a[2] - 3)))).toBeCloseTo(4 * Math.tan(5 * Math.PI / 180), 3)
  })
  it('a rect light: the real width x height rectangle in its own plane, an arrow out of its front, reach when selected', () => {
    const e = at('light_rect', { width: '4', height: '2', radius: '8' }, [90, 0, 0]) // facing down: the panel lies in the xz plane
    const icon = gizmo(e, false)
    const corners = new Set(segments(icon).flatMap(([a, b]) => [a, b]).filter((p) => Math.abs(p[1] - 2) < 1e-9).map((p) => p.map((x) => +x.toFixed(6)).join(',')))
    const xs = [...corners].map((s) => Number(s.split(',')[0])), zs = [...corners].map((s) => Number(s.split(',')[2]))
    expect(Math.max(...xs) - Math.min(...xs)).toBeCloseTo(4, 5)
    expect(Math.max(...zs) - Math.min(...zs)).toBeCloseTo(2, 5)  // right = +x, up = +z for pitch 90
    expect(Math.min(...segments(icon).flatMap(([a, b]) => [a[1], b[1]]))).toBeLessThan(2) // the arrow points down
    expect(Math.min(...segments(gizmo(e, true)).flatMap(([a, b]) => [a[1], b[1]]))).toBeCloseTo(2 - 8, 5) // rays from the corners to the radius
  })
  it('a sky light: a dome above its position whose size grows when selected', () => {
    const icon = gizmo(at('light_sky'), false), sel = gizmo(at('light_sky'), true)
    expect(Math.max(...segments(icon).flatMap(([a, b]) => [a[1], b[1]]))).toBeLessThan(2 + 1)
    expect(Math.max(...segments(sel).flatMap(([a, b]) => [a[1], b[1]]))).toBeCloseTo(2 + 2, 5)
  })
  it('follows the entity: moving it moves the gizmo, nothing else changes', () => {
    const a = gizmo(at('light_spot', { radius: '4' }), true)
    const b = lightGizmo(ent('light_spot', { radius: '4' }, { origin: [11, 2, 3] as any }), lightParams(ent('light_spot', { radius: '4' }))!, true)
    expect(b.length).toBe(a.length)
    for (let i = 0; i < a.length; i += 3) { expect(b[i] - a[i]).toBeCloseTo(10, 9); expect(b[i + 1]).toBeCloseTo(a[i + 1], 9) }
  })
})

describe('typed property helpers', () => {
  it('colour <-> hex <-> "r g b"', () => {
    expect(parseColor1('1 0.5 0.25')).toEqual([1, 0.5, 0.25])
    expect(parseColor1('1 0.5')).toBeNull()
    expect(parseColor1('a b c')).toBeNull()
    expect(colorToHex([1, 0.5, 0])).toBe('#ff8000')
    expect(colorToHex([2, -1, 0])).toBe('#ff0000') // clamped
    expect(hexToColor('#ff8000')![1]).toBeCloseTo(128 / 255)
    expect(hexToColor('nope')).toBeNull()
    expect(formatColor([1, 0.5019607843, 0])).toBe('1 0.502 0')
    expect(formatColor(hexToColor('#ffffff')!)).toBe('1 1 1')
  })
  it('numbers stay inside the .ent range, integers are rounded, junk is rejected', () => {
    const r = key('radius', 'real', '10', { min: 0.1, max: 1000 })
    expect(clampValue(r, 5000)).toBe(1000)
    expect(clampValue(r, -3)).toBe(0.1)
    expect(clampValue(r, 2.5)).toBe(2.5)
    const s = key('style', 'integer', '0', { min: 0, max: 63 })
    expect(clampValue(s, 7.6)).toBe(8)
    expect(clampValue(s, 99)).toBe(63)
    expect(clampValue(s, NaN)).toBeNaN()
  })
  it('knows what is a default, what is checked, which keys are typed', () => {
    const k = key('brightness', 'real', '1')
    expect(currentValue(ent('light_point'), k)).toBe('1')
    expect(isDefault(ent('light_point'), k)).toBe(true)
    expect(isDefault(ent('light_point', { brightness: '1' }), k)).toBe(true)
    expect(isDefault(ent('light_point', { brightness: '2' }), k)).toBe(false)
    expect(isChecked('1')).toBe(true); expect(isChecked('0')).toBe(false); expect(isChecked('')).toBe(false)
    expect(propertyKeys(classDef('x', [key('a', 'real', '1'), { key: 'b', type: 'string', name: 'b', description: '' }])).map((x) => x.key)).toEqual(['a', 'b'])
    expect(propertyKeys(classDef('x', [key('a', 'real', '1'), key('angles', 'angles', '0 0 0')]), new Set(['angles'])).map((x) => x.key)).toEqual(['a'])
    expect(propertyKeys(undefined)).toEqual([])
  })
})
