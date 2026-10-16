// Preview icons for the Place tab. Entities get a small isometric box of their real proportions in the
// colour from their definition (2D canvas, instant). Prefabs are drawn with the editor's own renderer from
// their real content (WebGL, one shared offscreen canvas) and cached as data URLs.
import type { Entity } from '../model/document'
import { brushBounds } from '../model/geometry'
import type { Box } from '../model/entityBox'
import { boundsOfBox, pointBox } from '../model/pointBox'
import { classOf, isBrushEntity } from '../model/document'
import type { Vec3 } from '../model/math'
import type { PrefabPreviews } from '../state/prefabPreview'
import type { TextureCache } from '../state/textures'
import { OrbitCamera } from './camera'
import { Renderer } from './renderer'
import { defaultViewportSettings } from './viewportSettings'

export const ICON_SIZE = 64
export type Rgb = [number, number, number]

// ---- isometric box (pure) ----------------------------------------------------------------------------
/** Viewed from +x +y +z: sx = (x - z) cos30, sy = (x + z) / 2 - y (screen y grows downwards). */
const iso = (p: Vec3): [number, number] => [(p[0] - p[2]) * 0.8660254, (p[0] + p[2]) * 0.5 - p[1]]

export interface IsoBox { top: [number, number][]; left: [number, number][]; right: [number, number][]; outline: [number, number][] }

/** The three visible faces of a box of `size` metres, scaled to fit a `px` square with `margin` px around it, centred. */
export function isoBox(size: Vec3, px: number, margin = 6): IsoBox {
  const [w, h, d] = size, o: Vec3 = [0, 0, 0]
  const c = (x: number, y: number, z: number): [number, number] => iso([x * w + o[0], y * h + o[1], z * d + o[2]])
  const faces = {
    top: [c(0, 1, 0), c(1, 1, 0), c(1, 1, 1), c(0, 1, 1)],
    right: [c(1, 0, 0), c(1, 0, 1), c(1, 1, 1), c(1, 1, 0)], // the +x face
    left: [c(0, 0, 1), c(1, 0, 1), c(1, 1, 1), c(0, 1, 1)], // the +z face
  }
  const all = [...faces.top, ...faces.right, ...faces.left, c(0, 0, 0), c(1, 0, 0), c(0, 0, 1), c(1, 0, 1), c(0, 0, 0)]
  const xs = all.map((p) => p[0]), ys = all.map((p) => p[1])
  const minX = Math.min(...xs), maxX = Math.max(...xs), minY = Math.min(...ys), maxY = Math.max(...ys)
  const k = (px - 2 * margin) / Math.max(1e-9, maxX - minX, maxY - minY)
  const ox = (px - (maxX - minX) * k) / 2 - minX * k, oy = (px - (maxY - minY) * k) / 2 - minY * k
  const f = (pts: [number, number][]): [number, number][] => pts.map(([x, y]) => [x * k + ox, y * k + oy])
  return { top: f(faces.top), left: f(faces.left), right: f(faces.right), outline: f([c(0, 1, 0), c(1, 1, 0), c(1, 0, 0), c(1, 0, 1), c(0, 0, 1), c(0, 1, 1)]) }
}

/** Near-black class colours would vanish on the dark panel; same lift the viewport markers use. */
export const iconColor = (c: Rgb | undefined): Rgb => (!c ? [1, 0.85, 0.25] : Math.max(...c) < 0.35 ? [0.6, 0.6, 0.6] : c)

const css = (c: Rgb, k: number): string => `rgb(${c.map((v) => Math.round(Math.min(1, v * k) * 255)).join(',')})`

const entityIcons = new Map<string, string>()
/** Data URL of a small isometric picture of the class's box ('' where canvas is unavailable). Cached per class. */
export function entityIcon(key: string, box: Box, color: Rgb | undefined, px = ICON_SIZE): string {
  key = `${key}|${box.min}|${box.max}|${color ?? ''}|${px}` // a changed .ent (box or colour) must not reuse the old picture
  const hit = entityIcons.get(key)
  if (hit !== undefined) return hit
  let url = ''
  try {
    const c = document.createElement('canvas'); c.width = c.height = px
    const g = c.getContext('2d')
    if (g) {
      const b = isoBox([box.max[0] - box.min[0], box.max[1] - box.min[1], box.max[2] - box.min[2]], px)
      const col = iconColor(color)
      const poly = (pts: [number, number][], fill: string) => { g.beginPath(); pts.forEach(([x, y], i) => (i ? g.lineTo(x, y) : g.moveTo(x, y))); g.closePath(); g.fillStyle = fill; g.fill() }
      poly(b.top, css(col, 1)); poly(b.left, css(col, 0.62)); poly(b.right, css(col, 0.42))
      g.lineJoin = 'round'; g.lineWidth = 1.5; g.strokeStyle = 'rgba(255,255,255,.55)'
      for (const f of [b.top, b.left, b.right]) { g.beginPath(); f.forEach(([x, y], i) => (i ? g.lineTo(x, y) : g.moveTo(x, y))); g.closePath(); g.stroke() }
      url = c.toDataURL('image/png')
    }
  } catch { url = '' }
  entityIcons.set(key, url)
  return url
}
export const clearEntityIcons = (): void => entityIcons.clear()

