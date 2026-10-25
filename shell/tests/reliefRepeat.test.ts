// A texture's relief follows the texture's repeat behaviour (mirror, anchor, alternate texture, edge clamp): the relief of a tile is placed by the same
// texture coordinates the picture is, in the Unlit view (plain and carved brushes) and in the Lit and Lighting-only views.
import { afterEach, describe, expect, it } from 'vitest'
import { buildCarvedMesh } from '../src/model/derived'
import type { DerivedEntry, World } from '../src/model/derived'
import { makeBox, faceNormal, triangulateFace } from '../src/model/geometry'
import type { Brush, MapDocument } from '../src/model/document'
import type { Relief } from '../src/model/relief'
import { AxisRepeat, TextureProps, defaultAxis, defaultProps, texEnv } from '../src/model/texprops'
import { planForFace } from '../src/model/uvmesh'
import { planHasRelief, planWithRelief } from '../src/model/reliefApply'
import { LightmapStore, LIT_VERTEX_FLOATS } from '../src/render/lightmapStore'
import { buildLitDraw, LIT_TEX_FLOATS } from '../src/render/litTextured'
import type { LitFace } from '../src/net/protocol'

afterEach(() => texEnv.setProvider(null))

const H = 0.2
/** One slope: depth H * x across the repeat (x = the relief's horizontal axis = u). Not symmetric, so mirroring it is visible. */
const ramp: Relief = { vertices: [[0, 0, 0], [1, 0, H], [1, 1, H], [0, 1, 0]], faces: [[0, 1, 2, 3]] }
const axis = (o: Partial<AxisRepeat>): AxisRepeat => ({ ...defaultAxis('start'), ...o })
interface Tex { relief?: Relief | null; u?: Partial<AxisRepeat>; v?: Partial<AxisRepeat> }
const provide = (textures: Record<string, Tex>): void => {
  texEnv.setProvider((path) => {
    const t = textures[path]
    if (!t) return undefined
    const props: TextureProps = { ...defaultProps(), relief: t.relief ?? null, repeat: { u: axis(t.u ?? {}), v: axis({ ...defaultAxis('end'), ...(t.v ?? {}) }) } }
    return { path, width: 64, height: 64, ppm: 64, props } // natural size 1 m x 1 m
  })
}

const topOf = (b: Brush) => b.faces.find((f) => faceNormal(b, f)[2] > 0.99)!
/** The +z face of a box x0..x1, 0..1, 0..1 (z = 1) as a carved fragment. */
function carved(x0: number, x1: number, texture: string): Map<string, number[]> {
  const b = makeBox('b', [x0, 0, 0], [x1, 1, 1], texture)
  const f = topOf(b)
  const entry = { entityId: 'w', brushId: 'b', instance: '', subtractors: [], fragments: [{ brush: 'b', face: f.id, v: f.loop.flatMap((i) => b.vertices[i]) }] } as unknown as DerivedEntry
  const world: World = { doc: { entities: [{ id: 'w', brushes: [b] }] } as unknown as MapDocument, prefabs: [] }
  return buildCarvedMesh(entry, world).textures
}
interface V { x: number; d: number; u: number }
/** The relief surface (what faces up): the connecting walls at the face's outline and between repeats (reliefApply.ts) stand upright and are left out. */
const verts = (a: number[] | undefined, z0 = 1): V[] => {
  const o: V[] = []
  for (let i = 0; a && i < a.length; i += 8) if (a[i + 5] > 1e-6) o.push({ x: a[i], d: a[i + 2] - z0, u: a[i + 6] })
  return o
}
const near = (v: V[], x: number): V[] => v.filter((p) => Math.abs(p.x - x) < 1e-6)

