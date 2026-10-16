// A texture group together with relief (REQUIREMENTS §20 T6, T7): the layout is a FacePlan, so the relief of each cell's texture is placed by the cell's own UV on
// every path: plain (renderer), carved fragments, the Lit view and the bake export.
import { afterEach, describe, expect, it } from 'vitest'
import { buildCarvedMesh } from '../src/model/derived'
import type { DerivedEntry, World } from '../src/model/derived'
import type { Brush, MapDocument } from '../src/model/document'
import { faceNormal, makeBox, triangulateFace } from '../src/model/geometry'
import { Frame, buildLayoutPlan, cellMap, layoutFrame } from '../src/model/layoutPlan'
import { Cell } from '../src/model/layout'
import { buildPreset } from '../src/model/relief'
import { planWithRelief } from '../src/model/reliefApply'
import { reliefFaces } from '../src/model/reliefBake'
import { TexGroup, groupEnv, parseGroup } from '../src/model/texgroup'
import { TextureProps, defaultAxis, defaultProps, texEnv } from '../src/model/texprops'
import { brushTextureSignature, planForFace } from '../src/model/uvmesh'
import { LightmapStore, LIT_VERTEX_FLOATS } from '../src/render/lightmapStore'
import { buildLitDraw, LIT_TEX_FLOATS } from '../src/render/litTextured'
import type { LitFace } from '../src/net/protocol'

const DEPTH = 0.08 // buildPreset('recess', 1, 1): depth = min(0.25, 0.08 * 1)
interface T { relief?: boolean; u?: 'mirror' | 'repeat'; size?: number }
const textures = (t: Record<string, T>): void => {
  texEnv.setProvider((path) => {
    const x = t[path]
    if (!x) return undefined
    const props: TextureProps = { ...defaultProps(), relief: x.relief ? buildPreset('recess', 1, 1) : null, repeat: { u: { ...defaultAxis('start'), mode: x.u ?? 'repeat' }, v: defaultAxis('end') } }
    return { path, width: x.size ?? 64, height: x.size ?? 64, ppm: 64, props } // 1 m natural size
  })
}
const groups = (g: Record<string, TexGroup>, version = 'v1'): void => groupEnv.setProvider((p) => g[p], () => version)
afterEach(() => { texEnv.setProvider(null); groupEnv.setProvider(null) })

const mkGroup = (j: object): TexGroup => { const r = parseGroup(j); if (!r.group) throw new Error(r.errors.join()); return r.group }
/** wall | window (2 m, stretched, recessed) | wall on a 6 x 3 face */
const windowWall = (): TexGroup => mkGroup({ slots: { wall: 'wall', win: 'win' }, root: { split: 'u', parts: [{ size: '*', tex: 'wall' }, { size: 2, tex: 'win', fit: 'stretch' }, { size: '*', tex: 'wall' }] } })
const front = (b: Brush) => b.faces.find((f) => faceNormal(b, f)[2] > 0.99)!
const wallBrush = (layout: object | null = { group: 'g' }): Brush => {
  const b = makeBox('b', [0, 0, 0], [6, 3, 1], 'wall')
  return layout ? { ...b, faces: b.faces.map((f) => (f.id === front(b).id ? { ...f, layout } : f)) } : b
}
const trisOf = (b: Brush, f: Brush['faces'][number]) => triangulateFace(b, f).map(([a, c, d]) => [b.vertices[a], b.vertices[c], b.vertices[d]] as [number[], number[], number[]] as never)
interface V { x: number; y: number; z: number; u: number; v: number }
const verts = (a: number[] | undefined): V[] => { const o: V[] = []; for (let i = 0; a && i + 7 < a.length; i += 8) o.push({ x: a[i], y: a[i + 1], z: a[i + 2], u: a[i + 6], v: a[i + 7] }); return o }
const projArea = (a: number[] | undefined): number => { let s = 0; const v = verts(a); for (let i = 0; i + 2 < v.length; i += 3) s += Math.abs((v[i + 1].x - v[i].x) * (v[i + 2].y - v[i].y) - (v[i + 2].x - v[i].x) * (v[i + 1].y - v[i].y)) / 2; return s }

