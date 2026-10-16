// Client side of the Core's live lighting (core/lighting_service.h): the lit mesh (geometry with lightmap coordinates), the lightmap
// pages (RGBA16F, linear, as baked: full dynamic range) kept up to date tile by tile, and the progress status. Pure data, no WebGL: the renderer
// uploads what `dirtySince()` reports. The mesh and the pages belong together: tiles of another revision are ignored.
import type { Inbound, LitFace, LitFaceMap } from '../net/protocol'
import type { World } from '../model/derived'
import { buildLitDraw, buildReliefPlainMesh } from './litTextured'
import { texEnv } from '../model/texprops'
import type { LitDraw } from './litTextured'
import type { Brush } from '../model/document'
import { analyzeLit, buildPlainMesh } from './litStale'
import type { LitAnalysis } from './litStale'

export interface DirtyRect { page: number; x: number; y: number; w: number; h: number; /** light style layer; absent = 0 (base) */ style?: number }
export type LitMeshMsg = Extract<Inbound, { type: 'lit_mesh' }>
export type TilesMsg = Extract<Inbound, { type: 'lightmap_tiles' }>
export type LightingStatus = Extract<Inbound, { type: 'lighting_status' }>
export type LightStylesMsg = Extract<Inbound, { type: 'light_styles' }>

export function decodeBase64(b64: string): Uint8Array {
  const bin = atob(b64)
  const out = new Uint8Array(bin.length)
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i)
  return out
}

/** Little endian float16 pixels (what the tiles carry). The buffer decodeBase64 returns starts at offset 0, so the view is aligned. */
export function decodeHalves(b64: string): Uint16Array {
  const bytes = decodeBase64(b64)
  return new Uint16Array(bytes.buffer, 0, bytes.length >> 1)
}

/**
 * The pixels of `r` out of an atlas page (RGBA16F, `atlas` texels per row) as one tightly packed buffer, ready for texSubImage3D with default
 * unpack state. Uploading from the page with UNPACK_ROW_LENGTH / SKIP_ROWS fails in Chromium for 3D textures, so the renderer never does.
 */
export function packRect(page: Uint16Array, atlas: number, r: DirtyRect): Uint16Array {
  if (r.x === 0 && r.w === atlas) return page.subarray(r.y * atlas * 4, (r.y + r.h) * atlas * 4) // full-width rows are contiguous
  const out = new Uint16Array(r.w * r.h * 4)
  for (let y = 0; y < r.h; y++) {
    const from = ((r.y + y) * atlas + r.x) * 4
    out.set(page.subarray(from, from + r.w * 4), y * r.w * 4)
  }
  return out
}

export const faceMapKey = (instance: string, entityId: string, brushId: string, faceId: number): string => `${instance}|${entityId}|${brushId}|${faceId}`

/** 1.0 as a half: the alpha of a pixel that holds a value. */
const HALF_ONE = 0x3c00

/** Floats per vertex of the lit mesh: position (3), normal (3), lightmap u, v, page. */
export const LIT_VERTEX_FLOATS = 12

export class LightmapStore {
  rev = -1
  atlas = 0
  pageCount = 0
  triangleCount = 0
  mesh: Float32Array | null = null
  /** The authored face of every surface piece of the mesh, in vertex order (for the albedo texture coordinates of the Lit view). */
  faces: LitFace[] = []
  /** Entities (`instance|id`) that have surfaces in the mesh = are lightmapped; every other brush entity is lit from the light volume. */
  litEntities = new Set<string>()
  /** Lightmap mapping of every lightmapped authored face, keyed `instance|entity|brush|face` (decals take the light of the face they lie on). */
  faceMaps = new Map<string, LitFaceMap>()
  private staleCache: { serial: number; doc: unknown; prefabs: unknown; texRev: number; value: LitAnalysis | null } | null = null
  private plainCache: { analysis: LitAnalysis | null; mesh: Float32Array } | null = null
  private reliefPlainCache: { serial: number; doc: unknown; prefabs: unknown; texRev: number; analysis: LitAnalysis | null; mesh: Float32Array | null } | null = null
  private freshBrushes = new WeakSet<Brush>()
  private drawCache: { serial: number; doc: unknown; prefabs: unknown; texRev: number; draw: LitDraw } | null = null
  /** Pages of the base layer (style 0). */
  pages: Uint16Array[] = []
  /** Pages of the other light style layers (same page count and size as the base): light = sum over layers of value(style, time) x layer. */
  private layerPages = new Map<number, Uint16Array[]>()
  /** Light style patterns from the Core (index = style id; '' = constant 1). */
  patterns: string[] = []
  styleHz = 10
  /** Increments whenever mesh and pages were replaced: the renderer rebuilds its GPU objects. */
  serial = 0
  /** Increments with every new mesh, also when the pages were kept (`keepPages`): the renderer then only re-uploads the vertex buffer. */
  meshSerial = 0
  status: LightingStatus | null = null
  private seq = 0
  private log: { seq: number; rect: DirtyRect }[] = []
  private logStart = 0 // sequence number of the oldest rectangle still remembered

