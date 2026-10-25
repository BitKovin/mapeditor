// Shared test vectors of the texture layout functions (REQUIREMENTS §27 G2 / G3): the Core has a C++ port of them for the BSP export (core/texplan.h).
// This test computes the TypeScript results for a fixed set of inputs and compares them with testdata/texplan_vectors.json; the Core's tests check the
// port against the same file. Set UPDATE_TEXPLAN_VECTORS=1 (or delete the file) to write it again after an intended change.
import { existsSync, readFileSync, writeFileSync } from 'node:fs'
import { afterAll, describe, expect, it } from 'vitest'
import type { Brush } from '../src/model/document'
import { makeBox, triangulateFace } from '../src/model/geometry'
import { solveLayout } from '../src/model/layout'
import { buildPreset, parseRelief, reliefToJson } from '../src/model/relief'
import { reliefTriangle } from '../src/model/reliefApply'
import { groupEnv, parseGroup } from '../src/model/texgroup'
import { TexInfo, parseProps, texEnv } from '../src/model/texprops'
import { axisSegments, faceUvAt, planForFace, retexture } from '../src/model/uvmesh'
import { projectFace } from '../src/model/uv'

const FILE = new URL('../../testdata/texplan_vectors.json', import.meta.url)
const r9 = (v: number): number => Math.round(v * 1e9) / 1e9

// ---- textures --------------------------------------------------------------------------------------------------------------------------------------
const reliefRaise = reliefToJson(buildPreset('raise', 1, 1))
const textures: { path: string; width: number; height: number; ppm: number; props: Record<string, unknown>; seamless: [boolean, boolean] | null }[] = [
  { path: 'v/wall', width: 256, height: 128, ppm: 64, props: {}, seamless: [true, true] },
  { path: 'v/mirror', width: 128, height: 128, ppm: 64, props: { repeat: { u: { mode: 'mirror' }, v: { mode: 'mirror' } } }, seamless: [true, true] },
  { path: 'v/anchor', width: 256, height: 256, ppm: 128, props: { repeat: { u: { mode: 'repeat', edge: 'clamp' }, v: { mode: 'anchor', anchor: 'end', split: 0.3, fill: 'mirror' } } }, seamless: [true, true] },
  { path: 'v/alt', width: 64, height: 64, ppm: 64, props: {}, seamless: [true, true] },
  { path: 'v/anchoralt', width: 128, height: 64, ppm: 64, props: { repeat: { u: { mode: 'anchor', anchor: 'start', fill: 'repeat', alt: { texture: 'v/alt', offset: [0.25, 0] } } } }, seamless: [true, true] },
  { path: 'v/anchormirror', width: 128, height: 128, ppm: 32, props: { repeat: { v: { mode: 'anchor', anchor: 'start', split: 0.6, fill: 'repeat' }, u: { mode: 'anchor', anchor: 'end', fill: 'mirror', alt: { texture: 'v/alt', offset: [0, 0.5] } } } }, seamless: [true, false] },
  { path: 'v/seam', width: 64, height: 64, ppm: 64, props: {}, seamless: [false, true] },
  { path: 'v/relief', width: 128, height: 128, ppm: 64, props: { relief: reliefRaise }, seamless: [true, true] },
  { path: 'v/reliefmirror', width: 128, height: 64, ppm: 64, props: { relief: reliefRaise, repeat: { u: { mode: 'mirror' } } }, seamless: [true, true] },
]
const infos = new Map<string, TexInfo>(textures.map((t) => [t.path, { path: t.path, width: t.width, height: t.height, ppm: t.ppm, props: parseProps(t.props, t.path), ...(t.seamless ? { seamless: t.seamless } : {}) }]))

