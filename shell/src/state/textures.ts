// Texture cache. Paths are Quake-style (no extension); Core finds the file via the game
// profile's extension order and sends the bytes. Decoded once, shared by every viewport.
import { EdgeSamples, edgesWrap } from '../model/seamless'
import { decodeTga } from '../model/tga'
import { AlphaKind, DEFAULT_PPM, TexInfo, TextureProps, defaultProps, parseProps, texEnv } from '../model/texprops'

export interface TexImage { width: number; height: number; source: ImageBitmap | HTMLCanvasElement | ImageData; /** per axis (u, v): do the opposite edges continue each other? Unknown = undefined. */ seamless?: [boolean, boolean]; /** what the alpha channel holds (the Core's classifyAlpha, REQUIREMENTS §23): none, on / off holes, or half transparency (glass) */ alpha?: AlphaKind }
/** A skybox's images (REQUIREMENTS §28): the six faces in the engine's order (model/sky.ts SKY_FACES), or one panorama. */
export interface SkyImages { panorama: boolean; faces: TexImage[] }
/** `image` of a skybox is its preview (the faces as a cross, or the panorama); `sky` holds what the views sample by direction. */
export type TexState = { status: 'loading' } | { status: 'missing'; error: string } | { status: 'ready'; image: TexImage; props: TextureProps; sky?: SkyImages }
/** `props` = the contents of `<texture file>.json` as Core read it (absent = no such file). `sky` = the files of a skybox (then `bytes` is empty). */
export type FetchTexture = (path: string) => Promise<{ ext: string; bytes: Uint8Array; props?: unknown; sky?: { panorama: boolean; images: { ext: string; bytes: Uint8Array }[] } }>
export type DecodeTexture = (ext: string, bytes: Uint8Array) => Promise<TexImage>

export const base64ToBytes = (b64: string): Uint8Array => Uint8Array.from(atob(b64), (c) => c.charCodeAt(0))

/** A w x h block of RGBA bytes cut out of an image (null when no canvas is available, e.g. in tests). */
function block(src: CanvasImageSource, x: number, y: number, w: number, h: number): Uint8ClampedArray | null {
  try {
    const c = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(w, h) : typeof document !== 'undefined' ? Object.assign(document.createElement('canvas'), { width: w, height: h }) : null
    const g = c?.getContext('2d') as CanvasRenderingContext2D | null
    if (!g) return null
    g.drawImage(src, x, y, w, h, 0, 0, w, h)
    return g.getImageData(0, 0, w, h).data
  } catch { return null }
}

/** Edge analysis (model/seamless.ts) from a handful of one-pixel-wide strips, so a 4k texture costs a few kilobytes, not a full read-back. */
export function analyseSeams(src: CanvasImageSource, width: number, height: number): [boolean, boolean] | undefined {
  const axis = (len: number, line: (i: number) => Uint8ClampedArray | null): boolean | undefined => {
    if (len < 8) return true
    const q = [Math.floor(len / 4), Math.floor(len / 2), Math.floor((3 * len) / 4)]
    const first = line(0), last = line(len - 1)
    const interior: EdgeSamples['interior'] = []
    for (const i of q) { const a = line(i), b = line(i + 1); if (!a || !b) return undefined; interior.push([a, b]) }
    if (!first || !last) return undefined
    return edgesWrap({ first, last, interior })
  }
  const u = axis(width, (x) => block(src, x, 0, 1, height)), v = axis(height, (y) => block(src, 0, y, width, 1))
  return u === undefined || v === undefined ? undefined : [u, v]
}

/** Some texel with alpha below 128 (RGBA bytes). */
/** The Core's classifyAlpha (core/albedo.cpp): clear < 16, solid >= 240, glass when at least a quarter of the non-solid texels are in between. */
export function classifyAlpha(rgba: ArrayLike<number>): AlphaKind {
  let clear = 0, semi = 0
  for (let i = 3; i < rgba.length; i += 4) { const a = rgba[i]; if (a < 16) clear++; else if (a < 240) semi++ }
  if (clear + semi === 0) return 'none'
  return semi * 4 >= clear + semi ? 'translucent' : 'cutout'
}

/**
 * The texels of an image, read back at most `max` x `max` with nearest sampling: every value is a real texel (no blended edges), so
 * classifyAlpha's proportions hold, while a 4k texture costs 1 MB instead of 64 MB of read-back on the main thread.
 */