describe('relief follows the repeat behaviour of its texture', () => {
  it('repeat: the relief restarts at every tile (a step at the joint)', () => {
    provide({ t: { relief: ramp } })
    const v = verts(carved(0, 2, 't').get('t'))
    expect(v.length).toBeGreaterThan(6)
    const joint = near(v, 1).map((p) => Math.round(p.d * 1e6) / 1e6)
    expect(joint).toContain(0); expect(joint).toContain(H) // end of tile 0, start of tile 1
  })

  it('mirror: every second tile is mirrored with its relief, so the joint is continuous', () => {
    provide({ t: { relief: ramp, u: { mode: 'mirror' } } })
    const v = verts(carved(0, 2, 't').get('t'))
    expect(v.length).toBeGreaterThan(6)
    // the relief rises to H at the joint from both sides and falls back to 0 at the far end; plain repeat would end at H there
    for (const p of near(v, 1)) expect(p.d).toBeCloseTo(H, 6)
    const ends = [...near(v, 0), ...near(v, 2)].map((p) => Math.round(p.d * 1e6) / 1e6)
    expect(ends.filter((d) => d === H).length).toBeLessThan(ends.length) // one end is at 0
    expect(Math.min(...ends)).toBe(0)
    for (const p of v) expect(p.d).toBeGreaterThanOrEqual(-1e-9)
  })

  it('anchor start: the relief is glued to the anchored face edge whatever the face position', () => {
    // face from x = 0.5: a plain repeat starts mid tile (depth H / 2 at the edge), an anchored one at the tile start (depth 0)
    provide({ t: { relief: ramp } })
    const plain = verts(carved(0.5, 3, 't').get('t'))
    const left = (v: V[]) => v.reduce((a, b) => (b.u < a.u ? b : a))
    expect(left(plain).d).toBeCloseTo(H * 0.5, 6)
    provide({ t: { relief: ramp, u: { mode: 'anchor', anchor: 'start', split: 0.5, fill: 'repeat' } } })
    const anch = verts(carved(0.5, 3, 't').get('t'))
    expect(left(anch).u).toBeCloseTo(0, 6)
    expect(left(anch).d).toBeCloseTo(0, 6)
  })

  it('alternate texture: the band takes the alternate texture\'s relief, the fixed part keeps its own (here none)', () => {
    provide({ a: { u: { mode: 'anchor', anchor: 'start', split: 0.5, fill: 'repeat', alt: { texture: 'b', offset: [0, 0] } } }, b: { relief: ramp } })
    const b = makeBox('b', [0, 0, 0], [3, 1, 1], 'a')
    expect(planHasRelief(planForFace(b, topOf(b))!)).toBe(true) // the face's own texture has no relief, the alternate has
    const m = carved(0, 3, 'a')
    const fixed = verts(m.get('a')), band = verts(m.get('b'))
    expect(fixed.length).toBeGreaterThan(0); expect(band.length).toBeGreaterThan(6)
    for (const p of fixed) expect(p.d).toBeCloseTo(0, 9)
    expect(Math.max(...band.map((p) => p.d))).toBeCloseTo(H, 6)
  })

  it('an alternate texture without relief and a face without any relief stay flat and cost nothing', () => {
    provide({ a: { u: { mode: 'anchor', anchor: 'start', split: 0.5, fill: 'repeat', alt: { texture: 'b', offset: [0, 0] } } }, b: {} })
    const b = makeBox('b', [0, 0, 0], [3, 1, 1], 'a')
    expect(planHasRelief(planForFace(b, topOf(b))!)).toBe(false)
    for (const list of carved(0, 3, 'a').values()) for (const p of verts(list)) expect(p.d).toBeCloseTo(0, 9)
  })

  it('edge clamp: each tile\'s relief is cut where the tile is inset from its border', () => {
    provide({ t: { relief: ramp } })
    const plain = verts(carved(0, 2, 't').get('t')).map((p) => p.d)
    expect(Math.min(...plain)).toBeCloseTo(0, 6); expect(Math.max(...plain)).toBeCloseTo(H, 6)
    provide({ t: { relief: ramp, u: { edge: 'clamp' } } })
    const clamped = verts(carved(0, 2, 't').get('t')).map((p) => p.d)
    expect(clamped.length).toBeGreaterThan(6)
    expect(Math.min(...clamped)).toBeGreaterThan(1e-3) // the first texel row of every tile is not shown: depth never reaches 0 ...
    expect(Math.max(...clamped)).toBeLessThan(H - 1e-3) // ... nor H
  })

  it('planWithRelief draws a piece flat when its texture has no relief, and keeps the winding and normal', () => {
    provide({ a: { u: { mode: 'mirror' } } })
    const b = makeBox('b', [0, 0, 0], [2, 1, 1], 'a')
    const f = topOf(b), plan = planForFace(b, f)!
    expect(planHasRelief(plan)).toBe(false)
    const out = new Map<string, number[]>()
    planWithRelief(plan, triangulateFace(b, f).map(([i, j, k]) => [b.vertices[i], b.vertices[j], b.vertices[k]] as [number[], number[], number[]] as never), [0, 0, 1], out)
    for (const p of verts(out.get('a'))) expect(p.d).toBeCloseTo(0, 9)
  })
})

