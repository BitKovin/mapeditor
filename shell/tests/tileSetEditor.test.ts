// @vitest-environment jsdom
import { afterEach, describe, expect, it, vi } from 'vitest'
import { groupEnv } from '../src/model/texgroup'
import { defaultProps, texEnv } from '../src/model/texprops'
import { TextureCache } from '../src/state/textures'
import { TileSetEnv, openTileSetEditor } from '../src/ui/tileSetEditor'

afterEach(() => { texEnv.setProvider(null); groupEnv.setProvider(null); document.body.replaceChildren(); vi.unstubAllGlobals() })
const tick = () => new Promise((r) => setTimeout(r, 0))

const TEX = ['bb/wall', 'bb/wall_window', 'bb/wall_door', 'bb/bottom_wall', 'bb/roof_cap']
function env(over: Partial<TileSetEnv> = {}): TileSetEnv {
  const textures = new TextureCache(async () => { throw new Error('none') }) // creating the cache registers its provider: set ours after it
  texEnv.setProvider((path) => ({ path, width: 204, height: 204, ppm: 64, props: defaultProps() }))
  return { textures, listTextures: async () => TEX, read: async () => ({}), save: vi.fn(async () => {}), apply: vi.fn(() => 2), saved: vi.fn(), ...over }
}
const q = <T extends HTMLElement>(sel: string) => document.querySelector(sel) as T
const qa = (sel: string) => [...document.querySelectorAll(sel)] as HTMLElement[]
const btn = (t: string, root: ParentNode = document) => [...root.querySelectorAll('button')].find((b) => b.textContent === t) as HTMLButtonElement
const lanes = () => qa('.ts-lane')
const cards = (i: number) => lanes()[i].querySelectorAll('.ts-card')
/** the picker dialog that is open now: click the tile of a texture */
async function pickInDialog(name: string): Promise<void> { await tick(); const t = qa('.tile').find((x) => x.title === name)!; t.click(); await tick() }

