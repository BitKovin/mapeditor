// Texture properties (REQUIREMENTS §19). Stored by Core next to the texture as `<texture file>.json`; the Shell keeps them with the decoded image.
//
//   * pixelsPerMeter: the texture's natural size in the world (size = pixels / ppm metres per repeat). null = the game profile's default.
//   * repeat.u / repeat.v: what happens beyond one tile, per axis (see model/uvmesh.ts for how it is turned into UV data):
//       repeat  plain tiling
//       mirror  ping-pong (every second tile flipped)
//       anchor  one edge of the texture stays glued to the same edge of the face; the part between `split` and the open edge repeats
//               (`fill` = repeat | mirror), optionally taken from another texture (`alt`).
//   Axis conventions are those of the image: U runs left to right ('start' = left), V runs top to bottom ('start' = top, 'end' = bottom).
//
// The GL sampler never changes (linear, mipmapped, REPEAT): repetition behaviour lives in the UV data only.
import { Relief, parseRelief, reliefToJson } from './relief'
export type Vec2 = [number, number]

export type AxisMode = 'repeat' | 'mirror' | 'anchor'
export type Fill = 'repeat' | 'mirror'
export type AnchorSide = 'start' | 'end'
/** What the texture's opposite edges mean on this axis. wrap: they continue each other (a seamless tile); clamp: they are borders - the filter must not blend the far edge in (the black line); auto: decided from the pixels. */
export type EdgeMode = 'auto' | 'wrap' | 'clamp'

/** Another texture that fills the repeating part. `offset` shifts it, in repeats of that texture (U, V): the phase that makes the pattern continue across the joint. */
export interface AltTexture { texture: string; offset: Vec2 }

export interface AxisRepeat {
  mode: AxisMode
  /** Anchor mode: the texture edge that stays glued to the face edge. */
  anchor: AnchorSide
  /** Anchor mode: where the fixed part ends, 0..1 along the axis. The part between the split and the OPEN edge (the one opposite the anchor) is the repeating band. */
  split: number
  /** Anchor mode: how the repeating band (or the alternate texture) is continued. */
  fill: Fill
  /** Anchor mode: the repeating part comes from this texture instead of the band. */
  alt: AltTexture | null
  /** Edge handling of this axis (any mode). */
  edge: EdgeMode
}

/** A rectangle of the texture (image coordinates 0..1, y down) that a face can be fitted to (Scythe-style hotspots, REQUIREMENTS §6 B5). */
export interface Hotspot { x: number; y: number; w: number; h: number }
export const MAX_HOTSPOTS = 64, HOTSPOT_MIN = 0.01

export interface TextureProps {
  /** null = the profile's default */
  pixelsPerMeter: number | null
  repeat: { u: AxisRepeat; v: AxisRepeat }
  hotspots: Hotspot[]
  /** The texture's relief mesh (model/relief.ts); null = flat. */
  relief: Relief | null
  /** How light (and the view) passes the texture (REQUIREMENTS §23). */
  transparency: Transparency
}

/** auto = cutout when the image has transparent texels, else opaque; cutout = alpha below `cutoff` is a hole; translucent = light passes, times
 *  (1 - opacity x alpha) and tinted by the texture colour by `tint`. The Core bakes it (core/albedo.h), the views draw it the same way. */
export type TransparencyMode = 'auto' | 'opaque' | 'cutout' | 'translucent'
export interface Transparency { mode: TransparencyMode; cutoff: number; opacity: number; tint: number }
export const defaultTransparency = (): Transparency => ({ mode: 'auto', cutoff: 0.5, opacity: 0.5, tint: 1 })
/** What an image's alpha holds: nothing, on / off holes (fences), or half transparency (glass). */
export type AlphaKind = 'none' | 'cutout' | 'translucent'
/** The mode as it applies to an image, as the Core resolves it (resolveTransparency): auto = opaque / cutout / translucent by the image's alpha;
 *  cutout without alpha is opaque. */
