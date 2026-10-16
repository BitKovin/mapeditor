import { afterEach, describe, expect, it } from 'vitest'
import { LightmapStore, LIT_VERTEX_FLOATS } from '../src/render/lightmapStore'
import { buildLitDraw, LIT_TEX_FLOATS } from '../src/render/litTextured'
import { makeBox, translateBrush, triangulateFace, faceNormal } from '../src/model/geometry'
import { buildPreset } from '../src/model/relief'
import { defaultProps, texEnv } from '../src/model/texprops'
import type { LitFace } from '../src/net/protocol'
import type { World } from '../src/model/derived'
import type { Brush, MapDocument } from '../src/model/document'

const TEX = 'wall/relief'
// the Core's mesh for a box (as in litStale.test.ts): lightmap u = x / 10, v = z / 10 on page 2, style mask 5
function coreMesh(b: Brush, texture: string, only?: (n: number[]) => boolean): { mesh: Float32Array; faces: LitFace[] } {
  const out: number[] = [], faces: LitFace[] = []
  for (const f of b.faces) {
    const n = faceNormal(b, f), tris = triangulateFace(b, f)
    if (only && !only(n)) continue
    for (const t of tris) for (const i of t) { const v = b.vertices[i]; out.push(...v, ...n, v[0] / 10, v[2] / 10, 2, 5, 0, 0) }
    faces.push({ i: '', e: 'w', bi: '', b: b.id, f: f.id, t: texture, n: tris.length, m: { u: [0.1, 0, 0, 0], v: [0, 0, 0.1, 0], pg: 2, k: [5, 0, 0] } })
  }
  return { mesh: Float32Array.from(out), faces }
}
const worldOf = (b: Brush): World => ({ doc: { entities: [{ id: 'w', brushes: [b] }] } as unknown as MapDocument, prefabs: [] })
function storeFor(b: Brush, texture = TEX, only?: (n: number[]) => boolean): LightmapStore {
  const { mesh, faces } = coreMesh(b, texture, only)
  const s = new LightmapStore()
  s.mesh = mesh; s.faces = faces; s.triangleCount = mesh.length / LIT_VERTEX_FLOATS / 3
  s.faceMaps = new Map(faces.map((f) => [`${f.i}|${f.e}|${f.b}|${f.f}`, f.m!]))
  return s
}
const withRelief = (path: string, kind: Parameters<typeof buildPreset>[0] | null): void => {
  texEnv.setProvider((p) => (p === path ? { path, width: 64, height: 64, ppm: 64, props: { ...defaultProps(), relief: kind ? buildPreset(kind, 1, 1) : null } } : undefined))
}
afterEach(() => texEnv.setProvider(null))