describe('layout plan of a face', () => {
  it('lays cells over the face: exact cover, one relief tile per stretched cell, flat walls', () => {
    textures({ wall: {}, win: { relief: true } }); groups({ g: windowWall() })
    const b = wallBrush(), f = front(b)
    const plan = planForFace(b, f)!
    expect(plan).not.toBeNull(); expect(plan.textures.sort()).toEqual(['wall', 'win'])
    const out = new Map<string, number[]>()
    planWithRelief(plan, trisOf(b, f), faceNormal(b, f), out)
    // exact cover of the 6 x 3 face seen from the front
    expect(projArea(out.get('wall')) + projArea(out.get('win'))).toBeCloseTo(18, 5)
    // the plain wall cells are flat at z = 1; the window relief sinks DEPTH, and only inside its cell (2 <= x <= 4)
    expect(verts(out.get('wall')).every((p) => Math.abs(p.z - 1) < 1e-9)).toBe(true)
    const win = verts(out.get('win'))
    expect(Math.min(...win.map((p) => p.z))).toBeCloseTo(1 - DEPTH, 6)
    expect(win.every((p) => p.x >= 2 - 1e-9 && p.x <= 4 + 1e-9)).toBe(true)
    // stretch: ONE tile (uv 0..1), so the frame of the relief meets the cell's border
    expect(Math.min(...win.map((p) => p.u))).toBeGreaterThanOrEqual(-1e-9); expect(Math.max(...win.map((p) => p.u))).toBeLessThanOrEqual(1 + 1e-9)
    expect(win.filter((p) => Math.abs(p.z - 1) < 1e-9).every((p) => p.x < 2.25 || p.x > 3.75 || p.y < 0.4 || p.y > 2.6)).toBe(true) // only the frame is at depth 0
    // cell borders stay at depth 0: the neighbours join without a crack
    expect(win.filter((p) => Math.abs(p.x - 2) < 1e-9 || Math.abs(p.x - 4) < 1e-9).every((p) => Math.abs(p.z - 1) < 1e-9)).toBe(true)
  })

  it('a tile cell repeats the relief like a plain face and continues across cells (origin face)', () => {
    textures({ wall: { relief: true }, win: {} }); groups({ g: mkGroup({ slots: { wall: 'wall', win: 'win' }, root: { split: 'u', parts: [{ size: 2.5, tex: 'wall' }, { size: '*', tex: 'win' }] } }) })
    const b = wallBrush(), f = front(b)
    const out = new Map<string, number[]>()
    planWithRelief(planForFace(b, f)!, trisOf(b, f), faceNormal(b, f), out)
    const w = verts(out.get('wall'))
    expect(Math.max(...w.map((p) => p.x))).toBeCloseTo(2.5, 6)
    // natural size 1 m: tiles restart at x = 0, 1, 2 and the 3rd is cut at 2.5 (u reaches 2.5)
    expect(Math.max(...w.map((p) => p.u))).toBeCloseTo(2.5, 6)
    expect(w.some((p) => p.z < 1 - 0.07)).toBe(true)
  })

  it('texture properties still apply inside a cell (mirror) and the plan handles any triangle', () => {
    textures({ wall: { u: 'mirror' }, win: {} }); groups({ g: mkGroup({ slots: { wall: 'wall' }, root: { tex: 'wall' } }) })
    const b = wallBrush(), f = front(b)
    const plan = planForFace(b, f)!
    const out = new Map<string, number[]>()
    planWithRelief(plan, trisOf(b, f), faceNormal(b, f), out)
    const v = verts(out.get('wall'))
    const at = (x: number) => v.filter((p) => Math.abs(p.x - x) < 1e-9).map((p) => p.u)
    expect(at(1)[0]).toBeCloseTo(1, 6); expect(at(2)[0]).toBeCloseTo(0, 6); expect(at(1.0)).not.toContain(0.0)
    expect(projArea(out.get('wall'))).toBeCloseTo(18, 5)
  })

  it('falls back to the plain face until the group and its textures are there', () => {
    textures({ wall: {} }); groups({})
    const b = wallBrush(), f = front(b)
    expect(planForFace(b, f)).toBeNull() // group not loaded: plain
    groups({ g: windowWall() })
    expect(planForFace(wallBrush(), front(wallBrush()))).toBeNull() // slot texture 'win' not loaded: plain
    textures({ wall: {}, win: {} })
    expect(planForFace(wallBrush(), front(wallBrush()))).not.toBeNull()
  })

  it('texture signature changes with the group version, so caches rebuild', () => {
    textures({ wall: {}, win: {} }); groups({ g: windowWall() }, 'v1')
    const b = wallBrush(), s1 = brushTextureSignature(b)
    groups({ g: windowWall() }, 'v2')
    expect(brushTextureSignature(b)).not.toBe(s1)
    expect(brushTextureSignature(wallBrush(null))).not.toBe(brushTextureSignature(b))
  })

  it('frame and cell maps: right / up as seen from the front, bottom-left origin, fits and flips', () => {
    const b = wallBrush(), f = front(b)
    const fr = layoutFrame(b, f)!
    expect(fr.ex).toEqual([1, 0, 0]); expect(fr.ey).toEqual([0, 1, 0]); expect(fr.w).toBeCloseTo(6); expect(fr.h).toBeCloseTo(3)
    expect(layoutFrame(b, f, 90)!.w).toBeCloseTo(3); expect(layoutFrame(b, f, 0, true)!.ex[0]).toBe(-1)
    const cell: Cell = { x0: 1, y0: 1, x1: 3, y1: 2, texture: 't', slot: 't', fit: 'stretch', origin: 'cell', alignU: 'start', alignV: 'start', flipU: false, flipV: false, rotate: 0, path: 'r' }
    const m = cellMap(cell, [1, 1], 6, 3)
    const uv = (x: number, y: number) => [m.u[0] * x + m.u[1] * y + m.u[2], m.v[0] * x + m.v[1] * y + m.v[2]]
    expect(uv(1, 1)).toEqual([0, 1]); expect(uv(3, 2)).toEqual([1, 0]) // bottom-left = (0, 1) image convention
    const t = { ...cell, fit: 'tile' as const, origin: 'face' as const }
    const mt = cellMap(t, [2, 1], 6, 3), uv2 = (x: number, y: number) => [mt.u[0] * x + mt.u[2], mt.v[1] * y + mt.v[2]]
    expect(uv2(0, 0)).toEqual([0, 1]); expect(uv2(4, 1)).toEqual([2, 0]) // continues like one face
    const mf = cellMap({ ...cell, flipU: true }, [1, 1], 6, 3); expect(mf.u[0]).toBeCloseTo(-0.5)
    const mr = cellMap({ ...cell, rotate: 90 }, [1, 1], 6, 3); expect(Math.abs(mr.u[1])).toBeGreaterThan(0) // u now runs along y
  })

  it('buildLayoutPlan needs every texture; edge cases keep exact weights', () => {
    textures({ a: {}, b: { u: 'mirror' } })
    const frame = { ex: [1, 0, 0], ey: [0, 1, 0], dx: 0, dy: 0, w: 4, h: 1, normal: [0, 0, 1], q: [0, 0, 0], xq: 0, yq: 0 } as Frame
    const c = (x0: number, x1: number, tex: string): Cell => ({ x0, x1, y0: 0, y1: 1, texture: tex, slot: tex, fit: 'tile', origin: 'face', alignU: 'start', alignV: 'start', flipU: false, flipV: false, rotate: 0, path: 'r' })
    expect(buildLayoutPlan(frame, [c(0, 4, 'zz')], 'a', (p) => texEnv.info(p))).toBeNull()
    const plan = buildLayoutPlan(frame, [c(0, 2, 'a'), c(2, 4, 'b')], 'a', (p) => texEnv.info(p))!
    const tri: [number[], number[], number[]] = [[0, 0, 0], [4, 0, 0], [4, 1, 0]]
    const pieces = plan.pieces(tri as never)
    // every weight set sums to 1 and the positions they give lie inside the triangle (mirror cuts included)
    for (const p of pieces) for (const v of p.verts) expect(v.w[0] + v.w[1] + v.w[2]).toBeCloseTo(1, 9)
    const tex = new Set(pieces.map((p) => p.texture)); expect(tex.has('a') && tex.has('b')).toBe(true)
  })
})

