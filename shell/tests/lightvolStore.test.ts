import { describe, expect, it } from 'vitest'
import { LightvolStore, entityLightKey, fromByte } from '../src/render/lightvolStore'
import { LightmapStore, LIT_VERTEX_FLOATS } from '../src/render/lightmapStore'
import { buildLitDraw, LIT_TEX_FLOATS } from '../src/render/litTextured'
import { parseInbound } from '../src/net/protocol'
import { emptyDocument } from '../src/model/document'
import { makeBox } from '../src/model/geometry'
import { defaultProjection } from '../src/model/uv'

const b64 = (bytes: Uint8Array) => btoa(String.fromCharCode(...bytes))

describe('light volume store', () => {
  it('decodes probes with the x2 range: byte 255 = 2.0, direction from lat / lng, validity from the flags', () => {
    const s = new LightvolStore()
    // probe 0: ambient (255, 128, 0), directed (0, 0, 255), lat 0 (straight up), valid. probe 1: invalid.
    const bytes = new Uint8Array([255, 128, 0, 0, 0, 255, 0, 0, 1, 0, 0, 0, 0, 0, 0, 128, 128, 0])
    expect(s.applyGrid({ type: 'lightvol', rev: 4, origin: [1, 2, 3], cell: 0.5, dims: [2, 1, 1], probes: b64(bytes) })).toBe(true)
    expect(s.ready).toBe(true)
    expect(s.probeCount).toBe(2)
    const p0 = s.probe(0), p1 = s.probe(1)
    expect(p0[0]).toBeCloseTo(2)
    expect(p0[1]).toBeCloseTo(fromByte(128))
    expect(p0[5]).toBeCloseTo(2)
    expect([p0[6], p0[7], p0[8]].map((v) => +v.toFixed(3))).toEqual([0, 1, 0])
    expect(p0[9]).toBe(1)
    expect(p1[9]).toBe(0)
    expect(s.valid(0)).toBe(true)
    expect(s.valid(1)).toBe(false)
    expect(s.origin).toEqual([1, 2, 3])
  })

  it('rejects a grid whose byte count does not match its dimensions', () => {
    const s = new LightvolStore()
    expect(s.applyGrid({ type: 'lightvol', rev: 1, origin: [0, 0, 0], cell: 1, dims: [2, 2, 2], probes: b64(new Uint8Array(9)) })).toBe(false)
    expect(s.ready).toBe(false)
  })

  it('keeps the light of an entity per instance and entity id, and clears', () => {
    const s = new LightvolStore()
    s.applyEntities({ type: 'entity_light', rev: 2, entities: [
      { i: '', e: 'door', dir: [0, 1, 0], lights: [{ s: 0, ambient: [0.1, 0.1, 0.1], directed: [1, 1, 1], dir: [0, 1, 0] }, { s: 5, ambient: [0, 0, 0], directed: [2, 2, 2], dir: [0, 1, 0] }] },
      { i: 'p1', e: 'door', dir: [1, 0, 0], lights: [{ s: 0, ambient: [0, 0, 0], directed: [0, 0, 0], dir: [1, 0, 0] }] }] })
    expect(s.entity('', 'door')?.map((l) => l.s)).toEqual([0, 5])  // one probe per light style layer
    expect(s.entity('', 'door')?.[0].directed).toEqual([1, 1, 1])
    expect(s.entity('', 'door')?.[1].directed).toEqual([2, 2, 2])
    expect(s.entity('p1', 'door')?.[0].dir).toEqual([1, 0, 0])
    expect(s.entity('', 'nope')).toBeUndefined()
    expect(entityLightKey('a', 'b')).toBe('a|b')
    s.clear()
    expect(s.entity('', 'door')).toBeUndefined()
  })
})

describe('protocol: lit mesh faces, lightvol, entity light', () => {
  it('keeps well formed faces and drops malformed ones', () => {
    const m = parseInbound(JSON.stringify({ type: 'lit_mesh', rev: 1, atlas: 8, pageCount: 1, triangleCount: 1, vertices: '', faces: [{ i: '', e: 'w', bi: '', b: 'x', f: 3, t: 'dev/grey', n: 1 }, { e: 'bad' }] })) as any
    expect(m.faces).toHaveLength(1)
    expect(m.faces[0].t).toBe('dev/grey')
  })
  it('a lit mesh without a face table (older Core) still parses', () => {
    const m = parseInbound(JSON.stringify({ type: 'lit_mesh', rev: 1, atlas: 8, pageCount: 1, triangleCount: 0, vertices: '' })) as any
    expect(m.faces).toEqual([])
  })
  it('keeps the other style layers of the grid', () => {
    const s = new LightvolStore()
    const one = (b: number[]) => b64(new Uint8Array(b))
    expect(s.applyGrid({ type: 'lightvol', rev: 1, origin: [0, 0, 0], cell: 1, dims: [1, 1, 1], probes: one([10, 0, 0, 0, 0, 0, 0, 0, 1]), layers: [{ s: 7, probes: one([255, 0, 0, 0, 0, 0, 0, 0, 1]) }] })).toBe(true)
    expect(s.probe(0)[0]).toBeCloseTo(fromByte(10))
    expect(s.probe(0, 7)[0]).toBeCloseTo(2)
    expect(s.probe(0, 99)[9]).toBe(0)  // no such layer
    expect(s.applyGrid({ type: 'lightvol', rev: 2, origin: [0, 0, 0], cell: 1, dims: [1, 1, 1], probes: one([0, 0, 0, 0, 0, 0, 0, 0, 1]), layers: [{ s: 7, probes: one([1, 2]) }] })).toBe(false) // wrong size
  })

  it('rejects malformed lightvol and entity_light', () => {
    expect(parseInbound(JSON.stringify({ type: 'lightvol', rev: 1, origin: [0, 0], cell: 1, dims: [1, 1, 1], probes: '' }))).toBeNull()
    expect(parseInbound(JSON.stringify({ type: 'lightvol', rev: 1, origin: [0, 0, 0], cell: 0, dims: [1, 1, 1], probes: '' }))).toBeNull()
    expect(parseInbound(JSON.stringify({ type: 'entity_light', rev: 1 }))).toBeNull()
    const ok = parseInbound(JSON.stringify({ type: 'entity_light', rev: 1, entities: [{ i: '', e: 'd', dir: [0, 1, 0], lights: [{ s: 0, ambient: [0, 0, 0], directed: [0, 0, 0] }, { s: 4, ambient: [0, 0, 0], directed: [1, 1, 1] }] }, { e: 'x' }, { i: '', e: 'y', dir: [0, 1, 0], lights: [] }, { i: '', e: 'nodir', lights: [{ s: 0, ambient: [0, 0, 0], directed: [0, 0, 0] }] }] })) as any
    expect(ok.entities).toHaveLength(1)
    // ONE direction per entity (REQUIREMENTS §27 G4): every layer carries the entity's
    expect(ok.entities[0].lights.map((l: any) => l.dir)).toEqual([[0, 1, 0], [0, 1, 0]])
  })
})

