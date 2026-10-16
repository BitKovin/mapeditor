import type { IContentRenderer } from 'dockview-core'
import { applyTexture } from '../state/actions'
import type { Store } from '../state/store'
import type { TextureCache } from '../state/textures'
import { showMenu } from './contextMenu'
import { h } from './dom'

const THUMB = 84
/** A single click applies the texture after this delay unless a second click (double-click = properties) arrives. */
export const APPLY_DELAY_MS = 250

/** Texture browser (UE's Content Browser): folder list + thumbnails. Click = choose, double-click = apply. */
export class ContentBrowserPanel implements IContentRenderer {
  readonly element = h('div', { class: 'panel cb' })
  private all: string[] = []
  private folder = ''
  private query = ''
  private tiles = new Map<string, HTMLCanvasElement>()
  private unsubs: (() => void)[] = []
  private grid = h('div', { class: 'thumbs' })
  private tree = h('div', { class: 'folders' })
  private count = h('span', { class: 'dim' })
  private observer: IntersectionObserver | null = null
  private wasReady = false
  private lastProfile: unknown = null

  /** `openProperties` = open the Texture Properties window (double-click or right-click > Properties). */
  constructor(private store: Store, private textures: TextureCache, private list: () => Promise<string[]>, private openProperties: (path: string) => void = () => {}, private openRelief: (path: string) => void = () => {}) {}
  private applyTimer: ReturnType<typeof setTimeout> | null = null

  init(): void {
    const search = h('input', { placeholder: 'Search textures', class: 'search' })
    search.addEventListener('input', () => { this.query = search.value.toLowerCase(); this.renderGrid() })
    const refresh = h('button', { title: 'Rescan the texture folder' }, '↻')
    refresh.addEventListener('click', () => void this.reload())
    this.element.append(
      h('div', { class: 'cb-bar' }, search, refresh, this.count),
      h('div', { class: 'cb-body' }, this.tree, this.grid))
    this.observer = new IntersectionObserver((entries) => {
      for (const e of entries) if (e.isIntersecting) { this.draw(e.target as HTMLCanvasElement); this.observer!.unobserve(e.target) }
    }, { root: this.grid, rootMargin: '100px' })
    this.unsubs.push(this.textures.onChange(() => this.tiles.forEach((c) => c.dataset.drawn !== '1' && this.draw(c))),
      this.store.subscribe(() => {
        this.markActive()
        // (re)list whenever Core becomes available -- the panel is created before the connection is up
        const ready = this.store.state.synced
        if (ready && !this.wasReady) void this.reload()
        this.wasReady = ready
        // the game profile changed (another assets folder, or game_profile.json edited): the texture list is stale
        const profile = this.store.state.profile
        if (profile !== this.lastProfile) { if (this.lastProfile !== null && ready) void this.reload(); this.lastProfile = profile }
      }))
    if (this.store.state.synced) void this.reload()
  }
  dispose(): void { this.unsubs.forEach((u) => u()); this.observer?.disconnect() }

  async reload(): Promise<void> {
    try { this.all = await this.list() } catch { this.all = []; this.store.setMessage('Could not list textures (is Core connected?)') }
    this.renderFolders(); this.renderGrid()
  }

  private renderFolders(): void {
    const folders = [...new Set(this.all.map((p) => p.split('/').slice(0, -1).join('/')))].sort()
    this.tree.replaceChildren(...['', ...folders.filter(Boolean)].map((f) => {
      const row = h('div', { class: `folder${f === this.folder ? ' sel' : ''}` }, f === '' ? 'All textures' : f)
      row.addEventListener('click', () => { this.folder = f; this.renderFolders(); this.renderGrid() })
      return row
    }))
  }

  private renderGrid(): void {
    const shown = this.all.filter((p) => (this.folder === '' || p.startsWith(this.folder + '/')) && p.toLowerCase().includes(this.query))
    this.count.textContent = `${shown.length} of ${this.all.length}`
    this.tiles.clear()
    this.grid.replaceChildren(...shown.map((path) => {
      const canvas = h('canvas', { width: String(THUMB), height: String(THUMB) })
      canvas.dataset.path = path
      this.tiles.set(path, canvas)
      const tile = h('div', { class: 'tile', title: path }, canvas, h('div', { class: 'tile-name' }, path.split('/').pop() ?? path))
      tile.dataset.path = path
      // clicking a texture applies it at once to whatever is selected (brushes, faces, decals); no Apply step
      // a double-click opens the properties window, so the apply waits a moment and is cancelled by the second click
      tile.addEventListener('click', () => {
        if (this.applyTimer) clearTimeout(this.applyTimer)
        this.store.setActiveTexture(path)
        this.applyTimer = setTimeout(() => { this.applyTimer = null; applyTexture(this.store, path) }, APPLY_DELAY_MS)
      })
      tile.addEventListener('dblclick', () => { if (this.applyTimer) { clearTimeout(this.applyTimer); this.applyTimer = null } this.openProperties(path) })
      tile.addEventListener('contextmenu', (e) => {
        e.preventDefault()
        this.store.setActiveTexture(path)
        showMenu(e.clientX, e.clientY, [
          { label: 'Properties...', run: () => this.openProperties(path) },
          { label: 'Relief...', run: () => this.openRelief(path) },
          { label: 'Apply to selection', run: () => applyTexture(this.store, path) },
        ], tile.ownerDocument)
      })
      this.observer?.observe(canvas)
      return tile
    }))
    this.markActive()
  }

  private markActive(): void {
    const active = this.store.state.activeTexture
    for (const t of this.grid.children) (t as HTMLElement).classList.toggle('sel', (t as HTMLElement).dataset.path === active)
  }

  private draw(c: HTMLCanvasElement): void {
    const st = this.textures.get(c.dataset.path!)
    const g = c.getContext('2d')!
    g.fillStyle = '#2a2a2a'; g.fillRect(0, 0, THUMB, THUMB)
    if (st.status === 'ready') { g.drawImage(st.image.source as CanvasImageSource, 0, 0, THUMB, THUMB); c.dataset.drawn = '1' }
    else if (st.status === 'missing') { g.fillStyle = '#c55'; g.fillText('?', 38, 46); c.dataset.drawn = '1' }
  }
}
