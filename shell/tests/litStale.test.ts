import { describe, expect, it } from 'vitest'
import { LightmapStore, LIT_VERTEX_FLOATS } from '../src/render/lightmapStore'
import { buildLitDraw, LIT_TEX_FLOATS } from '../src/render/litTextured'
import { makeBox, translateBrush } from '../src/model/geometry'
import { triangulateFace, faceNormal } from '../src/model/geometry'
import type { LitFace } from '../src/net/protocol'
import type { World } from '../src/model/derived'
import type { Brush, MapDocument } from '../src/model/document'

// the Core's mesh for a box: one piece (2 triangles) per face, normal = face normal, lightmap uv = (x / 10, z / 10) on page 0
// `back`: the pieces are cut walls (the Core's FaceSide::Back): the subtractor's face seen from inside the carved room, normal and winding reversed.
function coreMesh(b: Brush, back = false): { mesh: Float32Array; faces: LitFace[] } {
  const out: number[] = [], faces: LitFace[] = []
  for (const f of b.faces) {
    const fn = faceNormal(b, f), tris = triangulateFace(b, f)
    const n: [number, number, number] = back ? [-fn[0], -fn[1], -fn[2]] : fn
    for (const t of tris) for (const i of (back ? [t[0], t[2], t[1]] : t)) { const v = b.vertices[i]; out.push(...v, ...n, v[0] / 10, v[2] / 10, 0, 1, 0, 0) }
    faces.push({ i: '', e: 'w', bi: '', b: b.id, f: f.id, t: 'dev/grey', n: tris.length, m: { u: [0.1, 0, 0, 0], v: [0, 0, 0.1, 0], pg: 0, k: [1, 0, 0] } })
  }
  return { mesh: Float32Array.from(out), faces }
}
const worldOf = (b: Brush): World => ({ doc: { entities: [{ id: 'w', brushes: [b] }] } as unknown as MapDocument, prefabs: [] })
function storeFor(b: Brush, back = false): LightmapStore {
  const { mesh, faces } = coreMesh(b, back)
  const s = new LightmapStore()
  s.mesh = mesh; s.faces = faces; s.triangleCount = mesh.length / LIT_VERTEX_FLOATS / 3
  s.faceMaps = new Map(faces.map((f) => [`${f.i}|${f.e}|${f.b}|${f.f}`, f.m!]))
  return s
}

describe('lit view of a brush that changed since the Core mesh', () => {
  it('an unchanged brush is drawn from the mesh', () => {
    const b = makeBox('a', [0, 0, 0], [2, 2, 2])
    expect(storeFor(b).staleness(worldOf(b))).toBeNull()
  })

  it('the triangles of a misc_model belong to no brush of the document and are never stale', () => {
    const b = makeBox('a', [0, 0, 0], [2, 2, 2])
    const s = storeFor(b)
    const tri = [0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 0.1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0.1, 0, 1, 0, 0] // three vertices, 12 floats each
    s.mesh = Float32Array.from([...Array.from(s.mesh!), ...tri])
    s.faces = [...s.faces, { i: '', e: 'm', bi: '', b: 'model', f: 0, t: '/models/x', n: 1 }]
    expect(s.staleness(worldOf(b))).toBeNull()
  })

  it('a moved brush is drawn at its new position with the light it had (lightmap uv follows the brush, positions do not)', () => {
    const b = makeBox('a', [0, 0, 0], [2, 2, 2])
    const s = storeFor(b)
    const moved = translateBrush(b, [5, 0, 3])
    const w = worldOf(moved)
    const a = s.staleness(w)!
    expect(a.stale.has('|a')).toBe(true)
    const draw = buildLitDraw(s.mesh!, s.faces, w, a)
    expect(draw.vertexCount).toBe(36) // nothing doubled, nothing lost
    let minX = Infinity, maxX = -Infinity
    for (let i = 0; i < draw.vertexCount; i++) {
      const o = i * LIT_TEX_FLOATS
      minX = Math.min(minX, draw.data[o]); maxX = Math.max(maxX, draw.data[o])
      // lightmap uv is that of the original position: x / 10 with x in 0..2 (the position minus the shift of 5)
      expect(draw.data[o + 6]).toBeCloseTo((draw.data[o] - 5) / 10, 5)
      expect(draw.data[o + 7]).toBeCloseTo((draw.data[o + 2] - 3) / 10, 5)
    }
    expect([minX, maxX]).toEqual([5, 7])
    // the Lighting-only mesh has the same content
    expect(s.plainMesh(w)!.length).toBe(36 * LIT_VERTEX_FLOATS)
  })

  it('a deleted brush disappears at once, and a new mesh (matching again) is used as is', () => {
    const b = makeBox('a', [0, 0, 0], [2, 2, 2])
    const s = storeFor(b)
    const gone: World = { doc: { entities: [{ id: 'w', brushes: [] }] } as unknown as MapDocument, prefabs: [] }
    expect(buildLitDraw(s.mesh!, s.faces, gone, s.staleness(gone)).vertexCount).toBe(0)
    const moved = translateBrush(b, [5, 0, 0])
    const s2 = storeFor(moved) // the Core caught up
    expect(s2.staleness(worldOf(moved))).toBeNull()
  })

  it('a resized brush is stale too', () => {
    const b = makeBox('a', [0, 0, 0], [2, 2, 2])
    const s = storeFor(b)
    expect(s.staleness(worldOf(makeBox('a', [0, 0, 0], [3, 2, 2])))).not.toBeNull()
  })
})