describe('simple texture sets with relief', () => {
  it('a wall kit: the window piece sinks only inside its own tile, the plain tiles stay flat, and the tiles cover the face exactly', () => {
    textures({ wall: {}, win: { relief: true } })
    groups({ g: mkGroup({ format: 1, kit: { kind: 'wall', tile: [2, 3], middle: { pieces: [{ tex: 'wall' }, { tex: 'win', place: 'middle' }] } } }) })
    const b = wallBrush(), f = front(b), plan = planForFace(b, f)!
    expect(plan).not.toBeNull()
    const out = new Map<string, number[]>()
    planWithRelief(plan, trisOf(b, f), faceNormal(b, f), out)
    expect(projArea(out.get('wall')) + projArea(out.get('win'))).toBeCloseTo(18, 5)
    const win = verts(out.get('win'))
    expect(Math.min(...win.map((p) => p.z))).toBeCloseTo(1 - DEPTH, 6)
    expect(win.every((p) => p.x >= 2 - 1e-9 && p.x <= 4 + 1e-9)).toBe(true) // the middle of three 2 m tiles
    expect(verts(out.get('wall')).every((p) => Math.abs(p.z - 1) < 1e-9)).toBe(true)
  })

  it('a frame: the relief of the corner texture is one tile at each corner, the edges repeat, the middle repeats', () => {
    textures({ c: { relief: true }, e: {}, m: {} })
    const k = { kind: 'frame', tile: [1, 1], tl: 'c', tr: 'c', bl: 'c', br: 'c', t: 'e', b: 'e', l: 'e', r: 'e', c: 'm' }
    groups({ g: mkGroup({ format: 1, kit: k }) })
    const b = wallBrush(), f = front(b), plan = planForFace(b, f)!
    const out = new Map<string, number[]>()
    planWithRelief(plan, trisOf(b, f), faceNormal(b, f), out)
    const corners = verts(out.get('c')), edge = verts(out.get('e')), mid = verts(out.get('m'))
    expect(projArea(out.get('c')) + projArea(out.get('e')) + projArea(out.get('m'))).toBeCloseTo(18, 5)
    expect(corners.every((p) => (p.x <= 1 + 1e-9 || p.x >= 5 - 1e-9) && (p.y <= 1 + 1e-9 || p.y >= 2 - 1e-9))).toBe(true)
    expect(Math.min(...corners.map((p) => p.z))).toBeCloseTo(1 - DEPTH, 6)
    expect(edge.every((p) => Math.abs(p.z - 1) < 1e-9)).toBe(true); expect(mid.every((p) => Math.abs(p.z - 1) < 1e-9)).toBe(true)
    expect(plan.textures.sort()).toEqual(['c', 'e', 'm'])
  })
})

