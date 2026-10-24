// Settings window (File > Settings…, Ctrl+,): a preferences window with a category list, like a desktop application's.
//   General      the game folder (= Core's assets folder), chosen with the system's folder dialog; what was found in it
//   Game profile game_profile.json in that folder: folders, .ent file, extensions, texture scale, light classes
//   Run game     the executable File > Play map starts, its arguments and working folder (per game folder, Core's settings.json)
//   Lighting     lighting.json next to Core (bake quality and live lighting)
//   Editor       defaults for new viewports and for the editor at start-up, recent maps
// Cancel / Apply / OK act on every page with changes. Changing the game folder is immediate (it reloads everything).
import type { CoreConnection } from '../net/connection'
import { msg } from '../net/protocol'
import type { FolderEntry, GameProfile } from '../net/protocol'
import { GRID_SIZES, ROT_SNAPS, SCALE_SNAPS } from '../render/viewportSettings'
import { DEFAULT_PREFS, EditorPrefs, editorPrefs } from '../state/editorPrefs'
import type { Store } from '../state/store'
import { confirmDialog, openDialog } from './dialog'
import { h } from './dom'
import { nativePick } from './nativeDialog'

export type SettingsPage = 'general' | 'profile' | 'game' | 'lighting' | 'editor'

interface Page { id: SettingsPage; label: string; el: HTMLElement; dirty(): boolean; apply(): Promise<string | null>; reload?(): void }

const stop = (el: HTMLElement) => { el.addEventListener('keydown', (e) => e.stopPropagation()); return el }
const row = (label: string, control: HTMLElement, hint = '') => h('label', { class: 'field', ...(hint ? { title: hint } : {}) }, h('span', {}, label), control)
const note = (text: string) => h('div', { class: 'dim prefs-note' }, text)
const textInput = (value = '', placeholder = '') => stop(h('input', { type: 'text', value, placeholder })) as HTMLInputElement
const numInput = (value: number, step = 'any') => stop(h('input', { type: 'number', step, value: String(value) })) as HTMLInputElement
const check = (on: boolean) => { const c = h('input', { type: 'checkbox' }); c.checked = on; return c }
const select = (values: number[], current: number, fmt: (v: number) => string) => {
  const s = h('select', {}, ...values.map((v) => h('option', { value: String(v) }, fmt(v))))
  s.value = String(current)
  return s
}
/** A path field with a Browse… button next to it. */
const pathField = (input: HTMLInputElement, browse: () => Promise<string | null | undefined>) => {
  const b = h('button', {}, 'Browse…')
  b.addEventListener('click', () => void browse().then((p) => { if (p) { input.value = p; input.dispatchEvent(new Event('input', { bubbles: true })) } }))
  return h('div', { class: 'pathrow' }, input, b)
}

