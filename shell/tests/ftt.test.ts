// Fast Texture Tool model (REQUIREMENTS §6 B5): transforms of the UV island, mirror, edge alignment, unwrap modes, drag maths and hit tests.
import { afterEach, describe, expect, it } from 'vitest'
import { faceNormal, makeBox } from '../src/model/geometry'
import type { Brush } from '../src/model/document'
import {
  IDENTITY, alignEdgeXform, applyXform, boundsCentre, compose, dragXform, faceUvPolygon, handlePoint, hitTest, idealSize, islandPolygons, mirrorAbout,
  nearestEdge, pointInPolygon, polygonsBounds, rotateAbout, scaleAbout, transformFaces, translate, unwrapFaces, Xform,
} from '../src/model/ftt'
import { defaultProps, texEnv } from '../src/model/texprops'

afterEach(() => texEnv.setProvider(null))
const withTex = () => texEnv.setProvider((path) => ({ path, width: 64, height: 64, ppm: 64, props: defaultProps() }))
const box = (): Brush => makeBox('b', [0, 0, 0], [4, 2, 1], 'dev/wall')
const faceWith = (b: Brush, test: (n: number[]) => boolean) => b.faces.find((f) => test(faceNormal(b, f)))!
const front = (b: Brush) => faceWith(b, (n) => n[2] > 0.99), right = (b: Brush) => faceWith(b, (n) => n[0] > 0.99)
const close = (a: number[], b: number[], d = 6) => { expect(a).toHaveLength(b.length); a.forEach((x, i) => expect(x).toBeCloseTo(b[i], d)) }

describe('transform algebra', () => {
  it('translate, rotate, scale, mirror and compose', () => {
    close(applyXform(translate(1, 2), [3, 4]), [4, 6])
    close(applyXform(rotateAbout(90, [0, 0]), [1, 0]), [0, 1]) // clockwise on screen: u right becomes v down
    close(applyXform(rotateAbout(90, [1, 1]), [2, 1]), [1, 2])
    close(applyXform(scaleAbout(2, 3, [1, 1]), [2, 2]), [3, 4]); close(applyXform(scaleAbout(2, 3, [1, 1]), [1, 1]), [1, 1])
    close(applyXform(mirrorAbout('u', 2), [3, 5]), [1, 5]); close(applyXform(mirrorAbout('v', 2), [3, 5]), [3, -1])
    const t = compose(translate(1, 0), scaleAbout(2, 2, [0, 0])) // scale first, then move
    close(applyXform(t, [1, 1]), [3, 2]); close(applyXform(compose(IDENTITY, t), [1, 1]), [3, 2])
  })
})

describe('transformFaces', () => {
  const check = (t: Xform, ids?: (b: Brush) => number[]) => {
    withTex()
    const b = box(), use = ids ? ids(b) : [front(b).id]
    const before = islandPolygons(b, use)
    const r = transformFaces(b, use, t)
    expect(r.changed).toBe(use.length)
    for (const { id, poly } of before) {
      const after = faceUvPolygon(r.brush, r.brush.faces.find((f) => f.id === id)!)
      poly.forEach((p, i) => close(after[i], applyXform(t, p)))   // exact on the plane, corner by corner
    }
    return r
  }
  it('moves, rotates, scales, shears-free flips: every corner lands exactly where the transform says', () => {
    check(translate(0.5, -0.25)); check(rotateAbout(33, [1, 0.5])); check(scaleAbout(0.5, 2, [0, 0])); check(mirrorAbout('u', 1.5)); check(mirrorAbout('v', 0.4))
    check(compose(rotateAbout(17, [0, 0]), scaleAbout(2, 1, [1, 1])))
  })
  it('leaves other faces alone and returns the same brush when nothing changed', () => {
    withTex(); const b = box(), f = front(b)
    const r = transformFaces(b, [f.id], translate(1, 1))
    for (const o of b.faces) if (o.id !== f.id) expect(r.brush.faces.find((x) => x.id === o.id)).toBe(o)
    expect(transformFaces(b, [999], translate(1, 1)).brush).toBe(b)
    expect(transformFaces(b, [f.id], scaleAbout(0, 0, [0, 0])).changed).toBe(0) // a collapsing transform is refused per face
  })
  it('the stored projection stays editable (free kind) and uvs are written', () => {
    withTex(); const b = box(), r = transformFaces(b, [front(b).id], rotateAbout(40, [0, 0]))
    const f = r.brush.faces.find((x) => x.id === front(b).id)!
    expect((f.projection as { kind?: string }).kind).toBe('free'); expect(f.uvs).toHaveLength(f.loop.length)
  })
  it('an island of two faces moves as one: a texture that continues over their shared edge still does', () => {
    withTex()
    const b0 = box(), a = front(b0).id, c = right(b0).id
    const flowed = unwrapFaces(b0, [a, c], 'follow')
    expect(flowed.changed).toBeGreaterThan(0)
    const seam = (b: Brush) => {
      const A = b.faces.find((f) => f.id === a)!, C = b.faces.find((f) => f.id === c)!
      const shared = A.loop.filter((i) => C.loop.includes(i))
      const pa = faceUvPolygon(b, A), pc = faceUvPolygon(b, C)
      return shared.map((i) => [pa[A.loop.indexOf(i)], pc[C.loop.indexOf(i)]])
    }
    for (const [x, y] of seam(flowed.brush)) close(x, y, 5)
    const moved = transformFaces(flowed.brush, [a, c], compose(rotateAbout(25, [0.3, 0.2]), scaleAbout(1.5, 1.5, [0, 0])))
    expect(moved.changed).toBe(2)
    for (const [x, y] of seam(moved.brush)) close(x, y, 5)
  })
})

