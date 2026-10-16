// The "Lightmap density" overlay (View menu): every triangle of the Core's lit mesh coloured by its lightmap texel density (texels per metre) and
// covered with a checker of the lightmap's own texels, so the size of one texel is visible on the surface. Pure geometry, no WebGL.
import { LIT_VERTEX_FLOATS } from './lightmapStore'
import type { LightmapStore } from './lightmapStore'

/** Floats per vertex of the overlay buffer: position (3), lightmap position in texels (2), density in texels per metre (1). */
export const DENSITY_STRIDE = 6

/** Ramp ends in texels per metre: <= LOW is blue, HIGH and above is red (logarithmic in between). */
export const DENSITY_LOW = 1, DENSITY_HIGH = 64

export interface DensityMesh {
  data: Float32Array
  /** Density range over the mesh in texels per metre (0 / 0 for an empty mesh). */
  min: number
  max: number
}

/**
 * Texels per metre of one triangle: the mapping is affine, so it is the ratio of the lengths of two edges in lightmap texels and in metres
 * (the larger of the two directions, the other is equal for the Core's isometric charts). 0 for a degenerate triangle.
 */
export function triangleDensity(mesh: Float32Array, tri: number, atlas: number): number {
  const v = (k: number) => tri * 3 * LIT_VERTEX_FLOATS + k * LIT_VERTEX_FLOATS
  let best = 0
  for (const [a, b] of [[0, 1], [1, 2], [2, 0]]) {
    const A = v(a), B = v(b)
    const world = Math.hypot(mesh[B] - mesh[A], mesh[B + 1] - mesh[A + 1], mesh[B + 2] - mesh[A + 2])
    if (world < 1e-9) continue
    const texels = Math.hypot((mesh[B + 6] - mesh[A + 6]) * atlas, (mesh[B + 7] - mesh[A + 7]) * atlas)
    best = Math.max(best, texels / world)
  }
  return best
}

export function buildDensityMesh(mesh: Float32Array, triangleCount: number, atlas: number): DensityMesh {
  const data = new Float32Array(triangleCount * 3 * DENSITY_STRIDE)
  let min = Infinity, max = 0
  for (let t = 0; t < triangleCount; t++) {
    const d = triangleDensity(mesh, t, atlas)
    if (d > 0) { min = Math.min(min, d); max = Math.max(max, d) }
    for (let k = 0; k < 3; k++) {
      const s = t * 3 * LIT_VERTEX_FLOATS + k * LIT_VERTEX_FLOATS, o = (t * 3 + k) * DENSITY_STRIDE
      data[o] = mesh[s]; data[o + 1] = mesh[s + 1]; data[o + 2] = mesh[s + 2]
      data[o + 3] = mesh[s + 6] * atlas; data[o + 4] = mesh[s + 7] * atlas
      data[o + 5] = d
    }
  }
  return { data, min: Number.isFinite(min) ? min : 0, max }
}

/** The ramp as a colour (blue, cyan, green, yellow, red); the shader has the same one. 0 (degenerate) is grey. */
export function densityColor(texelsPerMetre: number): [number, number, number] {
  if (!(texelsPerMetre > 0)) return [0.5, 0.5, 0.5]
  const t = Math.min(1, Math.max(0, Math.log2(texelsPerMetre / DENSITY_LOW) / Math.log2(DENSITY_HIGH / DENSITY_LOW)))
  const stops: [number, number, number][] = [[0.15, 0.25, 0.95], [0.1, 0.8, 0.9], [0.2, 0.85, 0.25], [0.95, 0.85, 0.15], [0.95, 0.2, 0.15]]
  const f = t * (stops.length - 1), i = Math.min(stops.length - 2, Math.floor(f)), u = f - i
  return [0, 1, 2].map((c) => stops[i][c] + (stops[i + 1][c] - stops[i][c]) * u) as [number, number, number]
}

/** The text of the viewport's legend for the range found. */
export function densityLegend(min: number, max: number): string {
  if (max <= 0) return 'Lightmap density: no lightmapped surfaces yet'
  const f = (v: number) => (v >= 10 ? String(Math.round(v)) : v.toFixed(1))
  return `Lightmap texels/m: ${f(min)} to ${f(max)} (blue ${DENSITY_LOW}, green 8, red ${DENSITY_HIGH}+)`
}

const cache = new WeakMap<LightmapStore, { meshSerial: number; value: DensityMesh | null }>()
/** The overlay of the store's current lit mesh, built once per mesh (the renderer and the legend both ask); null without a mesh. */
export function densityOf(store: LightmapStore): DensityMesh | null {
  const c = cache.get(store)
  if (c && c.meshSerial === store.meshSerial) return c.value
  const value = store.mesh && store.triangleCount > 0 ? buildDensityMesh(store.mesh, store.triangleCount, store.atlas) : null
  cache.set(store, { meshSerial: store.meshSerial, value })
  return value
}
