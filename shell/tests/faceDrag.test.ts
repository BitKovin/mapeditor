import { describe, expect, it } from 'vitest'
import type { Brush } from '../src/model/document'
import { dragFace } from '../src/model/faceDrag'
import { faceNormal, makeBox, signedVolume } from '../src/model/geometry'
import { dot } from '../src/model/math'

const ok = (r: ReturnType<typeof dragFace>): Brush => { if ('error' in r) throw new Error(r.error); return r.brush }
const near = (a: number, b: number, e = 1e-9) => expect(Math.abs(a - b)).toBeLessThan(e)
// box faces: 0 -Z, 1 +Z, 2 -X, 3 +X, 4 -Y, 5 +Y

describe('dragFace (Radiant-style plane drag)', () => {
  it('moves one face of a box; the other five planes keep their normals', () => {
    const b = makeBox('b', [0, 0, 0], [2, 2, 2])
    const r = ok(dragFace(b, 3, 3))
    near(signedVolume(r), 5 * 2 * 2)
    b.faces.forEach((f, i) => { const a = faceNormal(b, f), c = faceNormal(r, r.faces[i]); near(dot(a, c), 1) })
    expect(r.vertices.filter((v) => v[0] === 5).length).toBe(4)
  })
  it('shrinks a face with a negative distance and refuses to pass the opposite side', () => {
    const b = makeBox('b', [0, 0, 0], [2, 2, 2])
    near(signedVolume(ok(dragFace(b, 1, -1))), 2 * 2 * 1)
    expect('error' in dragFace(b, 1, -2.5)).toBe(true)
    expect('error' in dragFace(b, 1, -2)).toBe(true) // collapsed to zero thickness
  })
  it('on a wedge, the slanted face moves along its own normal and its neighbours stay put', () => {
    // triangular prism: base square 0..2 (x,y), roof plane from x=0,z=0 up to x=2,z=2 -> faces with 3 corners per vertex
    const v: [number, number, number][] = [[0, 0, 0], [2, 0, 0], [2, 0, 2], [0, 2, 0], [2, 2, 0], [2, 2, 2]]
    const wedge: Brush = { id: 'w', vertices: v, faces: [
      { id: 0, loop: [0, 3, 4, 1] },   // bottom -Z
      { id: 1, loop: [1, 4, 5, 2] },   // +X
      { id: 2, loop: [0, 1, 2] },      // -Y
      { id: 3, loop: [3, 5, 4] },      // +Y
      { id: 4, loop: [0, 2, 5, 3] },   // slanted
    ] }
    expect(signedVolume(wedge)).toBeGreaterThan(0)
    const n0 = faceNormal(wedge, wedge.faces[4])
    const r = ok(dragFace(wedge, 4, 0.5))
    const n1 = faceNormal(r, r.faces[4])
    near(dot(n0, n1), 1, 1e-9)
    // slanted face really moved by 0.5 along its normal
    near(dot(n0, r.vertices[0]) - dot(n0, wedge.vertices[0]), 0.5, 1e-9)
    for (const i of [0, 1, 2, 3]) near(dot(faceNormal(wedge, wedge.faces[i]), faceNormal(r, r.faces[i])), 1, 1e-9)
  })
  it('drags a face with a 3+ face corner (pyramid side) by re-intersecting the planes', () => {
    const v: [number, number, number][] = [[0, 0, 0], [2, 0, 0], [2, 2, 0], [0, 2, 0], [1, 1, 2]]
    const pyr: Brush = { id: 'p', vertices: v, faces: [
      { id: 0, loop: [0, 3, 2, 1] }, { id: 1, loop: [0, 1, 4] }, { id: 2, loop: [1, 2, 4] }, { id: 3, loop: [2, 3, 4] }, { id: 4, loop: [3, 0, 4] },
    ] }
    const n0 = faceNormal(pyr, pyr.faces[1])
    for (const dist of [0.2, -0.2]) {
      const r = ok(dragFace(pyr, 1, dist))
      const f = r.faces.find((x) => x.id === 1)!
      near(dot(faceNormal(r, f), n0), 1, 1e-9)
      near(dot(n0, r.vertices[f.loop[0]]) - dot(n0, v[0]), dist, 1e-9)
      expect(signedVolume(r)).toBeGreaterThan(0)
      for (const id of [0, 2, 3, 4]) expect(r.faces.some((x) => x.id === id)).toBe(true)
    }
    expect('error' in dragFace(pyr, 1, -5)).toBe(true)
  })
  it('keeps the texture where it was on the neighbours (uv fit follows the new corners)', () => {
    const b = makeBox('b', [0, 0, 0], [2, 2, 2])
    // +Y face (id 5) with uvs = world x / z -> after dragging +X face outward by 1 the +Y face grows in x; its uvs must continue linearly
    const f = b.faces[5]
    b.faces[5] = { ...f, uvs: f.loop.map((i) => [b.vertices[i][0], b.vertices[i][2]] as [number, number]) }
    const r = ok(dragFace(b, 3, 1))
    r.faces[5].uvs!.forEach((uv, k) => { const p = r.vertices[r.faces[5].loop[k]]; near(uv[0], p[0]); near(uv[1], p[2]) })
  })
})

import { extendFace } from '../src/model/faceDrag'
import { isConvex } from '../src/model/brushOps'
import { isProperSolid } from '../src/model/meshOps'
import { validateBrush } from '../src/model/geometry'