export function openSettings(store: Store, conn: CoreConnection, start: SettingsPage = 'general'): void {
  const unsubs: (() => void)[] = []
  const d = openDialog('Settings', { width: 780, keepOnBackdrop: true, onClose: () => unsubs.forEach((u) => u()) })
  d.root.querySelector('.modal')?.classList.add('prefs-window')
  const st = () => store.state
  const err = h('div', { class: 'fd-error prefs-error' })
  const pages: Page[] = [generalPage(), profilePage(), gamePage(), lightingPage(), editorPage()]

  // ---- frame: category list | page; footer buttons ---------------------------------------------------------------
  const nav = h('div', { class: 'prefs-nav', role: 'tablist', 'aria-orientation': 'vertical' })
  const holder = h('div', { class: 'prefs-page' })
  const apply = h('button', {}, 'Apply'), ok = h('button', { class: 'primary' }, 'OK'), cancel = h('button', {}, 'Cancel')
  const navBtns = pages.map((p) => {
    const b = h('button', { class: 'prefs-nav-item', role: 'tab' }, p.label)
    b.addEventListener('click', () => show(p))
    nav.append(b)
    return b
  })
  function show(p: Page): void {
    navBtns.forEach((b, i) => { b.classList.toggle('on', pages[i] === p); b.setAttribute('aria-selected', String(pages[i] === p)) })
    holder.replaceChildren(p.el)
  }
  const refreshButtons = () => {
    const any = pages.some((p) => p.dirty())
    apply.toggleAttribute('disabled', !any)
    navBtns.forEach((b, i) => b.classList.toggle('dirty', pages[i].dirty()))
  }
  d.body.addEventListener('input', refreshButtons)
  d.body.addEventListener('change', refreshButtons)
  async function applyAll(): Promise<boolean> {
    err.textContent = ''
    for (const p of pages) {
      if (!p.dirty()) continue
      const e = await p.apply()
      if (e) { show(p); err.textContent = e; refreshButtons(); return false }
    }
    refreshButtons()
    return true
  }
  apply.addEventListener('click', () => void applyAll())
  ok.addEventListener('click', () => void applyAll().then((done) => { if (done) d.close() }))
  cancel.addEventListener('click', () => d.close())
  d.body.append(h('div', { class: 'prefs' }, nav, holder), h('div', { class: 'modal-buttons prefs-foot' }, err, cancel, apply, ok))
  show(pages.find((p) => p.id === start) ?? pages[0])
  refreshButtons()

  // ---- General: the game folder ---------------------------------------------------------------------------------
  function generalPage(): Page {
    const folder = h('input', { type: 'text', readonly: '', value: st().assetsRoot }) as HTMLInputElement
    const browse = h('button', {}, 'Change…')
    const info = h('div', { class: 'prefs-info' })
    const files = h('div', { class: 'prefs-info dim' })
    const refresh = () => {
      const s = st()
      folder.value = s.assetsRoot
      info.replaceChildren(...[
        s.profile ? `Game profile: ${s.profile.name}` : 'Game profile: none (built-in defaults)',
        s.entityDefs.length ? `Entity definitions: ${s.entityDefs.length} classes from ${s.profile?.entityDefinitions ?? ''}` : `Entity definitions: ${s.profile?.entityDefinitions ?? 'entities.ent'} not found - the built-in list is used`,
        ...s.entityWarnings.slice(0, 5).map((w) => `⚠ ${w}`),
        ...(s.entityWarnings.length > 5 ? [`… and ${s.entityWarnings.length - 5} more warnings`] : []),
      ].map((l) => h('div', {}, l)))
      const c = s.coreSettings
      files.replaceChildren(...(c ? [h('div', {}, `Editor settings: ${c.settingsFile || '(not saved)'}`), h('div', {}, `Lighting settings: ${c.lightingFile || '(none)'}`)] : []))
    }
    refresh()
    unsubs.push(store.subscribe(refresh))
    browse.addEventListener('click', () => void (async () => {
      let p = await nativePick(conn, store, { kind: 'folder', title: 'Choose the game folder', startDir: st().assetsRoot })
      if (p === undefined) p = await pickFolderInPage(conn, st().assetsRoot)
      if (!p || p === st().assetsRoot) return
      if (st().dirty && !(await confirmDialog('Unsaved changes', 'The current map has unsaved changes. Switching the game folder re-opens the map from the new folder and discards them. Continue?', 'Switch folder'))) return
      try {
        await conn.whenIdle() // a document_sync is ignored while our own edit is still in flight
        const r = await conn.request((id) => msg.setAssetsRoot(id, p!))
        if (!r.ok) { err.textContent = r.error ?? 'Could not switch'; return }
        err.textContent = ''
        store.setMessage(r.persisted === false ? 'Game folder changed (could not be saved to settings.json)' : `Game folder: ${r.assetsRoot}`)
        for (const pg of pages) pg.reload?.() // the profile and the game launch belong to the folder
      } catch (e) { err.textContent = (e as Error).message }
    })())
    const el = h('div', {},
      h('div', { class: 'section' }, 'Game folder'),
      note('The game\'s data folder (for example GameData): textures, maps, prefabs and game_profile.json are found in it, and paths in maps are relative to it.'),
      h('div', { class: 'pathrow' }, folder, browse), info,
      h('div', { class: 'section' }, 'Files'), files)
    return { id: 'general', label: 'General', el, dirty: () => false, apply: async () => null }
  }

  // ---- Game profile: game_profile.json ---------------------------------------------------------------------------
  function profilePage(): Page {
    const el = h('div', {})
    let fields: { key: string; read(): unknown; initial: string }[] = []
    let loaded = false
    const relFolder = (input: HTMLInputElement, title: string) => pathField(input, () => nativePick(conn, store, { kind: 'folder', title, startDir: input.value, relative: true }))
    async function load(): Promise<void> {
      el.replaceChildren(note('Loading…'))
      let file: Record<string, unknown> = {}, p: GameProfile | undefined
      try {
        const r = await conn.request((id) => msg.readGameProfile(id))
        if (!r.ok) throw new Error(r.error ?? 'cannot read the profile')
        file = (r.file ?? {}) as Record<string, unknown>; p = r.profile
      } catch (e) { el.replaceChildren(note((e as Error).message)); return }
      if (!p) return
      const lighting = ((p as unknown as { lighting?: { blockLight?: string[]; receiveBakedLight?: string[] } }).lighting) ?? {}
      const name = textInput(p.name), tex = textInput(p.textureRoot), maps = textInput(p.mapRoot), prefabs = textInput(p.prefabRoot), groups = textInput(p.texGroupRoot ?? 'texgroups')
      const ent = textInput(p.entityDefinitions)
      const texExt = textInput(p.textureExtensions.join(', ')), modelExt = textInput(p.modelExtensions.join(', '))
      const ppm = numInput(p.pixelsPerMeter ?? 64, '1')
      // the export format (REQUIREMENTS §31 X2): File > Export and Play map use it
      const exporter = h('select', {}, h('option', { value: 'rbsp' }, 'rBSP (.bsp)'), h('option', { value: 'obj' }, 'OBJ + JSON (.obj)')) as HTMLSelectElement
      exporter.value = (p as unknown as { exporter?: string }).exporter === 'obj' ? 'obj' : 'rbsp'
      const block = stop(h('textarea', { rows: '3', placeholder: 'func_door' }, (lighting.blockLight ?? []).join('\n'))) as HTMLTextAreaElement
      const recv = stop(h('textarea', { rows: '3', placeholder: 'func_wall' }, (lighting.receiveBakedLight ?? []).join('\n'))) as HTMLTextAreaElement
      const list = (s: string) => s.split(/[\s,;]+/).map((x) => x.trim()).filter(Boolean)
      const exts = (s: string) => list(s).map((x) => (x.startsWith('.') ? x : `.${x}`))
      const f = (key: string, read: () => unknown) => ({ key, read, initial: JSON.stringify(read()) })
      fields = [
        f('name', () => name.value.trim()), f('textureRoot', () => tex.value.trim()), f('mapRoot', () => maps.value.trim()), f('prefabRoot', () => prefabs.value.trim()),
        f('texGroupRoot', () => groups.value.trim()), f('entityDefinitions', () => ent.value.trim()), f('textureExtensions', () => exts(texExt.value)),
        f('modelExtensions', () => exts(modelExt.value)), f('pixelsPerMeter', () => Number(ppm.value)), f('exporter', () => exporter.value),
        f('lighting.blockLight', () => list(block.value)), f('lighting.receiveBakedLight', () => list(recv.value)),
      ]
      loaded = true
      el.replaceChildren(
        ...(Object.keys(file).length ? [] : [note('This folder has no game_profile.json yet: the values below are the built-in defaults, and Apply creates the file.')]),
        h('div', { class: 'section' }, 'Game'), row('Name', name),
        row('Export format', exporter, 'What File > Export and Play map write: rBSP for the engine, or OBJ + JSON (.obj geometry, .json entities, .lightvol, lightmaps)'),
        h('div', { class: 'section' }, 'Folders (inside the game folder)'),
        row('Textures', relFolder(tex, 'Textures folder')), row('Maps', relFolder(maps, 'Maps folder')), row('Prefabs', relFolder(prefabs, 'Prefabs folder')), row('Texture groups', relFolder(groups, 'Texture groups folder')),
        row('Entity definitions', pathField(ent, () => nativePick(conn, store, { kind: 'open', title: 'Entity definitions file', startDir: ent.value.split('/').slice(0, -1).join('/'), filters: [{ name: 'Entity definitions (*.ent;*.def)', pattern: '*.ent;*.def' }], relative: true })), 'Quake 3 style .ent (XML) file'),
        h('div', { class: 'section' }, 'Files'),
        row('Texture extensions', texExt, 'Looked for in this order, e.g. .tga, .jpg, .png'), row('Model extensions', modelExt),
        row('Texture scale (px/m)', ppm, 'Texture pixels per metre when a texture has no properties of its own (64 = Quake 3)'),
        h('div', { class: 'section' }, 'Lighting classes'),
        note('One classname per line. worldspawn, func_group and func_detail always block light and receive baked light; brush entities that do not receive baked light are lit from the light volumes.'),
        row('Block light', block), row('Receive baked light', recv))
      refreshButtons()
    }
    const el0: Page = {
      id: 'profile', label: 'Game profile', el,
      dirty: () => loaded && fields.some((x) => JSON.stringify(x.read()) !== x.initial),
      reload: () => { loaded = false; void load() },
      apply: async () => {
        const changes: Record<string, unknown> = {}
        for (const x of fields) {
          if (JSON.stringify(x.read()) === x.initial) continue
          const [a, b] = x.key.split('.')
          if (b) changes[a] = { ...((changes[a] as object) ?? {}), [b]: x.read() }
          else changes[a] = x.read()
        }
        try {
          const r = await conn.request((id) => msg.writeGameProfile(id, changes))
          if (!r.ok) return `Game profile: ${r.error ?? 'refused'}`
          await load()
          return null
        } catch (e) { return (e as Error).message }
      },
    }
    void load()
    return el0
  }

  // ---- Run game: the executable File > Play map starts --------------------------------------------------------------
  function gamePage(): Page {
    const exe = textInput('', 'C:\\Games\\MyGame\\game.exe'), args = textInput(), wd = textInput('', "(the executable's folder)")
    let initial = ''
    const value = () => JSON.stringify({ exe: exe.value.trim(), args: args.value, workingDir: wd.value.trim() })
    const load = () => { const g = st().coreSettings?.game; exe.value = g?.exe ?? ''; args.value = g?.args ?? '--map {mapname}.bsp'; wd.value = g?.workingDir ?? ''; initial = value(); refreshButtonsSoon() }
    load()
    const el = h('div', {},
      h('div', { class: 'section' }, 'Play map (F5)'),
      note('File > Play map exports the map as BSP next to the map file and starts the game with it. Stored per game folder.'),
      row('Executable', pathField(exe, () => nativePick(conn, store, { kind: 'open', title: 'Game executable', startDir: exe.value ? exe.value.replace(/[\\/][^\\/]*$/, '') : st().assetsRoot, filters: [{ name: 'Programs (*.exe)', pattern: '*.exe' }] }))),
      row('Arguments', args), row('Working folder', pathField(wd, () => nativePick(conn, store, { kind: 'folder', title: 'Working folder', startDir: wd.value || st().assetsRoot }))),
      note('In the arguments and the working folder: {mapname} = the map\'s name (e1m1), {map} = the .bsp inside the game folder (maps/e1m1.bsp), {bsp} = its full path, {game} = the game folder.'))
    return {
      id: 'game', label: 'Run game', el, dirty: () => value() !== initial, reload: load,
      apply: async () => {
        try {
          const r = await conn.request((id) => msg.setSettings(id, { game: JSON.parse(value()) }))
          if (!r.ok) return r.error ?? 'Could not save'
          initial = value()
          return null
        } catch (e) { return (e as Error).message }
      },
    }
  }

  // ---- Lighting: lighting.json -----------------------------------------------------------------------------------
  function lightingPage(): Page {
    const el = h('div', {})
    let rows: { key: string; read(): number | boolean; initial: number | boolean }[] = []
    async function load(): Promise<void> {
      el.replaceChildren(note('Loading…'))
      let config: Record<string, number | boolean>, defaults: Record<string, number | boolean>, file = ''
      try {
        const r = await conn.request((id) => msg.readLightingConfig(id))
        if (!r.ok || !r.config) throw new Error(r.error ?? 'cannot read the lighting settings')
        config = r.config; defaults = r.defaults ?? {}; file = typeof r.file === 'string' ? r.file : ''
      } catch (e) { el.replaceChildren(note((e as Error).message)); rows = []; return }
      rows = []
      const known = new Set<string>()
      const groups = LIGHTING_GROUPS.map(([title, keys]) => {
        const items = keys.filter(([k]) => k in config).map(([k, label, hint]) => { known.add(k); return lightingRow(k, label, hint, config[k], defaults[k]) })
        return items.length ? [h('div', { class: 'section' }, title), ...items] : []
      }).flat()
      const other = Object.keys(config).filter((k) => !known.has(k)).map((k) => lightingRow(k, k, '', config[k], defaults[k]))
      el.replaceChildren(note(`Stored in ${file}. A BSP export uses changes at once; the live lighting in the viewports uses them after Core is restarted.`),
        ...groups, ...(other.length ? [h('div', { class: 'section' }, 'Other'), ...other] : []))
      refreshButtons()
    }
    function lightingRow(key: string, label: string, hint: string, value: number | boolean, def: number | boolean | undefined): HTMLElement {
      const input = typeof value === 'boolean' ? check(value) : key === 'denoiseQuality' ? select([0, 1, 2], value, (v) => ['Fast', 'Balanced', 'High'][v]) : numInput(value)
      const read = () => (input instanceof HTMLInputElement && input.type === 'checkbox' ? input.checked : Number(input.value))
      const reset = h('button', { class: 'reset', title: def === undefined ? '' : `Default: ${def}` }, '↺')
      const line = h('label', { class: 'field prop', title: hint ? `${hint}\n(${key})` : key }, h('span', {}, label), input, reset)
      const mark = () => { const changed = def !== undefined && read() !== def; line.classList.toggle('changed', changed); reset.style.visibility = changed ? '' : 'hidden' }
      reset.addEventListener('click', (e) => {
        e.preventDefault()
        if (def === undefined) return
        if (input instanceof HTMLInputElement && input.type === 'checkbox') input.checked = def as boolean; else input.value = String(def)
        mark(); refreshButtons()
      })
      input.addEventListener('input', mark); input.addEventListener('change', mark)
      mark()
      rows.push({ key, read, initial: value })
      return line
    }
    void load()
    return {
      id: 'lighting', label: 'Lighting', el,
      dirty: () => rows.some((r) => r.read() !== r.initial),
      apply: async () => {
        const changes: Record<string, number | boolean> = {}
        for (const r of rows) if (r.read() !== r.initial) changes[r.key] = r.read()
        try {
          const r = await conn.request((id) => msg.writeLightingConfig(id, changes))
          if (!r.ok) return `Lighting: ${r.error ?? 'refused'}`
          await load()
          if (r.warning) return r.warning
          store.setMessage('Lighting settings saved: BSP export uses them now, the live view after Core restarts')
          return null
        } catch (e) { return (e as Error).message }
      },
    }
  }

  // ---- Editor: preferences -----------------------------------------------------------------------------------------
  function editorPage(): Page {
    const p0 = editorPrefs(st().coreSettings?.editor)
    const grid = select(GRID_SIZES, p0.gridSize, (v) => `${v} m`), snap = check(p0.snapEnabled)
    const rot = select(ROT_SNAPS, p0.rotSnap, (v) => `${v}°`), scale = select(SCALE_SNAPS, p0.scaleSnap, (v) => String(v))
    const fly = numInput(p0.flySpeed), showGrid = check(p0.showGrid), uv = check(p0.uvLock), auto = check(p0.autoDetail)
    const read = (): EditorPrefs => ({ gridSize: Number(grid.value), snapEnabled: snap.checked, rotSnap: Number(rot.value), scaleSnap: Number(scale.value), flySpeed: Number(fly.value) || DEFAULT_PREFS.flySpeed, showGrid: showGrid.checked, uvLock: uv.checked, autoDetail: auto.checked })
    let initial = JSON.stringify(read())
    const recentInfo = h('span', { class: 'dim' }), clear = h('button', {}, 'Clear recent maps')
    const refreshRecent = () => { const n = st().coreSettings?.recentMaps.length ?? 0; recentInfo.textContent = n ? `${n} map${n === 1 ? '' : 's'} in File > Open Recent` : 'No recent maps'; clear.toggleAttribute('disabled', !n) }
    refreshRecent()
    unsubs.push(store.subscribe(refreshRecent))
    clear.addEventListener('click', () => void conn.request((id) => msg.setSettings(id, { clearRecent: true })).catch((e) => { err.textContent = (e as Error).message }))
    const el = h('div', {},
      h('div', { class: 'section' }, 'New viewports'),
      row('Grid size', grid), row('Snap to grid', h('span', {}, snap)), row('Rotation snap', rot), row('Scale snap', scale),
      row('Fly speed (m/s)', fly), row('Show grid', h('span', {}, showGrid)),
      h('div', { class: 'section' }, 'At start-up'),
      row('UV lock on', h('span', {}, uv), 'Textures stay on the faces when brushes move'), row('Concave → func_detail', h('span', {}, auto), 'An edit that makes a world brush concave moves it into a func_detail instead of being refused'),
      h('div', { class: 'section' }, 'Recent maps'), h('div', { class: 'pathrow' }, recentInfo, clear))
    return {
      id: 'editor', label: 'Editor', el, dirty: () => JSON.stringify(read()) !== initial,
      apply: async () => {
        try {
          const r = await conn.request((id) => msg.setSettings(id, { editor: { ...(st().coreSettings?.editor ?? {}), ...read() } }))
          if (!r.ok) return r.error ?? 'Could not save'
          initial = JSON.stringify(read())
          return null
        } catch (e) { return (e as Error).message }
      },
    }
  }

  function refreshButtonsSoon(): void { queueMicrotask(() => { if (apply.isConnected) refreshButtons() }) }
}

