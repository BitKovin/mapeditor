import type { IContentRenderer } from 'dockview-core'
import { applyTexGroup } from '../state/actions'
import type { Store } from '../state/store'
import type { TexGroupCache } from '../state/texgroups'
import type { TextureCache } from '../state/textures'
import { APPLY_DELAY_MS } from './contentBrowser'
import { showMenu } from './contextMenu'
import { confirmDialog } from './dialog'
import { h } from './dom'
import { drawGroupPreview } from './groupPreview'

export interface GroupsPanelEnv {
  list: () => Promise<string[]>
  /** opens an editor: an existing group (the right editor is chosen by what the file is), or a new one of `kind` (wall / frame = simple sets, advanced = rules) */
  edit: (path: string | null, kind?: 'wall' | 'frame' | 'advanced') => void
  duplicate: (from: string, to: string) => Promise<void>
  remove: (path: string) => Promise<void>
}

/**
 * Texture Groups browser (REQUIREMENTS §20 T8). Two tabs: Simple (texture sets: rows of pieces, 9-slice frames) and Advanced (rule groups). Click = apply to the selected faces / brushes (each face takes the rule branch that fits its orientation),
 * double-click = edit, right-click = menu. The thumbnail is the group laid out on a sample wall, drawn by the same solver as the viewports.
 */
export class TexGroupsPanel implements IContentRenderer {
  readonly element = h('div', { class: 'panel cb' })
  private all: string[] = []
  private query = ''
  private tab: 'simple' | 'advanced' = 'simple'
  private bar = h('div', { class: 'cb-bar' })
  private list = h('div', { class: 'gl-list' })
  private count = h('span', { class: 'dim' })
  private unsubs: (() => void)[] = []
  private canvases = new Map<string, HTMLCanvasElement>()
  private timer: ReturnType<typeof setTimeout> | null = null
  private wasReady = false
  private lastProfile: unknown = null

  constructor(private store: Store, private textures: TextureCache, private groups: TexGroupCache, private env: GroupsPanelEnv) {}

  init(): void {
    const search = h('input', { placeholder: 'Search texture groups', class: 'search' })
    search.addEventListener('input', () => { this.query = search.value.toLowerCase(); this.render() })
    const refresh = h('button', { title: 'Rescan the texture group folder' }, '↻'); refresh.addEventListener('click', () => void this.reload())
    const tabs = h('div', { class: 'gl-tabs' })
    const tabBtn = (id: 'simple' | 'advanced', label: string, title: string) => { const b = h('button', { title, class: this.tab === id ? 'active' : '' }, label); b.addEventListener('click', () => { this.tab = id; tabs.querySelectorAll('button').forEach((x) => x.classList.toggle('active', x === b)); this.buttons(); this.render() }); return b }
    tabs.append(tabBtn('simple', 'Simple', 'Texture sets: wall pieces in rows, 9-slice frames'), tabBtn('advanced', 'Advanced', 'Rule groups: splits, repeats, conditions, parameters'))
    this.element.append(tabs, this.bar, this.list)
    this.bar.append(search, h('span'), refresh, this.count)
    this.buttons(search, refresh)
    const redraw = () => this.canvases.forEach((c, p) => this.draw(c, p))
    this.unsubs.push(this.groups.onChange(() => this.render()), this.textures.onChange(redraw), this.store.subscribe(() => {
      const ready = this.store.state.synced
      if (ready && !this.wasReady) void this.reload()
      this.wasReady = ready
      const profile = this.store.state.profile
      if (profile !== this.lastProfile) { if (this.lastProfile !== null && ready) void this.reload(); this.lastProfile = profile }
    }))
    if (this.store.state.synced) void this.reload()
  }
  /** The "New" buttons of the current tab. */
  private buttons(search?: HTMLElement, refresh?: HTMLElement): void {
    const s = search ?? (this.bar.querySelector('input') as HTMLElement), r = refresh ?? (this.bar.querySelector('button[title^="Rescan"]') as HTMLElement)
    const mk = (label: string, title: string, kind: 'wall' | 'frame' | 'advanced') => { const b = h('button', { title }, label); b.addEventListener('click', () => this.env.edit(null, kind)); return b }
    this.bar.replaceChildren(s, ...(this.tab === 'simple' ? [mk('New wall set...', 'Pieces of a wall in rows: window, door, plain, rooftop', 'wall'), mk('New 9-slice frame...', 'Corners, edges and middle from nine textures; edges and middle repeat', 'frame')] : [mk('New advanced group...', 'Start a rule group from a template', 'advanced')]), r, this.count)
  }

