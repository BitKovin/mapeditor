// @vitest-environment jsdom
import { afterEach, describe, expect, it, vi } from 'vitest'
import { GROUP_TEMPLATES } from '../src/model/groupTools'
import { groupEnv } from '../src/model/texgroup'
import { defaultProps, texEnv } from '../src/model/texprops'
import { TextureCache } from '../src/state/textures'
import { GroupEditorEnv, openTexGroupEditor } from '../src/ui/texGroupEditor'

afterEach(() => { texEnv.setProvider(null); groupEnv.setProvider(null); document.body.replaceChildren(); vi.useRealTimers() })
const tick = () => new Promise((r) => setTimeout(r, 0))

function env(over: Partial<GroupEditorEnv> = {}) {
  texEnv.setProvider((path) => ({ path, width: 64, height: 64, ppm: 64, props: defaultProps() }))
  const e: GroupEditorEnv = {
    textures: new TextureCache(async () => { throw new Error('none') }), listTextures: async () => ['dev/a'],
    read: async () => GROUP_TEMPLATES[0].group, save: vi.fn(async () => {}), apply: vi.fn(() => 3), saved: vi.fn(), ...over,
  }
  return e
}
const q = <T extends HTMLElement>(sel: string) => document.querySelector(sel) as T
const btn = (t: string) => [...document.querySelectorAll('button')].find((b) => b.textContent === t) as HTMLButtonElement

describe('texture group editor window', () => {
  it('a new group starts from the facade template, with its slots listed and no problems', async () => {
    await openTexGroupEditor(env(), null); await tick() // the slot textures load (a few at a time)
    expect(q<HTMLTextAreaElement>('textarea').value).toContain('"floorHeight"')
    expect(document.querySelectorAll('.gp-slot')).toHaveLength(6)
    expect(document.querySelector('.gp-err')).toBeNull(); expect(q('.gp-problems').textContent).not.toMatch(/unknown/)
    expect(q('.gp-problems').textContent).toMatch(/was not found in the assets folder/) // this test's loader finds no texture: said, not hidden
    expect(q('.gp-params').textContent).toContain('Floor height (m)')
  })

  it('typing is parsed live: JSON errors and unknown names are listed, and a fix clears them', async () => {
    vi.useFakeTimers()
    await openTexGroupEditor(env(), null)
    const ta = q<HTMLTextAreaElement>('textarea')
    ta.value = ta.value.replace('"tex": "roof"', '"tex": "nope"'); ta.dispatchEvent(new Event('input')); vi.advanceTimersByTime(300)
    expect(q('.gp-problems').textContent).toMatch(/unknown slot 'nope'/)
    ta.value = '{ broken'; ta.dispatchEvent(new Event('input')); vi.advanceTimersByTime(300)
    expect(q('.gp-problems .gp-err').textContent).toMatch(/JSON/)
    expect(document.querySelectorAll('.gp-slot').length).toBeGreaterThan(0) // the last good group stays on screen
  })

  it('Save writes the parsed file under a valid name; bad names and broken text are refused', async () => {
    const e = env(); await openTexGroupEditor(e, null); await tick()
    const name = q<HTMLInputElement>('input.search'), status = () => q('.gp-bar .dim').textContent
    btn('Save').click(); await tick(); expect(e.save).not.toHaveBeenCalled(); expect(status()).toMatch(/Name/)
    name.value = '../evil'; btn('Save').click(); await tick(); expect(e.save).not.toHaveBeenCalled()
    name.value = 'bld/facade'; btn('Save').click(); await tick()
    expect(e.save).toHaveBeenCalledWith('bld/facade', expect.objectContaining({ format: 1, name: 'Building facade' })); expect(e.saved).toHaveBeenCalled()
    const ta = q<HTMLTextAreaElement>('textarea'); ta.value = '{ x'; btn('Save').click(); await tick()
    expect(e.save).toHaveBeenCalledTimes(1); expect(status()).toMatch(/Fix the errors/)
  })

  it('Apply to selection saves first, then applies and reports the face count; a failed save does not apply', async () => {
    const e = env(); await openTexGroupEditor(e, null); await tick()
    q<HTMLInputElement>('input.search').value = 'g1'; btn('Apply to selection').click(); await tick()
    expect(e.apply).toHaveBeenCalledWith('g1'); expect(q('.gp-bar .dim').textContent).toMatch(/applied to 3 face/)
    const bad = env({ save: vi.fn(async () => { throw new Error('disk full') }) }); document.body.replaceChildren()
    await openTexGroupEditor(bad, null); await tick(); q<HTMLInputElement>('input.search').value = 'g2'; btn('Apply to selection').click(); await tick()
    expect(bad.apply).not.toHaveBeenCalled(); expect(q('.gp-bar .dim').textContent).toMatch(/disk full/)
  })

  it('an existing group is read from Core and its name cannot be changed', async () => {
    const e = env({ read: vi.fn(async () => ({ format: 1, name: 'Mine', slots: { a: 'dev/a' }, root: 'a' })) })
    await openTexGroupEditor(e, 'kits/mine')
    expect(e.read).toHaveBeenCalledWith('kits/mine'); expect(q<HTMLTextAreaElement>('textarea').value).toContain('"Mine"')
    expect(q<HTMLInputElement>('input.search').hasAttribute('readonly')).toBe(true)
  })

  it('a template can replace the text', async () => {
    await openTexGroupEditor(env(), null)
    const sel = q<HTMLSelectElement>('.gp-bar select'); sel.value = 'floor'; sel.dispatchEvent(new Event('change'))
    expect(q<HTMLTextAreaElement>('textarea').value).toContain('Bordered floor'); expect(q('.gp-params').textContent).toContain('Border (m)')
  })
})
