// What the UV window shows in its Lightmap mode (REQUIREMENTS §9 "lightmap UVs editable in the same UV window"): where the charts of the selected faces sit
// on the lightmap page and how dense they are. Pure; the data is what Core already sends with the lit mesh (`LitFaceMap`: world position -> texel of a page).
// Placement of charts is Core's job (lightmap_uv.cpp packs them); what the author controls is the density of a face (`lightmapScale`).
import type { LitFaceMap } from '../net/protocol'
import { halfToFloat } from '../render/half'
import type { Brush, Face } from './document'
import type { Vec3 } from './math'
import { lightmapScaleOf } from './uv'

export type Vec2 = [number, number]

// Core sends the map in page units (0..1 across the page, lighting_service.cpp divides by the atlas size); `atlas` (texels per page side) turns it into texels.

/** The texel of a page a world point lands on. */
export const texelOf = (m: LitFaceMap, p: Vec3, atlas: number): Vec2 => [atlas * (m.u[0] * p[0] + m.u[1] * p[1] + m.u[2] * p[2] + m.u[3]), atlas * (m.v[0] * p[0] + m.v[1] * p[1] + m.v[2] * p[2] + m.v[3])]

/** Texels per metre along the chart's two axes (the length of the gradients of u and v). */
export const densityOf = (m: LitFaceMap, atlas: number): Vec2 => [atlas * Math.hypot(m.u[0], m.u[1], m.u[2]), atlas * Math.hypot(m.v[0], m.v[1], m.v[2])]

export interface Chart { faceId: number; page: number; polygon: Vec2[]; min: Vec2; max: Vec2; density: Vec2; scale: number }

export function chartOf(b: Brush, f: Face, m: LitFaceMap, atlas: number): Chart {
  const polygon = f.loop.map((i) => texelOf(m, b.vertices[i], atlas))
  const xs = polygon.map((p) => p[0]), ys = polygon.map((p) => p[1])
  return { faceId: f.id, page: m.pg, polygon, min: [Math.min(...xs), Math.min(...ys)], max: [Math.max(...xs), Math.max(...ys)], density: densityOf(m, atlas), scale: lightmapScaleOf(f) }
}

/** The charts of `faceIds` that have light data (a face without a map is not lightmapped or not baked yet). */
export function chartsOf(b: Brush, faceIds: number[], mapOf: (faceId: number) => LitFaceMap | undefined, atlas: number): Chart[] {
  const out: Chart[] = []
  for (const id of faceIds) {
    const f = b.faces.find((x) => x.id === id), m = mapOf(id)
    if (f && m) out.push(chartOf(b, f, m, atlas))
  }
  return out
}

/**
 * A page as an 8 bit picture for the backdrop: every `step`-th texel of an RGBA16F page, tone mapped (x / (1 + x), gamma 2.2). Texels that hold no value yet
 * (alpha 0) stay transparent. Returns an `out` x `out` RGBA buffer.
 */
export function pagePicture(page: Uint16Array, atlas: number, out: number): Uint8ClampedArray<ArrayBuffer> {
  const img = new Uint8ClampedArray(new ArrayBuffer(out * out * 4))
  const step = atlas / out
  for (let y = 0; y < out; y++) for (let x = 0; x < out; x++) {
    const o = ((Math.floor(y * step) * atlas) + Math.floor(x * step)) * 4, d = (y * out + x) * 4
    if (halfToFloat(page[o + 3]) <= 0) continue
    for (let c = 0; c < 3; c++) { const v = Math.max(0, halfToFloat(page[o + c])); img[d + c] = Math.round(255 * Math.pow(v / (1 + v), 1 / 2.2)) }
    img[d + 3] = 255
  }
  return img
}
