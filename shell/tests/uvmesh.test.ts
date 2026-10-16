import { afterEach, describe, expect, it } from 'vitest'
import { makeBox } from '../src/model/geometry'
import { faceNormal, triangulateFace } from '../src/model/geometry'
import { Vec3, cross, dot, length, sub } from '../src/model/math'
import { AxisRepeat, TexInfo, TextureProps, defaultProps, parseProps, propsToJson, safeTexturePath, texEnv } from '../src/model/texprops'
import { AltRatio, MAX_CELLS, edgeInsetTexels, axisSegments, axisValueAt, affineFromCorners, faceAffine, makePlan, planForFace, projectionAffine, retexture } from '../src/model/uvmesh'
import { projectFace } from '../src/model/uv'

const axis = (o: Partial<AxisRepeat>): AxisRepeat => ({ mode: 'anchor', anchor: 'end', split: 0.4, fill: 'mirror', alt: null, edge: 'auto', ...o })
const info = (path: string, w: number, h: number, props: Partial<TextureProps> = {}, ppm = 64): TexInfo => ({ path, width: w, height: h, ppm, props: { ...defaultProps(), ...props } })
const triArea = (t: readonly Vec3[]): number => length(cross(sub(t[1], t[0]), sub(t[2], t[0]))) / 2
afterEach(() => texEnv.setProvider(null))

// a tiny deterministic PRNG
const rng = (seed: number) => () => { seed = (seed * 1664525 + 1013904223) >>> 0; return seed / 4294967296 }

describe('texture properties', () => {
  it('defaults and lenient parsing', () => {
    const d = defaultProps()
    expect(d.pixelsPerMeter).toBeNull()
    expect(d.repeat.u.mode).toBe('repeat')
    expect(parseProps(null)).toEqual(d)
    expect(parseProps('junk')).toEqual(d)
    const p = parseProps({ pixelsPerMeter: 128, repeat: { v: { mode: 'anchor', anchor: 'end', split: 5, fill: 'repeat', alt: { texture: 'a/b', offset: [0.25, 1] } }, u: { mode: 'bogus' } } })
    expect(p.pixelsPerMeter).toBe(128)
    expect(p.repeat.v).toEqual({ mode: 'anchor', anchor: 'end', split: 0.98, fill: 'repeat', edge: 'auto', alt: { texture: 'a/b', offset: [0.25, 1] } })
    expect(p.repeat.u.mode).toBe('repeat')
  })
  it('round-trips through JSON', () => {
    const p = parseProps({ pixelsPerMeter: 32, repeat: { u: { mode: 'mirror' }, v: { mode: 'anchor', anchor: 'start', split: 0.3, fill: 'mirror', alt: { texture: 'x/y', offset: [0, 0.5] } } } })
    expect(parseProps(JSON.parse(JSON.stringify(propsToJson(p))))).toEqual(p)
  })
  it('rejects bad numbers and unsafe alternate paths', () => {
    const p = parseProps({ pixelsPerMeter: -3, repeat: { v: { mode: 'anchor', alt: { texture: '../etc/passwd', offset: [0, 0] } } } })
    expect(p.pixelsPerMeter).toBeNull()
    expect(p.repeat.v.alt).toBeNull()
    expect(parseProps({ pixelsPerMeter: 1e9 }).pixelsPerMeter).toBe(4096)
    for (const bad of ['', '/abs', 'a\\b', 'a/../b', 'a//b', 'a:b']) expect(safeTexturePath(bad)).toBeNull()
    expect(safeTexturePath('base/wall_01')).toBe('base/wall_01')
  })
  it('a texture cannot be its own alternate', () => {
    expect(parseProps({ repeat: { v: { mode: 'anchor', alt: { texture: 'a/b', offset: [0, 0] } } } }, 'a/b').repeat.v.alt).toBeNull()
  })
})