/** lighting.json keys by topic: [key, label, hint]. Keys Core sends that are not listed here appear under "Other". */
const LIGHTING_GROUPS: [string, [string, string, string][]][] = [
  ['Lightmaps', [['texelsPerMeter', 'Texels per metre', 'Lightmap resolution'], ['atlasSize', 'Atlas size (px)', 'Size of one lightmap page (a power of two)'], ['padding', 'Padding (texels)', ''], ['mergeAngleDeg', 'Smoothing angle (°)', 'Faces meeting at less than this angle share their lighting']]],
  ['Quality', [['maxPasses', 'Passes', 'Refinement passes per texel'], ['directSupersample', 'Direct supersampling', ''], ['areaSamplesPerPass', 'Area light samples / pass', ''], ['skySamplesPerPass', 'Sky samples / pass', ''], ['giSamplesPerPass', 'Bounce samples / pass', ''], ['giScale', 'Bounce grid scale', '1, 2 or 4: coarser bounce lighting is faster'], ['bounces', 'Bounce lighting', ''], ['albedo', 'Default albedo', 'Reflectance of surfaces without a texture'], ['indirectWindow', 'Indirect window', '']]],
  ['Denoising', [['denoise', 'Denoise bounce light', 'Needs Intel Open Image Denoise next to Core'], ['denoiseQuality', 'Denoise quality', '']]],
  ['Light volumes', [['lightvol', 'Bake light volumes', 'Lighting for brush entities and models'], ['lightvolGridSize', 'Grid size (m)', ''], ['lightvolMaxProbes', 'Max probes', '0 = no limit'], ['lightvolNormals', 'Directions per probe', ''], ['lightvolGiSamples', 'Bounce samples', ''], ['lightvolProbesPerStep', 'Probes per step', '']]],
  ['Live view', [['exposure', 'Exposure', ''], ['defaultSourceSize', 'Default light size (m)', 'Lights with source_size 0 are shown as spheres of this radius'], ['defaultSunSize', 'Default sun size (°)', ''], ['firstSendFraction', 'First result after (fraction)', ''], ['maxHoldSeconds', 'Max hold (s)', ''], ['flushIntervalMs', 'Tile send interval (ms)', '']]],
  ['Performance', [['useGpu', 'Use the GPU', 'Vulkan ray tracing when available'], ['useTextures', 'Coloured bounces from textures', ''], ['gpuTexelsPerStep', 'GPU texels per step', ''], ['gpuTargetStepMs', 'GPU step target (ms)', ''], ['gpuMaxStepScale', 'GPU max step scale', ''], ['texelsPerStep', 'CPU texels per step', ''], ['tickBudgetMs', 'Tick budget (ms)', ''], ['chunkTexels', 'Chunk size (texels)', '']]],
]

