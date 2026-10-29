// Client side of the Core's light volumes (core/lightvol.h, DESIGN §6.7): the probe grid (for the preview overlay) and the light every
// brush entity that is not lightmapped receives: ambient + directed * max(0, n . dir). Pure data, no WebGL.
import type { EntityLightEntry, EntityStyleLight, Inbound } from '../net/protocol'
import { decodeBase64 } from './lightmapStore'

export type LightvolMsg = Extract<Inbound, { type: 'lightvol' }>
export type EntityLightMsg = Extract<Inbound, { type: 'entity_light' }>

export const PROBE_BYTES = 9
/** Light volume bytes cover 0..2 (REQUIREMENTS §9: the engine stores value / 2). */
export const fromByte = (b: number): number => (b / 255) * 2

/** One light style layer's probe for an entity (linear, x2 range decoded). */
export type EntityLight = EntityStyleLight
export const entityLightKey = (instance: string, entityId: string): string => `${instance}|${entityId}`

export class LightvolStore {
  rev = -1
  origin: [number, number, number] = [0, 0, 0]
  cell = 1
  dims: [number, number, number] = [0, 0, 0]
  /** The probes as the Core sent them (PROBE_BYTES each, style 0): a level can have tens of millions, so they stay bytes and are decoded where they are looked at. */
  bytes: Uint8Array = new Uint8Array(0)
  /** The same for every other light style layer. */
  styleBytes = new Map<number, Uint8Array>()
  /** Increments whenever the grid was replaced (the preview rebuilds its GPU data). */
  serial = 0
  private lights = new Map<string, EntityLight[]>()
  entityRev = -1

  get probeCount(): number { return this.bytes.length / PROBE_BYTES }
  get ready(): boolean { return this.probeCount > 0 }

  /** Probe `i` (x fastest, then y, then z) of a style layer as 10 floats: ambient rgb, directed rgb, dir xyz, valid (0 / 1). */
  probe(i: number, style = 0, out: Float32Array = new Float32Array(10)): Float32Array {
    const bytes = style === 0 ? this.bytes : this.styleBytes.get(style)
    if (!bytes || (i + 1) * PROBE_BYTES > bytes.length) { out.fill(0); return out }
    const b = i * PROBE_BYTES
    for (let c = 0; c < 6; c++) out[c] = fromByte(bytes[b + c])
    const lat = (bytes[b + 6] / 255) * Math.PI, lng = (bytes[b + 7] / 255) * 2 * Math.PI - Math.PI
    out[6] = Math.sin(lat) * Math.cos(lng); out[7] = Math.cos(lat); out[8] = Math.sin(lat) * Math.sin(lng)
    out[9] = bytes[b + 8] & 1
    return out
  }

  /** Whether probe `i` of the base layer was baked (not inside a solid). */
  valid(i: number): boolean { return (this.bytes[i * PROBE_BYTES + 8] & 1) !== 0 }

  applyGrid(m: LightvolMsg): boolean {
    const n = m.dims[0] * m.dims[1] * m.dims[2]
    if (!Number.isInteger(n) || n <= 0) return false
    const base = decodeBase64(m.probes)
    if (base.length !== n * PROBE_BYTES) return false
    const layers = new Map<number, Uint8Array>()
    for (const l of m.layers ?? []) { const d = decodeBase64(l.probes); if (d.length !== n * PROBE_BYTES) return false; layers.set(l.s, d) }
    this.bytes = base
    this.styleBytes = layers
    this.origin = m.origin; this.cell = m.cell; this.dims = m.dims; this.rev = m.rev
    this.serial++
    return true
  }

  applyEntities(m: EntityLightMsg): void {
    this.lights = new Map(m.entities.map((e: EntityLightEntry) => [entityLightKey(e.i, e.e), e.lights]))
    this.entityRev = m.rev
    this.serial++
  }

  /**
   * The light at a point of the level (a model that moves or is not baked in: a pickup, a character): the probes around it, trilinear, probes that were not baked
   * (inside a solid) left out and the rest renormalised. One entry per style layer, style 0 first; the direction is the base layer's, as for brush entities
   * (ONE direction per object). null when no probe around the point is valid (outside the grid, or inside geometry).
   */
  sampleAt(p: readonly [number, number, number]): EntityLight[] | null {
    if (!this.ready) return null
    const [nx, ny, nz] = this.dims
    const f = [0, 1, 2].map((k) => (p[k] - this.origin[k]) / this.cell)
    const i0 = f.map((x) => Math.floor(x)), t = f.map((x, k) => x - i0[k])
    const corners: { i: number; w: number }[] = []
    let wsum = 0
    for (let c = 0; c < 8; c++) {
      const x = i0[0] + (c & 1), y = i0[1] + ((c >> 1) & 1), z = i0[2] + ((c >> 2) & 1)
      if (x < 0 || y < 0 || z < 0 || x >= nx || y >= ny || z >= nz) continue
      const i = x + nx * (y + ny * z)
      if (!this.valid(i)) continue
      const w = ((c & 1) ? t[0] : 1 - t[0]) * (((c >> 1) & 1) ? t[1] : 1 - t[1]) * (((c >> 2) & 1) ? t[2] : 1 - t[2])
      if (w <= 0) continue
      corners.push({ i, w }); wsum += w
    }
    if (wsum <= 1e-6) return null
    const tmp = new Float32Array(10)
    const layer = (style: number): EntityLight => {
      const amb: [number, number, number] = [0, 0, 0], dc: [number, number, number] = [0, 0, 0], dir: [number, number, number] = [0, 0, 0]
      for (const { i, w } of corners) {
        this.probe(i, style, tmp)
        const k = w / wsum
        for (let c = 0; c < 3; c++) { amb[c] += tmp[c] * k; dc[c] += tmp[3 + c] * k }
        const strength = (tmp[3] + tmp[4] + tmp[5]) * k // brighter probes steer the direction
        for (let c = 0; c < 3; c++) dir[c] += tmp[6 + c] * strength
      }
      const l = Math.hypot(dir[0], dir[1], dir[2])
      return { s: style, ambient: amb, directed: dc, dir: l > 1e-9 ? [dir[0] / l, dir[1] / l, dir[2] / l] : [0, 1, 0] }
    }
    const base = layer(0)
    const out = [base]
    for (const s of this.styleBytes.keys()) { const l = layer(s); l.dir = base.dir; out.push(l) }
    return out
  }

  /** The light of a brush entity per style layer, or undefined (not baked yet / not lit from the grid). */
  entity(instance: string, entityId: string): EntityLight[] | undefined { return this.lights.get(entityLightKey(instance, entityId)) }

  clear(): void {
    if (!this.ready && this.lights.size === 0) return
    this.bytes = new Uint8Array(0)
    this.styleBytes = new Map()
    this.lights = new Map()
    this.rev = this.entityRev = -1
    this.serial++
  }
}