// ---- prefab icons ----------------------------------------------------------------------------------
/** Bounds of what a set of entities shows: brushes plus the boxes of point entities. */
export function contentBounds(entities: Entity[], entityBox: (c: string) => Box): { min: Vec3; max: Vec3 } | null {
  const min: Vec3 = [Infinity, Infinity, Infinity], max: Vec3 = [-Infinity, -Infinity, -Infinity]
  let any = false
  const grow = (lo: Vec3, hi: Vec3) => { any = true; for (let i = 0; i < 3; i++) { min[i] = Math.min(min[i], lo[i]); max[i] = Math.max(max[i], hi[i]) } }
  for (const e of entities) {
    for (const b of e.brushes ?? []) { if (b.op === 'subtract') continue; const bb = brushBounds(b); if (bb) grow(bb.min, bb.max) }
    if (!isBrushEntity(e) && classOf(e) !== 'worldspawn') { const bb = boundsOfBox(pointBox(e, entityBox)); grow(bb.min, bb.max) }
  }
  return any ? { min, max } : null
}

export interface IconEnv {
  textures: TextureCache
  previews: PrefabPreviews
  entityColor: (c: string) => Rgb | undefined
  entityBox: (c: string) => Box
}

/** Renders prefab content into small PNG data URLs with the real renderer. */
export class PrefabIcons {
  private urls = new Map<string, string>()
  private failed = new Set<string>()
  private wanted = new Set<string>()
  /** Everyone who asked for a file's icon hears about every (re)made picture of it. */
  private subs = new Map<string, Set<(url: string | null) => void>>()
  private renderer: Renderer | null = null
  private canvas: HTMLCanvasElement | null = null
  private retries = new Map<string, number>()
  private off: (() => void) | null = null

  constructor(private env: IconEnv, private px = 96) {}

  /** The icon if it exists now; otherwise starts making it. `cb` is called whenever a picture is (re)made, or with null when this prefab cannot be drawn. */
  get(file: string, cb: (url: string | null) => void): string | null {
    let set = this.subs.get(file)
    if (!set) this.subs.set(file, (set = new Set()))
    set.add(cb)
    const have = this.urls.get(file)
    if (have) return have
    if (this.failed.has(file)) { queueMicrotask(() => cb(null)); return null }
    this.wanted.add(file)
    this.off ??= this.env.previews.onChange(() => this.pump())
    this.pump()
    return null
  }

  /** Stops telling `cb` about `file` (its row was redrawn). */
  forget(file: string, cb: (url: string | null) => void): void { this.subs.get(file)?.delete(cb) }

  /** Forget every picture (files changed on disk, or another assets folder). Subscribers stay and are asked again by their next `get`. */
  clear(): void { this.urls.clear(); this.failed.clear(); this.retries.clear(); this.wanted.clear() }
  dispose(): void { this.off?.(); this.renderer?.dispose() }

  private notify(file: string, url: string | null): void { this.subs.get(file)?.forEach((f) => f(url)) }

  private pump(): void {
    for (const file of [...this.wanted]) {
      const content = this.env.previews.entities(file)
      if (!content) {
        if (this.env.previews.error(file)) { this.failed.add(file); this.wanted.delete(file); this.notify(file, null) }
        continue
      }
      this.wanted.delete(file)
      const url = this.render(file, content)
      if (url === null) { this.failed.add(file); this.notify(file, null); continue }
      this.urls.set(file, url)
      this.notify(file, url)
      this.refreshWhenTexturesArrive(file, content)
    }
  }

  private render(_file: string, entities: Entity[]): string | null {
    const bounds = contentBounds(entities, this.env.entityBox)
    if (!bounds) return null
    try {
      if (!this.renderer) {
        this.canvas = document.createElement('canvas')
        this.renderer = new Renderer(this.canvas, this.env.textures)
      }
      const cam = new OrbitCamera()
      cam.yaw = 0.75; cam.pitch = 0.5
      cam.frame(bounds.min, bounds.max)
      const settings = { ...defaultViewportSettings(), showGrid: false, showOrigins: false, showOutlines: true, showSubtractive: false }
      this.renderer.draw({
        camera: cam, settings, selection: null, gizmo: null, preview: null, highlight: null,
        entities: entities.map((entity) => ({ entity, readonly: true })),
        entityColor: this.env.entityColor, entityBox: this.env.entityBox,
        pixelSize: [this.px, this.px], background: [0.13, 0.145, 0.175, 1],
      })
      return this.canvas!.toDataURL('image/png') // read in the same task as the draw, while the buffer is still there
    } catch { return null }
  }

  /** Textures arrive after the first draw; redo the picture once they are in (a few times at most per prefab). */
  private refreshWhenTexturesArrive(file: string, entities: Entity[]): void {
    const loading = entities.some((e) => (e.brushes ?? []).some((b) => b.faces.some((f) => this.env.textures.get(f.texture ?? 'dev/grey').status === 'loading')))
    const n = this.retries.get(file) ?? 0
    if (!loading || n >= 4) return
    this.retries.set(file, n + 1)
    const stop = this.env.textures.onChange(() => {
      stop()
      window.setTimeout(() => { if (!this.urls.has(file)) return; this.urls.delete(file); this.wanted.add(file); this.pump() }, 150)
    })
  }
}
