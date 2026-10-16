// @vitest-environment jsdom
import { describe, expect, it, vi } from 'vitest'
import { msg, parseInbound } from '../src/net/protocol'
import { DEFAULT_PREFS, editorPrefs, viewportDefaults } from '../src/state/editorPrefs'
import { menuBar } from '../src/ui/menuBar'
import { makeApp } from './helpers'

describe('protocol: settings window messages', () => {
  it('fills a settings message from an older Core with defaults', () => {
    expect(parseInbound('{"type":"settings","assetsRoot":"C:/g"}')).toEqual({
      type: 'settings', assetsRoot: 'C:/g', recentMaps: [], game: { exe: '', args: '+map {mapname}', workingDir: '' }, editor: {}, nativeDialogs: false, settingsFile: '', lightingFile: '',
    })
    const m = parseInbound(JSON.stringify({ type: 'settings', assetsRoot: 'C:/g', recentMaps: ['maps/a.map.json', 5], game: { exe: 'C:/g/x.exe', args: '-map {mapname}' }, editor: { gridSize: 1 }, nativeDialogs: true }))
    expect(m).toMatchObject({ recentMaps: ['maps/a.map.json'], game: { exe: 'C:/g/x.exe', args: '-map {mapname}', workingDir: '' }, editor: { gridSize: 1 }, nativeDialogs: true })
  })
  it('builds the requests', () => {
    expect(JSON.parse(msg.nativeDialog(1, { kind: 'folder', title: 'T' }))).toEqual({ type: 'native_dialog', requestId: 1, kind: 'folder', title: 'T' })
    expect(JSON.parse(msg.setSettings(2, { clearRecent: true }))).toEqual({ type: 'set_settings', requestId: 2, clearRecent: true })
    expect(JSON.parse(msg.writeGameProfile(3, { name: 'x' }))).toEqual({ type: 'write_game_profile', requestId: 3, profile: { name: 'x' } })
    expect(JSON.parse(msg.writeLightingConfig(4, { bounces: false }))).toEqual({ type: 'write_lighting_config', requestId: 4, config: { bounces: false } })
    expect(JSON.parse(msg.launchGame(5, 'maps/a.bsp'))).toEqual({ type: 'launch_game', requestId: 5, map: 'maps/a.bsp' })
  })
})

describe('editor preferences', () => {
  it('validates what settings.json holds, falling back to the defaults', () => {
    expect(editorPrefs(undefined)).toEqual(DEFAULT_PREFS)
    const p = editorPrefs({ gridSize: 1, rotSnap: 45, scaleSnap: 7, flySpeed: 'fast', uvLock: false })
    expect(p).toMatchObject({ gridSize: 1, rotSnap: 45, scaleSnap: DEFAULT_PREFS.scaleSnap, flySpeed: DEFAULT_PREFS.flySpeed, uvLock: false })
    expect(viewportDefaults(p)).toEqual({ gridSize: 1, snapEnabled: true, rotSnap: 45, scaleSnap: 0.1, flySpeed: 8, showGrid: true })
  })
  it('sets UV lock and auto detail from the preferences once, when Core first sends them', () => {
    const { store, sock } = makeApp(); sock().open()
    sock().receive({ type: 'settings', assetsRoot: 'C:/g', editor: { uvLock: false, autoDetail: false } })
    expect(store.state.uvLock).toBe(false)
    expect(store.state.autoDetail).toBe(false)
    store.setUvLock(true)
    sock().receive({ type: 'settings', assetsRoot: 'C:/g', editor: { uvLock: false }, recentMaps: ['maps/b.map.json'] })
    expect(store.state.uvLock).toBe(true) // the person's own toggle wins after start-up
    expect(store.state.coreSettings?.recentMaps).toEqual(['maps/b.map.json'])
  })
  it('remembers the last export result (Play map waits for it)', () => {
    const { store, sock } = makeApp(); sock().open()
    sock().receive({ type: 'export_result', ok: true, bsp: 'maps/a.bsp' })
    expect(store.state.lastExport).toMatchObject({ ok: true, bsp: 'maps/a.bsp' })
  })
})

describe('menu bar', () => {
  it('opens a menu, runs an item, opens a submenu on hover and closes on Escape', () => {
    const run = vi.fn(), recent = vi.fn()
    const bar = menuBar([{ label: 'File', items: () => [{ label: 'Open…', shortcut: 'Ctrl+O', action: run }, { label: 'Open Recent', submenu: () => [{ label: 'a', action: recent }] }, '-', { label: 'Off', disabled: true, action: run }] }, { label: 'Edit', items: () => [{ label: 'Undo', action: run }] }])
    document.body.append(bar)
    const title = (t: string) => [...bar.querySelectorAll<HTMLElement>('.menu-title')].find((b) => b.textContent === t)!
    const item = (t: string) => [...document.querySelectorAll<HTMLElement>('.menu-item')].find((b) => b.textContent?.includes(t))!
    title('File').dispatchEvent(new MouseEvent('mousedown', { bubbles: true }))
    expect(document.querySelectorAll('.menu-popup')).toHaveLength(1)
    expect(item('Open…').textContent).toContain('Ctrl+O')
    expect(item('Off').hasAttribute('disabled')).toBe(true)
    item('Open Recent').dispatchEvent(new MouseEvent('mouseenter'))
    expect(document.querySelectorAll('.menu-popup')).toHaveLength(2)
    item('a').click()
    expect(recent).toHaveBeenCalledOnce()
    expect(document.querySelectorAll('.menu-popup')).toHaveLength(0)
    // hovering another title while one is open switches menus
    title('File').dispatchEvent(new MouseEvent('mousedown', { bubbles: true }))
    title('Edit').dispatchEvent(new MouseEvent('mouseenter'))
    expect(item('Undo')).toBeTruthy()
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))
    expect(document.querySelectorAll('.menu-popup')).toHaveLength(0)
    expect(run).not.toHaveBeenCalled()
    bar.remove()
  })
})