  /** The display multiplier of the level (Level settings, exposure); 1 = lightmaps exactly as baked. */
  get exposure(): number { return this.status?.exposure ?? 1 }
  /** The level's display brightness offset (Level settings, colour correction), added after the exposure; 0 = none. */
  get brightness(): number { return this.status?.brightness ?? 0 }

  /** The style layers that have pixels, ascending, 0 (the base) first. */
  get styles(): number[] { return [0, ...[...this.layerPages.keys()].sort((a, b) => a - b)] }
  pagesOf(style: number): Uint16Array[] | undefined { return style === 0 ? this.pages : this.layerPages.get(style) }

  applyStyles(m: LightStylesMsg): void { this.patterns = m.patterns; this.styleHz = m.hz; this.serial++ }

  get ready(): boolean { return this.mesh !== null && this.triangleCount > 0 && this.pages.length > 0 }

  applyMesh(m: LitMeshMsg): void {
    const bytes = decodeBase64(m.vertices)
    const floats = bytes.length >> 2
    const expected = m.triangleCount * 3 * LIT_VERTEX_FLOATS
    this.mesh = floats === expected ? new Float32Array(bytes.buffer, 0, floats) : null // little endian, as every browser target
    this.triangleCount = this.mesh ? m.triangleCount : 0
    this.faces = m.faces ?? []
    this.litEntities = new Set(this.faces.map((f) => `${f.i}|${f.e}`))
    this.faceMaps = new Map()
    for (const f of this.faces) if (f.m) this.faceMaps.set(faceMapKey(f.i, f.e, f.b, f.f), f.m)
    this.freshBrushes = new WeakSet()
    this.staleCache = null; this.plainCache = null; this.reliefPlainCache = null
    // The Core mirrors what we hold. With keepPages it sends only the tiles that differ, so the pages (and the GPU texture made of them) must stay;
    // that is what makes dragging a brush cheap: no page reset, no texture upload of the whole atlas. Without it (first mesh, other atlas shape, the
    // Core thinks we lost them) everything starts from empty pages.
    const keep = m.keepPages && this.pages.length > 0 && m.atlas === this.atlas && m.pageCount === this.pageCount
    this.rev = m.rev
    this.meshSerial++
    if (keep) return
    this.atlas = m.atlas
    this.pageCount = m.pageCount
    this.pages = Array.from({ length: m.pageCount }, () => new Uint16Array(m.atlas * m.atlas * 4))
    this.layerPages = new Map()
    this.resetLog()
    this.serial++
  }

  /** Returns how many tiles were applied (0 for another revision or malformed tiles). */
  applyTiles(m: TilesMsg): number {
    if (m.rev !== this.rev || m.atlas !== this.atlas) return 0
    let applied = 0
    const style = m.style ?? 0
    let pages = this.pagesOf(style)
    if (!pages) { // the first tile of a style layer: its pages start black
      pages = Array.from({ length: this.pageCount }, () => new Uint16Array(this.atlas * this.atlas * 4))
      this.layerPages.set(style, pages)
      this.serial++ // the renderer's texture array grows by this layer
    }
    for (const t of m.tiles) {
      const page = pages[t.page]
      if (!page || t.x + t.w > this.atlas || t.y + t.h > this.atlas) continue
      const px = decodeHalves(t.rgba)
      if (px.length !== t.w * t.h * 4) continue
      for (let y = 0; y < t.h; y++)
        for (let x = 0; x < t.w; x++) {
          const s = (y * t.w + x) * 4
          if (px[s + 3] === 0) continue // alpha 0: nothing was baked here, keep what we have
          const d = ((t.y + y) * this.atlas + t.x + x) * 4
          page[d] = px[s]; page[d + 1] = px[s + 1]; page[d + 2] = px[s + 2]; page[d + 3] = HALF_ONE
        }
      this.log.push({ seq: ++this.seq, rect: { page: t.page, x: t.x, y: t.y, w: t.w, h: t.h, style } })
      applied++
    }
    return applied
  }