// ---- groups ----------------------------------------------------------------------------------------------------------------------------------------
const groups: Record<string, unknown> = {
  'g/facade': {
    format: 1, name: 'facade',
    params: { floorHeight: { type: 'number', default: 3, min: 2, max: 6 }, roof: { type: 'bool', default: true } },
    slots: { wall: 'v/wall', win: 'v/mirror', cap: 'v/anchor', alt: 'v/alt' },
    fallback: 'wall',
    rules: { bay: { split: 'u', parts: [{ size: 1, node: 'wall' }, { size: '*', node: { choice: [{ tex: 'win', weight: 2 }, 'alt'], mode: 'random' } }, { size: { natural: 'alt' }, node: 'wall' }] } },
    root: { split: 'v', parts: [
      { size: { param: 'floorHeight' }, repeat: { size: { param: 'floorHeight' }, as: 'floor', fit: 'floor' }, node: { split: 'u', parts: [{ size: '*', repeat: { size: 2.5, as: 'bay' }, node: { ref: 'bay' } }] } },
      { size: 0.5, when: { var: 'param.roof' }, node: { tex: 'cap', fit: 'stretch', alignV: 'end' } },
      { size: '*', node: { choice: [{ when: { var: 'w', '>': 5 }, node: { tex: 'wall', fit: 'width', rotate: 90 } }, 'alt'] } },
    ], leftover: 'center', fill: 'alt' },
  },
  'g/kitwall': {
    format: 1, kit: { kind: 'wall', tile: [1.5, 1.5], maxStretch: 20,
      bottom: { pieces: [{ tex: 'v/mirror', place: 'once' }, { tex: 'v/wall', weight: 3 }] },
      middle: { rows: 2, pieces: [{ cells: [['v/alt', 'v/seam'], ['v/wall', 'v/wall']] }, { cells: [['v/wall'], ['v/anchor']], weight: 3, match: 'w 1' }] },
      top: { pieces: [{ tex: 'v/anchoralt', place: 'left' }, { tex: 'v/wall' }] } },
  },
  'g/kitframe': { format: 1, kit: { kind: 'frame', tile: [1, 0.75], tl: 'v/alt', t: 'v/wall', tr: 'v/alt', l: 'v/mirror', c: 'v/anchor', r: 'v/mirror', bl: 'v/seam', b: 'v/wall', br: 'v/seam' } },
}
const parsedGroups = new Map(Object.entries(groups).map(([k, g]) => [k, parseGroup(g).group!]))
texEnv.setProvider((p) => infos.get(p))
groupEnv.setProvider((n) => parsedGroups.get(n))
afterAll(() => { texEnv.setProvider(null); groupEnv.setProvider(null) })

// ---- brushes ---------------------------------------------------------------------------------------------------------------------------------------
function brushes(): Brush[] {
  const out: Brush[] = []
  const a = makeBox('a', [0, 0, 0], [5.3, 3.7, 2.2], 'v/mirror')
  a.faces[0].texture = 'v/anchor'
  a.faces[1].texture = 'v/anchoralt'
  a.faces[2].texture = 'v/seam'
  a.faces[3].texture = 'v/anchormirror'
  a.faces[4].projection = { scale: [1.5, 0.7], shift: [0.3, -0.2], rotate: 30, shear: 0.2 }
  a.faces[5].repeat = { u: { mode: 'repeat', edge: 'clamp' }, v: { mode: 'mirror' } }
  for (const f of a.faces) if (f.id % 2 === 0) f.uvs = projectFace(a, f)
  out.push(a)
  const b = makeBox('b', [-1, 0.5, -3], [7.2, 9.6, -2.5], 'v/wall')
  b.faces[0].layout = { group: 'g/facade', params: { floorHeight: 2.6 } }
  b.faces[1].layout = { group: 'g/kitwall', seed: 77 }
  b.faces[2].layout = { group: 'g/kitframe', rotate: 90, flipU: true }
  b.faces[3].projection = { scale: [2, 2], shift: [0, 0], rotate: 0, kind: 'ortho', basis: [[0, 0, 1], [0, -1, 0]] }
  b.faces[3].texture = 'v/reliefmirror'
  b.faces[4].texture = 'v/relief'
  out.push(b)
  // a wedge (non-axis faces) with uvs that are not affine on one face
  const w: Brush = { id: 'w', vertices: [[0, 0, 0], [3, 0, 0], [3, 0, 2], [0, 0, 2], [0, 2.5, 0], [3, 1.2, 0]], faces: [
    { id: 0, loop: [0, 1, 2, 3], texture: 'v/anchor' },
    { id: 1, loop: [0, 4, 5, 1], texture: 'v/mirror', uvs: [[0, 0], [0, 1.3], [1.7, 0.4], [1.5, 0]] },
    { id: 2, loop: [3, 2, 5, 4], texture: 'v/anchoralt' },
    { id: 3, loop: [0, 3, 4], texture: 'v/wall' },
    { id: 4, loop: [1, 5, 2], texture: 'v/seam' },
  ] }
  out.push(w)
  return out
}

