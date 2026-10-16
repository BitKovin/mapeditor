// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { ContentBrowserPanel, APPLY_DELAY_MS } from '../src/ui/contentBrowser'
import { TextureCache } from '../src/state/textures'
import { makeApp } from './helpers'

class IO { observe() {} disconnect() {} unobserve() {} }
;(globalThis as any).IntersectionObserver ??= IO

async function panel(open: (p: string) => void) {
  const app = makeApp()
  const textures = new TextureCache(async () => { throw new Error('none') })
  const cb = new ContentBrowserPanel(app.store, textures, async () => ['dev/wall', 'dev/floor'], open)
  document.body.append(cb.element)
  cb.init()
  await cb.reload()
  return { app, cb }
}
const tile = (cb: ContentBrowserPanel, name: string) => cb.element.querySelector(`.tile[data-path="${name}"]`) as HTMLElement

beforeEach(() => vi.useFakeTimers())
afterEach(() => { vi.useRealTimers(); document.body.replaceChildren() })

describe('content browser: apply, double-click properties, right-click menu', () => {
  it('a single click chooses the texture at once and applies it after the delay', async () => {
    const open = vi.fn()
    const { app, cb } = await panel(open)
    tile(cb, 'dev/wall').click()
    expect(app.store.state.activeTexture).toBe('dev/wall')
    vi.advanceTimersByTime(APPLY_DELAY_MS + 5)
    expect(open).not.toHaveBeenCalled()
  })
  it('a double-click opens the properties and never applies the texture', async () => {
    const open = vi.fn()
    const { app, cb } = await panel(open)
    const applied = vi.spyOn(app.store, 'edit')
    const t = tile(cb, 'dev/floor')
    t.click(); t.click(); t.dispatchEvent(new MouseEvent('dblclick', { bubbles: true }))
    vi.advanceTimersByTime(APPLY_DELAY_MS * 3)
    expect(open).toHaveBeenCalledWith('dev/floor')
    expect(applied).not.toHaveBeenCalled()
  })
  it('right-click shows a menu with Properties', async () => {
    const open = vi.fn()
    const { cb } = await panel(open)
    tile(cb, 'dev/wall').dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true, clientX: 10, clientY: 10 }))
    const items = [...document.querySelectorAll('.ctx-item')] as HTMLElement[]
    const props = items.find((i) => i.textContent?.startsWith('Properties'))!
    expect(props).toBeTruthy()
    props.click()
    expect(open).toHaveBeenCalledWith('dev/wall')
  })
})
