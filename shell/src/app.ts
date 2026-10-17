import { openDialog } from './ui/dialog'
import { createDockview, DockviewApi, DockviewGroupPanel, IDockviewPanel, themeDark } from 'dockview-core'
import { CoreConnection } from './net/connection'
import { msg } from './net/protocol'
import { gizmoLength, ringBasis } from './render/gizmo'
import { deleteSelection, pivotOf } from './state/actions'
import { Store } from './state/store'
import { base64ToBytes, TextureCache } from './state/textures'
import { PrefabPreviews } from './state/prefabPreview'
import { PrefabIcons, clearEntityIcons } from './render/icons'
import { ContentBrowserPanel } from './ui/contentBrowser'
import { confirmDialog } from './ui/dialog'
import { chooseFile, joinPath, parentDir } from './ui/fileDialog'
import { MAP_FILTERS, nativePick } from './ui/nativeDialog'
import { formatFilters, withFormatExt } from './state/exportFormat'
import { editorPrefs, viewportDefaults } from './state/editorPrefs'
import { openSettings } from './ui/settingsDialog'
import { h } from './ui/dom'
import { menuBar } from './ui/menuBar'
import { isOutsideWindow } from './ui/windowGeometry'
import { HistoryPanel, InspectorPanel, OutlinerPanel } from './ui/panels'
import { LevelSettingsPanel } from './ui/levelSettingsPanel'
import { MeshEditPanel, PlacePanel } from './ui/placePanel'
import { TexturePanel } from './ui/texturePanel'
import { openTextureProperties } from './ui/texturePropsWindow'
import { openReliefEditor } from './ui/reliefEditor'
import { propsToJson } from './model/texprops'
import { TexGroupCache } from './state/texgroups'
import { TexGroupsPanel } from './ui/texGroupsPanel'
import { toggleFastTextureTool } from './ui/fastTextureTool'
import { openTexGroupEditor, GroupEditorEnv } from './ui/texGroupEditor'
import { openTileSetEditor, TileSetEnv } from './ui/tileSetEditor'
import { applyTexGroup } from './state/actions'
import { MAX_VIEWPORTS, ViewportPanel } from './ui/viewport'