describe('mirror feature', () => {
  it('Mirror U / V flips the island about its centre: bounds stay, the picture reverses', () => {
    withTex(); const b = box(), f = front(b)
    const poly = faceUvPolygon(b, f), bounds = polygonsBounds([poly])!, c = boundsCentre(bounds)
    for (const axis of ['u', 'v'] as const) {
      const r = transformFaces(b, [f.id], mirrorAbout(axis, axis === 'u' ? c[0] : c[1]))
      const after = faceUvPolygon(r.brush, r.brush.faces.find((x) => x.id === f.id)!), nb = polygonsBounds([after])!
      close(nb.min, bounds.min); close(nb.max, bounds.max)
      const i = axis === 'u' ? 0 : 1
      poly.forEach((p, k) => expect(after[k][i]).toBeCloseTo(bounds.min[i] + bounds.max[i] - p[i], 6))
    }
    // mirroring twice is the identity
    const once = transformFaces(b, [f.id], mirrorAbout('u', c[0])).brush
    const twice = transformFaces(once, [f.id], mirrorAbout('u', c[0])).brush
    faceUvPolygon(twice, twice.faces.find((x) => x.id === f.id)!).forEach((p, k) => close(p, poly[k]))
  })
})

describe('align edge', () => {
  it('rotates the nearest axis onto the edge, about its midpoint', () => {
    const a: [number, number] = [0, 0], c: [number, number] = [Math.cos(0.35), Math.sin(0.35)]
    const t = alignEdgeXform(a, c)!
    const [p, q] = [applyXform(t, a), applyXform(t, c)]
    expect(q[1] - p[1]).toBeCloseTo(0, 9); expect(Math.hypot(q[0] - p[0], q[1] - p[1])).toBeCloseTo(1, 9)
    const mid = applyXform(t, [(a[0] + c[0]) / 2, (a[1] + c[1]) / 2]); close(mid, [(a[0] + c[0]) / 2, (a[1] + c[1]) / 2])
    const vert = alignEdgeXform([0, 0], [0.1, 1])!, [v0, v1] = [applyXform(vert, [0, 0]), applyXform(vert, [0.1, 1])]
    expect(v1[0] - v0[0]).toBeCloseTo(0, 9)
    expect(alignEdgeXform([0, 0], [1, 0])).toBe(IDENTITY); expect(alignEdgeXform([1, 1], [1, 1])).toBeNull()
  })
})

describe('unwrap modes', () => {
  it('ortho and axial re-project each face; follow needs two faces', () => {
    withTex(); const b = box(), ids = [front(b).id, right(b).id]
    expect(unwrapFaces(b, ids, 'ortho').changed).toBe(2); expect(unwrapFaces(b, ids, 'axial').changed).toBe(2)
    expect(unwrapFaces(b, [front(b).id], 'follow').changed).toBe(0)
    expect(unwrapFaces(b, [999], 'ortho').brush).toBe(b)
  })
})

