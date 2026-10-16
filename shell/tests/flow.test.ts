import { describe, expect, it } from 'vitest'
import { Face } from '../src/model/document'
import { flowFaces } from '../src/model/flow'
import { faceNormal, makeBox } from '../src/model/geometry'
import { projectFace, projectionFromAffine, projectionOf } from '../src/model/uv'
import { faceAffine } from '../src/model/uvmesh'

// box x 0..4, y 0..2, z 0..1; the front wall is +z
const box = () => {
  const b0 = makeBox('b', [0, 0, 0], [4, 2, 1], 't')
  const front = b0.faces.find((f) => faceNormal(b0, f)[2] > 0.9)!
  const p = { scale: [2, 2] as [number, number], shift: [0.25, 0.5] as [number, number], rotate: 0 }
  const b = { ...b0, faces: b0.faces.map((f) => (f.id === front.id ? { ...f, texture: 'road', projection: p, uvs: projectFace(b0, f, p) } : f)) }
  return { b, front: b.faces.find((f) => f.id === front.id)!, by: (pred: (n: number[]) => boolean) => b.faces.find((f) => pred(faceNormal(b, f)))! }
}
const uvAt = (b: ReturnType<typeof box>['b'], f: Face, q: number[]): [number, number] => { const m = faceAffine(b, f); return [m.u[0] * q[0] + m.u[1] * q[1] + m.u[2] * q[2] + m.u[3], m.v[0] * q[0] + m.v[1] * q[1] + m.v[2] * q[2] + m.v[3]] }

