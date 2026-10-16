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
  /** Per probe: ambient rgb, directed rgb, dir xyz, valid (0 / 1) = 10 floats (style 0). */
  probes: Float32Array = new Float32Array(0)
  /** The same for every other light style layer. */
  styleProbes = new Map<number, Float32Array>()
  /** Increments whenever the grid was replaced (the preview rebuilds its GPU data). */
  serial = 0
  private lights = new Map<string, EntityLight[]>()
  entityRev = -1

  get probeCount(): number { return this.probes.length / 10 }
  get ready(): boolean { return this.probeCount > 0 }

  private decode(b64: string, n: number): Float32Array | null {
    const bytes = decodeBase64(b64)
    if (bytes.length !== n * PROBE_BYTES) return null
    const out = new Float32Array(n * 10)
    for (let i = 0; i < n; i++) {
      const b = i * PROBE_BYTES, o = i * 10
      for (let c = 0; c < 6; c++) out[o + c] = fromByte(bytes[b + c])
      const lat = (bytes[b + 6] / 255) * Math.PI, lng = (bytes[b + 7] / 255) * 2 * Math.PI - Math.PI
      out[o + 6] = Math.sin(lat) * Math.cos(lng); out[o + 7] = Math.cos(lat); out[o + 8] = Math.sin(lat) * Math.sin(lng)
      out[o + 9] = bytes[b + 8] & 1
    }
    return out
  }

  applyGrid(m: LightvolMsg): boolean {
    const n = m.dims[0] * m.dims[1] * m.dims[2]
    if (!Number.isInteger(n) || n <= 0) return false
    const base = this.decode(m.probes, n)
    if (!base) return false
    const layers = new Map<number, Float32Array>()
    for (const l of m.layers ?? []) { const d = this.decode(l.probes, n); if (!d) return false; layers.set(l.s, d) }
    this.probes = base
    this.styleProbes = layers
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
    this.probes = new Float32Array(0)
    this.styleProbes = new Map()
    this.lights = new Map()
    this.rev = this.entityRev = -1
    this.serial++
  }
}