  /** Which tab a group belongs to, or null while it loads. */
  private tabOf(path: string): 'simple' | 'advanced' | null {
    const st = this.groups.state(path)
    if (st.status === 'loading') return null
    const raw = st.status === 'ready' ? st.raw : null
    return raw && typeof raw === 'object' && (raw as { kit?: unknown }).kit !== undefined ? 'simple' : 'advanced'
  }

  dispose(): void { this.unsubs.forEach((u) => u()); if (this.timer) clearTimeout(this.timer) }

  async reload(): Promise<void> {
    try { this.all = await this.env.list() } catch { this.all = []; this.store.setMessage('Could not list texture groups (is Core connected?)') }
    this.render()
  }

  private draw(c: HTMLCanvasElement, path: string): void {
    const g = this.groups.group(path)
    if (!g) { const x = c.getContext('2d'); if (x) { x.fillStyle = '#232323'; x.fillRect(0, 0, c.width, c.height) } return }
    drawGroupPreview(c, g, { w: 8, h: 6, facing: 'wall', seed: 1 }, this.textures, { outlines: false, margin: 2 })
  }

  private apply(path: string): void {
    if (!this.store.canEdit) return
    const g = this.groups.group(path)
    const n = applyTexGroup(this.store, path, g?.fallback ? g.slots[g.fallback] : undefined, { wallsOnly: g?.kit?.kind === 'wall' })
    this.store.setMessage(n ? `Texture group applied to ${n} face${n === 1 ? '' : 's'}${g?.kit?.kind === 'wall' ? ' (roof and floor faces of a whole brush are left alone; pick a face to put the set on it)' : ''}` : 'Select a brush or a face to apply the group to')
  }

  private render(): void {
    const mine = this.all.filter((p) => this.tabOf(p) === this.tab)
    const shown = mine.filter((p) => p.toLowerCase().includes(this.query))
    this.count.textContent = `${shown.length} of ${mine.length}`
    this.canvases.clear()
    if (mine.length === 0) { this.list.replaceChildren(h('div', { class: 'dim pad' }, this.tab === 'simple' ? 'No texture sets yet. "New wall set" builds a wall from window, door, plain and rooftop pieces; "New 9-slice frame" builds a frame whose edges repeat.' : 'No advanced groups. They are rule sets (splits, repeats, conditions): "New advanced group" starts from a template.')); return }
    this.list.replaceChildren(...shown.map((path) => {
      const canvas = h('canvas', { width: '96', height: '72' })
      this.canvases.set(path, canvas)
      const g = this.groups.group(path)
      const desc = g?.description ? g.description : ''
      const row = h('div', { class: 'gl-row', title: `${path}\nClick: apply to the selection. Double-click: edit. Right-click: more.` }, canvas, h('div', {}, h('div', { class: 'gl-name' }, g?.name || path), h('div', { class: 'dim' }, path), desc ? h('div', { class: 'gl-desc' }, desc) : h('span')))
      row.addEventListener('click', () => { if (this.timer) clearTimeout(this.timer); this.timer = setTimeout(() => this.apply(path), APPLY_DELAY_MS) })
      row.addEventListener('dblclick', () => { if (this.timer) clearTimeout(this.timer); this.env.edit(path) })
      row.addEventListener('contextmenu', (e) => {
        e.preventDefault()
        showMenu(e.clientX, e.clientY, [
          { label: 'Apply to selection', run: () => this.apply(path), disabled: !this.store.canEdit },
          { label: 'Edit...', run: () => this.env.edit(path) },
          { label: 'Duplicate', run: () => void this.env.duplicate(path, `${path}_copy`).then(() => this.reload()).catch((err: Error) => this.store.setMessage(`Duplicate failed: ${err.message}`)) },
          { sep: true, label: '' },
          { label: 'Delete...', run: () => void confirmDialog('Delete texture group', `Delete '${path}'? Faces that use it are drawn plainly afterwards.`, 'Delete').then((ok) => ok ? this.env.remove(path).then(() => this.reload()).catch((err: Error) => this.store.setMessage(`Delete failed: ${err.message}`)) : undefined) },
        ], this.element.ownerDocument)
      })
      this.draw(canvas, path)
      return row
    }))
  }
}