function sampleTexels(src: CanvasImageSource, width: number, height: number, max = 512): Uint8ClampedArray | null {
  const k = Math.min(1, max / Math.max(width, height)), w = Math.max(1, Math.round(width * k)), h = Math.max(1, Math.round(height * k))
  if (k === 1) return block(src, 0, 0, width, height)
  try {
    const c = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(w, h) : typeof document !== 'undefined' ? Object.assign(document.createElement('canvas'), { width: w, height: h }) : null
    const g = c?.getContext('2d') as CanvasRenderingContext2D | null
    if (!g) return null
    g.imageSmoothingEnabled = false
    g.drawImage(src, 0, 0, width, height, 0, 0, w, h)
    return g.getImageData(0, 0, w, h).data
  } catch { return null }
}

export const browserDecode: DecodeTexture = async (ext, bytes) => {
  if (ext === '.tga') {
    const t = decodeTga(bytes)
    const bmp = await createImageBitmap(new ImageData(t.data as Uint8ClampedArray<ArrayBuffer>, t.width, t.height), { premultiplyAlpha: 'none' })
    return { width: t.width, height: t.height, source: bmp, seamless: analyseSeams(bmp, t.width, t.height), alpha: classifyAlpha(t.data) }
  }
  const type = ext === '.png' ? 'image/png' : 'image/jpeg'
  const bmp = await createImageBitmap(new Blob([bytes as BlobPart], { type }), { premultiplyAlpha: 'none' })
  // only a PNG can carry alpha (a JPEG never has any)
  const rgba = ext === '.png' ? sampleTexels(bmp, bmp.width, bmp.height) : null
  return { width: bmp.width, height: bmp.height, source: bmp, seamless: analyseSeams(bmp, bmp.width, bmp.height), alpha: rgba ? classifyAlpha(rgba) : 'none' }
}

/**
 * The browser's preview of a skybox: a panorama as it is, six faces as a cross (west, north, east, south in a row, up above north, down below it), each
 * turned so that neighbouring edges meet. Without a canvas (tests) the first image.
 */
export function skyPreview(sky: SkyImages): TexImage {
  if (sky.panorama || sky.faces.length !== 6) return sky.faces[0]
  try {
    const s = Math.max(1, Math.min(128, sky.faces[1].width))
    const c = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(s * 4, s * 3) : typeof document !== 'undefined' ? Object.assign(document.createElement('canvas'), { width: s * 4, height: s * 3 }) : null
    const g = c?.getContext('2d') as CanvasRenderingContext2D | null
    if (!c || !g) return sky.faces[0]
    // [face, column, row, quarter turns clockwise]: _up has west at its top (north at its left), _dn east at its top (north at its left)
    const place: [number, number, number, number][] = [[0, 0, 1, 0], [5, 1, 1, 0], [1, 2, 1, 0], [4, 3, 1, 0], [2, 1, 0, -1], [3, 1, 2, 1]]
    for (const [f, col, row, turn] of place) {
      g.save()
      g.translate(col * s + s / 2, row * s + s / 2)
      g.rotate((turn * Math.PI) / 2)
      g.drawImage(sky.faces[f].source as CanvasImageSource, -s / 2, -s / 2, s, s)
      g.restore()
    }
    return { width: s * 4, height: s * 3, source: c as unknown as HTMLCanvasElement, alpha: 'none' }
  } catch { return sky.faces[0] }
}

/** Textures read from Core at the same time; the rest wait in line (map textures before browser thumbnails). */
export const MAX_PARALLEL_LOADS = 4

export class TextureCache {
  private entries = new Map<string, TexState>()
  private listeners = new Set<() => void>()
  /** The game profile's pixels per meter (a texture without its own value uses it). */
  private defaultPpm = DEFAULT_PPM
  /** `schedule` runs the batched change notification (see scheduleFlush); the editor passes one that waits for the next frame. */
  constructor(private fetchTexture: FetchTexture, private decode: DecodeTexture = browserDecode, private schedule: (run: () => void) => void = queueMicrotask) {
    // the pure UV functions (model/uvmesh.ts, heat map) look textures up here
    texEnv.setProvider((path) => this.info(path))
  }

  setDefaultPpm(ppm: number): void {
    const v = Number.isFinite(ppm) && ppm > 0 ? ppm : DEFAULT_PPM
    if (v === this.defaultPpm) return
    this.defaultPpm = v
    texEnv.bump(); this.listeners.forEach((f) => f())
  }
  get profilePpm(): number { return this.defaultPpm }

  /** Size, effective pixels per meter and properties of a texture; undefined while it is loading or missing (the lookup starts the load). */
  info(path: string): TexInfo | undefined {
    const st = this.get(path)
    if (st.status !== 'ready') return undefined
    return { path, width: st.image.width, height: st.image.height, ppm: st.props.pixelsPerMeter ?? this.defaultPpm, props: st.props, seamless: st.image.seamless, alpha: st.image.alpha }
  }

  /** The images of a loaded skybox (undefined while loading, when missing, or for an ordinary texture). */
  sky(path: string): SkyImages | undefined {
    const st = this.get(path)
    return st.status === 'ready' ? st.sky : undefined
  }