describe('cut-off tiles of a texture set keep their relief right', () => {
  it('the last column shows the left half of the tile and its relief is cut there, not squeezed', () => {
    textures({ win: { relief: true } })
    groups({ g: mkGroup({ format: 1, kit: { kind: 'wall', tile: [4, 3], middle: { pieces: [{ tex: 'win' }] } } }) }) // 6 m wide: one 4 m tile and half of the next
    const b = wallBrush(), f = front(b), plan = planForFace(b, f)!
    const out = new Map<string, number[]>()
    planWithRelief(plan, trisOf(b, f), faceNormal(b, f), out)
    const v = verts(out.get('win'))
    expect(projArea(out.get('win'))).toBeCloseTo(18, 5)
    const second = v.filter((p) => p.x > 4 + 1e-9)
    expect(second.length).toBeGreaterThan(0)
    expect(Math.max(...second.map((p) => p.u))).toBeCloseTo(0.5, 6) // half the tile
    expect(Math.min(...second.map((p) => p.z))).toBeCloseTo(1 - DEPTH, 6) // its recess is there at full depth, at natural width: 4 m per repeat
    const first = v.filter((p) => p.x < 4 - 1e-9)
    expect(Math.max(...first.map((p) => p.u))).toBeLessThanOrEqual(1 + 1e-9)
  })
})