/** In-page folder browser (Core lists folders with browse_filesystem), for a Core that cannot show the system's folder dialog. */
function pickFolderInPage(conn: CoreConnection, startAt: string): Promise<string | null> {
  return new Promise((resolve) => {
    let result: string | null = null
    const d = openDialog('Choose the game folder', { width: 620, onClose: () => resolve(result) })
    const path = stop(h('input', { class: 'fd-name', placeholder: 'Absolute folder path, e.g. C:\\Games\\MyGame\\GameData' })) as HTMLInputElement
    const list = h('div', { class: 'fd-list' }), err = h('div', { class: 'fd-error' }), badge = h('div', { class: 'dim' })
    const up = h('button', {}, '↑ Up'), go = h('button', {}, 'Go'), use = h('button', { class: 'primary' }, 'Use this folder'), close = h('button', {}, 'Cancel')
    let here = '', parent: string | null = null
    async function browse(to: string): Promise<void> {
      err.textContent = ''
      try {
        const r = await conn.request((id) => msg.browseFilesystem(id, to))
        if (!r.ok) { err.textContent = r.error ?? 'Cannot open that folder'; return }
        here = r.path ?? ''; parent = r.parent ?? null
        path.value = here
        up.toggleAttribute('disabled', parent === null)
        use.toggleAttribute('disabled', here === '')
        badge.textContent = here === '' ? 'Pick a drive' : r.hasProfile ? '✓ game_profile.json found here' : 'No game_profile.json here - built-in defaults would be used'
        list.replaceChildren()
        const item = (label: string, target: string, icon: string) => {
          const el = h('div', { class: 'fd-row' }, h('span', { class: 'fd-icon' }, icon), label)
          el.addEventListener('click', () => void browse(target))
          list.append(el)
        }
        for (const drv of r.drives ?? []) item(drv, drv, '💽')
        for (const f of (r.dirs ?? []) as FolderEntry[]) item(f.name, f.path, '📁')
        if (!list.children.length) list.append(h('div', { class: 'dim pad' }, 'No sub-folders.'))
      } catch (e) { err.textContent = (e as Error).message }
    }
    up.addEventListener('click', () => void browse(parent ?? ''))
    go.addEventListener('click', () => void browse(path.value.trim()))
    path.addEventListener('keydown', (e) => { if (e.key === 'Enter') void browse(path.value.trim()) })
    use.addEventListener('click', () => { result = here || null; d.close() })
    close.addEventListener('click', () => d.close())
    d.body.append(h('div', { class: 'fd-bar' }, up, path, go), list, badge, err, h('div', { class: 'modal-buttons' }, close, use))
    void browse(startAt)
  })
}