  /**
   * The mesh regrouped per texture with albedo coordinates (Lit view). Rebuilt when a new mesh arrived or the document / prefabs changed
   * (a texture was shifted or replaced); null without a mesh or a face table.
   */
  litDraw(world: World): LitDraw | null {
    if (!this.mesh || this.faces.length === 0) return null
    const c = this.drawCache
    if (c && c.serial === this.meshSerial && c.doc === world.doc && c.prefabs === world.prefabs && c.texRev === texEnv.rev) return c.draw
    const draw = buildLitDraw(this.mesh, this.faces, world, this.staleness(world))
    this.drawCache = { serial: this.meshSerial, doc: world.doc, prefabs: world.prefabs, texRev: texEnv.rev, draw }
    return draw
  }

  /**
   * The brushes that moved or changed since the Core built this mesh, and their current faces with the light they had (see litStale.ts): the
   * views show those at once instead of at the old position until the next mesh arrives. null = the mesh matches the document.
   */
  staleness(world: World): LitAnalysis | null {
    if (!this.mesh || this.faces.length === 0) return null
    const c = this.staleCache
    if (c && c.serial === this.meshSerial && c.doc === world.doc && c.prefabs === world.prefabs && c.texRev === texEnv.rev) return c.value
    const value = analyzeLit(this.mesh, this.faces, world, this.faceMaps, this.freshBrushes)
    this.staleCache = { serial: this.meshSerial, doc: world.doc, prefabs: world.prefabs, texRev: texEnv.rev, value }
    return value
  }

  /** The mesh the Lighting-only view draws: the Core's mesh, or (brushes changed) without their old pieces and with their current faces. */
  plainMesh(world: World | undefined): Float32Array | null {
    if (!this.mesh) return null
    const a = world ? this.staleness(world) : null
    if (world) {
      // faces whose texture has a relief: the relief laid over their pieces (litTextured.ts); the Core's mesh is flat
      const r = this.reliefPlainCache
      if (!(r && r.serial === this.meshSerial && r.doc === world.doc && r.prefabs === world.prefabs && r.texRev === texEnv.rev && r.analysis === a)) {
        this.reliefPlainCache = { serial: this.meshSerial, doc: world.doc, prefabs: world.prefabs, texRev: texEnv.rev, analysis: a, mesh: buildReliefPlainMesh(this.mesh, this.faces, world, a) }
      }
      if (this.reliefPlainCache!.mesh) return this.reliefPlainCache!.mesh
    }
    if (!a) return this.mesh
    if (this.plainCache?.analysis === a) return this.plainCache.mesh
    const mesh = buildPlainMesh(this.mesh, this.faces, a)
    this.plainCache = { analysis: a, mesh }
    return mesh
  }

  applyStatus(m: LightingStatus): void {
    this.status = m
    if (!m.enabled) this.clear()
  }

  /**
   * The rectangles changed since `cursor` (for texSubImage), and the new cursor. Every renderer keeps its own cursor, so several viewports
   * can follow one store. `full` = the log no longer reaches back that far: re-upload whole pages.
   */
  dirtySince(cursor: number): { rects: DirtyRect[]; cursor: number; full: boolean } {
    if (cursor < this.logStart) return { rects: [], cursor: this.seq, full: true }
    const rects = this.log.filter((e) => e.seq > cursor).map((e) => e.rect)
    if (this.log.length > 4096) { this.log = this.log.slice(-2048); this.logStart = this.log[0].seq - 1 }
    return { rects, cursor: this.seq, full: false }
  }
  /** Whole-page rectangles: what a renderer uploads right after it (re)created its GPU texture, whatever the change log still remembers. */
  fullRects(): DirtyRect[] {
    const out: DirtyRect[] = []
    for (const style of this.styles) this.pagesOf(style)!.forEach((_, page) => out.push({ page, x: 0, y: 0, w: this.atlas, h: this.atlas, style }))
    return out
  }
  private resetLog(): void { this.log = []; this.logStart = this.seq }

  clear(): void {
    if (!this.mesh && !this.pages.length) return
    this.mesh = null
    this.faces = []
    this.litEntities = new Set()
    this.faceMaps = new Map()
    this.drawCache = null
    this.staleCache = null; this.plainCache = null; this.reliefPlainCache = null
    this.triangleCount = 0
    this.pages = []
    this.layerPages = new Map()
    this.resetLog()
    this.rev = -1
    this.serial++
    this.meshSerial++
  }

  /** 0..1: how far the refinement has come (chunks that reached their last pass count fully), for a progress label. */
  progress(): number {
    const s = this.status
    if (!s || !s.enabled || s.chunks === 0) return 0
    if (s.converged) return 1
    return Math.max(0, Math.min(1, (s.minPasses + (s.chunks - s.stale) / s.chunks) / (s.maxPasses + 1)))
  }
}