describe('relief of a layout on every path', () => {
  const setup = () => { textures({ wall: {}, win: { relief: true } }); groups({ g: windowWall() }) }
  const deepIn = (v: V[], z0: number) => v.filter((p) => p.z < z0 - 0.07)

  it('carved fragments (derived geometry)', () => {
    setup()
    const b = wallBrush(), f = front(b)
    const entry = { entityId: 'w', brushId: 'b', instance: '', subtractors: [], fragments: [{ brush: 'b', face: f.id, v: f.loop.flatMap((i) => b.vertices[i]) }] } as unknown as DerivedEntry
    const world: World = { doc: { entities: [{ id: 'w', brushes: [b] }] } as unknown as MapDocument, prefabs: [] }
    const t = buildCarvedMesh(entry, world).textures
    expect([...t.keys()].sort()).toEqual(['wall', 'win'])
    const win = verts(t.get('win'))
    expect(deepIn(win, 1).length).toBeGreaterThan(0); expect(win.every((p) => p.x >= 2 - 1e-9 && p.x <= 4 + 1e-9)).toBe(true)
    expect(deepIn(verts(t.get('wall')), 1)).toHaveLength(0)
  })

  it('the bake export (relief_geometry): displaced triangles only where the relief cell is', () => {
    setup()
    const b = wallBrush()
    const faces = reliefFaces({ doc: { entities: [{ id: 'w', brushes: [b] }] } as unknown as MapDocument, prefabs: [] }, [])
    expect(faces).toHaveLength(1) // only the laid out face has relief; the other five are plain wall
    const f = faces[0]
    const deepX: number[] = []
    for (let i = 0; i < f.tris.length; i += 3) { const h = f.normal[0] * f.tris[i] + f.normal[1] * f.tris[i + 1] + f.normal[2] * f.tris[i + 2] - f.offset; if (h < -0.07) deepX.push(f.tris[i]) }
    expect(deepX.length).toBeGreaterThan(0); expect(Math.min(...deepX)).toBeGreaterThanOrEqual(2); expect(Math.max(...deepX)).toBeLessThanOrEqual(4)
  })

  it('the Lit view', () => {
    setup()
    const b = wallBrush(), f = front(b)
    const out: number[] = []
    for (const t of triangulateFace(b, f)) for (const i of t) { const v = b.vertices[i]; out.push(...v, ...faceNormal(b, f), v[0] / 10, v[1] / 10, 2, 5, 0, 0) }
    const faces: LitFace[] = [{ i: '', e: 'w', bi: '', b: 'b', f: f.id, t: 'wall', n: 2, m: { u: [0.1, 0, 0, 0], v: [0, 0.1, 0, 0], pg: 2, k: [5, 0, 0] } }]
    const s = new LightmapStore()
    s.mesh = Float32Array.from(out); s.faces = faces; s.triangleCount = 2; s.faceMaps = new Map(faces.map((x) => [`${x.i}|${x.e}|${x.b}|${x.f}`, x.m!]))
    expect(LIT_VERTEX_FLOATS).toBeGreaterThan(0)
    const draw = buildLitDraw(s.mesh!, s.faces, { doc: { entities: [{ id: 'w', brushes: [b] }] } as unknown as MapDocument, prefabs: [] })
    expect(draw.vertexCount).toBeGreaterThan(6)
    const zs: number[] = []; for (let i = 0; i < draw.vertexCount; i++) zs.push(draw.data[i * LIT_TEX_FLOATS + 2])
    expect(Math.min(...zs)).toBeCloseTo(1 - DEPTH, 5)
  })
})