// ---- Lit / Lighting-only ---------------------------------------------------------------------------------------------------------------------------
function coreMesh(b: Brush, texture: string): { mesh: Float32Array; faces: LitFace[] } {
  const out: number[] = [], faces: LitFace[] = []
  const f = topOf(b), n = faceNormal(b, f), tris = triangulateFace(b, f)
  for (const t of tris) for (const i of t) { const v = b.vertices[i]; out.push(...v, ...n, v[0] / 10, v[2] / 10 + v[1] / 100, 3, 5, 0, 0) } // lightmap uv affine in the position
  faces.push({ i: '', e: 'w', bi: '', b: b.id, f: f.id, t: texture, n: tris.length, m: { u: [0.1, 0, 0, 0], v: [0, 0.01, 0.1, 0], pg: 3, k: [5, 0, 0] } })
  return { mesh: Float32Array.from(out), faces }
}
function litStore(b: Brush, texture: string): LightmapStore {
  const { mesh, faces } = coreMesh(b, texture)
  const s = new LightmapStore()
  s.mesh = mesh; s.faces = faces; s.triangleCount = mesh.length / LIT_VERTEX_FLOATS / 3
  s.faceMaps = new Map(faces.map((f) => [`${f.i}|${f.e}|${f.b}|${f.f}`, f.m!]))
  return s
}
const worldOf = (b: Brush): World => ({ doc: { entities: [{ id: 'w', brushes: [b] }] } as unknown as MapDocument, prefabs: [] })

