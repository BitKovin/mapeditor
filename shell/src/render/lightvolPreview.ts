// The "Lightvol samples" overlay (View menu): one small smooth-shaded sphere per baked probe of the light volume, every vertex coloured by what a
// surface facing along its normal would receive from the probe (ambient + directed * max(0, n . dir)), and a line from it along the direction its
// directed light comes from (length = how strong that light is). Pure geometry, no WebGL.
import type { LightvolStore } from './lightvolStore'

export interface ProbePreview {
  /** Interleaved position (3) + colour (3) + unit normal (3) of every triangle vertex. */
  tris: Float32Array
  /** The same layout for every line end (normal = 0: lines are not shaded). */
  lines: Float32Array
  /** Probes drawn and probes that are baked at all (the preview thins out above `maxProbes`). */
  shown: number
  total: number
}

export const PROBE_PREVIEW_MAX = 6000
/** Floats per vertex of the buffers above. */
export const PREVIEW_STRIDE = 9

const clamp01 = (v: number): number => Math.min(1, Math.max(0, v))
/** Display colour of linear light `v` (the Lit view's own transfer: exposure, clamp, sRGB). */
export const displayChannel = (v: number, exposure: number): number => {
  const c = clamp01(v * exposure)
  return c <= 0.0031308 ? c * 12.92 : 1.055 * Math.pow(c, 1 / 2.4) - 0.055
}

/** Subdivisions of the icosahedron the probe spheres start from: 0 = 20 triangles, 1 = 80, 2 = 320. */
const SPHERE_LEVEL = 1

/** A unit icosphere as triangles of unit vertices (= their own normals). Built once. */
function icosphere(level: number): [number, number, number][][] {
  const t = (1 + Math.sqrt(5)) / 2
  const norm = (v: number[]): [number, number, number] => { const l = Math.hypot(v[0], v[1], v[2]); return [v[0] / l, v[1] / l, v[2] / l] }
  const V = [[-1, t, 0], [1, t, 0], [-1, -t, 0], [1, -t, 0], [0, -1, t], [0, 1, t], [0, -1, -t], [0, 1, -t], [t, 0, -1], [t, 0, 1], [-t, 0, -1], [-t, 0, 1]].map(norm)
  const F = [[0, 11, 5], [0, 5, 1], [0, 1, 7], [0, 7, 10], [0, 10, 11], [1, 5, 9], [5, 11, 4], [11, 10, 2], [10, 7, 6], [7, 1, 8],
    [3, 9, 4], [3, 4, 2], [3, 2, 6], [3, 6, 8], [3, 8, 9], [4, 9, 5], [2, 4, 11], [6, 2, 10], [8, 6, 7], [9, 8, 1]]
  let tris = F.map((f) => f.map((i) => V[i]))
  for (let k = 0; k < level; k++) {
    const next: [number, number, number][][] = []
    for (const [a, b, c] of tris) {
      const ab = norm([a[0] + b[0], a[1] + b[1], a[2] + b[2]]), bc = norm([b[0] + c[0], b[1] + c[1], b[2] + c[2]]), ca = norm([c[0] + a[0], c[1] + a[1], c[2] + a[2]])
      next.push([a, ab, ca], [b, bc, ab], [c, ca, bc], [ab, bc, ca])
    }
    tris = next
  }
  return tris
}
const SPHERE = icosphere(SPHERE_LEVEL)

export function buildProbePreview(store: LightvolStore, exposure = 1, maxProbes = PROBE_PREVIEW_MAX): ProbePreview {
  const [nx, ny] = store.dims
  const n = store.probeCount
  const valid: number[] = []
  for (let i = 0; i < n; i++) if (store.probes[i * 10 + 9] > 0) valid.push(i)
  const stride = Math.max(1, Math.ceil(valid.length / maxProbes))
  const picked: number[] = []
  for (let k = 0; k < valid.length; k += stride) picked.push(valid[k])
  const r = store.cell * 0.12
  const tris = new Float32Array(picked.length * SPHERE.length * 3 * PREVIEW_STRIDE)
  const lines = new Float32Array(picked.length * 2 * PREVIEW_STRIDE)
  let t = 0, l = 0
  for (const i of picked) {
    const x = i % nx, y = Math.floor(i / nx) % ny, z = Math.floor(i / (nx * ny))
    const px = store.origin[0] + x * store.cell, py = store.origin[1] + y * store.cell, pz = store.origin[2] + z * store.cell
    const o = i * 10, P = store.probes
    const amb = [P[o], P[o + 1], P[o + 2]], dir = [P[o + 3], P[o + 4], P[o + 5]], d = [P[o + 6], P[o + 7], P[o + 8]]
    // smooth shading: each vertex shows the light a surface facing along its normal receives (ambient + directed * max(0, n . dir))
    for (const tri of SPHERE) for (const n of tri) {
      const k = Math.max(0, n[0] * d[0] + n[1] * d[1] + n[2] * d[2])
      tris.set([px + n[0] * r, py + n[1] * r, pz + n[2] * r,
        displayChannel(amb[0] + dir[0] * k, exposure), displayChannel(amb[1] + dir[1] * k, exposure), displayChannel(amb[2] + dir[2] * k, exposure),
        n[0], n[1], n[2]], t)
      t += PREVIEW_STRIDE
    }
    // the line takes the colour of a surface facing the directed light: ambient + directed
    const rgb = [0, 1, 2].map((c) => displayChannel(amb[c] + dir[c], exposure))
    // the directed part: points to where the light comes from; a probe with no directed light shows no line
    const strength = Math.max(dir[0], dir[1], dir[2])
    if (strength > 1e-3) {
      const len = store.cell * 0.5 * clamp01(strength)
      lines.set([px, py, pz, rgb[0], rgb[1], rgb[2], 0, 0, 0, px + P[o + 6] * len, py + P[o + 7] * len, pz + P[o + 8] * len, 1, 1, 1, 0, 0, 0], l)
      l += 2 * PREVIEW_STRIDE
    }
  }
  return { tris, lines: lines.subarray(0, l), shown: picked.length, total: valid.length }
}