  /** The properties as stored (defaults for a texture that is not loaded). */
  propsOf(path: string): TextureProps {
    const st = this.entries.get(path)
    return st?.status === 'ready' ? st.props : defaultProps()
  }

  /** A property edit (from the properties window): kept in memory at once, every viewport redraws; the caller writes it to Core. */
  setProps(path: string, props: TextureProps): void {
    const st = this.entries.get(path)
    if (st?.status !== 'ready') return
    this.entries.set(path, { ...st, props })
    texEnv.bump()
    this.listeners.forEach((f) => f())
  }

  /** Something texture-dependent outside the cache changed (a texture group): rebuild every cached mesh and redraw. */
  notify(): void { texEnv.bump(); this.listeners.forEach((f) => f()) }

  /** Called (at most once per frame) when a texture the map uses arrived or changed: meshes and previews of the world redraw. */
  onChange(fn: () => void): () => void { this.listeners.add(fn); return () => void this.listeners.delete(fn) }
  /** Called (at most once per frame) when any texture finished loading, also one only a thumbnail asked for (see request). */
  onLoad(fn: () => void): () => void { this.loadListeners.add(fn); return () => void this.loadListeners.delete(fn) }

  /** Current state; starts loading on first request, ahead of thumbnails. A texture asked for here is one the map uses. */
  get(path: string): TexState {
    const e = this.entries.get(path)
    this.wanted.add(path)
    if (!e) return this.enqueue(path, 'high')
    if (this.low.delete(path)) this.high.push(path) // waiting as a thumbnail: the map needs it now
    return e
  }

  /** The state without starting a load (undefined = never asked for). */
  peek(path: string): TexState | undefined { return this.entries.get(path) }

  /** For a thumbnail: loads after everything the map needs, and does not make the viewports rebuild when it arrives. */
  request(path: string): TexState {
    return this.entries.get(path) ?? this.enqueue(path, 'low')
  }

  /** A thumbnail went out of view before its turn: do not load it after all (no-op once the load started). */
  cancel(path: string): void {
    if (this.low.delete(path) && !this.wanted.has(path)) this.entries.delete(path)
  }

  private inFlight = 0
  private high: string[] = []
  private low = new Set<string>()
  private wanted = new Set<string>()
  private loadListeners = new Set<() => void>()
  private dirtyEnv = false
  private dirtyLoad = false
  private flushing = false

  private enqueue(path: string, priority: 'high' | 'low'): TexState {
    const e: TexState = { status: 'loading' }
    this.entries.set(path, e)
    if (priority === 'high') this.high.push(path); else this.low.add(path)
    this.pump()
    return e
  }

  private pump(): void {
    while (this.inFlight < MAX_PARALLEL_LOADS) {
      let next = this.high.shift()
      if (next === undefined) { next = this.low.values().next().value; if (next === undefined) return; this.low.delete(next) }
      this.inFlight++
      void this.load(next).finally(() => { this.inFlight--; this.pump() })
    }
  }

  private async load(path: string): Promise<void> {
    try {
      const { ext, bytes, props, sky } = await this.fetchTexture(path)
      if (sky) {
        const images: SkyImages = { panorama: sky.panorama, faces: await Promise.all(sky.images.map((im) => this.decode(im.ext, im.bytes))) }
        if (!images.faces.length) throw new Error(`skybox '${path}' has no images`)
        this.entries.set(path, { status: 'ready', image: skyPreview(images), props: parseProps(props, path), sky: images })
      } else this.entries.set(path, { status: 'ready', image: await this.decode(ext, bytes), props: parseProps(props, path) })
    } catch (e) {
      this.entries.set(path, { status: 'missing', error: (e as Error).message })
    }
    // meshes built without this texture (plain UVs) are rebuilt with its size and properties; a thumbnail-only texture is in no mesh
    if (this.wanted.has(path)) this.dirtyEnv = true
    this.dirtyLoad = true
    this.scheduleFlush()
  }

  /** Many textures arriving close together make one redraw, not one per texture (every viewport rebuilds its meshes on a change). */
  private scheduleFlush(): void {
    if (this.flushing) return
    this.flushing = true
    const run = () => {
      this.flushing = false
      const env = this.dirtyEnv, load = this.dirtyLoad
      this.dirtyEnv = this.dirtyLoad = false
      if (env) { texEnv.bump(); this.listeners.forEach((f) => f()) }
      if (load) this.loadListeners.forEach((f) => f())
    }
    this.schedule(run)
  }

  /** Forget everything (e.g. after the assets root changes). */
  clear(): void {
    this.entries.clear(); this.high = []; this.low.clear(); this.wanted.clear()
    texEnv.bump(); this.listeners.forEach((f) => f()); this.loadListeners.forEach((f) => f())
  }
}