describe('extendFace (Alt-drag extend)', () => {
  const textured = (): Brush => {
    const b = makeBox('b', [0, 0, 0], [2, 2, 2])
    // a simple planar mapping per face: u,v = the two coordinates that vary on the face
    const axes: [number, number][] = [[0, 1], [0, 1], [1, 2], [1, 2], [0, 2], [0, 2]]
    b.faces.forEach((f, i) => { f.uvs = f.loop.map((vi) => [b.vertices[vi][axes[i][0]], b.vertices[vi][axes[i][1]]] as [number, number]) })
    return b
  }
  const get = (r: ReturnType<typeof extendFace>): Brush => { if ('error' in r) throw new Error(r.error); return r.brush }

  it('grows a new convex box out of the face and leaves the source alone', () => {
    const b = textured(), before = JSON.stringify(b)
    const n = get(extendFace(b, 3, 3, 'new')) // +X face
    expect(JSON.stringify(b)).toBe(before)
    expect(n.id).toBe('new')
    expect(n.op).toBe('add')
    near(signedVolume(n), 3 * 2 * 2)
    expect(validateBrush(n, true)).toBeNull()
    expect(isConvex(n)).toBe(true)
    expect(isProperSolid(n)).toBe(true)
    expect(Math.max(...n.vertices.map((v) => v[0]))).toBeCloseTo(5)
    expect(Math.min(...n.vertices.map((v) => v[0]))).toBeCloseTo(2)
    expect(n.faces.length).toBe(6)
    // face 0 is the end cap: it faces the same way as the dragged face
    near(dot(faceNormal(n, n.faces.find((f) => f.id === 0)!), [1, 0, 0]), 1)
  })

  it('the side walls continue the mapping of the faces they grow from, so textures run on without a seam', () => {
    const b = textured()
    const n = get(extendFace(b, 3, 3, 'new'))
    // every side wall of the +X extension lies in the plane of a neighbouring face of the box (-Z, +Z, -Y, +Y):
    // a vertex shared with the box has the same uv as in the box, a vertex further out continues the same linear map
    for (const f of n.faces.filter((x) => x.id >= 2)) {
      expect(f.uvs).toBeDefined()
      const P = f.loop.map((vi) => n.vertices[vi])
      const flat = (a: number) => P.every((p) => Math.abs(p[a] - P[0][a]) < 1e-9)
      const second = flat(2) ? 1 : 2 // wall in a z = const plane continues the (x, y) map, in a y = const plane the (x, z) map
      expect(flat(1) || flat(2)).toBe(true)
      f.loop.forEach((vi, k) => { near(f.uvs![k][0], n.vertices[vi][0]); near(f.uvs![k][1], n.vertices[vi][second]) })
    }
  })

  it('keeps op and carve scope (extending a subtractive brush gives a subtractive brush)', () => {
    const b: Brush = { ...textured(), op: 'subtract', carve: { self: false, structural: true, nonStructural: true } }
    const n = get(extendFace(b, 1, 1, 'new'))
    expect(n.op).toBe('subtract')
    expect(n.carve).toEqual({ self: false, structural: true, nonStructural: true })
  })

  it('works on a slanted face and on the face of a wedge', () => {
    const v: [number, number, number][] = [[0, 0, 0], [2, 0, 0], [2, 0, 2], [0, 2, 0], [2, 2, 0], [2, 2, 2]]
    const wedge: Brush = { id: 'w', vertices: v, faces: [
      { id: 0, loop: [0, 3, 4, 1] }, { id: 1, loop: [1, 4, 5, 2] }, { id: 2, loop: [0, 2, 5, 3] }, { id: 3, loop: [0, 1, 2] }, { id: 4, loop: [3, 5, 4] },
    ] }
    const n = get(extendFace(wedge, 2, 1, 'new'))
    expect(isConvex(n)).toBe(true)
    expect(signedVolume(n)).toBeGreaterThan(0)
  })

  it('refuses zero / negative distance and unknown faces', () => {
    const b = makeBox('b', [0, 0, 0], [2, 2, 2])
    expect('error' in extendFace(b, 3, 0, 'n')).toBe(true)
    expect('error' in extendFace(b, 3, -1, 'n')).toBe(true)
    expect('error' in extendFace(b, 99, 1, 'n')).toBe(true)
  })
})

describe('Alt + drag resizes the brush in place (REQUIREMENTS §17 #4, §24 C1)', () => {
  it('outward grows, inward shrinks the same brush; too far inward is refused, never inside out', () => {
    const b = makeBox('b', [0, 0, 0], [2, 1, 1], 'dev/wall')
    const top = b.faces.find((f) => f.loop.every((v) => b.vertices[v][2] === 1))!
    const grown = dragFace(b, top.id, 0.5), shrunk = dragFace(b, top.id, -0.25)
    if ('error' in grown || 'error' in shrunk) throw new Error('drag failed')
    expect(grown.brush.id).toBe('b'); expect(shrunk.brush.id).toBe('b')
    expect(Math.max(...grown.brush.vertices.map((p) => p[2]))).toBeCloseTo(1.5, 9)
    expect(Math.max(...shrunk.brush.vertices.map((p) => p[2]))).toBeCloseTo(0.75, 9)
    expect('error' in dragFace(b, top.id, -1.5)).toBe(true)
  })
})