// ---- the vectors -----------------------------------------------------------------------------------------------------------------------------------
function compute(): Record<string, unknown> {
  const axisCfgs = [
    { mode: 'repeat' }, { mode: 'mirror' }, { mode: 'anchor', anchor: 'start', split: 0.3, fill: 'mirror' }, { mode: 'anchor', anchor: 'start', split: 0.45, fill: 'repeat' },
    { mode: 'anchor', anchor: 'end', split: 0.7, fill: 'mirror' }, { mode: 'anchor', anchor: 'end', split: 0.2, fill: 'repeat' },
  ]
  const alts = [null, { k: 0.5, off: 0.25 }, { k: 2.3, off: -0.1 }]
  const ranges = [[-2.3, 3.7], [0.2, 0.8], [-0.5, 0.5], [0.9, 5.1], [-4.25, -0.75]]
  const tiles = [undefined, { main: true, alt: false }, { main: true, alt: true }]
  const axis: unknown[] = []
  for (const c of axisCfgs) {
    const cfg = parseProps({ repeat: { u: c } }).repeat.u
    for (const alt of alts) for (const [lo, hi] of ranges) for (const t of tiles) {
      if (alt && c.mode !== 'anchor') continue
      const segs = axisSegments(cfg, alt, lo, hi, t)
      axis.push({ cfg: c, alt, lo, hi, tiles: t ?? null, segs: segs && segs.map((s) => ({ lo: r9(s.lo), hi: r9(s.hi), s: r9(s.s), c: r9(s.c), alt: s.role === 'alt', tile: s.tile ?? null })) })
    }
  }
  const faces: unknown[] = []
  for (const b of brushes()) for (const f of b.faces) {
    const uvAt = faceUvAt(b, f)
    const pts = f.loop.map((i) => b.vertices[i])
    const c = pts.reduce((s, p) => [s[0] + p[0] / pts.length, s[1] + p[1] / pts.length, s[2] + p[2] / pts.length], [0, 0, 0])
    const samples = [...pts, c].map((p) => uvAt(p as [number, number, number]).map(r9))
    const plan = planForFace(b, f)
    const tris = triangulateFace(b, f).map((t) => t.map((i) => b.vertices[i]) as [[number, number, number], [number, number, number], [number, number, number]])
    const pieces = plan ? retexture(plan, tris).map((o) => ({ texture: o.texture, src: o.src, verts: o.verts.map((v) => ({ w: v.w.map(r9), uv: v.uv.map(r9) })) })) : null
    faces.push({ brush: b.id, face: f.id, uvSamples: samples, textures: plan ? plan.textures : null, pieces })
  }
  const layouts: unknown[] = []
  const sizes = [[3.1, 2.2], [7.4, 9.1], [12.2, 6.3], [1.3, 0.9]]
  for (const name of Object.keys(groups)) for (const [w, h] of sizes) for (const seed of [1, 4242]) {
    const r = solveLayout({ group: parsedGroups.get(name)!, w, h, seed, facing: 'wall', tilt: 90, info: (p) => infos.get(p), fallbackTexture: 'v/wall' })
    layouts.push({ group: name, w, h, seed, truncated: r.truncated, cells: r.cells.map((c) => ({ x0: r9(c.x0), y0: r9(c.y0), x1: r9(c.x1), y1: r9(c.y1), texture: c.texture, fit: c.fit, origin: c.origin, alignU: c.alignU, alignV: c.alignV, flipU: c.flipU, flipV: c.flipV, rotate: c.rotate, cropU: c.cropU ?? null, cropV: c.cropV ?? null, path: c.path })) })
  }
  const reliefs: unknown[] = []
  const relief = infos.get('v/relief')!.props.relief!
  const triCases: { tri: [number, number, number][]; uv: [number, number][]; n: [number, number, number] }[] = [
    { tri: [[0, 0, 0], [2, 0, 0], [0, 2, 0]], uv: [[0, 1], [1, 1], [0, 0]], n: [0, 0, 1] },
    { tri: [[0, 0, 0], [3, 0, 0], [3, 2, 0]], uv: [[0.2, 1.1], [2.4, 1.1], [2.4, -0.4]], n: [0, 0, 1] },
    { tri: [[0, 0, 0], [0, 0, 2], [0, 2, 0]], uv: [[1, 1], [0, 1], [1, 0]], n: [-1, 0, 0] },  // mirrored mapping
  ]
  for (const t of triCases) {
    const out = reliefTriangle(t.tri as never, t.uv as never, t.n, relief)
    reliefs.push({ tri: t.tri, uv: t.uv, n: t.n, out: out ? out.map(r9) : null })
  }
  // connecting walls: a ramp (its borders do not join) and a twist (the heights across a seam cross each other), with and without the face's outline closed
  const ramp = { vertices: [[0, 0, 0.1], [1, 0, 0], [1, 1, -0.05], [0, 1, 0.05]], faces: [[0, 1, 2, 3]] }
  const twist = { vertices: [[0, 0, 0.1], [1, 0, 0], [1, 1, 0.1], [0, 1, 0]], faces: [[0, 1, 2], [0, 2, 3]] }
  const wallCases: { relief: unknown; tri: [number, number, number][]; uv: [number, number][]; n: [number, number, number]; border: number; nbr: boolean }[] = []
  for (const r of [ramp, twist, reliefRaise]) for (const t of triCases) for (const border of [0, 7, 3, 4]) for (const nbr of [false, true]) wallCases.push({ relief: r, ...t, border, nbr })
  // a face with a relief of its own 0.05 high, lying behind the plane x = 0 (a convex corner there)
  const nbrAt = (a: number[], b: number[]) => (Math.abs(a[0]) < 1e-9 && Math.abs(b[0]) < 1e-9 ? { n: [-1, 0, 0] as [number, number, number], spans: [{ t0: 0, t1: 1, z0: 0.05, z1: 0.05 }, { t0: 1, t1: 1.5, z0: 0.05, z1: 0.05 }] } : null)
  const walls = wallCases.map((c) => {
    const out = reliefTriangle(c.tri as never, c.uv as never, c.n, parseRelief(c.relief)!, undefined, c.border, c.nbr ? nbrAt as never : undefined)
    return { relief: c.relief, tri: c.tri, uv: c.uv, n: c.n, border: c.border, nbr: c.nbr, out: out ? out.map(r9) : null }
  })
  return { format: 1, textures, groups, brushes: brushes(), axis, faces, layouts, reliefs: { relief: reliefRaise, cases: reliefs, walls } }
}

describe('texture layout vectors (shared with the Core)', () => {
  it('the TypeScript results equal testdata/texplan_vectors.json', () => {
    const now = compute()
    if (process.env.UPDATE_TEXPLAN_VECTORS === '1' || !existsSync(FILE)) writeFileSync(FILE, JSON.stringify(now) + '\n')
    const stored = JSON.parse(readFileSync(FILE, 'utf8'))
    expect(JSON.parse(JSON.stringify(now))).toEqual(stored)
    // the vectors exercise every behaviour (a plan that does nothing would make the comparison meaningless)
    const f = (now.faces as { pieces: unknown[] | null }[])
    expect(f.filter((x) => x.pieces).length).toBeGreaterThan(8)
    expect((now.layouts as { cells: unknown[] }[]).every((l) => l.cells.length > 0)).toBe(true)
  })
})
