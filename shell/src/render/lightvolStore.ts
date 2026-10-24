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