describe('textured lit mesh', () => {
  // one quad on the floor of a box: two triangles; the Core sends pos, normal, lm u, v, page per vertex
  const vert = (x: number, z: number, lu: number, lv: number) => [x, 0, z, 0, 1, 0, lu, lv, 0, 4, 0, 0] // + the style mask (style 3 = bit 2)
  const quad = new Float32Array([...vert(0, 0, 0, 0), ...vert(4, 0, 1, 0), ...vert(4, 4, 1, 1), ...vert(0, 0, 0, 0), ...vert(4, 4, 1, 1), ...vert(0, 4, 0, 1)])
  const doc = emptyDocument()
  const box = makeBox('b', [0, -1, 0], [4, 0, 4], 'brick/red')
  doc.entities.push({ id: 'w', classname: 'worldspawn', brushes: [box] } as any)
  const top = box.faces.find((f) => Math.abs(f.id) >= 0 && f.loop.length === 4 && box.vertices[f.loop[0]][1] === 0 && box.vertices[f.loop[1]][1] === 0)!

  it('groups by texture and appends the albedo UV projected with the parent face', () => {
    const d = buildLitDraw(quad, [{ i: '', e: 'w', bi: '', b: 'b', f: top.id, t: 'brick/red', n: 2 }], { doc, prefabs: [] })
    expect(d.vertexCount).toBe(6)
    expect(d.groups).toEqual([{ texture: 'brick/red', first: 0, count: 6 }])
    const proj = defaultProjection() // 2 m per repeat: 4 m wide = 2 repeats
    const at = (v: number) => [d.data[v * LIT_TEX_FLOATS + 12], d.data[v * LIT_TEX_FLOATS + 13]]
    expect(Math.abs(at(1)[0] - at(0)[0])).toBeCloseTo(4 / proj.scale[0])
    expect(d.data[6]).toBe(0) // the lightmap u of vertex 0 is kept
    expect(d.data[LIT_TEX_FLOATS + 6]).toBe(1)
    expect(d.data[9]).toBe(4) // and the style mask too
  })

  it('splits per texture and ignores a face table that runs past the mesh', () => {
    const faces = [{ i: '', e: 'w', bi: '', b: 'b', f: top.id, t: 'a', n: 1 }, { i: '', e: 'w', bi: '', b: 'b', f: top.id, t: 'b', n: 1 }, { i: '', e: 'w', bi: '', b: 'b', f: top.id, t: 'c', n: 5 }]
    const d = buildLitDraw(quad, faces, { doc, prefabs: [] })
    expect(d.groups.map((g) => [g.texture, g.first, g.count])).toEqual([['a', 0, 3], ['b', 3, 3]])
    expect(d.vertexCount).toBe(6)
  })

  it('a missing parent face still draws (default projection)', () => {
    const d = buildLitDraw(quad, [{ i: '', e: 'w', bi: '', b: 'gone', f: 9, t: 'x', n: 2 }], { doc, prefabs: [] })
    expect(d.vertexCount).toBe(6)
  })
})

describe('lightmap store: faces and lightmapped entities', () => {
  it('remembers which entities have surfaces in the mesh, rebuilds the draw only when something changed', () => {
    const s = new LightmapStore()
    const doc = emptyDocument()
    const prefabs: any[] = []
    const verts = new Float32Array(3 * LIT_VERTEX_FLOATS)
    s.applyMesh({ type: 'lit_mesh', rev: 1, atlas: 8, pageCount: 1, triangleCount: 1, vertices: b64(new Uint8Array(verts.buffer)), faces: [{ i: 'p', e: 'w', bi: '', b: 'x', f: 1, t: 't', n: 1 }] })
    expect(s.litEntities.has('p|w')).toBe(true)
    expect(s.litEntities.has('|door')).toBe(false)
    const a = s.litDraw({ doc, prefabs })
    expect(a).not.toBeNull()
    expect(s.litDraw({ doc, prefabs })).toBe(a)
    expect(s.litDraw({ doc: { ...doc }, prefabs })).not.toBe(a)
    s.clear()
    expect(s.litEntities.size).toBe(0)
    expect(s.litDraw({ doc, prefabs })).toBeNull()
  })
})
