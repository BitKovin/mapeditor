// Texture thumbnails for scrolling grids (Content Browser, texture pickers). Only tiles on screen are loaded, as thumbnails (after the
// textures the map needs, without making the viewports rebuild); a tile scrolled away before its turn is dropped from the queue.
// Each thumbnail keeps the texture's aspect ratio and is drawn once into a small cached canvas, so re-filtering a grid costs nothing.
import type { TextureCache } from '../state/textures'
import { drawFitted } from './fitImage'
import { h } from './dom'
import { isCubemapPath, isSkyTexture } from '../model/sky'

const cache = new WeakMap<TextureCache, Map<string, HTMLCanvasElement>>()
const thumbsOf = (t: TextureCache) => { let m = cache.get(t); if (!m) { m = new Map(); cache.set(t, m) } return m }

/** The SKY badge of a tile (REQUIREMENTS §28 K7): skyboxes, and textures the export treats as sky; null for the others. */
export function skyBadge(path: string): HTMLElement | null {
  if (!isSkyTexture(path)) return null
  return h('span', { class: 'tile-badge', title: isCubemapPath(path) ? 'Skybox: drawn by direction (env/ cubemap), no lightmap, blocks no light' : 'Sky texture: no lightmap, blocks no light; the engine draws it flat (a skybox is env/<name>_cube)' }, 'SKY')
}

/** Forget the drawn thumbnails of `textures` (e.g. the texture list was rescanned and files may have changed). */
export function forgetThumbnails(textures: TextureCache): void { thumbsOf(textures).clear() }

export class ThumbnailLoader {
  private observer: IntersectionObserver | null = null
  private visible = new Set<HTMLCanvasElement>()   // observed and on screen, not drawn yet
  private observed = new Set<HTMLCanvasElement>()  // waiting to scroll into view
  private unsub: () => void

  /** `root` = the scrolling element the tiles are in. `described` is told the texture's size once it is known (for a tooltip). */
  constructor(private textures: TextureCache, root: HTMLElement, private size = 84, private described?: (canvas: HTMLCanvasElement, path: string, w: number, h: number) => void) {
    if (typeof IntersectionObserver !== 'undefined')
      this.observer = new IntersectionObserver((entries) => {
        for (const e of entries) {
          const c = e.target as HTMLCanvasElement
          if (e.isIntersecting) { this.visible.add(c); this.show(c) }
          else { this.visible.delete(c); if (c.dataset.drawn !== '1') this.textures.cancel(c.dataset.path!) }
        }
      }, { root, rootMargin: '200px' })
    this.unsub = textures.onLoad(() => { for (const c of this.visible) if (c.isConnected) this.paint(c); else this.visible.delete(c) })
  }

  /** A canvas for `path`; it is filled in when it scrolls into view. */
  canvas(path: string): HTMLCanvasElement {
    const c = document.createElement('canvas')
    c.width = c.height = this.size
    c.dataset.path = path
    const done = thumbsOf(this.textures).get(path)
    if (done) { c.getContext('2d')?.drawImage(done, 0, 0); c.dataset.drawn = '1'; queueMicrotask(() => this.describe(c, path)) } // once it is in its tile
    else if (this.observer) { this.observer.observe(c); this.observed.add(c) }
    else { this.visible.add(c); this.show(c) } // no IntersectionObserver (tests): everything counts as visible
    return c
  }

  /** The grid is being rebuilt: tiles made so far are gone, and those still waiting for their texture are not loaded any more. */
  reset(): void {
    for (const c of this.observed) { this.observer?.unobserve(c); if (c.dataset.drawn !== '1') this.textures.cancel(c.dataset.path!) }
    for (const c of this.visible) if (c.dataset.drawn !== '1') this.textures.cancel(c.dataset.path!)
    this.observed.clear(); this.visible.clear()
  }

  dispose(): void { this.reset(); this.observer?.disconnect(); this.unsub() }

  private show(c: HTMLCanvasElement): void {
    if (c.dataset.drawn === '1') return
    this.textures.request(c.dataset.path!)
    this.paint(c)
  }

  private paint(c: HTMLCanvasElement): void {
    if (c.dataset.drawn === '1') return
    const path = c.dataset.path!, st = this.textures.peek(path), g = c.getContext('2d')
    if (!g || !st || st.status === 'loading') { if (g && c.dataset.blank !== '1') { g.fillStyle = '#2a2a2a'; g.fillRect(0, 0, this.size, this.size); c.dataset.blank = '1' } return }
    if (st.status === 'missing') { g.fillStyle = '#2a2a2a'; g.fillRect(0, 0, this.size, this.size); g.fillStyle = '#c55'; g.font = '16px sans-serif'; g.textAlign = 'center'; g.fillText('?', this.size / 2, this.size / 2 + 6) }
    else {
      drawFitted(g, st.image.source as CanvasImageSource, st.image.width, st.image.height, this.size, this.size)
      const keep = document.createElement('canvas')
      keep.width = keep.height = this.size
      keep.getContext('2d')?.drawImage(c, 0, 0)
      thumbsOf(this.textures).set(path, keep)
      this.describe(c, path)
    }
    c.dataset.drawn = '1'
    this.observer?.unobserve(c)
    this.observed.delete(c)
    this.visible.delete(c)
  }

  private describe(c: HTMLCanvasElement, path: string): void {
    const st = this.textures.peek(path)
    if (st?.status === 'ready') this.described?.(c, path, st.image.width, st.image.height)
  }
}