describe('wall set editor', () => {
  it('a new wall set shows the three rows in order Top, Middle, Bottom with their hints', async () => {
    await openTileSetEditor(env(), null, 'wall')
    expect(lanes().map((l) => l.querySelector('b')!.textContent)).toEqual(['Top row', 'Middle rows', 'Bottom row'])
    expect(q('.ts-board').textContent).toMatch(/always above everything else/); expect(q('.ts-board').textContent).toMatch(/Repeats upward/)
    expect(q('.gp-problems').textContent).toMatch(/no usable piece/)
  })

  it('+ Add in a row picks a texture, creates a piece and selects it', async () => {
    await openTileSetEditor(env(), null, 'wall')
    btn('+ Add', lanes()[1]).click(); await pickInDialog('bb/wall')
    expect(cards(1)).toHaveLength(1); expect(q('.ts-inspector').textContent).toMatch(/Selected piece/)
    expect(q('.gp-problems').textContent).not.toMatch(/no usable piece/)
  })

  it('Add textures sorts several at once by name (bottom, roof, door, window) and says how many', async () => {
    await openTileSetEditor(env(), null, 'wall')
    btn('Add textures...').click(); await tick()
    for (const n of TEX) qa('.tile').find((x) => x.title === n)!.click()
    btn('Add 5').click(); await tick()
    expect(cards(0)).toHaveLength(1) // roof_cap
    expect(cards(2).length).toBeGreaterThanOrEqual(2) // bottom_wall + door (+ plain filler)
    expect(cards(1).length).toBeGreaterThanOrEqual(2) // wall + wall_window
    expect(q('.gp-bar .dim').textContent).toMatch(/pieces added/)
  })

  it('the inspector: how often, where, wide / tall blocks, row', async () => {
    await openTileSetEditor(env(), null, 'wall')
    btn('+ Add', lanes()[1]).click(); await pickInDialog('bb/wall')
    const ins = () => q('.ts-inspector')
    btn('Rare', ins()).click(); expect(btn('Rare', ins()).classList.contains('active')).toBe(true); expect(cards(1)[0].textContent).toMatch(/Rare/)
    const where = ins().querySelector('select') as HTMLSelectElement; where.value = 'once'; where.dispatchEvent(new Event('change')); expect(cards(1)[0].textContent).toMatch(/once/)
    // wide: a block of two textures that always stay together
    const wide = ins().querySelectorAll('.ts-step')[0]; btn('+', wide as HTMLElement).click()
    expect(cards(1)[0].querySelectorAll('canvas')).toHaveLength(2)
    // tall changes the whole row
    const tall = ins().querySelectorAll('.ts-step')[1]; btn('+', tall as HTMLElement).click()
    expect(cards(1)[0].querySelectorAll('canvas')).toHaveLength(4); expect(lanes()[1].textContent).toMatch(/2 tile rows/)
    // move to the bottom row with the Row selector
    const row = ins().querySelectorAll('select')[1] as HTMLSelectElement; row.value = 'bottom'; row.dispatchEvent(new Event('change'))
    expect(cards(1)).toHaveLength(0); expect(cards(2)).toHaveLength(1)
  })

  it('a texture of a block can be changed by clicking its square', async () => {
    const e = env(); await openTileSetEditor(e, null, 'wall')
    btn('+ Add', lanes()[1]).click(); await pickInDialog('bb/wall')
    btn('+', q('.ts-inspector').querySelectorAll('.ts-step')[0] as HTMLElement).click()
    ;(q('.ts-inspector').querySelectorAll('canvas')[1] as HTMLElement).click(); await pickInDialog('bb/wall_door')
    btn('Save').click(); q<HTMLInputElement>('input.search').value = 'sets/x'; btn('Save').click(); await tick()
    expect(e.save).toHaveBeenCalledWith('sets/x', expect.objectContaining({ kit: expect.objectContaining({ middle: { pieces: [{ cells: [['bb/wall', 'bb/wall_door']], weight: 3 }] } }) }))
  })

  it('dragging a card onto another row moves it', async () => {
    await openTileSetEditor(env(), null, 'wall')
    btn('+ Add', lanes()[1]).click(); await pickInDialog('bb/wall')
    const store: Record<string, string> = {}
    const dt = { setData: (k: string, v: string) => { store[k] = v }, getData: (k: string) => store[k] ?? '' }
    const card = cards(1)[0] as HTMLElement
    const start = new Event('dragstart', { bubbles: true }); Object.assign(start, { dataTransfer: dt }); card.dispatchEvent(start)
    const drop = new Event('drop', { bubbles: true, cancelable: true }); Object.assign(drop, { dataTransfer: dt }); lanes()[0].dispatchEvent(drop)
    expect(cards(1)).toHaveLength(0); expect(cards(0)).toHaveLength(1)
  })

  it('right-click: duplicate and delete', async () => {
    await openTileSetEditor(env(), null, 'wall')
    btn('+ Add', lanes()[1]).click(); await pickInDialog('bb/wall')
    cards(1)[0].dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, clientX: 5, clientY: 5 }))
    const item = qa('.ctx-item, .menu-item, [role=menuitem], .ctxmenu div').find((x) => x.textContent === 'Duplicate')
    expect(item).toBeTruthy(); item!.click(); expect(cards(1)).toHaveLength(2)
  })

  it('Save validates the name, writes the kit, then Save and apply applies; broken names are refused', async () => {
    const e = env(); await openTileSetEditor(e, null, 'wall')
    btn('+ Add', lanes()[1]).click(); await pickInDialog('bb/wall')
    const name = q<HTMLInputElement>('input.search'), status = () => q('.gp-bar .dim').textContent
    btn('Save').click(); await tick(); expect(e.save).not.toHaveBeenCalled(); expect(status()).toMatch(/Name/)
    name.value = '../bad'; btn('Save').click(); await tick(); expect(e.save).not.toHaveBeenCalled()
    name.value = 'sets/brick'; btn('Save and apply to selection').click(); await tick()
    expect(e.save).toHaveBeenCalledWith('sets/brick', expect.objectContaining({ format: 1, kit: expect.objectContaining({ kind: 'wall' }) }))
    expect(e.apply).toHaveBeenCalledWith('sets/brick'); expect(status()).toMatch(/applied to 2 face/); expect(e.saved).toHaveBeenCalled()
  })

  it('an empty set cannot be saved as a usable set? it can (the editor starts there), but a failing save says why', async () => {
    const e = env({ save: vi.fn(async () => { throw new Error('disk full') }) }); await openTileSetEditor(e, null, 'wall')
    q<HTMLInputElement>('input.search').value = 'a'; btn('Save').click(); await tick(); expect(q('.gp-bar .dim').textContent).toMatch(/disk full/)
  })

  it('opens an existing set from Core, keeps its name read only; a rule group is refused with a pointer to Advanced', async () => {
    const e = env({ read: vi.fn(async () => ({ format: 1, name: 'Mine', kit: { kind: 'wall', middle: { pieces: [{ tex: 'bb/wall', weight: 3 }, { tex: 'bb/wall_window', place: 'middle' }] }, top: { pieces: ['bb/roof_cap'] } } })) })
    await openTileSetEditor(e, 'sets/mine')
    expect(e.read).toHaveBeenCalledWith('sets/mine'); expect(q<HTMLInputElement>('input.search').hasAttribute('readonly')).toBe(true)
    expect(cards(1)).toHaveLength(2); expect(cards(0)).toHaveLength(1); expect(cards(1)[0].textContent).toMatch(/Often/)
    document.body.replaceChildren()
    const alertFn = vi.fn(); vi.stubGlobal('alert', alertFn)
    await openTileSetEditor(env({ read: async () => ({ format: 1, slots: {}, root: 'a' }) }), 'adv/x')
    expect(alertFn).toHaveBeenCalledWith(expect.stringContaining('Advanced')); expect(document.querySelector('.ts-board')).toBeNull()
  })

  it('tile size: automatic from the first texture, or typed in metres', async () => {
    const e = env(); await openTileSetEditor(e, null, 'wall')
    btn('+ Add', lanes()[1]).click(); await pickInDialog('bb/wall')
    const auto = q('.ts-options').querySelector('input[type=checkbox]') as HTMLInputElement
    expect(auto.checked).toBe(true)
    auto.checked = false; auto.dispatchEvent(new Event('change'))
    const w = q('.ts-options').querySelectorAll('input[type=number]')[0] as HTMLInputElement
    expect(Number(w.value)).toBeCloseTo(3.19, 1) // 204 px at 64 px per meter
    w.value = '2'; w.dispatchEvent(new Event('change'))
    q<HTMLInputElement>('input.search').value = 's'; btn('Save').click(); await tick()
    expect((e.save as ReturnType<typeof vi.fn>).mock.calls[0][1].kit.tile[0]).toBe(2)
  })
})