describe('flow texture', () => {
  it('continues around a corner: the neighbouring wall is unfolded into the front wall plane, nothing stretched', () => {
    const { b, front, by } = box()
    const right = by((n) => n[0] > 0.9)
    const r = flowFaces(b, front.id, [right.id])!
    expect(r.flowed).toEqual([right.id])
    const nr = r.brush.faces.find((f) => f.id === right.id)!
    expect(nr.texture).toBe('road')
    // the shared edge (x = 4, z = 1) has the same UV on both faces
    for (const y of [0, 1, 2]) {
      const a = uvAt(r.brush, front, [4, y, 1]), c = uvAt(r.brush, nr, [4, y, 1])
      expect(c[0]).toBeCloseTo(a[0], 9); expect(c[1]).toBeCloseTo(a[1], 9)
    }
    // going back along the right wall (z from 1 to 0) continues u: u = (4 + (1 - z)) / 2 + 0.25
    expect(uvAt(r.brush, nr, [4, 1, 0])[0]).toBeCloseTo((4 + 1) / 2 + 0.25, 9)
    expect(uvAt(r.brush, nr, [4, 1, 0])[1]).toBeCloseTo(uvAt(r.brush, front, [4, 1, 1])[1], 9)
    // lengths are preserved: 1 m of wall = 0.5 repeat
    const d = uvAt(r.brush, nr, [4, 1, 0])[0] - uvAt(r.brush, nr, [4, 1, 1])[0]
    expect(d).toBeCloseTo(1 / 2, 9)
  })
  it('over the top edge: v continues upwards', () => {
    const { b, front, by } = box()
    const top = by((n) => n[1] > 0.9)
    const r = flowFaces(b, front.id, [top.id])!
    const nt = r.brush.faces.find((f) => f.id === top.id)!
    // front: v = (-y)/2 + 0.5 ; over the top edge (y = 2, z = 1) going back (z -> 0) it keeps decreasing like y increased
    expect(uvAt(r.brush, nt, [1, 2, 0])[1]).toBeCloseTo(uvAt(r.brush, front, [1, 2, 1])[1] - 1 / 2, 9)
    expect(uvAt(r.brush, nt, [1, 2, 1])[0]).toBeCloseTo(uvAt(r.brush, front, [1, 2, 1])[0], 9)
  })
  it('a target two faces away flows through the face in between (shortest chain), in order', () => {
    const { b, front, by } = box()
    const right = by((n) => n[0] > 0.9), back = by((n) => n[2] < -0.9)
    const r = flowFaces(b, front.id, [back.id])!
    expect(r.flowed).toHaveLength(2)
    expect(r.flowed.includes(back.id)).toBe(true)
    // whichever neighbour of the front wall was chosen as the way, the faces agree along their shared edge (the texture is continuous)
    const mid = r.brush.faces.find((f) => f.id === r.flowed[0])!, nb = r.brush.faces.find((f) => f.id === back.id)!
    const shared = mid.loop.filter((v) => nb.loop.includes(v))
    expect(shared).toHaveLength(2)
    for (const v of shared) {
      const a = mid.uvs![mid.loop.indexOf(v)], c = nb.uvs![nb.loop.indexOf(v)]
      expect(c[0]).toBeCloseTo(a[0], 9); expect(c[1]).toBeCloseTo(a[1], 9)
    }
    // nothing stretched: the back wall's uv extent equals its size over the repeat size
    const us = nb.uvs!.map((q) => q[0]), vs = nb.uvs!.map((q) => q[1])
    expect(Math.hypot(Math.max(...us) - Math.min(...us), Math.max(...vs) - Math.min(...vs)) * 2).toBeCloseTo(Math.hypot(4, 2), 9)
    void right
  })
  it('flowed faces get an editable projection that reproduces their UVs exactly', () => {
    const { b, front, by } = box()
    const right = by((n) => n[0] > 0.9)
    const r = flowFaces(b, front.id, [right.id])!
    const nr = r.brush.faces.find((f) => f.id === right.id)!
    const p = projectionOf(nr)
    expect(p.kind).toBe('free')
    const re = projectFace(r.brush, nr, p)
    re.forEach((q, i) => { expect(q[0]).toBeCloseTo(nr.uvs![i][0], 8); expect(q[1]).toBeCloseTo(nr.uvs![i][1], 8) })
  })
  it('the source is unchanged; unreachable / unknown ids give null; an empty target list gives null', () => {
    const { b, front, by } = box()
    const right = by((n) => n[0] > 0.9)
    const r = flowFaces(b, front.id, [right.id])!
    expect(r.brush.faces.find((f) => f.id === front.id)).toBe(front)
    expect(flowFaces(b, 999, [right.id])).toBeNull()
    expect(flowFaces(b, front.id, [999])).toBeNull()
    expect(flowFaces(b, front.id, [])).toBeNull()
  })
  it('the source repeat override travels with the texture', () => {
    const { b, front, by } = box()
    const right = by((n) => n[0] > 0.9)
    const withRep = { ...b, faces: b.faces.map((f) => (f.id === front.id ? { ...f, repeat: { u: { mode: 'mirror' } } } : f)) }
    const r = flowFaces(withRep, front.id, [right.id])!
    expect(r.brush.faces.find((f) => f.id === right.id)!.repeat).toEqual({ u: { mode: 'mirror' } })
  })
})

describe('projectionFromAffine', () => {
  it('round-trips random in-plane affine maps (rotation, shear, scale, mirror)', () => {
    let s = 5; const r = () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 4294967296 - 0.5 }
    const b = makeBox('b', [0, 0, 0], [3, 2, 1], 't'); const f = b.faces.find((x) => faceNormal(b, x)[2] > 0.9)!
    const n = faceNormal(b, f)
    for (let k = 0; k < 20; k++) {
      const gu: [number, number, number] = [r() * 3 + 0.2, r() * 3, 0], gv: [number, number, number] = [r() * 3, r() * 3 + 0.2, 0]
      const du = r() * 2, dv = r() * 2
      const p = projectionFromAffine(n, gu, gv, du, dv)
      if (!p) continue
      const uv = projectFace(b, f, p)
      f.loop.forEach((vi, i) => {
        const q = b.vertices[vi]
        expect(uv[i][0]).toBeCloseTo(gu[0] * q[0] + gu[1] * q[1] + du, 7)
        expect(uv[i][1]).toBeCloseTo(gv[0] * q[0] + gv[1] * q[1] + dv, 7)
      })
    }
  })
  it('degenerate maps give null', () => {
    expect(projectionFromAffine([0, 0, 1], [0, 0, 0], [0, 1, 0], 0, 0)).toBeNull()
    expect(projectionFromAffine([0, 0, 1], [1, 0, 0], [0, 0, 0], 0, 0)).toBeNull()
  })
})
