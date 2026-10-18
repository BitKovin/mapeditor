// @vitest-environment jsdom
import { afterEach, describe, expect, it } from 'vitest'
import { ViewportPanel } from '../src/ui/viewport'
import { MapDocument, emptyDocument, findEntity } from '../src/model/document'
import type { Entity } from '../src/model/document'
import { makeBox } from '../src/model/geometry'
import { gizmoLength } from '../src/render/gizmo'
import type { Vec3 } from '../src/model/math'
import { makeApp } from './helpers'

// Selecting an entity's origin (clicking its square) uses the usual move gizmo: axis arrows that move the origin alone.
class RO { observe() {} disconnect() {} unobserve() {} }
;(globalThis as any).ResizeObserver ??= RO

const W = 800, H = 600
const door = (): Entity => ({ id: 'e_door', classname: 'func_door', keyvalues: {}, origin: [2, 0, 2], brushes: [makeBox('d1', [2, 0, 2], [3, 1, 3])] })
const lamp = (): Entity => ({ id: 'e_lamp', classname: 'info_player_start', keyvalues: {}, origin: [0, 1, 0], angles: [0, 0, 0] })
const doc = (): MapDocument => ({ ...emptyDocument(), entities: [door(), lamp()] })

const made: ViewportPanel[] = []
afterEach(() => { made.splice(0).forEach((v) => v.dispose()) })

function setup() {
  const app = makeApp()
  app.sync(doc())
  const vp = new ViewportPanel(app.store, { onChange: () => () => {}, get: () => null } as any)
  made.push(vp)
  document.body.append(vp.element)
  const canvas = vp.element.querySelector('canvas')!
  canvas.getBoundingClientRect = () => ({ left: 0, top: 0, right: W, bottom: H, width: W, height: H, x: 0, y: 0, toJSON() {} }) as DOMRect
  Object.defineProperty(canvas, 'clientWidth', { value: W }); Object.defineProperty(canvas, 'clientHeight', { value: H })
  Object.assign(canvas, { setPointerCapture() {}, releasePointerCapture() {}, hasPointerCapture: () => false })
  vp.frameAll()
  const v = vp as any
  const ev = (p: { x: number; y: number }, over: object = {}) => ({ clientX: p.x, clientY: p.y, button: 0, buttons: 1, pointerId: 1, shiftKey: false, ctrlKey: false, metaKey: false, altKey: false, preventDefault() {}, stopPropagation() {}, ...over }) as unknown as PointerEvent
  const key = (k: string) => v.onKey({ key: k, ctrlKey: false, metaKey: false, altKey: false, shiftKey: false, preventDefault() {}, stopPropagation() {} })
  /** Screen position of the move gizmo's arrow for `axis` at its pivot, `extra` metres further along it. */
  const arrow = (pivot: Vec3, axis: 0 | 1 | 2, extra = 0) => {
    const p: Vec3 = [...pivot]
    p[axis] += gizmoLength(v.camera.distanceTo(pivot)) * 0.6 + extra
    return vp.worldToScreen(p)
  }
  const entity = (id: string) => findEntity(app.store.state.doc, id)!
  return { app, vp, v, ev, key, arrow, entity }
}