export function resolvedTransparency(t: Transparency, alpha: AlphaKind): 'opaque' | 'cutout' | 'translucent' {
  if (t.mode === 'translucent') return 'translucent'
  if (t.mode === 'opaque') return 'opaque'
  if (t.mode === 'cutout') return alpha === 'none' ? 'opaque' : 'cutout'
  return alpha === 'translucent' ? 'translucent' : alpha === 'cutout' ? 'cutout' : 'opaque'
}
/** The opacity that applies: auto glass uses the painted alpha as it is (opacity 1). */
export const effectiveOpacity = (t: Transparency, alpha: AlphaKind): number => (t.mode === 'auto' && alpha === 'translucent' ? 1 : t.opacity)
export function parseTransparency(j: unknown): Transparency {
  const d = defaultTransparency()
  if (!j || typeof j !== 'object') return d
  const o = j as Record<string, unknown>
  const unit = (v: unknown, dflt: number) => (fin(v) ? clamp(v, 0, 1) : dflt)
  return {
    mode: o.mode === 'opaque' || o.mode === 'cutout' || o.mode === 'translucent' ? o.mode : 'auto',
    cutoff: unit(o.cutoff, d.cutoff), opacity: unit(o.opacity, d.opacity), tint: unit(o.tint, d.tint),
  }
}

export const PPM_MIN = 1, PPM_MAX = 4096, DEFAULT_PPM = 64
export const SPLIT_MIN = 0.02, SPLIT_MAX = 0.98

export const defaultAxis = (anchor: AnchorSide): AxisRepeat => ({ mode: 'repeat', anchor, split: 0.5, fill: 'mirror', alt: null, edge: 'auto' })
export const defaultProps = (): TextureProps => ({ pixelsPerMeter: null, repeat: { u: defaultAxis('start'), v: defaultAxis('end') }, hotspots: [], relief: null, transparency: defaultTransparency() })

const fin = (v: unknown): v is number => typeof v === 'number' && Number.isFinite(v)
const clamp = (v: number, lo: number, hi: number): number => Math.min(hi, Math.max(lo, v))