describe('relief in the Lit view', () => {
  it('a face with a relief gets more triangles, depth along its normal, and the lightmap of the flat face', () => {
    const b = makeBox('a', [0, 0, 0], [2, 2, 2])
    const flat = buildLitDraw(storeFor(b).mesh!, storeFor(b).faces, worldOf(b))
    withRelief(TEX, 'recess')
    const s = storeFor(b)
    const draw = buildLitDraw(s.mesh!, s.faces, worldOf(b))
    expect(draw.vertexCount).toBeGreaterThan(flat.vertexCount)
    expect(draw.vertexCount % 3).toBe(0)
    let deeper = 0
    for (let i = 0; i < draw.vertexCount; i++) {
      const o = i * LIT_TEX_FLOATS, x = draw.data[o], y = draw.data[o + 1], z = draw.data[o + 2]
      // inside the box bounds only on the sunken side: nothing leaves the box outwards for a recess
      expect(x).toBeGreaterThanOrEqual(-1e-6); expect(x).toBeLessThanOrEqual(2 + 1e-6)
      expect(y).toBeGreaterThanOrEqual(-1e-6); expect(y).toBeLessThanOrEqual(2 + 1e-6)
      expect(z).toBeGreaterThanOrEqual(-1e-6); expect(z).toBeLessThanOrEqual(2 + 1e-6)
      const inside = Math.min(x, 2 - x, y, 2 - y, z, 2 - z)
      if (inside > 1e-3) deeper++
      // page and style mask come from the flat triangle
      expect(draw.data[o + 8]).toBe(2); expect(draw.data[o + 9]).toBe(5)
    }
    expect(deeper).toBeGreaterThan(0)
  })

  it('lightmap u / v of every new vertex is the flat face\'s map of the vertex projected onto the face plane', () => {
    withRelief(TEX, 'raise')
    const b = makeBox('a', [0, 0, 0], [2, 2, 2])
    const s = storeFor(b, TEX, (n) => n[2] > 0.99) // only the +z face (z = 2)
    const draw = buildLitDraw(s.mesh!, s.faces, worldOf(b))
    expect(draw.vertexCount).toBeGreaterThan(6)
    let raised = 0
    for (let i = 0; i < draw.vertexCount; i++) {
      const o = i * LIT_TEX_FLOATS
      // the relief moves points along z only: the lightmap coordinate is that of (x, 2) whatever the height
      expect(draw.data[o + 6]).toBeCloseTo(draw.data[o] / 10, 5)
      expect(draw.data[o + 7]).toBeCloseTo(2 / 10, 5)
      if (draw.data[o + 2] > 2 + 1e-3) raised++
    }
    expect(raised).toBeGreaterThan(0)
  })

  it('the normal slot stays a unit normal on every vertex: the relief is lit by the bake (shadows, slopes), nothing is darkened here', () => {
    withRelief(TEX, 'recess')
    const b = makeBox('a', [0, 0, 0], [2, 2, 2])
    const s = storeFor(b)
    const draw = buildLitDraw(s.mesh!, s.faces, worldOf(b))
    for (let i = 0; i < draw.vertexCount; i++) { const o = i * LIT_TEX_FLOATS; expect(Math.hypot(draw.data[o + 3], draw.data[o + 4], draw.data[o + 5])).toBeCloseTo(1, 4) }
  })

  it('a texture without relief is untouched, so is a flat one', () => {
    const b = makeBox('a', [0, 0, 0], [2, 2, 2])
    const s = storeFor(b, 'dev/grey')
    expect(buildLitDraw(s.mesh!, s.faces, worldOf(b)).vertexCount).toBe(36)
    withRelief('dev/grey', null)
    expect(buildLitDraw(s.mesh!, s.faces, worldOf(b)).vertexCount).toBe(36)
  })

  it('a changed brush (overlay) gets the relief too', () => {
    withRelief(TEX, 'recess')
    const b = makeBox('a', [0, 0, 0], [2, 2, 2])
    const s = storeFor(b)
    const moved = translateBrush(b, [5, 0, 3])
    for (const f of moved.faces) f.texture = TEX
    const w = worldOf(moved)
    const a = s.staleness(w)!
    expect(a).not.toBeNull()
    const moved0 = buildLitDraw(s.mesh!, s.faces, w, a)
    expect(moved0.vertexCount).toBeGreaterThan(36)
  })
})

describe('relief in the Lighting-only view', () => {
  it('the plain mesh gets the relief (more vertices, same layout) and keeps the Core\'s when nothing has one', () => {
    const b = makeBox('a', [0, 0, 0], [2, 2, 2])
    const s0 = storeFor(b, 'dev/grey')
    expect(s0.plainMesh(worldOf(b))).toBe(s0.mesh)
    withRelief(TEX, 'recess')
    const s = storeFor(b)
    const w = worldOf(b)
    const plain = s.plainMesh(w)!
    expect(plain).not.toBe(s.mesh)
    expect(plain.length).toBeGreaterThan(36 * LIT_VERTEX_FLOATS)
    expect(plain.length % (3 * LIT_VERTEX_FLOATS)).toBe(0)
    expect(s.plainMesh(w)).toBe(plain) // cached: the renderer compares identities
    // lightmap v of the first vertex pair is still inside the face's range and the page is kept
    for (let i = 0; i < plain.length; i += LIT_VERTEX_FLOATS) { expect(plain[i + 8]).toBe(2); expect(plain[i + 9]).toBe(5) }
  })

  it('changing the relief of the texture rebuilds the plain mesh', () => {
    const b = makeBox('a', [0, 0, 0], [2, 2, 2])
    withRelief(TEX, 'recess')
    const s = storeFor(b), w = worldOf(b)
    const a = s.plainMesh(w)!
    texEnv.setProvider((p) => (p === TEX ? { path: p, width: 64, height: 64, ppm: 64, props: { ...defaultProps(), relief: buildPreset('window', 1, 1) } } : undefined))
    texEnv.bump?.()
    expect(s.plainMesh(w)).not.toBe(a)
  })
})
