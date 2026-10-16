import { describe, expect, it } from 'vitest'
import { buildEntityDecals, buildLitDecals, LIT_TEX_FLOATS } from '../src/render/litTextured'
import { LightmapStore, LIT_VERTEX_FLOATS, faceMapKey } from '../src/render/lightmapStore'
import { parseInbound } from '../src/net/protocol'
import type { DecalEntry } from '../src/model/derived'
import type { LitFaceMap } from '../src/net/protocol'

const b64 = (u: Uint8Array) => btoa(String.fromCharCode(...u))
// a decal on the floor: a 2 x 2 quad at y = 0 with its own texture coordinates
const quad = [0, 0, 0, 2, 0, 0, 2, 0, 2, 0, 0, 2]
const uv = [0, 0, 1, 0, 1, 1, 0, 1]
const decal = (face: number, entityId = 'w', instance = ''): DecalEntry => ({ decalId: 'd', instance: '', texture: 'decals/blood', fragments: [{ entityId, instance, brush: 'floor', face, v: quad, uv }] })
// lightmap uv = (x / 8 + 0.25, z / 8 + 0.5) on page 1, style layers 3 and 5
const map: LitFaceMap = { u: [1 / 8, 0, 0, 0.25], v: [0, 0, 1 / 8, 0.5], pg: 1, k: [4 + 16, 0, 0] }

describe('Lit view decals', () => {
  it('a decal on a lightmapped face takes that face\'s lightmap coordinates, page and style mask; its albedo uv stays the projector\'s', () => {
    const maps = new Map([[faceMapKey('', 'w', 'floor', 4), map]])
    const d = buildLitDecals([decal(4)], maps)
    expect(d.vertexCount).toBe(6) // a quad = 2 triangles
    expect(d.groups).toEqual([{ texture: 'decals/blood', first: 0, count: 6 }])
    const v = (i: number) => Array.from(d.data.subarray(i * LIT_TEX_FLOATS, (i + 1) * LIT_TEX_FLOATS))
    const p0 = v(0), p1 = v(1) // corner (0,0,0) and (2,0,0)
    expect(p0.slice(0, 3)).toEqual([0, 0, 0])
    expect(Math.abs(p0[4])).toBeCloseTo(1) // the polygon's unit normal (its sign follows the winding)
    expect(Math.hypot(p0[3], p0[4], p0[5])).toBeCloseTo(1)
    expect(p0[6]).toBeCloseTo(0.25)
    expect(p0[7]).toBeCloseTo(0.5)
    expect(p1[6]).toBeCloseTo(0.5)   // x = 2 -> 2/8 + 0.25
    expect(p0[8]).toBe(1)             // page
    expect(p0.slice(9, 12)).toEqual([20, 0, 0]) // style mask of the face
    expect(p0.slice(12)).toEqual([0, 0])
    expect(p1.slice(12)).toEqual([1, 0])
  })

  it('fragments on faces without a lightmap are not in the lightmapped draw, but in the per-entity draw (lit from the light volume)', () => {
    const maps = new Map([[faceMapKey('', 'w', 'floor', 4), map]])
    const door = decal(7, 'door')
    expect(buildLitDecals([door], maps).vertexCount).toBe(0)
    const e = buildEntityDecals([decal(4), door], maps)
    expect(e.groups).toEqual([{ instance: '', entityId: 'door', texture: 'decals/blood', first: 0, count: 6 }])
    expect(e.vertexCount).toBe(6)
    expect(Array.from(e.data.subarray(6, 8))).toEqual([0, 0]) // position, normal, uv per vertex: uv of the first corner
  })

  it('prefab instances are told apart (the same entity id in two instances)', () => {
    const maps = new Map([[faceMapKey('p1', 'w', 'floor', 4), map]])
    expect(buildLitDecals([decal(4, 'w', 'p1')], maps).vertexCount).toBe(6)
    expect(buildLitDecals([decal(4, 'w', 'p2')], maps).vertexCount).toBe(0)
  })

  it('degenerate fragments are skipped', () => {
    const maps = new Map([[faceMapKey('', 'w', 'floor', 4), map]])
    const bad: DecalEntry = { decalId: 'd', instance: '', texture: 't', fragments: [{ entityId: 'w', instance: '', brush: 'floor', face: 4, v: [0, 0, 0, 1, 0, 0], uv: [0, 0, 1, 0] }] }
    expect(buildLitDecals([bad], maps).vertexCount).toBe(0)
    expect(buildEntityDecals([bad], new Map()).vertexCount).toBe(0)
  })
})

describe('lit mesh face maps', () => {
  const mesh = (faces: unknown[]) => ({ type: 'lit_mesh' as const, rev: 1, atlas: 8, pageCount: 1, triangleCount: 1, vertices: b64(new Uint8Array(new Float32Array(3 * LIT_VERTEX_FLOATS).buffer)), faces: faces as any })
  it('the store keeps the mapping of each face once, keyed instance|entity|brush|face', () => {
    const s = new LightmapStore()
    s.applyMesh(mesh([{ i: '', e: 'w', bi: '', b: 'floor', f: 4, t: 't', n: 1, m: map }, { i: '', e: 'w', bi: '', b: 'floor', f: 4, t: 't', n: 1 }]))
    expect(s.faceMaps.size).toBe(1)
    expect(s.faceMaps.get('|w|floor|4')).toEqual(map)
    s.clear()
    expect(s.faceMaps.size).toBe(0)
  })
  it('protocol keeps well formed maps and drops malformed ones (the face itself stays)', () => {
    const m = parseInbound(JSON.stringify({ type: 'lit_mesh', rev: 1, atlas: 8, pageCount: 1, triangleCount: 0, vertices: '', faces: [
      { i: '', e: 'w', bi: '', b: 'a', f: 1, t: 't', n: 1, m: map }, { i: '', e: 'w', bi: '', b: 'b', f: 2, t: 't', n: 1, m: { u: [1], v: [], pg: 0, k: [0, 0, 0] } }] })) as any
    expect(m.faces).toHaveLength(2)
    expect(m.faces[0].m.pg).toBe(1)
    expect(m.faces[1].m).toBeUndefined()
  })
})