export function startApp(root: HTMLElement, coreUrl: string): void {
  let store!: Store
  let textures!: TextureCache
  let groups!: TexGroupCache
  const conn = new CoreConnection(coreUrl, {
    onStatus: (s) => store.setStatus(s),
    onMessage: (m) => { if (m.type === 'profile') { textures.setDefaultPpm(m.profile.pixelsPerMeter ?? 64); textures.clear(); groups.clear(); previews.clear(); prefabIcons.clear(); clearEntityIcons() } store.onMessage(m) },
    onAck: (rev, doc) => store.onAck(rev, doc),
    onRejected: (m) => store.onRejected(m),
  })
  store = new Store(conn)
  textures = new TextureCache(async (path) => {
    const r = await conn.request((id) => msg.readTexture(id, path))
    if (r.ok && r.sky) return { ext: '', bytes: new Uint8Array(0), sky: { panorama: r.sky.panorama, images: r.sky.images.map((im) => ({ ext: im.ext, bytes: base64ToBytes(im.data) })) } }
    if (!r.ok || !r.data || !r.ext) throw new Error(r.error ?? `texture '${path}' not found`)
    return { ext: r.ext, bytes: base64ToBytes(r.data), props: r.props }
  }, undefined, (run) => { if (document.visibilityState === 'visible') requestAnimationFrame(run); else setTimeout(run, 0) })  // textures arriving in one frame redraw once
  // texture groups (REQUIREMENTS §20): rule sets stored by Core next to the textures; a change rebuilds every mesh through the texture environment
  groups = new TexGroupCache(async (path) => {
    const r = await conn.request((id) => msg.readTexGroup(id, path))
    if (!r.ok || r.group === undefined) throw new Error(r.error ?? `texture group '${path}' not found`)
    return r.group
  }, () => textures.notify())
  const listGroups = async (): Promise<string[]> => (await conn.request((id) => msg.listTexGroups(id))).texgroups ?? []
  const saveGroup = async (path: string, group: unknown): Promise<void> => {
    const r = await conn.request((id) => msg.writeTexGroup(id, path, group))
    if (!r.ok) throw new Error(r.error ?? 'Core refused')
    groups.set(path, group)
  }
  const deleteGroup = async (path: string): Promise<void> => {
    const r = await conn.request((id) => msg.deleteTexGroup(id, path))
    if (!r.ok) throw new Error(r.error ?? 'Core refused')
    groups.remove(path)
  }
  const readGroup = async (path: string): Promise<unknown> => {
    const r = await conn.request((id) => msg.readTexGroup(id, path))
    if (!r.ok || r.group === undefined) throw new Error(r.error ?? `texture group '${path}' not found`)
    return r.group
  }
  let groupsPanels: TexGroupsPanel[] = []
  const editorEnv: GroupEditorEnv = {
    textures, listTextures: () => listTextures(), read: readGroup, save: saveGroup,
    apply: (path) => { const g = groups.group(path); return applyTexGroup(store, path, g?.fallback ? g.slots[g.fallback] : undefined, { wallsOnly: g?.kit?.kind === 'wall' }) },
    saved: () => { for (const p of groupsPanels) void p.reload() },
  }
  const setEnv: TileSetEnv = { textures, listTextures: () => listTextures(), read: readGroup, save: saveGroup, apply: editorEnv.apply, saved: editorEnv.saved }
  /** Opens the right editor: simple sets (a "kit" in the file) get the visual set editor, everything else the advanced one. */
  const openGroup = (path: string | null, kind?: 'wall' | 'frame' | 'advanced'): void => {
    if (path === null) { if (kind === 'wall' || kind === 'frame') void openTileSetEditor(setEnv, null, kind); else void openTexGroupEditor(editorEnv, null); return }
    void readGroup(path).then((raw) => { if (raw && typeof raw === 'object' && (raw as { kit?: unknown }).kit !== undefined) void openTileSetEditor(setEnv, path); else void openTexGroupEditor(editorEnv, path) }).catch(() => void openTexGroupEditor(editorEnv, path))
  }
  const duplicateGroup = async (from: string, to: string): Promise<void> => { await saveGroup(to, await readGroup(from)) }
  // prefab content for previews: files the map already uses come from the open library, everything else is read from Core on demand
  const previews: PrefabPreviews = new PrefabPreviews(async (path) => {
    const r = await conn.request((id) => msg.readPrefab(id, path))
    if (!r.ok || !r.doc) throw new Error(r.error ?? `prefab '${path}' not found`)
    return r.doc
  }, () => store.state.library)
  const prefabIcons = new PrefabIcons({ textures, previews, entityColor: (c) => store.entityColor(c), entityBox: (c) => store.entityBox(c) })
  const listTextures = async (): Promise<string[]> => (await conn.request((id) => msg.listTextures(id))).textures ?? []

  const openProps = (path: string): void => {
    openTextureProperties({
      textures, listTextures, activeTexture: () => store.state.activeTexture,
      save: async (p, props) => { const r = await conn.request((id) => msg.writeTextureProps(id, p, propsToJson(props))); if (!r.ok) throw new Error(r.error ?? 'Core refused') },
    }, path)
  }

  const openRelief = (path: string): void => {
    openReliefEditor({
      textures,
      save: async (p, props) => { const r = await conn.request((id) => msg.writeTextureProps(id, p, propsToJson(props))); if (!r.ok) throw new Error(r.error ?? 'Core refused') },
    }, path)
  }

  const viewports = new Set<ViewportPanel>()
  /** Live viewports in creation order (disposed ones, e.g. after a popout round trip, are pruned). */
  const liveViewports = (): ViewportPanel[] => { for (const v of [...viewports]) if (v.disposed) viewports.delete(v); return [...viewports] }
  let viewportSeq = 0
  const bar = h('div', { class: 'topbar' }), body = h('div', { class: 'dock' }), status = h('div', { class: 'statusbar' })
  root.replaceChildren(bar, body, status)
  const dock: DockviewApi = createDockview(body, {
    theme: themeDark,
    // Shift-drag of a tab makes a floating panel inside this window (Dockview default); real OS windows are made by popOut() below
    createRightHeaderActionComponent: (group) => popoutButton(group),
    createComponent: ({ id, name }) => {
      switch (name) {
        case 'viewport': {
          // the popout button opens a real OS window (positioned popup), which can be dragged to another monitor
          const v = new ViewportPanel(store, textures, () => { const p = dock.getPanel(id); if (p) popOut(p) }, previews)
          if (store.state.coreSettings) v.applyDefaults(viewportDefaults(editorPrefs(store.state.coreSettings.editor)))  // Settings > Editor
          viewports.add(v); return v
        }
        case 'place': return new PlacePanel(store, targetPoint, listDir, { prefabIcons, previews })
        case 'meshedit': return new MeshEditPanel(store, (id) => (liveViewports()[0]?.runTool(id) ?? false))
        case 'outliner': return new OutlinerPanel(store)
        case 'inspector': return new InspectorPanel(store)
        case 'texture': return new TexturePanel(store, textures, openProps, () => [...viewports][0]?.settings.gridSize ?? 0.25, () => { const c = liveViewports()[0]?.camera.basis; return c ? { right: c.right, up: c.up } : null }, openRelief, openGroup, () => toggleFastTextureTool({ store, textures }))
        case 'groups': { const p = new TexGroupsPanel(store, textures, groups, { list: listGroups, edit: openGroup, duplicate: duplicateGroup, remove: deleteGroup }); groupsPanels.push(p); return p }
        case 'browser': return new ContentBrowserPanel(store, textures, listTextures, openProps, openRelief)
        case 'history': return new HistoryPanel(store)
        case 'level': return new LevelSettingsPanel(store, { textures, listTextures })
        default: throw new Error(`unknown panel '${name}'`)
      }
    },
  })

  /**
   * Moves a panel or a whole group into its own OS window (draggable to another monitor). `at` = where the cursor was, in screen
   * coordinates, when the panel was dropped outside the browser window; without it the window opens next to the main one.
   * Closing that window puts the content back; a viewport that Dockview did not take back is recreated (scheduleRestore).
   */
  function popOut(item: IDockviewPanel | DockviewGroupPanel, at?: { x: number; y: number }): void {
    const panels = 'panels' in item ? item.panels : [item]
    const group = 'panels' in item ? item : item.group
    if (group.api.location.type === 'popout') return
    const viewportIds = panels.filter((p) => p.api.component === 'viewport').map((p) => p.id)
    const r = group.element.getBoundingClientRect()
    const big = viewportIds.length > 0
    const w = big ? Math.min(1280, Math.round(window.screen.availWidth * 0.7)) : Math.max(320, Math.round(r.width)), hgt = big ? Math.min(800, Math.round(window.screen.availHeight * 0.7)) : Math.max(240, Math.round(r.height))
    const left = at ? Math.round(at.x - Math.min(w / 2, 160)) : Math.round(window.screenX + 60), top = at ? Math.round(at.y - 16) : Math.round(window.screenY + 60)
    void dock.addPopoutGroup(item, {
      position: { left, top, width: w, height: hgt },
      // closing the OS window must not lose a viewport: if the dock did not take it back, make a fresh one in the main window
      onWillClose: ({ window: w2 }) => scheduleRestore(viewportIds, w2),
      onDidOpen: ({ window: w2 }) => { hookPopout(w2); w2.addEventListener('pagehide', () => scheduleRestore(viewportIds, w2)) },
    }).then((ok) => { if (!ok) store.setMessage('The browser blocked the new window: allow pop-ups for this page') })
  }

  /** A button in every group's header that opens the group in its own window. */
  function popoutButton(group: DockviewGroupPanel) {
    const b = h('button', { class: 'dv-popout-btn', title: 'Open this panel group in its own window (drag that window to another monitor). Dragging a tab outside the browser window does the same.' }, '⧉')
    b.addEventListener('click', () => popOut(group))
    const sync = () => { b.style.display = group.api.location.type === 'popout' ? 'none' : '' }
    return { element: b, init: () => { sync(); group.api.onDidLocationChange(sync) }, dispose: () => {} }
  }

  // Dragging a tab (or a group's header) out of the browser window and letting go opens it in its own window under the cursor
  let dragged: IDockviewPanel | DockviewGroupPanel | null = null
  // Dockview's own will-drag events are an optional module that is not included here, so the drag source is found from the DOM
  root.ownerDocument.addEventListener('dragstart', (e) => {
    const el = e.target as HTMLElement | null
    dragged = null
    const tab = el?.closest?.('.dv-tab'), void_ = el?.closest?.('.dv-void-container')
    const group = dock.groups.find((g) => g.element.contains(el as Node))
    if (!group || !(tab || void_)) return
    if (tab) {
      const text = tab.textContent?.trim()
      dragged = group.panels.find((p) => p.title === text) ?? null
    } else dragged = group
    if (dragged) store.setMessage('Drop outside the browser window to open this in its own window')
  }, true)
  root.ownerDocument.addEventListener('dragend', (e) => {
    const item = dragged
    dragged = null
    if (!item) return
    if (e.dataTransfer && e.dataTransfer.dropEffect !== 'none') return // dropped on a dock target: Dockview handled it
    if (!isOutsideWindow(e.screenX, e.screenY)) return
    popOut(item, { x: e.screenX, y: e.screenY })
  }, true)

  const targetPoint = (): [number, number, number] => {
    const t = liveViewports()[0]?.camera.target ?? [0, 0, 0]
    return [Math.round(t[0]), 0, Math.round(t[2])]
  }
  function addViewport(): void {
    if (viewports.size >= MAX_VIEWPORTS) return store.setMessage(`At most ${MAX_VIEWPORTS} viewports`)
    const n = ++viewportSeq
    const ref = [...dock.panels].find((x) => x.api.component === 'viewport')
    dock.addPanel({ id: `viewport-${n}`, component: 'viewport', title: `Viewport ${n}`, ...(ref ? { position: { referenceGroup: ref.group, direction: 'right' as const } } : {}) })
  }
  let restoreTimer = 0
  /** After a popout window was closed (not merely navigating): make sure its viewport lives in the main window again. */
  function scheduleRestore(ids: string[], win?: Window): void {
    if (!ids.length) return
    window.clearTimeout(restoreTimer)
    restoreTimer = window.setTimeout(() => {
      if (win && !win.closed) return // a pagehide while the popup loads is not a close
      if (!document.querySelector('.viewport canvas')) for (const id of ids) restoreViewport(id)
    }, 500)
  }
  /** A viewport that was in a popout window comes back to the main window when that window is closed. */
  function restoreViewport(oldId: string): void {
    const dead = dock.getPanel(oldId)
    if (dead) dock.removePanel(dead) // it still exists but its group lives in the closed window
    const n = ++viewportSeq
    const ref = [...dock.panels].find((x) => x.api.component === 'viewport')
    dock.addPanel({ id: `viewport-${n}`, component: 'viewport', title: `Viewport ${n}`, ...(ref ? { position: { referenceGroup: ref.group, direction: 'right' as const } } : { position: { referencePanel: 'place', direction: 'right' as const } }) })
  }
  const MAP_EXT = '.map.json'
  const listDir = async (dir: string) => {
    const r = await conn.request((id) => msg.listDirectory(id, dir))
    if (!r.ok) throw new Error(r.error ?? 'cannot list folder')
    return { path: r.path ?? '', entries: r.entries ?? [] }
  }
  const mapDir = (): string => store.state.path ? parentDir(store.state.path) : (store.state.profile?.mapRoot ?? 'maps')
  /** Saves to `path` (undefined = the current file). Waits until Core has every edit made so far. */
  async function save(path?: string): Promise<boolean> {
    if (!store.canEdit) { store.setMessage('Not connected to Core - cannot save'); return false }
    if (!path && !store.state.path) return saveAs()
    try {
      await conn.whenIdle()
      const snapshot = store.state.doc
      const r = await conn.request((id) => msg.saveDocument(id, path))
      if (!r.ok) { store.setMessage(r.error ?? 'Save failed'); return false }
      store.onSaved(r.path ?? '', snapshot)
      return true
    } catch (e) { store.setMessage((e as Error).message); return false }
  }
  /** `p` ending in `ext` (the system's save dialog sees ".map.json" as ".json"). */
  const withExt = (p: string, ext: string): string => p.toLowerCase().endsWith(ext) ? p : p.replace(/\.(map\.)?json$|\.map$|\.bsp$/i, '') + ext
  async function saveAs(): Promise<boolean> {
    const name = store.state.path.split('/').pop() || `untitled${MAP_EXT}`
    let p = await nativePick(conn, store, { kind: 'save', title: 'Save map as', startDir: mapDir(), fileName: name, filters: MAP_FILTERS, defaultExt: 'map.json', relative: true })
    if (p === undefined) p = await chooseFile({ mode: 'save', title: 'Save map as', ext: MAP_EXT, startDir: mapDir(), startName: store.state.path.split('/').pop() ?? '', list: listDir })
    return p ? save(withExt(p, MAP_EXT)) : false
  }
  /** File > Export (REQUIREMENTS §25 E9, §31): the map as it is now, in the game's format (game_profile.json "exporter"; rBSP: <name>.bsp +
   *  <name>/lm_NNNN.exr), baked in the Core. While it runs the button cancels. */
  async function exportMap(): Promise<void> {
    if (!store.canEdit) { store.setMessage('Not connected to Core - cannot export'); return }
    if (store.state.exporting) { await conn.request((id) => msg.exportCancel(id)).catch(() => undefined); return }
    const f = store.state.exportFormat, title = `Export ${f.name}`, fileName = `${mapBase()}${f.extension}`
    let p = await nativePick(conn, store, { kind: 'save', title, startDir: mapDir(), fileName, filters: formatFilters(f), defaultExt: f.extension.slice(1), relative: true })
    if (p === undefined) p = await chooseFile({ mode: 'save', title, ext: f.extension, startDir: mapDir(), startName: fileName, list: listDir })
    if (p) await startExport(withFormatExt(p, f))
  }
  const mapBase = (): string => (store.state.path.split('/').pop() ?? '').replace(/\.map\.json$/, '') || 'untitled'
  /** Starts the export to `p` (assets relative); the path the Core writes (with its format's extension), or null when it refused. */
  async function startExport(p: string): Promise<string | null> {
    try {
      await conn.whenIdle()
      const r = await conn.request((id) => msg.exportMap(id, p))
      if (!r.ok) { store.setMessage(`Export: ${r.error ?? 'refused'}`); return null }
      store.onMessage({ type: 'export_progress', stage: 'starting', fraction: 0 })
      return typeof r.path === 'string' && r.path ? r.path : p
    } catch (e) { store.setMessage((e as Error).message); return null }
  }
  /** File > Play map (F5): exports <map>.<format's extension> next to the map file, then starts the game set in Settings > Run game with it. */
  async function playMap(): Promise<void> {
    if (!store.canEdit) { store.setMessage('Not connected to Core - cannot play'); return }
    if (store.state.exporting) { store.setMessage('An export is already running'); return }
    if (!store.state.coreSettings?.game.exe) { store.setMessage('Play map: choose the game executable first'); openSettings(store, conn, 'game'); return }
    const before = store.state.lastExport
    const bsp = await startExport(joinPath(mapDir(), `${mapBase()}${store.state.exportFormat.extension}`))
    if (!bsp) return
    const done = await new Promise<NonNullable<typeof before>>((resolve) => {
      const un = store.subscribe(() => { const r = store.state.lastExport; if (r && r !== before && !store.state.exporting) { un(); resolve(r) } })
    })
    if (!done.ok) { if (!done.cancelled) store.setMessage(`Play map: the export failed: ${done.error}`); return }
    try {
      const r = await conn.request((id) => msg.launchGame(id, bsp))
      store.setMessage(r.ok ? `Started the game: ${r.commandLine ?? ''}` : `Play map: ${r.error ?? 'the game did not start'}`)
    } catch (e) { store.setMessage((e as Error).message) }
  }
  /** True when it is fine to throw away the current document (nothing unsaved, or the person said so). */
  const okToDiscard = async (): Promise<boolean> =>
    !store.state.dirty || confirmDialog('Unsaved changes', `${store.state.path || 'The current map'} has unsaved changes. Discard them?`, 'Discard')
  async function open(): Promise<void> {
    if (!(await okToDiscard())) return
    let p = await nativePick(conn, store, { kind: 'open', title: 'Open map', startDir: mapDir(), filters: MAP_FILTERS, relative: true })
    if (p === undefined) p = await chooseFile({ mode: 'open', title: 'Open map', ext: MAP_EXT, startDir: mapDir(), startName: store.state.path.split('/').pop(), list: listDir })
    if (p) await openPath(p)
  }
  async function openPath(p: string): Promise<void> {
    try { await conn.whenIdle(); const r = await conn.request((id) => msg.openDocument(id, p)); if (!r.ok) store.setMessage(r.error ?? 'Open failed') }
    catch (e) { store.setMessage((e as Error).message) }
  }
  async function openRecent(p: string): Promise<void> { if (p !== store.state.path && await okToDiscard()) await openPath(p) }

  // ---- menu bar: every action lives here (with its shortcut); the toolbar below keeps the frequent ones -------------
  const exportTip = (): string => store.state.exportFormat.id === 'obj'
    ? `Export the map as OBJ (the game's format, game_profile.json "exporter"): bakes the final lighting, writes <name>.obj, <name>.json (entities), <name>.lightvol and <name>/lm_*.exr`
    : `Export the map as ${store.state.exportFormat.name} (the game's format, game_profile.json "exporter"): bakes the final lighting (Level settings: bake quality), writes <name>${store.state.exportFormat.extension} and <name>/lm_*.exr`
  const exportLabel = () => store.state.exporting ? `Cancel export (${Math.round(store.state.exporting.fraction * 100)}%)` : `Export ${store.state.exportFormat.name}…`
  /** View menu: brings a panel to the front, or docks it again where it lives by default if it was closed. */
  const PANELS: [id: string, title: string, near: string, dir: 'within' | 'left' | 'right' | 'below'][] = [
    ['place', 'Place', 'viewport-1', 'left'], ['meshedit', 'Mesh Edit', 'place', 'within'], ['outliner', 'Outliner', 'viewport-1', 'right'],
    ['inspector', 'Details', 'outliner', 'below'], ['texture', 'Texture', 'place', 'below'], ['level', 'Level', 'inspector', 'within'],
    ['browser', 'Content Browser', 'viewport-1', 'below'], ['groups', 'Texture Groups', 'browser', 'within'], ['history', 'History', 'browser', 'within'],
  ]
  const showPanel = (id: string): void => {
    const p = dock.getPanel(id)
    if (p) { p.api.setActive(); return }
    const [, title, near, dir] = PANELS.find((x) => x[0] === id)!
    const ref = dock.getPanel(near) ?? [...dock.panels].find((x) => x.api.component === 'viewport')
    dock.addPanel({ id, component: id, title, ...(ref ? { position: { referencePanel: ref.id, direction: dir } } : {}) })
  }
  bar.append(menuBar([
    { label: 'File', items: () => [
      { label: 'Open…', shortcut: 'Ctrl+O', title: 'Open a map from the game folder', action: () => void open() },
      { label: 'Open Recent', disabled: !store.state.coreSettings?.recentMaps.length, submenu: () => [
        ...(store.state.coreSettings?.recentMaps ?? []).map((p, i) => ({ label: `${i + 1}  ${p}`, checked: p === store.state.path, action: () => void openRecent(p) })),
        '-', { label: 'Clear Recent', action: () => void conn.request((id) => msg.setSettings(id, { clearRecent: true })).catch(() => undefined) },
      ] },
      '-',
      { label: 'Save', shortcut: 'Ctrl+S', action: () => void save() },
      { label: 'Save As…', shortcut: 'Ctrl+Shift+S', title: 'Save under a new name', action: () => void saveAs() },
      '-',
      { label: exportLabel(), title: exportTip(), action: () => void exportMap() },
      { label: 'Play Map', shortcut: 'F5', title: 'Export the map as BSP next to the map file and start the game with it (Settings > Run game)', action: () => void playMap() },
      '-',
      { label: 'Game Folder…', title: 'Choose the game folder (textures, maps, game_profile.json)', action: () => openSettings(store, conn, 'general') },
      { label: 'Settings…', shortcut: 'Ctrl+,', title: 'Game folder, game profile, running the game, lighting, editor preferences', action: () => openSettings(store, conn) },
    ] },
    { label: 'Edit', items: () => [
      { label: 'Undo', shortcut: 'Ctrl+Z', action: () => store.undo() },
      { label: 'Redo', shortcut: 'Ctrl+Y', action: () => store.redo() },
      '-',
      { label: 'Delete', shortcut: 'Del', title: 'Delete the selection', action: () => deleteSelection(store) },
      '-',
      { label: 'UV lock', checked: store.state.uvLock, title: 'On = textures stay on the faces when you move, rotate or scale brushes', action: () => store.setUvLock(!store.state.uvLock) },
    ] },
    { label: 'Tools', items: () => [
      { label: 'Select', shortcut: 'Q', checked: store.state.tool === 'select', action: () => store.setTool('select') },
      { label: 'Shape', shortcut: 'B', checked: store.state.tool === 'box', title: 'Draw a shape: drag, release, move, click. Pick the shape in the Place panel', action: () => store.setTool('box') },
      { label: 'Decal', checked: store.state.tool === 'decal', title: 'Click a surface to place a decal projector', action: () => store.setTool('decal') },
      '-',
      { label: 'Brush mode', shortcut: 'Tab', checked: store.state.editMode === 'object', action: () => store.setEditMode('object') },
      { label: 'Mesh mode', shortcut: 'Tab', checked: store.state.editMode === 'mesh', action: () => store.setEditMode('mesh') },
      '-',
      { label: 'Fast Texture Tool', shortcut: 'Shift+Q', action: () => toggleFastTextureTool({ store, textures }) },
    ] },
    { label: 'View', items: () => [
      { label: 'Add viewport', disabled: liveViewports().length >= MAX_VIEWPORTS, title: `At most ${MAX_VIEWPORTS}; the ⧉ button of a group moves it to its own window`, action: addViewport },
      { label: 'Frame all', shortcut: 'F', action: () => liveViewports()[0]?.frameAll() },
      '-',
      ...PANELS.map(([id, title]) => ({ label: title, title: `Show the ${title} panel (reopens it if it was closed)`, action: () => showPanel(id) })),
      '-',
      // layouts (REQUIREMENTS §35): kept between sessions; presets save one under a name
      { label: 'Save layout preset…', title: 'Keep the current arrangement of the panels under a name', action: () => saveLayoutPreset() },
      { label: 'Layout presets', disabled: !Object.keys(layoutPresets()).length, submenu: () => Object.keys(layoutPresets()).sort((x, y) => x.localeCompare(y)).map((n) => ({ label: n, title: `Arrange the panels as in the preset "${n}"`, action: () => { applyLayout(layoutPresets()[n] ?? null, `Layout: ${n}`) } })) },
      { label: 'Delete layout preset', disabled: !Object.keys(layoutPresets()).length, submenu: () => Object.keys(layoutPresets()).sort((x, y) => x.localeCompare(y)).map((n) => ({ label: n, action: () => { const ps = layoutPresets(); delete ps[n]; writeJson(PRESETS_KEY, ps); store.setMessage(`Layout preset "${n}" deleted`) } })) },
      { label: 'Reset layout', title: 'The standard arrangement of the panels', action: () => { resetLayout(); store.setMessage('Default layout') } },
    ] },
  ]))

  // ---- toolbar: only the frequent actions, grouped; everything is also in the menus --------------------------------
  const btn = (label: string, title: string, fn: () => void, cls = '') => { const b = h('button', { title, class: cls }, label); b.addEventListener('click', fn); return b }
  const group = (...kids: HTMLElement[]) => h('div', { class: 'tb-group' }, ...kids)
  const toolBtns = { select: btn('Select', 'Select tool (Q)', () => store.setTool('select'), 'tool'), box: btn('Shape', 'Draw a shape: drag, release, move, click (B). Pick the shape in the Place panel', () => store.setTool('box'), 'tool'), decal: btn('Decal', 'Click a surface to place a decal projector', () => store.setTool('decal'), 'tool') }
  const exportBtn = btn('', 'Cancel the export', () => void exportMap(), 'tb-export')
  const uvLockBtn = btn('UV lock', 'UV lock (global): on = textures stay on the faces when you move, rotate or scale brushes', () => store.setUvLock(!store.state.uvLock), 'tool')
  const modeBtns = { object: btn('Brush', 'Brush mode: NetRadiant-style object editing (Tab toggles Brush / Mesh)', () => store.setEditMode('object'), 'tool'), mesh: btn('Mesh', 'Mesh mode: Blender-style vertex / edge / face editing of the selected brush (Tab)', () => store.setEditMode('mesh'), 'tool') }
  const tools = h('div', { class: 'toolbar' },
    group(btn('Save', 'Save (Ctrl+S)', () => void save()), btn('Undo', 'Undo (Ctrl+Z)', () => store.undo()), btn('Redo', 'Redo (Ctrl+Y)', () => store.redo())),
    group(btn('▶ Play', 'Play map (F5): export the map as BSP and start the game with it (Settings > Run game)', () => void playMap(), 'tb-play')),
    h('span', { class: 'tb-label' }, 'Tool'), group(toolBtns.select, toolBtns.box, toolBtns.decal),
    h('span', { class: 'tb-label' }, 'Mode'), group(modeBtns.object, modeBtns.mesh),
    group(uvLockBtn), group(btn('Delete', 'Delete selection (Del)', () => deleteSelection(store))),
    group(btn('+ Viewport', `Add a viewport (max ${MAX_VIEWPORTS}); drag a tab out of the browser window, or use the ⧉ button of a group, to move it to another monitor`, addViewport)),
    h('span', { class: 'tb-spacer' }), exportBtn)
  bar.append(tools)

  const conn$ = h('span', { class: 'status' }), path$ = h('span', { class: 'dim' }), hint$ = h('span', { class: 'hint' }), msg$ = h('span', { class: 'message' })
  status.append(conn$, path$, hint$, msg$)
  const HINTS: Record<string, string> = {
    select: 'LMB select (Shift = add) · drag arrows to move · G/R/S move/rotate/scale · Ctrl+D duplicate · RMB+drag look, +WASD/QE fly, +wheel speed · MMB pan · wheel dolly · F frame · Tab mesh mode · Del delete',
    decal: 'Decal: click a surface to place a projector (size in the Place tab → Decals)',
    box: 'Box: drag on a surface, release, move for height, click · Shift = 1:1:1 · Esc cancel · grid snap in the viewport header',
  }
  store.subscribe(() => {
    const s = store.state
    conn$.textContent = s.status === 'connected' ? (s.synced ? `● Connected  rev ${s.rev}` : '● Syncing…') : s.status === 'connecting' ? '● Connecting…' : '● Disconnected'
    conn$.className = `status ${s.status === 'connected' && s.synced ? 'ok' : 'bad'}`
    path$.textContent = s.path ? `${s.path}${s.dirty ? ' •' : ''}` : (s.dirty ? '(unsaved) •' : '')
    path$.className = s.dirty ? 'dirty' : 'dim'
    root.ownerDocument.title = `${s.dirty ? '• ' : ''}${s.path || 'untitled'} - Level Editor`
    hint$.textContent = s.editMode === 'mesh' ? 'Mesh: 1/2/3 vertex/edge/face · click / Shift-click / drag-box · Alt-click or double-click loop · Ctrl+Alt ring · Ctrl-click path · G R S · E extrude · I inset · Ctrl+B bevel · Ctrl+R loop cut · K knife · O proportional · J connect · Ctrl+M mirror · M merge · X dissolve · F fill · Tab back to Brush mode' : (HINTS[s.tool] ?? '')
    msg$.textContent = s.message ?? ''; msg$.title = s.message ?? ''
    for (const k of ['select', 'box', 'decal'] as const) toolBtns[k].classList.toggle('on', s.tool === k)
    for (const k of ['object', 'mesh'] as const) modeBtns[k].classList.toggle('on', s.editMode === k)
    uvLockBtn.classList.toggle('on', s.uvLock)
    exportBtn.textContent = exportLabel(); exportBtn.hidden = !s.exporting
  })

  // ---- keys (Blender/UE conventions, DESIGN.md §17) ---------------------------------
  const onAppKey = (e: KeyboardEvent): void => {
    const tag = (e.target as HTMLElement | null)?.tagName
    if (tag === 'INPUT' || tag === 'SELECT' || tag === 'TEXTAREA') return
    if ((e.target as HTMLElement | null)?.closest?.('.modal')) return // dialogs own the keyboard
    const mod = e.ctrlKey || e.metaKey, k = e.key.toLowerCase()
    if (mod && k === 'z') { e.preventDefault(); e.shiftKey ? store.redo() : store.undo() }
    else if (mod && k === 'y') { e.preventDefault(); store.redo() }
    else if (mod && k === 's') { e.preventDefault(); void (e.shiftKey ? saveAs() : save()) }
    else if (mod && k === 'o') { e.preventDefault(); void open() }
    else if (mod && k === ',') { e.preventDefault(); openSettings(store, conn) }
    else if (k === 'f5' && !mod) { e.preventDefault(); void playMap() }
    else if (mod) return
    else if (k === 'delete' || k === 'backspace' || k === 'x') { e.preventDefault(); deleteSelection(store) }
    else if (k === 'tab') { e.preventDefault(); store.setEditMode(store.state.editMode === 'mesh' ? 'object' : 'mesh') }  // Brush mode <-> Mesh mode
    else if (k === 'q' && e.shiftKey) { e.preventDefault(); toggleFastTextureTool({ store, textures }) }
    else if (k === 'q') store.setTool('select')
    else if (k === 'b') store.setTool('box')
    else if (k === 'f') liveViewports()[0]?.frameAll()
  }
  root.ownerDocument.addEventListener('keydown', onAppKey)
  // a popout window is its own document: keys pressed there never reach the main window's listeners
  const popoutDocs = new Set<Document>()
  const hookPopout = (w2: Window): void => {
    if (popoutDocs.has(w2.document)) return
    popoutDocs.add(w2.document)
    w2.document.addEventListener('keydown', onAppKey)
    w2.addEventListener('pagehide', () => { w2.document.removeEventListener('keydown', onAppKey); popoutDocs.delete(w2.document) })
  }

  // ---- layout (REQUIREMENTS §35): (Place, Mesh Edit) over Texture | viewport over (Content Browser, Texture Groups, History) | Outliner over (Details, Level).
  // The arrangement is kept in this browser's / app's local storage and restored on the next start; named presets live next to it.
  const LAYOUT_KEY = 'levelEditor.layout.v1', PRESETS_KEY = 'levelEditor.layoutPresets.v1'
  function readJson<T>(key: string): T | null { try { const v = localStorage.getItem(key); return v ? (JSON.parse(v) as T) : null } catch { return null } }
  function writeJson(key: string, value: unknown): void { try { localStorage.setItem(key, JSON.stringify(value)) } catch { /* storage blocked: the layout just is not kept */ } }
  type SavedLayout = { dock: ReturnType<DockviewApi['toJSON']>; reopen?: string[] }
  const layoutPresets = (): Record<string, SavedLayout> => readJson<Record<string, SavedLayout>>(PRESETS_KEY) ?? {}
  /** The current arrangement. Panels in their own OS windows are not kept as windows (a browser may block them on start): they reopen docked. */
  function snapshot(): SavedLayout {
    const j = dock.toJSON()
    const reopen: string[] = []
    for (const g of j.popoutGroups ?? []) for (const id of g.data?.views ?? []) reopen.push(id)
    const { popoutGroups: _p, ...rest } = j
    void _p
    return { dock: rest as ReturnType<DockviewApi['toJSON']>, reopen }
  }
  function defaultLayout(): void {
    viewportSeq = 0
    addViewport()
    dock.addPanel({ id: 'browser', component: 'browser', title: 'Content Browser', position: { referencePanel: 'viewport-1', direction: 'below' } })
    dock.addPanel({ id: 'groups', component: 'groups', title: 'Texture Groups', position: { referencePanel: 'browser', direction: 'within' } })
    dock.addPanel({ id: 'history', component: 'history', title: 'History', position: { referencePanel: 'browser', direction: 'within' } })
    dock.addPanel({ id: 'place', component: 'place', title: 'Place', position: { direction: 'left' } })
    dock.addPanel({ id: 'meshedit', component: 'meshedit', title: 'Mesh Edit', position: { referencePanel: 'place', direction: 'within' } })
    dock.addPanel({ id: 'texture', component: 'texture', title: 'Texture', position: { referencePanel: 'place', direction: 'below' } })
    dock.addPanel({ id: 'outliner', component: 'outliner', title: 'Outliner', position: { direction: 'right' } })
    dock.addPanel({ id: 'inspector', component: 'inspector', title: 'Details', position: { referencePanel: 'outliner', direction: 'below' } })
    dock.addPanel({ id: 'level', component: 'level', title: 'Level', position: { referencePanel: 'inspector', direction: 'within' } })
    for (const id of ['place', 'texture', 'inspector', 'browser', 'viewport-1']) dock.getPanel(id)?.api.setActive()
    // sizes only take effect once the layout has been measured
    requestAnimationFrame(() => {
      const H = body.clientHeight, W = body.clientWidth
      dock.layout(W, H) // Dockview's own resize observer may not have run yet; sizes set before it would be rescaled away
      const clamp = (v: number, lo: number, hi: number) => Math.round(Math.min(hi, Math.max(lo, v)))
      dock.getPanel('place')?.api.setSize({ width: clamp(W * 0.16, 240, 360) })
      dock.getPanel('outliner')?.api.setSize({ width: clamp(W * 0.21, 260, 440) })
      dock.getPanel('place')?.api.setSize({ height: Math.round(H * 0.32) })
      dock.getPanel('outliner')?.api.setSize({ height: Math.round(H * 0.32) })
      dock.getPanel('browser')?.api.setSize({ height: Math.round(H * 0.33) })
    })
  }
  /** Replaces the arrangement; false (and the arrangement unchanged or the default) when the saved one cannot be used. */
  function applyLayout(saved: SavedLayout | null, message?: string): boolean {
    if (!saved?.dock) return false
    try {
      dock.fromJSON(saved.dock)
    } catch (e) {
      console.warn('saved layout not usable', e)
      resetLayout()
      store.setMessage('The saved layout could not be used: back to the default')
      return false
    }
    viewportSeq = Math.max(0, ...[...dock.panels].map((p) => /^viewport-(\d+)$/.exec(p.id)).filter((m) => m).map((m) => Number(m![1])))
    for (const id of saved.reopen ?? []) { if (id.startsWith('viewport-')) { if (!dock.getPanel(id)) addViewport() } else if (PANELS.some((x) => x[0] === id)) showPanel(id) }
    if (![...dock.panels].some((p) => p.api.component === 'viewport')) addViewport()
    if (message) store.setMessage(message)
    return true
  }
  function resetLayout(): void { dock.clear(); defaultLayout() }
  function saveLayoutPreset(): void {
    const d = openDialog('Save layout preset', { width: 360 })
    const presets = layoutPresets()
    const name = h('input', { type: 'text', placeholder: 'Name', value: '' }) as HTMLInputElement
    const ok = h('button', { class: 'primary' }, 'Save'), cancel = h('button', {}, 'Cancel')
    const commit = () => {
      const n = name.value.trim()
      if (!n) return void name.focus()
      writeJson(PRESETS_KEY, { ...layoutPresets(), [n]: snapshot() })
      store.setMessage(`Layout preset "${n}" saved${presets[n] ? ' (replaced)' : ''}`)
      d.close()
    }
    ok.addEventListener('click', commit)
    cancel.addEventListener('click', () => d.close())
    name.addEventListener('keydown', (e) => { e.stopPropagation(); if (e.key === 'Enter') commit() })
    d.body.append(h('div', { class: 'modal-text' }, 'The panels and their sizes as they are now; View > Layout lists the presets. A name that exists is replaced.'), name, h('div', { class: 'modal-buttons' }, cancel, ok))
    name.focus()
  }
  if (!applyLayout(readJson<SavedLayout>(LAYOUT_KEY))) defaultLayout()
  // keep the arrangement: after every change (debounced) and when the window closes
  let layoutTimer = 0
  const keepLayout = () => { window.clearTimeout(layoutTimer); layoutTimer = window.setTimeout(() => writeJson(LAYOUT_KEY, snapshot()), 400) }
  dock.onDidLayoutChange(keepLayout)
  window.addEventListener('pagehide', () => writeJson(LAYOUT_KEY, snapshot()))
  store.showPanel = showPanel  // right-click > Properties

  // closing the tab with unsaved work asks first (the browser shows its own generic prompt)
  window.addEventListener('beforeunload', (e) => { if (store.state.dirty) e.preventDefault() })

  // Settings > Editor: viewports made before Core sent the preferences take them when they arrive
  let prefsApplied = false
  store.subscribe(() => {
    const c = store.state.coreSettings
    if (prefsApplied || !c) return
    prefsApplied = true
    for (const v of liveViewports()) v.applyDefaults(viewportDefaults(editorPrefs(c.editor)))
  })

  let framed = false
  store.subscribe(() => { if (!framed && store.state.synced) { framed = true; liveViewports()[0]?.frameAll() } })

  // Test hook (?debug): lets the browser e2e test aim at things instead of guessing pixels.
  if (new URLSearchParams(location.search).has('debug')) {
    ;(window as any).__editor = {
      store, textures, open: (p: string) => conn.request((id) => msg.openDocument(id, p)),
      viewport: () => liveViewports()[0],
      popout: (id: string) => dock.addPopoutGroup(dock.getPanel(id)!, { onDidOpen: ({ window: w2 }) => hookPopout(w2) }),
      popOut: (id: string, at?: { x: number; y: number }) => popOut(dock.getPanel(id)!, at),
      previews, prefabIcons,
      clipPoints: (pts: [number, number, number][]) => liveViewports()[0]?.debugClipPoints(pts) ?? false,
      faceHandle: () => liveViewports()[0]?.debugFaceHandle() ?? null,
      /** Screen position of the point on the rotation ring about `axis` at angle `a` (radians). */
      ringPoint(axis: 0 | 1 | 2, a: number) {
        const v = liveViewports()[0], sel = store.state.selection
        const pivot = sel && pivotOf(store.state.doc, sel)
        if (!v || !pivot) return null
        const [u, w] = ringBasis(axis), r = gizmoLength(v.camera.distanceTo(pivot))
        const p: [number, number, number] = [0, 1, 2].map((k) => pivot[k] + (u[k] * Math.cos(a) + w[k] * Math.sin(a)) * r) as [number, number, number]
        return v.worldToScreen(p)
      },
      gizmoHandle(axis: 0 | 1 | 2) {
        const v = liveViewports()[0], sel = store.state.selection
        const pivot = sel && pivotOf(store.state.doc, sel)
        if (!v || !pivot) return null
        const p: [number, number, number] = [...pivot]
        p[axis] += gizmoLength(v.camera.distanceTo(pivot)) * 0.6
        return { from: v.worldToScreen(pivot), to: v.worldToScreen(p) }
      },
    }
  }
  conn.connect()
}