describe('subtractive brushes in the lit view', () => {
  it('a moved subtractor adds no lit surfaces of its own (its volume is tooling)', () => {
    const b: Brush = { ...makeBox('cut', [0, 0, 0], [2, 2, 2]), op: 'subtract' }
    const s = storeFor(b)
    const moved: Brush = { ...translateBrush(b, [5, 0, 3]), op: 'subtract' }
    const w = worldOf(moved)
    const a = s.staleness(w)!
    expect(a.stale.has('|cut')).toBe(true) // the old cut walls are out of date
    expect(a.overlay).toEqual([]) // nothing is drawn lit in their place
    expect(buildLitDraw(s.mesh!, s.faces, w, a).vertexCount).toBe(0)
    expect(s.plainMesh(w)!.length).toBe(0)
  })
  it('the walls a subtractor cut (reversed normals) are unchanged while it stays, and are drawn', () => {
    // regression: they were compared with the authored normal unsigned, looked changed at once, and the subtractor fix then dropped them: carved geometry vanished in Lit
    const b: Brush = { ...makeBox('cut', [0, 0, 0], [2, 2, 2]), op: 'subtract' }
    const s = storeFor(b, true)
    const w = worldOf(b)
    expect(s.staleness(w)).toBeNull()
    const draw = buildLitDraw(s.mesh!, s.faces, w, s.staleness(w))
    expect(draw.vertexCount).toBe(36)
    expect(s.plainMesh(w)!.length).toBe(36 * LIT_VERTEX_FLOATS)
  })
  it('cut walls follow a subtractor that moved: they go stale (the next mesh replaces them) and nothing is drawn lit in their place', () => {
    const b: Brush = { ...makeBox('cut', [0, 0, 0], [2, 2, 2]), op: 'subtract' }
    const s = storeFor(b, true)
    const moved: Brush = { ...translateBrush(b, [0, 0, 3]), op: 'subtract' }
    const a = s.staleness(worldOf(moved))!
    expect(a.stale.has('|cut')).toBe(true)
    expect(a.overlay).toEqual([])
    // a subtractor that only turned (same planes, other normals) is changed as well
    const turned: Brush = { ...makeBox('cut', [0, 0, 0], [2, 2, 5]), op: 'subtract' }
    expect(s.staleness(worldOf(turned))!.stale.has('|cut')).toBe(true)
  })
  it('an ordinary brush still gets its overlay', () => {
    const b = makeBox('a', [0, 0, 0], [2, 2, 2])
    const s = storeFor(b)
    expect(s.staleness(worldOf(translateBrush(b, [5, 0, 3])))!.overlay.length).toBeGreaterThan(0)
  })
})