describe('axis segments', () => {
  const cfgs: [string, AxisRepeat, AltRatio | null][] = []
  for (const anchor of ['start', 'end'] as const) for (const fill of ['repeat', 'mirror'] as const) for (const split of [0.25, 0.4, 0.9]) {
    cfgs.push([`anchor ${anchor} ${fill} split ${split}`, axis({ anchor, fill, split }), null])
    cfgs.push([`anchor ${anchor} ${fill} alt`, axis({ anchor, fill, split }), { k: 0.8, off: 0.3 }])
    cfgs.push([`anchor ${anchor} ${fill} alt k=1.7 off=-0.6`, axis({ anchor, fill, split }), { k: 1.7, off: -0.6 }])
  }
  cfgs.push(['mirror', axis({ mode: 'mirror' }), null], ['repeat', axis({ mode: 'repeat' }), null])

  for (const [name, cfg, alt] of cfgs) {
    it(`${name}: segments tile the range and agree with the reference function`, () => {
      const r = rng(7)
      for (let trial = 0; trial < 20; trial++) {
        const lo = -8 + r() * 10, hi = lo + 0.05 + r() * 9
        const segs = axisSegments(cfg, alt, lo, hi)!
        expect(segs).not.toBeNull()
        expect(segs[0].lo).toBeCloseTo(lo, 9)
        expect(segs[segs.length - 1].hi).toBeCloseTo(hi, 9)
        for (let i = 1; i < segs.length; i++) expect(segs[i].lo).toBeCloseTo(segs[i - 1].hi, 9)
        for (const s of segs) {
          for (let k = 0; k < 5; k++) {
            const t = s.lo + (s.hi - s.lo) * (0.02 + 0.96 * r())
            const ref = axisValueAt(cfg, alt, t)
            expect(s.s * t + s.c).toBeCloseTo(ref.t, 8)
            expect(s.role).toBe(ref.role)
          }
        }
      }
    })
  }

  it('mirror folds are continuous across every cut (the cut points are real kinks, not jumps)', () => {
    const segs = axisSegments(axis({ mode: 'mirror' }), null, -2.5, 3.5)!
    for (let i = 1; i < segs.length; i++) {
      const t = segs[i].lo
      expect(segs[i - 1].s * t + segs[i - 1].c).toBeCloseTo(segs[i].s * t + segs[i].c, 9)
    }
  })
  it('anchor without alternate texture: repeat jumps, mirror stays continuous', () => {
    const rep = axisSegments(axis({ fill: 'repeat', split: 0.4 }), null, -1.2, 1)!
    const mir = axisSegments(axis({ fill: 'mirror', split: 0.4 }), null, -1.2, 1)!
    const gap = (segs: typeof rep) => { let g = 0; for (let i = 1; i < segs.length; i++) { const t = segs[i].lo; g = Math.max(g, Math.abs(segs[i - 1].s * t + segs[i - 1].c - (segs[i].s * t + segs[i].c))) } return g }
    expect(gap(mir)).toBeLessThan(1e-9)
    expect(gap(rep)).toBeGreaterThan(0.3) // a tile boundary of a plain repeat of the band
  })
  it('the fixed part is never touched: t in [0, 1] is the identity for Anchor end', () => {
    const segs = axisSegments(axis({}), null, -3, 1)!
    const main = segs.filter((s) => s.lo >= 0)
    expect(main.length).toBe(1)
    expect(main[0].s).toBe(1); expect(main[0].c).toBe(0)
  })
  it('gives up (null) instead of exploding', () => {
    expect(axisSegments(axis({ fill: 'mirror', split: 0.02 }), null, -1e6, 1)).toBeNull()
  })
})