describe('frame (9-slice) editor', () => {
  it('nine slots with labels; choosing and clearing; the saved file has the nine keys', async () => {
    const e = env(); await openTileSetEditor(e, null, 'frame')
    expect(qa('.ts-slot')).toHaveLength(9); expect(q('.ts-frame').textContent).toMatch(/Top left corner/); expect(q('.ts-frame').textContent).toMatch(/Middle \(repeats\)/)
    for (const s of qa('.ts-slot')) { s.click(); await pickInDialog('bb/wall') }
    expect(q('.gp-problems').textContent).not.toMatch(/has no texture/)
    qa('.ts-slot')[4].dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, clientX: 1, clientY: 1 }))
    qa('.ctx-item, .menu-item, [role=menuitem], .ctxmenu div').find((x) => x.textContent === 'Clear')!.click()
    expect(q('.gp-problems').textContent).toMatch(/slot 'c' has no texture/)
    q<HTMLInputElement>('input.search').value = 'frames/f'; btn('Save').click(); await tick()
    const saved = (e.save as ReturnType<typeof vi.fn>).mock.calls[0][1].kit
    expect(Object.keys(saved).sort()).toEqual(['b', 'bl', 'br', 'c', 'kind', 'l', 'r', 't', 'tl', 'tr'].sort()); expect(saved.c).toBeNull(); expect(saved.tl).toBe('bb/wall')
  })
})

describe('Lines up with (Match names) in the editor', () => {
  it('sets a name on a piece, shows it on the card and in the saved file; a bad name clears it; names are offered for other pieces', async () => {
    const e = env(); await openTileSetEditor(e, null, 'wall')
    btn('+ Add', lanes()[1]).click(); await pickInDialog('bb/wall_window')
    const field = () => q('.ts-inspector').querySelector('input[type=text]') as HTMLInputElement
    field().value = 'window 2'; field().dispatchEvent(new Event('change'))
    expect(cards(1)[0].textContent).toMatch(/= window 2/)
    btn('+ Add', lanes()[2]).click(); await pickInDialog('bb/bottom_wall')
    expect(q('#ts-matches').querySelectorAll('option')).toHaveLength(1) // the name from the middle row is offered here
    field().value = '<bad>'; field().dispatchEvent(new Event('change')); expect(cards(2)[0].textContent).not.toMatch(/=/)
    q<HTMLInputElement>('input.search').value = 'sets/m'; btn('Save').click(); await tick()
    const kit = (e.save as ReturnType<typeof vi.fn>).mock.calls[0][1].kit
    expect(kit.middle.pieces[0].match).toBe('window 2'); expect(kit.bottom.pieces[0].match).toBeUndefined()
  })
})

describe('Max stretch and the saved file', () => {
  it('the field is stored as a percentage (default not written); the file has the kit and a root placeholder that older Cores accept', async () => {
    const e = env(); await openTileSetEditor(e, null, 'wall')
    btn('+ Add', lanes()[1]).click(); await pickInDialog('bb/wall')
    const field = () => [...q('.ts-options').querySelectorAll('input[type=number]')].find((i) => (i as HTMLInputElement).max === '500') as HTMLInputElement
    expect(field().value).toBe('25')
    q<HTMLInputElement>('input.search').value = 'sets/a'; btn('Save').click(); await tick()
    const first = (e.save as ReturnType<typeof vi.fn>).mock.calls[0][1]
    expect(first.kit.maxStretch).toBeUndefined(); expect(first).toHaveProperty('root'); expect(first).toHaveProperty('kit')
    field().value = '10'; field().dispatchEvent(new Event('change')); btn('Save').click(); await tick()
    expect((e.save as ReturnType<typeof vi.fn>).mock.calls[1][1].kit.maxStretch).toBe(10)
    field().value = '0'; field().dispatchEvent(new Event('change')); btn('Save').click(); await tick()
    expect((e.save as ReturnType<typeof vi.fn>).mock.calls[2][1].kit.maxStretch).toBe(0)
  })
  it('a file with kit and the root placeholder is read as a simple set', async () => {
    const { parseGroup } = await import('../src/model/texgroup')
    const g = parseGroup({ format: 1, kit: { kind: 'wall', middle: { pieces: ['bb/wall'] } }, root: {} })
    expect(g.errors).toEqual([]); expect(g.group!.kit?.kind).toBe('wall'); expect(g.warnings).toEqual([])
  })
})
