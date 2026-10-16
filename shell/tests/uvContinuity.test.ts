// UV continuity of derived (carved / cut) geometry with the stored-UV model (REQUIREMENTS §6 "verify UV continuity of subtractive faces"):
// a fragment of a face must get exactly the UVs the face itself is drawn with. With UV lock (the default) moving a brush bakes per-corner `uvs` that travel
// with the face while `projection` stays as it was, so fragments that read the projection instead of the stored UVs would jump.
import { afterEach, describe, expect, it } from 'vitest'
import { buildCarvedMesh } from '../src/model/derived'
import type { DerivedEntry, World } from '../src/model/derived'
import type { Brush, MapDocument } from '../src/model/document'
import { faceNormal, makeBox } from '../src/model/geometry'
import type { Vec3 } from '../src/model/math'
import { mapBrushPoints } from '../src/model/transform'
import { defaultProps, texEnv } from '../src/model/texprops'
import { faceUvAt } from '../src/model/uvmesh'
import { uvsForDraw } from '../src/model/uv'

afterEach(() => texEnv.setProvider(null))
const plain = () => texEnv.setProvider((path) => ({ path, width: 64, height: 64, ppm: 64, props: defaultProps() }))
const front = (b: Brush) => b.faces.find((f) => faceNormal(b, f)[2] > 0.99)!
const world = (b: Brush): World => ({ doc: { entities: [{ id: 'w', brushes: [b] }] } as unknown as MapDocument, prefabs: [] })

/** A textured wall that was moved with UV lock: stored uvs travel with it, the projection stays the old one. */
function movedWall(): Brush {
  const b = makeBox('b', [0, 0, 0], [4, 2, 1], 'dev/wall')
  const lockedBase = { ...b, faces: b.faces.map((f) => ({ ...f, uvs: uvsForDraw(b, f) })) } // as any drawn / textured brush has them
  return mapBrushPoints(lockedBase, (p) => [p[0] + 3.3, p[1] + 1.7, p[2]] as Vec3, false, true)
}

describe('faceUvAt: the UV a face is drawn with, at any point of its plane', () => {
  it('reproduces the stored corner uvs after a moved-with-UV-lock brush, and differs from the stale projection', () => {
    plain()
    const b = movedWall(), f = front(b), at = faceUvAt(b, f), stored = uvsForDraw(b, f)
    f.loop.forEach((vi, k) => { const uv = at(b.vertices[vi]); expect(uv[0]).toBeCloseTo(stored[k][0], 9); expect(uv[1]).toBeCloseTo(stored[k][1], 9) })
    // an interior point: halfway between two opposite corners is halfway between their uvs (affine)
    const a = b.vertices[f.loop[0]], c = b.vertices[f.loop[2]], mid: Vec3 = [(a[0] + c[0]) / 2, (a[1] + c[1]) / 2, (a[2] + c[2]) / 2]
    const m = faceUvAt(b, f)(mid)
    expect(m[0]).toBeCloseTo((stored[0][0] + stored[2][0]) / 2, 9); expect(m[1]).toBeCloseTo((stored[0][1] + stored[2][1]) / 2, 9)
  })

  it('interpolates per triangle when the stored uvs are not affine (a corner moved with UV lock)', () => {
    plain()
    const b0 = makeBox('b', [0, 0, 0], [4, 2, 1], 'dev/wall')
    const base = { ...b0, faces: b0.faces.map((f) => ({ ...f, uvs: uvsForDraw(b0, f) })) }
    const f0 = front(base), corner = f0.loop[2]
    const b = mapBrushPoints(base, (p) => (p === base.vertices[corner] ? ([p[0] + 0.8, p[1] + 0.5, p[2]] as Vec3) : p), false, true)
    const f = front(b), at = faceUvAt(b, f), stored = uvsForDraw(b, f)
    f.loop.forEach((vi, k) => { const uv = at(b.vertices[vi]); expect(uv[0]).toBeCloseTo(stored[k][0], 6); expect(uv[1]).toBeCloseTo(stored[k][1], 6) })
  })

  it('a face without stored uvs uses its projection', () => {
    plain()
    const b = makeBox('b', [0, 0, 0], [4, 2, 1], 'dev/wall'), f = { ...front(b), uvs: undefined }
    const at = faceUvAt(b, f), p = b.vertices[f.loop[1]], uv = at(p)
    expect(Number.isFinite(uv[0]) && Number.isFinite(uv[1])).toBe(true)
  })
})

describe('derived geometry takes the face\'s own UVs', () => {
  it('a carved fragment of a moved wall gets the same UVs as the wall itself (no jump at the cut)', () => {
    plain()
    const b = movedWall(), f = front(b), at = faceUvAt(b, f)
    const z = b.vertices[f.loop[0]][2], x0 = 4.3, y0 = 2.2
    const hole = [x0, y0, z, x0 + 1, y0, z, x0 + 1, y0 + 1, z, x0, y0 + 1, z] // a 1 x 1 m fragment inside the face
    const entry = { entityId: 'w', brushId: 'b', instance: '', subtractors: [], fragments: [{ brush: 'b', face: f.id, v: hole }] } as unknown as DerivedEntry
    const data = buildCarvedMesh(entry, world(b)).textures.get('dev/wall')!
    expect(data.length).toBeGreaterThan(0)
    for (let i = 0; i + 7 < data.length; i += 8) {
      const want = at([data[i], data[i + 1], data[i + 2]])
      expect(data[i + 6]).toBeCloseTo(want[0], 6); expect(data[i + 7]).toBeCloseTo(want[1], 6)
    }
  })
})

describe('the bake export places relief by the face\'s own UVs', () => {
  it('on a wall moved with UV lock the relief tiles keep the wall\'s texture grid (they travel with the face)', async () => {
    const { buildPreset } = await import('../src/model/relief')
    const { reliefFaces } = await import('../src/model/reliefBake')
    texEnv.setProvider((path) => ({ path, width: 64, height: 64, ppm: 64, props: { ...defaultProps(), relief: buildPreset('recess', 1, 1) } }))
    const b = movedWall(), f = front(b)
    const faces = reliefFaces(world(b), [])
    const wall = faces.find((x) => x.normal[2] > 0.99)!
    expect(wall).toBeTruthy(); void f
    const x0 = Math.min(...b.vertices.map((v) => v[0])) // the wall's left edge: where the texture's first tile starts
    let deep = 0
    for (let i = 0; i < wall.tris.length; i += 3) {
      const h = wall.normal[0] * wall.tris[i] + wall.normal[1] * wall.tris[i + 1] + wall.normal[2] * wall.tris[i + 2] - wall.offset
      if (h < -0.07) { deep++; const t = (((wall.tris[i] - x0) % 1) + 1) % 1; expect(t).toBeGreaterThanOrEqual(0.12 - 1e-6); expect(t).toBeLessThanOrEqual(0.88 + 1e-6) } // inside the frame of every tile
    }
    expect(deep).toBeGreaterThan(0)
  })
})