describe('face plans', () => {
  // a wall in the XY plane facing +z, x in [0, 4], y in [0, H]
  const wall = (H: number): Vec3[] => [[0, 0, 0], [4, 0, 0], [4, H, 0], [0, H, 0]]
  const natural = 204 / 64
  // v grows downwards: v = (H - y) / natural, u = x / natural
  const mapFor = (H: number) => ({ u: [1 / natural, 0, 0, 0] as [number, number, number, number], v: [0, -1 / natural, 0, H / natural] as [number, number, number, number] })
  const extFor = (H: number) => ({ umin: 0, umax: 4 / natural, vmin: 0, vmax: H / natural })
  const tris = (H: number): [Vec3, Vec3, Vec3][] => { const w = wall(H); return [[w[0], w[1], w[2]], [w[0], w[2], w[3]]] }

  const A = (props: Partial<TextureProps>) => info('wall/pt_1', 204, 204, props)
  const wallProps = (alt: AxisRepeat['alt'], fill: AxisRepeat['fill'] = 'mirror'): Partial<TextureProps> => ({ repeat: { u: axis({ mode: 'repeat' }), v: axis({ anchor: 'end', split: 0.4, fill, alt }) } })

  it('plain properties need no plan', () => {
    expect(makePlan({ texture: A({}), info: () => undefined, map: mapFor(5), extent: extFor(5) })).toBeNull()
  })

  /** UV of the world point (x, y) read from the pieces of the wall. */
  function sample(H: number, plan: NonNullable<ReturnType<typeof makePlan>>, x: number, y: number): { texture: string; uv: [number, number] } {
    const ts = tris(H)
    for (const t of ts) for (const piece of plan.pieces(t)) {
      // is (x, y) inside this convex piece (counter-clockwise seen from +z)?
      const pts = piece.verts.map((v) => [v.w[0] * t[0][0] + v.w[1] * t[1][0] + v.w[2] * t[2][0], v.w[0] * t[0][1] + v.w[1] * t[1][1] + v.w[2] * t[2][1]])
      let inside = true
      for (let i = 0; i < pts.length; i++) {
        const a = pts[i], b = pts[(i + 1) % pts.length]
        if ((b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0]) < -1e-9) { inside = false; break }
      }
      if (!inside) continue
      // barycentric within the convex polygon: use a fan triangle that contains the point
      for (let i = 1; i + 1 < pts.length; i++) {
        const [p0, p1, p2] = [pts[0], pts[i], pts[i + 1]]
        const d = (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (p1[1] - p0[1])
        if (Math.abs(d) < 1e-12) continue
        const l1 = ((x - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (y - p0[1])) / d
        const l2 = ((p1[0] - p0[0]) * (y - p0[1]) - (x - p0[0]) * (p1[1] - p0[1])) / d
        const l0 = 1 - l1 - l2
        if (l0 < -1e-9 || l1 < -1e-9 || l2 < -1e-9) continue
        const v0 = piece.verts[0].uv, v1 = piece.verts[i].uv, v2 = piece.verts[i + 1].uv
        return { texture: piece.texture, uv: [l0 * v0[0] + l1 * v1[0] + l2 * v2[0], l0 * v0[1] + l1 * v1[1] + l2 * v2[1]] }
      }
    }
    throw new Error(`no piece covers (${x}, ${y})`)
  }

  it('wallpaper acceptance: bottom stays at the bottom, the wallpaper above is mirrored', () => {
    for (const H of [2, natural, 5, 9.3]) {
      const plan = makePlan({ texture: A(wallProps(null)), info: () => undefined, map: mapFor(H), extent: extFor(H) })!
      expect(plan).not.toBeNull()
      // the bottom edge of the wall is the bottom edge of the texture, whatever the wall's height
      expect(sample(H, plan, 1, 0.0001).uv[1]).toBeCloseTo(1, 3)
      // one metre above the bottom: one metre up the texture (inside the first tile, v counts down)
      if (H > 1.1) expect(sample(H, plan, 1, 1).uv[1]).toBeCloseTo(1 - 1 / natural, 6)
      if (H > natural + 0.5) {
        // just above the top of the texture the picture is mirrored: y = natural + d shows the row d below the top edge
        const d = 0.3
        const top = sample(H, plan, 1, natural + d).uv[1]
        expect(top).toBeCloseTo(d / natural, 6)
        expect(top).toBeGreaterThanOrEqual(0)
        // v stays inside the band [0, split] in the whole mirrored part
        for (let y = natural; y < H; y += 0.17) { const v = sample(H, plan, 1, y + 0.01).uv[1]; expect(v).toBeGreaterThanOrEqual(-1e-9); expect(v).toBeLessThanOrEqual(0.4 + 1e-9) }
      }
    }
  })

  it('a taller wall changes nothing below: the lower part is identical for every wall height', () => {
    const lo = makePlan({ texture: A(wallProps(null)), info: () => undefined, map: mapFor(4), extent: extFor(4) })!
    const hi = makePlan({ texture: A(wallProps(null)), info: () => undefined, map: mapFor(7), extent: extFor(7) })!
    for (const y of [0.05, 0.5, 1.2, 2.9]) expect(sample(7, hi, 1.3, y).uv[1]).toBeCloseTo(sample(4, lo, 1.3, y).uv[1], 6)
  })

  it('with another texture above: its own texture, continuing at the joint, and it repeats', () => {
    const B = info('wall/pt_1_1', 204, 204)
    const lookup = (p: string) => (p === B.path ? B : undefined)
    const H = 8
    const plan = makePlan({ texture: A(wallProps({ texture: B.path, offset: [0, 0] }, 'repeat')), info: lookup, map: mapFor(H), extent: extFor(H) })!
    expect(sample(H, plan, 1, 1).texture).toBe('wall/pt_1')
    const above = sample(H, plan, 1, natural + 0.5)
    expect(above.texture).toBe('wall/pt_1_1')
    // just above the joint the second texture shows its last row (a seamless tile continues), going up its v decreases
    expect(sample(H, plan, 1, natural + 0.01).uv[1]).toBeCloseTo(-0.01 / natural, 6)
    expect(sample(H, plan, 1, natural + 1).uv[1]).toBeCloseTo(-1 / natural, 6)
    // horizontally the pattern continues in step with the first texture
    expect(sample(H, plan, 1.7, natural + 0.5).uv[0]).toBeCloseTo(1.7 / natural, 6)
  })

  it('an alternate texture of another size is scaled by the ratio of the natural sizes', () => {
    const B = info('wall/small', 128, 128) // 2 m per repeat at 64 ppm
    const H = 8
    const plan = makePlan({ texture: A(wallProps({ texture: B.path, offset: [0, 0] }, 'repeat')), info: (p) => (p === B.path ? B : undefined), map: mapFor(H), extent: extFor(H) })!
    const s = sample(H, plan, 1, natural + 1)
    expect(s.uv[1]).toBeCloseTo(-1 / 2, 6) // one metre = half a repeat of the 2 m texture
    expect(s.uv[0]).toBeCloseTo(1 / 2, 6)
  })

  it('the alternate texture mirrored: cut at every tile, the cuts are kinks', () => {
    const B = info('wall/pt_1_1', 204, 204)
    const H = 12
    const plan = makePlan({ texture: A(wallProps({ texture: B.path, offset: [0, 0] }, 'mirror')), info: (p) => (p === B.path ? B : undefined), map: mapFor(H), extent: extFor(H) })!
    // moving up through a tile boundary of the alternate texture, v goes 0 -> 1 -> 0
    const vAt = (d: number) => sample(H, plan, 1, natural + d).uv[1]
    expect(vAt(0.1)).toBeCloseTo(0.1 / natural, 6)
    expect(vAt(natural - 0.1)).toBeCloseTo((natural - 0.1) / natural, 6)
    expect(vAt(natural + 0.1)).toBeCloseTo(1 - 0.1 / natural, 6)
  })

  it('alternate texture not loaded yet: the band is used (both cases are supported)', () => {
    const H = 8
    const plan = makePlan({ texture: A(wallProps({ texture: 'wall/missing', offset: [0, 0] }, 'mirror')), info: () => undefined, map: mapFor(H), extent: extFor(H) })!
    expect(sample(H, plan, 1, natural + 0.3).texture).toBe('wall/pt_1')
  })

  it('horizontal mirror cuts at every tile and is independent of the vertical setting', () => {
    const props: Partial<TextureProps> = { repeat: { u: axis({ mode: 'mirror' }), v: axis({ mode: 'repeat' }) } }
    const plan = makePlan({ texture: A(props), info: () => undefined, map: mapFor(2), extent: extFor(2) })!
    expect(sample(2, plan, 0.5, 1).uv[0]).toBeCloseTo(0.5 / natural, 6)
    expect(sample(2, plan, natural + 0.5, 1).uv[0]).toBeCloseTo(1 - 0.5 / natural, 6)
    // vertical stays a plain continuation
    expect(sample(2, plan, 0.5, 1).uv[1]).toBeCloseTo((2 - 1) / natural, 6)
  })

  it('pieces tile the triangle exactly: areas add up, weights are barycentric, winding is kept', () => {
    const B = info('wall/pt_1_1', 204, 204)
    const cases: Partial<TextureProps>[] = [
      wallProps(null), wallProps(null, 'repeat'), wallProps({ texture: B.path, offset: [0.2, 0.3] }, 'mirror'),
      { repeat: { u: axis({ mode: 'mirror' }), v: axis({ mode: 'mirror' }) } },
      { repeat: { u: axis({ anchor: 'start', fill: 'mirror' }), v: axis({ anchor: 'start', split: 0.7 }) } },
    ]
    const r = rng(3)
    for (const props of cases) {
      const H = 6
      const plan = makePlan({ texture: A(props), info: (p) => (p === B.path ? B : undefined), map: mapFor(H), extent: extFor(H) })!
      expect(plan).not.toBeNull()
      for (let k = 0; k < 8; k++) {
        const t: [Vec3, Vec3, Vec3] = [[r() * 4, r() * H, 0], [r() * 4, r() * H, 0], [r() * 4, r() * H, 0]]
        if (triArea(t) < 0.05) continue
        const n0 = cross(sub(t[1], t[0]), sub(t[2], t[0]))
        let sum = 0
        for (const piece of plan.pieces(t)) {
          const ps = piece.verts.map((v) => { expect(v.w[0] + v.w[1] + v.w[2]).toBeCloseTo(1, 9); for (const c of v.w) expect(c).toBeGreaterThanOrEqual(-1e-9); return [0, 1, 2].map((a) => v.w[0] * t[0][a] + v.w[1] * t[1][a] + v.w[2] * t[2][a]) as Vec3 })
          for (let i = 1; i + 1 < ps.length; i++) { sum += triArea([ps[0], ps[i], ps[i + 1]]); expect(dot(cross(sub(ps[i], ps[0]), sub(ps[i + 1], ps[0])), n0)).toBeGreaterThanOrEqual(-1e-9) }
        }
        expect(sum).toBeCloseTo(triArea(t), 7)
      }
    }
  })

  it('the cut points are invisible: all piece vertices lie on the face plane', () => {
    const plan = makePlan({ texture: A(wallProps(null)), info: () => undefined, map: mapFor(9), extent: extFor(9) })!
    for (const t of tris(9)) for (const piece of plan.pieces(t)) for (const v of piece.verts) expect(v.w[0] * t[0][2] + v.w[1] * t[1][2] + v.w[2] * t[2][2]).toBeCloseTo(0, 12)
  })

  it('refuses faces that would need too many pieces (falls back to plain repeat)', () => {
    const props: Partial<TextureProps> = { repeat: { u: axis({ mode: 'mirror' }), v: axis({ mode: 'mirror' }) } }
    const ext = { umin: 0, umax: 500, vmin: 0, vmax: 500 }
    expect(makePlan({ texture: A(props), info: () => undefined, map: { u: [1, 0, 0, 0], v: [0, 1, 0, 0] }, extent: ext })).toBeNull()
    expect(MAX_CELLS).toBeGreaterThan(100)
  })

  it('Anchor start: the texture start edge is glued to the face start, the rest extends toward the end', () => {
    const props: Partial<TextureProps> = { repeat: { u: axis({ anchor: 'start', fill: 'mirror', split: 0.5 }), v: axis({ mode: 'repeat' }) } }
    const plan = makePlan({ texture: A(props), info: () => undefined, map: mapFor(2), extent: extFor(2) })!
    expect(sample(2, plan, 0.001, 1).uv[0]).toBeCloseTo(0.001 / natural, 5)
    expect(sample(2, plan, 0.3, 1).uv[0]).toBeCloseTo(0.3 / natural, 6)
    // beyond the texture: the band [0.5, 1] reflected at the edge
    expect(sample(2, plan, natural + 0.2, 1).uv[0]).toBeCloseTo(1 - 0.2 / natural, 6)
  })
})

describe('integration with faces and the texture environment', () => {
  const brushWithTexture = () => makeBox('b', [0, 0, 0], [4, 5, 0.2], 'wall/pt_1')
  const top = (b: ReturnType<typeof makeBox>) => b.faces.find((f) => faceNormal(b, f)[2] > 0.9)!
  const provide = (props: TextureProps) => texEnv.setProvider((p) => (p === 'wall/pt_1' ? { path: p, width: 204, height: 204, ppm: 64, props } : undefined))

  it('planForFace: null for plain props or an unknown texture, a plan for behaviour', () => {
    const b = brushWithTexture()
    expect(planForFace(b, top(b))).toBeNull()
    provide(defaultProps())
    expect(planForFace(b, top(b))).toBeNull()
    const p = defaultProps(); p.repeat.v = axis({})
    provide(p)
    expect(planForFace(b, top(b))).not.toBeNull()
  })

  it('plans are cached per brush and invalidated when the texture environment changes', () => {
    const b = brushWithTexture()
    const p = defaultProps(); p.repeat.v = axis({})
    provide(p)
    const a = planForFace(b, top(b))
    expect(planForFace(b, top(b))).toBe(a)
    texEnv.bump()
    expect(planForFace(b, top(b))).not.toBe(a)
  })

  it('face map: stored affine uvs are reproduced, non-affine uvs fall back to the projection', () => {
    const b = brushWithTexture()
    const f = top(b)
    const uvs = projectFace(b, f)
    const m = faceAffine(b, { ...f, uvs })
    const q = projectionAffine(faceNormal(b, f), { scale: [2, 2], shift: [0, 0], rotate: 0 })
    for (const i of f.loop) { const v = b.vertices[i]; expect(m.u[0] * v[0] + m.u[1] * v[1] + m.u[2] * v[2] + m.u[3]).toBeCloseTo(q.u[0] * v[0] + q.u[1] * v[1] + q.u[2] * v[2] + q.u[3], 8) }
    const bent = uvs.map((u, i) => (i === 0 ? ([u[0] + 0.5, u[1]] as [number, number]) : u))
    expect(affineFromCorners(f.loop.map((i) => b.vertices[i]), bent, faceNormal(b, f))).toBeNull()
    expect(() => faceAffine(b, { ...f, uvs: bent })).not.toThrow()
  })

  it('a face override replaces the texture setting for that face only, and is part of the plan cache key', () => {
    const b = brushWithTexture()
    provide(defaultProps())
    const f = top(b)
    expect(planForFace(b, f)).toBeNull()
    expect(planForFace(b, { ...f, repeat: { u: { mode: 'mirror' } } })).not.toBeNull()
    expect(planForFace(b, { ...f, repeat: { v: { edge: 'clamp' } } })).not.toBeNull()
    expect(planForFace(b, { ...f, repeat: { u: { edge: 'wrap' } } })).toBeNull()
    expect(planForFace(b, f)).toBeNull() // the plain face is not affected by the earlier overrides
    // a clamped texture can be forced to wrap on one face
    texEnv.setProvider((p) => (p === 'wall/pt_1' ? { path: p, width: 204, height: 204, ppm: 64, props: defaultProps(), seamless: [false, false] } : undefined))
    expect(planForFace(b, f)).not.toBeNull()
    expect(planForFace(b, { ...f, repeat: { u: { edge: 'wrap' }, v: { edge: 'wrap' } } })).toBeNull()
  })

  it('retexture: one entry per output triangle, source index kept', () => {
    const b = brushWithTexture()
    const p = defaultProps(); p.repeat.u = axis({ mode: 'mirror' }); p.repeat.v = axis({ mode: 'mirror' })
    provide(p)
    const f = top(b)
    const plan = planForFace(b, f)!
    const tris = triangulateFace(b, f).map((t) => t.map((i) => b.vertices[i]) as [Vec3, Vec3, Vec3])
    const out = retexture(plan, tris)
    expect(out.length).toBeGreaterThan(tris.length)
    expect(new Set(out.map((o) => o.src))).toEqual(new Set(tris.map((_, i) => i)))
    for (const o of out) expect(o.verts.length).toBe(3)
  })
})

describe('edge inset size', () => {
  it('about 1% of the texture, between half a texel and two', () => {
    expect(edgeInsetTexels(16)).toBe(0.5); expect(edgeInsetTexels(64)).toBeCloseTo(0.64, 9); expect(edgeInsetTexels(204)).toBe(2); expect(edgeInsetTexels(4096)).toBe(2)
  })
})

describe('edge clamp: no blending with the opposite edge (the black line)', () => {
  const natural = 204 / 64
  const H = 7
  const map = { u: [1 / natural, 0, 0, 0] as [number, number, number, number], v: [0, -1 / natural, 0, H / natural] as [number, number, number, number] }
  const ext = { umin: 0, umax: 4 / natural, vmin: 0, vmax: H / natural }
  const wall: [Vec3, Vec3, Vec3][] = [[[0, 0, 0], [4, 0, 0], [4, H, 0]], [[0, 0, 0], [4, H, 0], [0, H, 0]]]
  const tex = (edge: 'auto' | 'wrap' | 'clamp', seamless?: [boolean, boolean], mode: 'repeat' | 'mirror' = 'repeat'): TexInfo => {
    const p = defaultProps()
    p.repeat.u = axis({ mode, edge }); p.repeat.v = axis({ mode, edge })
    return { path: 'w', width: 204, height: 204, ppm: 64, props: p, seamless }
  }
  const h = edgeInsetTexels(204) / 204 // the inset: 2 texels at this size

  it('wrap (seamless) keeps the plain repeat: nothing to cut', () => {
    expect(makePlan({ texture: tex('wrap'), info: () => undefined, map, extent: ext })).toBeNull()
    expect(makePlan({ texture: tex('auto', [true, true]), info: () => undefined, map, extent: ext })).toBeNull()
    expect(makePlan({ texture: tex('auto'), info: () => undefined, map, extent: ext })).toBeNull() // not analysed
  })
  it('clamp, or auto on a texture whose edges do not match: cut at every tile, every UV stays a texel inside', () => {
    for (const t of [tex('clamp'), tex('auto', [false, false])]) {
      const plan = makePlan({ texture: t, info: () => undefined, map, extent: ext })!
      expect(plan).not.toBeNull()
      let pieces = 0
      for (const tri of wall) for (const pc of plan.pieces(tri)) {
        pieces++
        const us = pc.verts.map((v) => v.uv[0]), vs = pc.verts.map((v) => v.uv[1])
        const tu = Math.floor((Math.min(...us) + Math.max(...us)) / 2), tv = Math.floor((Math.min(...vs) + Math.max(...vs)) / 2)
        for (const u of us) { expect(u - tu).toBeGreaterThanOrEqual(h - 1e-9); expect(u - tu).toBeLessThanOrEqual(1 - h + 1e-9) }
        for (const v of vs) { expect(v - tv).toBeGreaterThanOrEqual(h - 1e-9); expect(v - tv).toBeLessThanOrEqual(1 - h + 1e-9) }
      }
      expect(pieces).toBeGreaterThan(wall.length * 2)
    }
  })
  it('only the failing axis is cut: seamless horizontally, clamped vertically', () => {
    const plan = makePlan({ texture: tex('auto', [true, false]), info: () => undefined, map, extent: ext })!
    const us = new Set<number>()
    for (const tri of wall) for (const pc of plan.pieces(tri)) for (const v of pc.verts) us.add(Math.round(v.uv[0] * 1e6))
    // u runs 0 .. 4 / natural continuously (no inset, no cut at u = 1)
    expect(Math.max(...us) / 1e6).toBeCloseTo(4 / natural, 5)
  })
  it('the picture is the same up to the one-texel squeeze: v at the middle of a tile', () => {
    const plan = makePlan({ texture: tex('clamp'), info: () => undefined, map, extent: ext })!
    // world y = 1 (v = (7 - 1) / natural = 1.882): tile 1, fraction 0.882 -> inset
    let v = NaN
    for (const tri of wall) for (const pc of plan.pieces(tri)) {
      const xs = pc.verts.map((q) => q.w[0] * tri[0][0] + q.w[1] * tri[1][0] + q.w[2] * tri[2][0]), ys = pc.verts.map((q) => q.w[0] * tri[0][1] + q.w[1] * tri[1][1] + q.w[2] * tri[2][1])
      if (Math.min(...ys) < 1 - 1e-6 && Math.max(...ys) > 1 + 1e-6 && Math.min(...xs) < 2 && Math.max(...xs) > 2 && v !== v) {
        // pick the vertex with y closest to 1 on this piece
        const i = ys.map((y) => Math.abs(y - 1)).indexOf(Math.min(...ys.map((y) => Math.abs(y - 1))))
        if (Math.abs(ys[i] - 1) < 1e-6) v = pc.verts[i].uv[1]
      }
    }
    if (v === v) expect(v).toBeCloseTo(1 + h + ((6 / natural) - 1) * (1 - 2 * h), 6)
  })
  it('mirror + clamp keeps the fold continuous and inside', () => {
    const plan = makePlan({ texture: tex('clamp', undefined, 'mirror'), info: () => undefined, map, extent: ext })!
    for (const tri of wall) for (const pc of plan.pieces(tri)) for (const v of pc.verts) {
      const f = v.uv[1] - Math.floor((Math.min(...pc.verts.map((q) => q.uv[1])) + Math.max(...pc.verts.map((q) => q.uv[1]))) / 2)
      expect(f).toBeGreaterThanOrEqual(h - 1e-9); expect(f).toBeLessThanOrEqual(1 - h + 1e-9)
    }
  })
  it('splitAtTiles: every segment lies in one tile and the range is still tiled exactly', () => {
    const segs = axisSegments(axis({ mode: 'repeat' }), null, -2.3, 3.7, { main: true, alt: false })!
    expect(segs[0].lo).toBeCloseTo(-2.3, 9); expect(segs[segs.length - 1].hi).toBeCloseTo(3.7, 9)
    for (let i = 1; i < segs.length; i++) expect(segs[i].lo).toBeCloseTo(segs[i - 1].hi, 9)
    for (const s of segs) { expect(s.tile).toBeDefined(); expect(s.s * s.lo + s.c).toBeGreaterThanOrEqual(s.tile! - 1e-9); expect(s.s * s.hi + s.c).toBeLessThanOrEqual(s.tile! + 1 + 1e-9) }
  })
  it('cap: a huge face falls back to the plain repeat instead of exploding', () => {
    const big = { umin: 0, umax: 5000, vmin: 0, vmax: 5000 }
    expect(makePlan({ texture: tex('clamp'), info: () => undefined, map: { u: [1, 0, 0, 0], v: [0, 1, 0, 0] }, extent: big })).toBeNull()
  })
})
