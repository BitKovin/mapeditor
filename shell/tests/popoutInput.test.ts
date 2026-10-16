// @vitest-environment jsdom
import { afterEach, describe, expect, it, vi } from 'vitest'
import { ViewportPanel } from '../src/ui/viewport'
import { showMenu, closeMenu } from '../src/ui/contextMenu'
import { makeApp } from './helpers'

// A popout window is a second Document; Dockview moves the viewport's element into it. Page-level listeners made
// for the main document never see events there, so WASD (and Shift/Esc) used to be dead in popouts.
class RO { observe() {} disconnect() {} unobserve() {} }
;(globalThis as any).ResizeObserver ??= RO

const keyEvent = (type: string, code: string) => new KeyboardEvent(type, { code, key: code === 'Escape' ? 'Escape' : code.replace('Key', '').toLowerCase(), bubbles: true, cancelable: true })

describe('a viewport in a popout window receives the page-level keys', () => {
  const made: ViewportPanel[] = []
  afterEach(() => { made.splice(0).forEach((v) => v.dispose()); closeMenu() })
  const setup = () => {
    const { store } = makeApp()
    const vp = new ViewportPanel(store, { onChange: () => () => {}, get: () => null } as any)
    made.push(vp)
    document.body.append(vp.element)
    const popout = document.implementation.createHTMLDocument('popout')
    return { vp, popout, flying: () => [...((vp as any).flyKeys as Set<string>)] }
  }

  it('fly keys held with RMB work in the main window (baseline)', () => {
    const { vp, flying } = setup()
    vp.element.querySelector('canvas')!.dispatchEvent(new Event('pointerenter'))
    ;(vp as any).drag = { kind: 'look' }
    document.dispatchEvent(keyEvent('keydown', 'KeyW'))
    expect(flying()).toEqual(['KeyW'])
  })

  it('...and after the element moved into another document', () => {
    const { vp, popout, flying } = setup()
    vp.element.querySelector('canvas')!.dispatchEvent(new Event('pointerenter'))
    popout.body.append(vp.element)                                    // what the popout does
    expect(vp.element.ownerDocument).toBe(popout)
    ;(vp as any).drag = { kind: 'look' }
    popout.dispatchEvent(keyEvent('keydown', 'KeyW'))                // not bound yet: the pointer has not reached the viewport there
    vp.element.querySelector('canvas')!.dispatchEvent(new Event('pointerenter')) // it does as soon as the pointer comes in
    popout.dispatchEvent(keyEvent('keydown', 'KeyD'))
    expect(flying()).toEqual(['KeyD'])
    popout.dispatchEvent(keyEvent('keyup', 'KeyD'))
    expect(flying()).toEqual([])
  })

  it('the main window stops being listened to once the viewport left it', () => {
    const { vp, popout, flying } = setup()
    const c = vp.element.querySelector('canvas')!
    c.dispatchEvent(new Event('pointerenter'))
    popout.body.append(vp.element)
    c.dispatchEvent(new Event('pointerenter'))
    ;(vp as any).drag = { kind: 'look' }
    document.dispatchEvent(keyEvent('keydown', 'KeyW'))
    expect(flying()).toEqual([])
  })

  it('moving back to the main window re-binds, and a held key of the old window is released', () => {
    const { vp, popout, flying } = setup()
    const c = vp.element.querySelector('canvas')!
    popout.body.append(vp.element)
    c.dispatchEvent(new Event('pointerenter'))
    ;(vp as any).drag = { kind: 'look' }
    popout.dispatchEvent(keyEvent('keydown', 'KeyS'))
    expect(flying()).toEqual(['KeyS'])
    document.body.append(vp.element)
    c.dispatchEvent(new Event('pointerenter'))
    expect(flying()).toEqual([])                                      // its keyup would have gone to the other window
    document.dispatchEvent(keyEvent('keydown', 'KeyA'))
    expect(flying()).toEqual(['KeyA'])
  })

  it('Escape cancels an armed pipette from a popout too', () => {
    const { vp, popout } = setup()
    const store = (vp as any).store
    const cb = vi.fn()
    store.setPipette(cb)
    popout.body.append(vp.element)
    vp.element.querySelector('canvas')!.dispatchEvent(new Event('pointerenter'))
    popout.dispatchEvent(keyEvent('keydown', 'Escape'))
    expect(cb).toHaveBeenCalledWith(null)
  })
})

describe('context menu opens in the window it was asked for', () => {
  it('appends to the given document, not the main one', () => {
    const popout = document.implementation.createHTMLDocument('popout')
    showMenu(10, 10, [{ label: 'Delete', run: () => {} }], popout)
    expect(popout.querySelector('.ctx-menu')).not.toBeNull()
    expect(document.querySelector('.ctx-menu')).toBeNull()
    closeMenu()
    expect(popout.querySelector('.ctx-menu')).toBeNull()
  })
})
