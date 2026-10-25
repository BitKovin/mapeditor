// @vitest-environment jsdom
import { afterEach, describe, expect, it } from 'vitest'
import { ViewportPanel } from '../src/ui/viewport'
import { MapDocument, emptyDocument, findEntity } from '../src/model/document'
import type { Entity } from '../src/model/document'
import { makeBox } from '../src/model/geometry'
import { gizmoLength } from '../src/render/gizmo'
import type { Vec3 } from '../src/model/math'
import { wholeEntitiesIfAll } from '../src/state/store'
import { makeApp } from './helpers'

class RO { observe() {} disconnect() {} unobserve() {} }
;(globalThis as any).ResizeObserver ??= RO

const W = 800, H = 600
const door = (): Entity => ({ id: 'e_door', classname: 'func_door', keyvalues: {}, origin: [2, 0, 2], brushes: [makeBox('d1', [2, 0, 2], [3, 1, 3]), makeBox('d2', [4, 0, 2], [5, 1, 3])] })
const world = (): Entity => ({ id: 'e_world', classname: 'worldspawn', keyvalues: {}, brushes: [makeBox('w1', [-3, 0, -3], [-2, 1, -2]), makeBox('w2', [-6, 0, -3], [-5, 1, -2])] })
const doc = (): MapDocument => ({ ...emptyDocument(), entities: [world(), door()] })

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
  const arrow = (pivot: Vec3, axis: 0 | 1 | 2, extra = 0) => {
    const p: Vec3 = [...pivot]
    p[axis] += gizmoLength(v.camera.distanceTo(pivot)) * 0.6 + extra
    return vp.worldToScreen(p)
  }
  return { app, vp, v, ev, arrow, store: app.store }
}

describe('selecting all brushes of a brush entity selects the entity', () => {
  it('selectMany of every brush collapses into the entity (what a double-click gives)', () => {
    const { store } = setup()
    store.selectMany([{ entityId: 'e_door', brushId: 'd1' }, { entityId: 'e_door', brushId: 'd2' }])
    expect(store.state.multi).toEqual([{ entityId: 'e_door' }])
    expect(store.state.selection).toEqual({ entityId: 'e_door' })
  })

  it('only some brushes stay brushes', () => {
    const { store } = setup()
    store.selectMany([{ entityId: 'e_door', brushId: 'd1' }])
    expect(store.state.selection).toEqual({ entityId: 'e_door', brushId: 'd1' })
  })

  it('ctrl-adding the last brush completes the entity', () => {
    const { store } = setup()
    store.select({ entityId: 'e_door', brushId: 'd1' })
    store.toggleSelect({ entityId: 'e_door', brushId: 'd2' })
    expect(store.state.multi).toEqual([{ entityId: 'e_door' }])
    expect(store.state.selection).toEqual({ entityId: 'e_door' })
  })

  it('worldspawn brushes stay separate, and other selections keep their order', () => {
    const { store } = setup()
    const all = [{ entityId: 'e_world', brushId: 'w1' }, { entityId: 'e_door', brushId: 'd1' }, { entityId: 'e_world', brushId: 'w2' }, { entityId: 'e_door', brushId: 'd2' }]
    expect(wholeEntitiesIfAll(store.state.doc, all)).toEqual([{ entityId: 'e_world', brushId: 'w1' }, { entityId: 'e_world', brushId: 'w2' }, { entityId: 'e_door' }])
    store.selectMany([{ entityId: 'e_world', brushId: 'w1' }, { entityId: 'e_world', brushId: 'w2' }])
    expect(store.state.multi).toHaveLength(2)
  })

  it('a double-click on a brush of the entity selects the entity', () => {
    const { vp, v, store } = setup()
    const at = vp.worldToScreen([2.5, 0.5, 2.5])
    v.onDblClick({ clientX: at.x, clientY: at.y, shiftKey: false, ctrlKey: false, metaKey: false, altKey: false } as unknown as MouseEvent)
    expect(store.state.selection).toEqual({ entityId: 'e_door' })
  })
})

describe('one brush of a brush entity still shows the origin to edit', () => {
  it('the origin handle shows for a single selected brush, and clicking it edits the origin with the move gizmo at the origin', () => {
    const { vp, v, ev, arrow, store } = setup()
    store.select({ entityId: 'e_door', brushId: 'd2' })
    expect(v.entityHandles()?.origin).toEqual([2, 0, 2])
    expect(v.entityHandles()?.angle).toBeNull()
    v.onDown(ev(vp.worldToScreen([2, 0, 2]))); v.onUp(ev(vp.worldToScreen([2, 0, 2])))
    expect(v.originSel).toBe('e_door')
    expect(v.pivotNow()).toEqual([2, 0, 2])
    v.onDown(ev(arrow([2, 0, 2], 0)))
    expect(v.drag).toMatchObject({ kind: 'gizmo', mode: 'move', axis: 0, origin: 'e_door' })
    v.onMove(ev(arrow([2, 0, 2], 0, 2)))
    v.onUp(ev(arrow([2, 0, 2], 0, 2)))
    const e = findEntity(store.state.doc, 'e_door')!
    expect(e.origin![0]).toBeCloseTo(4, 5)
    expect(e.brushes!.map((b) => b.id)).toEqual(['d1', 'd2'])
    expect(e.brushes![1].vertices.every((p) => p[0] >= 4 - 1e-6)).toBe(true) // the brush stayed where it was
    expect(store.state.selection).toEqual({ entityId: 'e_door', brushId: 'd2' })
  })

  it('no handle for a face selection, for worldspawn brushes, or for several selected brushes', () => {
    const { v, store } = setup()
    store.select({ entityId: 'e_door', brushId: 'd1', faceId: 0 })
    expect(v.entityHandles()).toBeNull()
    store.select({ entityId: 'e_world', brushId: 'w1' })
    expect(v.entityHandles()).toBeNull()
    store.selectMany([{ entityId: 'e_world', brushId: 'w1' }, { entityId: 'e_world', brushId: 'w2' }])
    expect(v.entityHandles()).toBeNull()
  })
})