describe('selecting the origin uses the usual move gizmo', () => {
  it('clicking the origin square selects the origin: no plane drag starts, and the gizmo becomes the move gizmo even in rotate mode', () => {
    const { vp, app, v, ev, entity } = setup()
    app.store.select({ entityId: 'e_door' })
    v.settings.gizmo = 'rotate'
    expect(v.gizmoMode()).toBe('rotate')
    v.onDown(ev(vp.worldToScreen([2, 0, 2])))
    expect(v.originSel).toBe('e_door')
    expect(v.drag).toBeNull()
    expect(v.gizmoMode()).toBe('move')
    v.onUp(ev(vp.worldToScreen([2, 0, 2])))
    expect(entity('e_door').origin).toEqual([2, 0, 2])
  })

  it('dragging the square itself no longer moves anything (it used to slide along the horizontal plane)', () => {
    const { vp, app, v, ev, entity } = setup()
    app.store.select({ entityId: 'e_door' })
    const at = vp.worldToScreen([2, 0, 2])
    v.onDown(ev(at))
    v.onMove(ev({ x: at.x + 60, y: at.y + 40 }))
    v.onUp(ev({ x: at.x + 60, y: at.y + 40 }))
    expect(entity('e_door').origin).toEqual([2, 0, 2])
  })

  it('an arrow of the gizmo then moves the origin alone: a brush entity keeps its brushes', () => {
    const { vp, app, v, ev, arrow, entity } = setup()
    app.store.select({ entityId: 'e_door' })
    const before = JSON.stringify(entity('e_door').brushes)
    v.onDown(ev(vp.worldToScreen([2, 0, 2]))); v.onUp(ev(vp.worldToScreen([2, 0, 2])))
    v.onDown(ev(arrow([2, 0, 2], 0)))
    expect(v.drag).toMatchObject({ kind: 'gizmo', mode: 'move', axis: 0, origin: 'e_door' })
    v.onMove(ev(arrow([2, 0, 2], 0, 2)))
    expect(entity('e_door').origin![0]).toBeCloseTo(4, 5)
    expect(entity('e_door').origin![1]).toBeCloseTo(0, 5)
    expect(entity('e_door').origin![2]).toBeCloseTo(2, 5)
    expect(JSON.stringify(entity('e_door').brushes)).toBe(before)
    v.onUp(ev(arrow([2, 0, 2], 0, 2)))
    expect(v.drag).toBeNull()
    expect(v.originSel).toBe('e_door') // the origin stays selected: more arrows can follow
  })

  it('without selecting the origin the gizmo still moves the whole entity (origin and brushes)', () => {
    const { app, v, ev, arrow, entity } = setup()
    app.store.select({ entityId: 'e_door' })
    v.onDown(ev(arrow([2, 0, 2], 0)))
    expect(v.drag.origin).toBeUndefined()
    v.onMove(ev(arrow([2, 0, 2], 0, 2)))
    v.onUp(ev(arrow([2, 0, 2], 0, 2)))
    expect(entity('e_door').origin![0]).toBeCloseTo(4, 5)
    expect(entity('e_door').brushes![0].vertices.every((p) => p[0] >= 4 - 1e-6)).toBe(true)
  })

  it('works the same for a point entity', () => {
    const { vp, app, v, ev, arrow, entity } = setup()
    app.store.select({ entityId: 'e_lamp' })
    v.onDown(ev(vp.worldToScreen([0, 1, 0]))); v.onUp(ev(vp.worldToScreen([0, 1, 0])))
    expect(v.originSel).toBe('e_lamp')
    v.onDown(ev(arrow([0, 1, 0], 1)))
    v.onMove(ev(arrow([0, 1, 0], 1, 1.5)))
    v.onUp(ev(arrow([0, 1, 0], 1, 1.5)))
    expect(entity('e_lamp').origin![1]).toBeCloseTo(2.5, 5)
  })

  it('Esc leaves the origin editing but keeps the entity selected; a click elsewhere or another selection ends it too', () => {
    const { vp, app, v, ev, key } = setup()
    app.store.select({ entityId: 'e_door' })
    v.onDown(ev(vp.worldToScreen([2, 0, 2]))); v.onUp(ev(vp.worldToScreen([2, 0, 2])))
    expect(v.originSel).toBe('e_door')
    key('Escape')
    expect(v.originSel).toBeNull()
    expect(app.store.state.selection?.entityId).toBe('e_door')

    v.onDown(ev(vp.worldToScreen([2, 0, 2]))); v.onUp(ev(vp.worldToScreen([2, 0, 2])))
    v.onDown(ev({ x: 3, y: 3 })); v.onUp(ev({ x: 3, y: 3 }))                 // empty space
    expect(v.originSel).toBeNull()

    app.store.select({ entityId: 'e_door' })
    v.onDown(ev(vp.worldToScreen([2, 0, 2]))); v.onUp(ev(vp.worldToScreen([2, 0, 2])))
    app.store.select({ entityId: 'e_lamp' })
    expect(v.originSelected()).toBe(false)
    app.store.select({ entityId: 'e_door' })                                  // coming back does not resurrect it
    expect(v.originSelected()).toBe(false)
  })
})