describe('ideal shape', () => {
  it('is the face extent in metres over the natural size, whatever the current scale', () => {
    withTex(); const b = box(), f = front(b)
    close(idealSize(b, f), [4, 2])
    const scaled = transformFaces(b, [f.id], scaleAbout(3, 0.5, [0, 0])).brush
    close(idealSize(scaled, scaled.faces.find((x) => x.id === f.id)!), [4, 2], 5)
  })
})

describe('drag maths', () => {
  const bounds = { min: [1, 2] as [number, number], max: [3, 4] as [number, number] }
  const o = { snap: 0, shift: false }
  it('move translates; snapping puts the island corner on the grid', () => {
    close(applyXform(dragXform('move', [2, 3], [2.3, 3.4], bounds, o), [1, 2]), [1.3, 2.4])
    close(applyXform(dragXform('move', [2, 3], [2.3, 3.4], bounds, { snap: 0.25, shift: false }), [1, 2]), [1.25, 2.5])
  })
  it('scale handles keep the opposite side fixed', () => {
    const e = dragXform('e', [3, 3], [4, 3.5], bounds, o) // east handle: u 3 -> 4 over a width of 2 => x1.5; v untouched
    close(applyXform(e, [1, 2]), [1, 2]); close(applyXform(e, [3, 4]), [4, 4])
    const nw = dragXform('nw', [1, 2], [0, 1], bounds, o) // pivot = se corner (3, 4)
    close(applyXform(nw, [3, 4]), [3, 4]); close(applyXform(nw, [1, 2]), [0, 1])
    const uni = dragXform('se', [3, 4], [5, 4], bounds, { snap: 0, shift: true }) // width x2, height kept by aspect lock => x2
    close(applyXform(uni, [3, 4]), [5, 6]); close(applyXform(uni, [1, 2]), [1, 2])
    const flip = dragXform('e', [3, 3], [0.5, 3], bounds, o) // dragged past the west edge: mirrored, not collapsed
    expect(applyXform(flip, [3, 3])[0]).toBeLessThan(1)
  })
  it('rotate follows the pointer around the centre, in 15 degree steps with Shift', () => {
    const c = boundsCentre(bounds)
    const t = dragXform('rotate', [c[0] + 1, c[1]], [c[0], c[1] + 1], bounds, o)
    close(applyXform(t, [c[0] + 1, c[1]]), [c[0], c[1] + 1])
    const s = dragXform('rotate', [c[0] + 1, c[1]], [c[0] + Math.cos(0.4), c[1] + Math.sin(0.4)], bounds, { snap: 0, shift: true })
    const q = applyXform(s, [c[0] + 1, c[1]]), ang = (Math.atan2(q[1] - c[1], q[0] - c[0]) * 180) / Math.PI
    expect(ang).toBeCloseTo(30, 6) // 22.9 deg snaps to 30
  })
})

describe('hit tests', () => {
  const poly: [number, number][] = [[1, 2], [3, 2], [3, 4], [1, 4]]
  const bounds = polygonsBounds([poly])!
  it('handles first, then the body, then nothing', () => {
    expect(hitTest(bounds, [poly], [3.01, 4.01], 0.05, 0.4)).toBe('se'); expect(hitTest(bounds, [poly], [2, 1.6], 0.05, 0.4)).toBe('rotate')
    expect(hitTest(bounds, [poly], [2, 3], 0.05, 0.4)).toBe('move'); expect(hitTest(bounds, [poly], [9, 9], 0.05, 0.4)).toBeNull()
    expect(handlePoint(bounds, 'rotate', 0.4)).toEqual([2, 1.6])
  })
  it('point in polygon and nearest edge', () => {
    expect(pointInPolygon([2, 3], poly)).toBe(true); expect(pointInPolygon([0, 3], poly)).toBe(false)
    expect(nearestEdge([poly], [2, 2.02], 0.1)).toEqual([[1, 2], [3, 2]]); expect(nearestEdge([poly], [2, 3], 0.1)).toBeNull()
  })
})