describe('relief follows the repeat behaviour in the lit views', () => {
  it('Lit: mirrored tiles carry a mirrored relief, lightmap coordinates stay the flat face\'s map, page and mask are kept', () => {
    provide({ t: { relief: ramp, u: { mode: 'mirror' } } })
    const b = makeBox('b', [0, 0, 0], [2, 1, 1], 't')
    const s = litStore(b, 't')
    const draw = buildLitDraw(s.mesh!, s.faces, worldOf(b))
    expect(draw.vertexCount).toBeGreaterThan(6)
    // the outline of the face is closed down to the surface (depth 0 at the ends of the ramp's high side)
    const walls: number[] = []
    for (let i = 0; i < draw.vertexCount; i++) if (Math.abs(draw.data[i * LIT_TEX_FLOATS + 5]) < 1e-6) walls.push(draw.data[i * LIT_TEX_FLOATS + 2] - 1)
    expect(walls.length).toBeGreaterThan(0)
    expect(Math.min(...walls)).toBeCloseTo(0, 6); expect(Math.max(...walls)).toBeCloseTo(H, 6)
    const d = (o: number) => draw.data[o + 2] - 1
    for (let i = 0; i < draw.vertexCount; i++) {
      const o = i * LIT_TEX_FLOATS, x = draw.data[o], y = draw.data[o + 1]
      expect(draw.data[o + 6]).toBeCloseTo(x / 10, 5)
      expect(draw.data[o + 7]).toBeCloseTo(0.1 + y / 100, 5) // v = z / 10 + y / 100 on the flat face z = 1
      expect(draw.data[o + 8]).toBe(3); expect(draw.data[o + 9]).toBe(5)
      const wall = Math.abs(draw.data[o + 5]) < 1e-6 // connecting walls at the face's outline stand upright and reach down to the surface
      if (Math.abs(x - 1) < 1e-6 && !wall) expect(d(o)).toBeCloseTo(H, 6) // continuous at the mirror joint
    }
    // one of the two ends sits at depth 0 (mirrored), plain repeat would put H at both
    const ends: number[] = []
    for (let i = 0; i < draw.vertexCount; i++) { const o = i * LIT_TEX_FLOATS; if ((Math.abs(draw.data[o]) < 1e-6 || Math.abs(draw.data[o] - 2) < 1e-6) && Math.abs(draw.data[o + 5]) > 1e-6) ends.push(Math.round(d(o) * 1e6) / 1e6) }
    expect(Math.min(...ends)).toBe(0)
  })

  it('Lighting-only: the same relief on the plain mesh', () => {
    provide({ t: { relief: ramp, u: { mode: 'mirror' } } })
    const b = makeBox('b', [0, 0, 0], [2, 1, 1], 't')
    const s = litStore(b, 't')
    const plain = s.plainMesh(worldOf(b))!
    expect(plain).not.toBe(s.mesh)
    let joint = 0
    for (let i = 0; i < plain.length; i += LIT_VERTEX_FLOATS) {
      if (Math.abs(plain[i] - 1) < 1e-6 && Math.abs(plain[i + 5]) > 1e-6) { expect(plain[i + 2] - 1).toBeCloseTo(H, 6); joint++ }
      expect(plain[i + 8]).toBe(3)
    }
    expect(joint).toBeGreaterThan(0)
  })

  it('Lit: the band of an alternate texture with a relief lands in the alternate texture\'s group', () => {
    provide({ a: { u: { mode: 'anchor', anchor: 'start', split: 0.5, fill: 'repeat', alt: { texture: 'b', offset: [0, 0] } } }, b: { relief: ramp } })
    const b = makeBox('b', [0, 0, 0], [3, 1, 1], 'a')
    const s = litStore(b, 'a')
    const draw = buildLitDraw(s.mesh!, s.faces, worldOf(b))
    const grp = (t: string) => draw.groups.find((g) => g.texture === t)
    expect(grp('a')).toBeDefined(); expect(grp('b')).toBeDefined()
    let max = 0
    for (let i = grp('b')!.first; i < grp('b')!.first + grp('b')!.count; i++) max = Math.max(max, draw.data[i * LIT_TEX_FLOATS + 2] - 1)
    expect(max).toBeCloseTo(H, 6)
    for (let i = grp('a')!.first; i < grp('a')!.first + grp('a')!.count; i++) expect(draw.data[i * LIT_TEX_FLOATS + 2] - 1).toBeCloseTo(0, 9)
  })
})

describe('texture signature', () => {
  it('changes when the relief of an alternate texture changes (the brush mesh must be rebuilt)', async () => {
    const { brushTextureSignature } = await import('../src/model/uvmesh')
    const axisAlt = { u: { mode: 'anchor' as const, anchor: 'start' as const, split: 0.5, fill: 'repeat' as const, alt: { texture: 'b', offset: [0, 0] as [number, number] } } }
    const b = makeBox('b', [0, 0, 0], [3, 1, 1], 'a')
    provide({ a: axisAlt, b: {} })
    const before = brushTextureSignature(b)
    provide({ a: axisAlt, b: { relief: ramp } })
    expect(brushTextureSignature(b)).not.toBe(before)
  })
})
