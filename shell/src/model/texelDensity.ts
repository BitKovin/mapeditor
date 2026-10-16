// Texel density heat map (REQUIREMENTS §19 A3).
//
// The texture's `pixelsPerMeter` says how sharp it was authored to look. A face that shows it larger or smaller than that has a different density:
//     density = texture pixels per repeat / metres per repeat      (per axis; pixels per metre on the surface)
//     e = log2(density / authored ppm)          0 = as authored; -1 = stretched 2x (blurry); +1 = squeezed 2x (denser than needed)
// A face is judged by its worst axis (the blurrier one). The colour ramp is green at 0, towards red for stretched, towards blue for dense.
import type { Brush, Face } from './document'
import { faceNormal } from './geometry'
import { Vec3, dot, length, scale, sub } from './math'
import { TexInfo, texEnv } from './texprops'
import { faceAffine } from './uvmesh'

/** The ramp saturates at +/- this many octaves (4x). */
export const HEAT_RANGE = 2

const planeGradient = (g: Vec3, n: Vec3): Vec3 => sub(g, scale(n, dot(g, n)))

/** Pixels per metre along u and v that the face's map gives a texture of `width` x `height` pixels. */
export function faceDensity(b: Brush, f: Face, width: number, height: number): [number, number] {
  const m = faceAffine(b, f)
  const n = faceNormal(b, f)
  const gu = length(planeGradient([m.u[0], m.u[1], m.u[2]], n)), gv = length(planeGradient([m.v[0], m.v[1], m.v[2]], n))
  return [width * gu, height * gv]
}

/** Octaves relative to the authored density for one face; null when the texture is not loaded. */
export function faceHeat(b: Brush, f: Face, info: TexInfo | undefined = texEnv.info(f.texture ?? 'dev/grey')): number | null {
  if (!info) return null
  const [du, dv] = faceDensity(b, f, info.width, info.height)
  const lo = Math.min(du, dv)
  if (!(lo > 1e-9)) return -HEAT_RANGE
  return Math.log2(lo / info.ppm)
}

/** Heat colour (0..1 rgb) of an octave value: blue (dense) - green (as authored) - red (stretched). */
export function heatColor(e: number): [number, number, number] {
  const t = Math.max(-1, Math.min(1, e / HEAT_RANGE)) // -1 stretched .. +1 dense
  const lerp = (a: number[], b: number[], k: number): [number, number, number] => [a[0] + (b[0] - a[0]) * k, a[1] + (b[1] - a[1]) * k, a[2] + (b[2] - a[2]) * k]
  const RED = [0.9, 0.12, 0.1], GREEN = [0.15, 0.8, 0.2], BLUE = [0.15, 0.3, 0.95]
  return t < 0 ? lerp(GREEN, RED, -t) : lerp(GREEN, BLUE, t)
}

/** A bucket index (0..N-1) of an octave value, so a viewport can draw every face of a bucket in one call. */
export const HEAT_BUCKETS = 17
export const heatBucket = (e: number): number => Math.round(((Math.max(-HEAT_RANGE, Math.min(HEAT_RANGE, e)) + HEAT_RANGE) / (2 * HEAT_RANGE)) * (HEAT_BUCKETS - 1))
export const bucketColor = (i: number): [number, number, number] => heatColor((i / (HEAT_BUCKETS - 1)) * 2 * HEAT_RANGE - HEAT_RANGE)

export const heatLabel = (e: number): string =>
  Math.abs(e) < 0.15 ? 'as authored' : e < 0 ? `stretched ${Math.pow(2, -e).toFixed(1)}x (blurry)` : `squeezed ${Math.pow(2, e).toFixed(1)}x (denser than needed)`
