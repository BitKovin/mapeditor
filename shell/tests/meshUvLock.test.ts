import { describe, expect, it } from 'vitest'
import { faceNormal, makeBox } from '../src/model/geometry'
import { transformVerts } from '../src/model/meshOps'
import { projectFace, projectionOf } from '../src/model/uv'
import { faceAffine } from '../src/model/uvmesh'

const setup = () => {
  const b0 = makeBox('b', [0, 0, 0], [4, 2, 1], 't')
  const f0 = b0.faces.find((f) => faceNormal(b0, f)[2] > 0.9)!
  // textured face with stored uvs, as after any texture edit
  const p = { ...projectionOf(f0), scale: [2, 2] as [number, number], shift: [0.1, 0.2] as [number, number] }
  const b = { ...b0, faces: b0.faces.map((f) => (f.id === f0.id ? { ...f, projection: p, uvs: projectFace(b0, f, p) } : f)) }
  const f = b.faces.find((x) => x.id === f0.id)!
  const top = f.loop.filter((i) => b.vertices[i][1] > 1)
  return { b, f, top }
}
const uvOf = (b: ReturnType<typeof setup>['b'], id: number) => b.faces.find((x) => x.id === id)!.uvs!

describe('UV lock in mesh mode', () => {
  it('tilting a face far out of its plane keeps its texture attached (no jump to new axes)', () => {
    const { b, f, top } = setup()
    const before = uvOf(b, f.id)
    const r = transformVerts(b, top, (q) => [q[0], q[1], q[2] + 3], true)
    expect('brush' in r).toBe(true)
    const after = (r as { brush: typeof b }).brush
    const nf = after.faces.find((x) => x.id === f.id)!
    // the face is still one face here (planar), its corners keep the UVs they had
    expect(nf.uvs).toBeTruthy()
    nf.loop.forEach((vi, k) => {
      const old = b.faces.find((x) => x.id === f.id)!.loop.indexOf(vi)
      expect(nf.uvs![k][0]).toBeCloseTo(before[old][0], 9)
      expect(nf.uvs![k][1]).toBeCloseTo(before[old][1], 9)
    })
  })
  it('without UV lock the face is re-projected (the old behaviour)', () => {
    const { b, f, top } = setup()
    const r = transformVerts(b, top, (q) => [q[0], q[1], q[2] + 3], false) as { brush: typeof b }
    const nf = r.brush.faces.find((x) => x.id === f.id)!
    const n = faceNormal(r.brush, nf)
    expect(Math.abs(n[1])).toBeGreaterThan(Math.abs(n[2]) - 1e-9) // dominant axis switched: axial now uses (x, z)
    const locked = (transformVerts(b, top, (q) => [q[0], q[1], q[2] + 3], true) as { brush: typeof b }).brush.faces.find((x) => x.id === f.id)!
    expect(JSON.stringify(nf.uvs)).not.toBe(JSON.stringify(locked.uvs))
  })
  it('a non-planar move splits the face; every piece keeps the old map (lock)', () => {
    const { b, f } = setup()
    const one = f.loop.filter((i) => b.vertices[i][1] > 1)[0]
    const r = transformVerts(b, [one], (q) => [q[0], q[1], q[2] + 1], true) as { brush: typeof b }
    const pieces = r.brush.faces.filter((x) => x.texture === f.texture && (x.id === f.id || x.id > Math.max(...b.faces.map((y) => y.id))))
    expect(pieces.length).toBeGreaterThan(1)
    const m = faceAffine(b, b.faces.find((x) => x.id === f.id)!)
    for (const pc of pieces) pc.loop.forEach((vi, k) => {
      const q = r.brush.vertices[vi]
      expect(pc.uvs![k][0]).toBeCloseTo(m.u[0] * q[0] + m.u[1] * q[1] + m.u[2] * q[2] + m.u[3], 9)
      expect(pc.uvs![k][1]).toBeCloseTo(m.v[0] * q[0] + m.v[1] * q[1] + m.v[2] * q[2] + m.v[3], 9)
    })
  })
  it('faces whose corners did not move are untouched', () => {
    const { b, f, top } = setup()
    const r = transformVerts(b, top, (q) => [q[0], q[1] + 0.5, q[2]], true) as { brush: typeof b }
    const other = b.faces.find((x) => x.id !== f.id && x.loop.every((i) => !top.includes(i)))!
    expect(r.brush.faces.find((x) => x.id === other.id)).toBe(other)
  })
})