/** A texture path as Core accepts it (relative, forward slashes, no `..`). */
export function safeTexturePath(p: unknown): string | null {
  if (typeof p !== 'string' || p.length === 0 || p.length > 260) return null
  if (p.startsWith('/') || p.startsWith('\\') || /[\x00-\x1f:\\*?"<>|]/.test(p)) return null
  if (p.split('/').some((s) => s === '..' || s === '')) return null
  return p
}

function parseAxis(j: unknown, anchor: AnchorSide, self: string | null): AxisRepeat {
  const d = defaultAxis(anchor)
  if (!j || typeof j !== 'object') return d
  const o = j as Record<string, unknown>
  const mode: AxisMode = o.mode === 'mirror' || o.mode === 'anchor' ? o.mode : 'repeat'
  const side: AnchorSide = o.anchor === 'start' || o.anchor === 'end' ? o.anchor : anchor
  const split = fin(o.split) ? clamp(o.split, SPLIT_MIN, SPLIT_MAX) : d.split
  const fill: Fill = o.fill === 'repeat' ? 'repeat' : 'mirror'
  let alt: AltTexture | null = null
  const a = o.alt as Record<string, unknown> | null | undefined
  if (a && typeof a === 'object') {
    const t = safeTexturePath(a.texture)
    const off = Array.isArray(a.offset) && a.offset.length === 2 && a.offset.every(fin) ? ([a.offset[0], a.offset[1]] as Vec2) : ([0, 0] as Vec2)
    if (t && t !== self) alt = { texture: t, offset: off }
  }
  const edge: EdgeMode = o.edge === 'wrap' || o.edge === 'clamp' ? o.edge : 'auto'
  return { mode, anchor: side, split, fill, alt, edge }
}

/** Lenient: anything unusable keeps the default (a hand-edited or damaged file must never break the editor). `self` = this texture's own path (it cannot be its own alternate). */
export function parseProps(json: unknown, self: string | null = null): TextureProps {
  const d = defaultProps()
  if (!json || typeof json !== 'object') return d
  const o = json as Record<string, unknown>
  const rep = (o.repeat && typeof o.repeat === 'object' ? o.repeat : {}) as Record<string, unknown>
  return {
    pixelsPerMeter: fin(o.pixelsPerMeter) && o.pixelsPerMeter > 0 ? clamp(o.pixelsPerMeter, PPM_MIN, PPM_MAX) : null,
    repeat: { u: parseAxis(rep.u, 'start', self), v: parseAxis(rep.v, 'end', self) },
    hotspots: parseHotspots(o.hotspots),
    relief: parseRelief(o.relief),
    transparency: parseTransparency(o.transparency),
  }
}

/** Valid rectangles only, clamped into the texture, at most MAX_HOTSPOTS. */
export function parseHotspots(j: unknown): Hotspot[] {
  if (!Array.isArray(j)) return []
  const out: Hotspot[] = []
  for (const r of j) {
    if (!r || typeof r !== 'object') continue
    const o = r as Record<string, unknown>
    if (![o.x, o.y, o.w, o.h].every(fin)) continue
    const x = clamp(o.x as number, 0, 1 - HOTSPOT_MIN), y = clamp(o.y as number, 0, 1 - HOTSPOT_MIN)
    const w = clamp(o.w as number, HOTSPOT_MIN, 1 - x), h = clamp(o.h as number, HOTSPOT_MIN, 1 - y)
    out.push({ x, y, w, h })
    if (out.length >= MAX_HOTSPOTS) break
  }
  return out
}

/** The JSON Core stores (Core merges it into the existing file and keeps unknown keys). */
export function propsToJson(p: TextureProps): Record<string, unknown> {
  const axis = (a: AxisRepeat) => ({ mode: a.mode, anchor: a.anchor, split: a.split, fill: a.fill, edge: a.edge, alt: a.alt ? { texture: a.alt.texture, offset: [a.alt.offset[0], a.alt.offset[1]] } : null })
  const out: Record<string, unknown> = { repeat: { u: axis(p.repeat.u), v: axis(p.repeat.v) }, hotspots: p.hotspots.map((r) => ({ x: r.x, y: r.y, w: r.w, h: r.h })) }
  if (p.pixelsPerMeter !== null) out.pixelsPerMeter = p.pixelsPerMeter
  out.relief = p.relief ? reliefToJson(p.relief) : null
  out.transparency = { mode: p.transparency.mode, cutoff: p.transparency.cutoff, opacity: p.transparency.opacity, tint: p.transparency.tint }
  return out
}

export const propsEqual = (a: TextureProps, b: TextureProps): boolean => JSON.stringify(propsToJson(a)) === JSON.stringify(propsToJson(b))

/** No axis changes the plain repeating sampler result: no cuts are needed. */
export const isPlainProps = (p: TextureProps): boolean => p.repeat.u.mode === 'repeat' && p.repeat.v.mode === 'repeat'

/**
 * Must this axis' texture edges be treated as borders (no blending with the opposite edge)? The sampler stays linear + REPEAT, so this is done in the
 * UV data (uvmesh.ts): faces are cut at tile boundaries and the UVs stay a texel inside the texture. 'auto' follows the pixel analysis (`seamless`).
 */
export const edgeClamp = (i: { props: TextureProps; seamless?: [boolean, boolean] }, axis: 0 | 1): boolean => {
  const e = (axis === 0 ? i.props.repeat.u : i.props.repeat.v).edge
  return e === 'clamp' || (e === 'auto' && i.seamless?.[axis] === false)
}

// ---- the texture environment ------------------------------------------------------------------------------------------------------------------------

/** What the pure UV functions need to know about a texture. */
export interface TexInfo { path: string; width: number; height: number; /** effective (profile default applied) */ ppm: number; props: TextureProps; /** per axis (u, v): do the opposite edges continue each other? Unknown (not analysed) = undefined = treated as seamless. */ seamless?: [boolean, boolean]; /** what the image's alpha holds */ alpha?: AlphaKind }

/** How a view draws a texture: [mode 0 opaque | 1 cutout | 2 translucent, cutoff, opacity] (uTransp of the renderer's shaders). */
export function drawTransparency(info: TexInfo | undefined): [number, number, number] {
  if (!info) return [0, 0, 0]
  const t = info.props.transparency, a = info.alpha ?? 'none', m = resolvedTransparency(t, a)
  return m === 'cutout' ? [1, t.cutoff, 0] : m === 'translucent' ? [2, t.cutoff, effectiveOpacity(t, a)] : [0, 0, 0]
}

/** Natural size in metres per repeat. */
export const naturalSize = (i: { width: number; height: number; ppm: number }): Vec2 => [i.width / i.ppm, i.height / i.ppm]

/**
 * The texture environment: the single place the pure UV functions (uvmesh.ts, heat map) look textures up. The TextureCache registers itself as the
 * provider. `info` returns undefined while a texture is still loading (callers then draw plainly) and `rev` increases whenever anything a
 * texture-dependent result could depend on changes (image loaded, properties edited, profile default changed): caches of derived meshes compare it.
 */
class TexEnv {
  rev = 0
  private provider: ((path: string) => TexInfo | undefined) | null = null
  setProvider(p: ((path: string) => TexInfo | undefined) | null): void { this.provider = p; this.rev++ }
  info(path: string): TexInfo | undefined { return this.provider?.(path) }
  bump(): void { this.rev++ }
}
export const texEnv = new TexEnv()

/**
 * A face's own repetition override (`face.repeat`, REQUIREMENTS §6 B4): per axis the mode (repeat / mirror) and the edge handling (wrap / clamp), replacing the
 * texture's setting for this face only. Absent / unknown values = the texture's setting. Anchor stays a texture property.
 */
export interface FaceRepeat { u?: { mode?: 'repeat' | 'mirror'; edge?: 'wrap' | 'clamp' }; v?: { mode?: 'repeat' | 'mirror'; edge?: 'wrap' | 'clamp' } }

export function parseFaceRepeat(j: unknown): FaceRepeat | null {
  if (!j || typeof j !== 'object') return null
  const o = j as Record<string, unknown>
  const axis = (a: unknown): { mode?: 'repeat' | 'mirror'; edge?: 'wrap' | 'clamp' } | undefined => {
    if (!a || typeof a !== 'object') return undefined
    const r = a as Record<string, unknown>
    const out: { mode?: 'repeat' | 'mirror'; edge?: 'wrap' | 'clamp' } = {}
    if (r.mode === 'repeat' || r.mode === 'mirror') out.mode = r.mode
    if (r.edge === 'wrap' || r.edge === 'clamp') out.edge = r.edge
    return out.mode || out.edge ? out : undefined
  }
  const u = axis(o.u), v = axis(o.v)
  return u || v ? { ...(u ? { u } : {}), ...(v ? { v } : {}) } : null
}

/** The texture info a face is planned with: the texture's properties with the face's override applied (the same object when there is none). */
export function withFaceRepeat(info: TexInfo, over: FaceRepeat | null): TexInfo {
  if (!over) return info
  const apply = (a: AxisRepeat, o: FaceRepeat['u']): AxisRepeat => {
    if (!o) return a
    // an overridden mode replaces anchor / mirror / repeat of the texture; edge replaces the edge handling
    return { ...a, ...(o.mode ? { mode: o.mode, alt: null } : {}), ...(o.edge ? { edge: o.edge } : {}) }
  }
  return { ...info, props: { ...info.props, repeat: { u: apply(info.props.repeat.u, over.u), v: apply(info.props.repeat.v, over.v) } } }
}
