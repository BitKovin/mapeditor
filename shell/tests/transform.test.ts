import { describe, expect, it } from 'vitest'
import type { MapDocument } from '../src/model/document'
import { makeBox, signedVolume, validateBrush } from '../src/model/geometry'
import { buildShape, defaultShapeParams, ShapeId } from '../src/model/shapes'
import { mapBrushPoints, mirrors, rotateAbout, scaleAbout, selectionPivot, transformSelections, translateBy } from '../src/model/transform'
import { planeThrough, Rect } from '../src/tools/boxTool'
import { projectFace } from '../src/model/uv'
import { apply3, rotationFromAngles } from '../src/model/rotation'

const near = (a: number[], b: number[], eps = 1e-9) => a.forEach((v, i) => expect(Math.abs(v - b[i])).toBeLessThan(eps))
const doc = (): MapDocument => ({
  schemaVersion: '0.2', prefabInstances: [],
  entities: [
    { id: 'w', classname: 'worldspawn', brushes: [makeBox('a', [0, 0, 0], [2, 2, 2]), makeBox('c', [10, 0, 0], [12, 2, 2])] },
    { id: 'p', classname: 'light', origin: [5, 5, 5], angles: [0, 10, 0] },
  ],
})

describe('point maps', () => {
  it('rotates a quarter turn about Y around a pivot', () => {
    near(rotateAbout([1, 0, 1], [0, 1, 0], Math.PI / 2)([2, 0, 1]), [1, 0, 0])
    near(rotateAbout([0, 0, 0], [0, 0, 1], Math.PI)([1, 2, 3]), [-1, -2, 3])
  })
  it('scales about a pivot, per axis', () => {
    near(scaleAbout([1, 1, 1], [2, 1, 0.5])([2, 3, 3]), [3, 3, 2])
  })
  it('a rotation keeps volume; a mirror flips winding unless the brush is turned back', () => {
    const b = makeBox('a', [0, 0, 0], [1, 2, 3])
    expect(signedVolume(mapBrushPoints(b, rotateAbout([0, 0, 0], [1, 1, 0], 0.7)))).toBeCloseTo(6)
    expect(mirrors([-1, 1, 1])).toBe(true)
    expect(mirrors([-1, -1, 1])).toBe(false)
    const m = mapBrushPoints(b, scaleAbout([0, 0, 0], [-1, 1, 1]))
    expect(signedVolume(m)).toBeLessThan(0)
    const fixed = mapBrushPoints(b, scaleAbout([0, 0, 0], [-1, 1, 1]), true)
    expect(signedVolume(fixed)).toBeCloseTo(6)
    expect(validateBrush(fixed, true)).toBeNull()
  })
  it('scaling one axis keeps faces planar and the brush convex', () => {
    expect(validateBrush(mapBrushPoints(makeBox('a', [0, 0, 0], [1, 1, 1]), scaleAbout([0.5, 0.5, 0.5], [3, 0.5, 2])), true)).toBeNull()
  })
})

describe('selections', () => {
  it('moves selected brushes only, whole entities entirely, and never a brush twice', () => {
    const d = doc()
    const moved = transformSelections(d, [{ entityId: 'w', brushId: 'a' }], translateBy([1, 0, 0]))
    expect(moved.entities[0].brushes![0].vertices[0][0]).toBe(1)
    expect(moved.entities[0].brushes![1]).toBe(d.entities[0].brushes![1])       // untouched brush is the same object
    const both = transformSelections(d, [{ entityId: 'w' }, { entityId: 'w', brushId: 'a' }], translateBy([1, 0, 0]))
    expect(both.entities[0].brushes![0].vertices[0][0]).toBe(1)                  // moved once, not twice
    expect(both.entities[0].brushes![1].vertices[0][0]).toBe(11)
    expect(transformSelections(d, [], translateBy([1, 0, 0]))).toBe(d)
  })
  it('rotating a point entity moves its origin and turns its yaw', () => {
    const r = transformSelections(doc(), [{ entityId: 'p' }], rotateAbout([0, 0, 0], [0, 1, 0], Math.PI / 2), false, 90)
    near(r.entities[1].origin!, [5, 5, -5], 1e-9)
    expect(r.entities[1].angles![1]).toBe(100)
  })
  it('pivot is the centre of the selection bounds', () => {
    near(selectionPivot(doc(), [{ entityId: 'w', brushId: 'a' }, { entityId: 'w', brushId: 'c' }])!, [6, 1, 1])
    near(selectionPivot(doc(), [{ entityId: 'p' }])!, [5, 5, 5])
    expect(selectionPivot(doc(), [])).toBeNull()
  })
})

describe('shapes', () => {
  const plane = planeThrough([0, 0, 0], [0, 1, 0])
  const rect: Rect = { origin: [0, 0, 0], du: 4, dv: 3 }
  let n = 0
  const id = () => `b${n++}`
  const build = (s: ShapeId, h = 2, r = rect) => {
    const out = buildShape(s, plane, r, h, 'dev/grey', defaultShapeParams(), id)
    if ('error' in out) throw new Error(out.error)
    return out.brushes
  }
  it('every shape yields valid convex solids that fit inside the drawn box', () => {
    for (const s of ['box', 'cylinder', 'wedge', 'arch', 'stairs'] as ShapeId[]) {
      const bs = build(s)
      expect(bs.length).toBeGreaterThan(0)
      for (const b of bs) {
        expect(validateBrush(b, true)).toBeNull()
        for (const v of b.vertices) {
          expect(v[0]).toBeGreaterThan(-1e-9); expect(v[0]).toBeLessThan(3 + 1e-9)   // this plane's u axis is world Z, v is X
          expect(v[2]).toBeGreaterThan(-1e-9); expect(v[2]).toBeLessThan(4 + 1e-9)
          expect(v[1]).toBeGreaterThan(-1e-9); expect(v[1]).toBeLessThan(2 + 1e-9)
        }
      }
    }
  })
  it('volumes are right', () => {
    expect(signedVolume(build('box')[0])).toBeCloseTo(24)
    expect(signedVolume(build('wedge')[0])).toBeCloseTo(12)
    const cyl = signedVolume(build('cylinder')[0])
    expect(cyl).toBeLessThan(Math.PI * 2 * 1.5 * 3 * 1.0001)
    expect(cyl).toBeGreaterThan(20 * 0.6)
    const steps = build('stairs').reduce((s, b) => s + signedVolume(b), 0)   // stacked boxes overlap: more than a solid ramp
    expect(steps).toBeGreaterThan(12)
    const arch = build('arch').reduce((s, b) => s + signedVolume(b), 0)
    expect(arch).toBeGreaterThan(0)
    expect(arch).toBeLessThan(24 * 0.7)
  })
  it('builds the same shape whichever way the rectangle and the height were dragged', () => {
    const a = build('wedge', 2, { origin: [0, 0, 0], du: 4, dv: 3 })[0]
    const b = build('wedge', -2, { origin: [4, 0, 3], du: -4, dv: -3 })[0]
    expect(signedVolume(b)).toBeCloseTo(signedVolume(a))
    expect(validateBrush(b, true)).toBeNull()
  })
  it('has many segments on request and rejects degenerate input', () => {
    expect(buildShape('cylinder', plane, rect, 2, 't', { ...defaultShapeParams(), sides: 24 }, id)).toMatchObject({ brushes: [{}] })
    expect(build('stairs').length).toBe(8)
    expect('error' in buildShape('box', plane, { origin: [0, 0, 0], du: 0, dv: 1 }, 1, 't', defaultShapeParams(), id)).toBe(true)
    expect('error' in buildShape('arch', plane, rect, 2, 't', { ...defaultShapeParams(), archThickness: 9 }, id)).toBe(true)
  })
})

describe('UV lock', () => {
  const faceUv = (b: ReturnType<typeof makeBox>, id: number) => projectFace(b, b.faces.find((f) => f.id === id)!)
  it('ON: a moved brush keeps the UVs it had (the texture travels with the faces)', () => {
    const b = makeBox('a', [0, 0, 0], [2, 2, 2], 'wall/a')
    const before = faceUv(b, 5)
    const moved = mapBrushPoints(b, translateBy([3, 0, 1]), false, true)
    expect(moved.faces.find((f) => f.id === 5)!.uvs).toEqual(before)
    expect(moved.vertices[0]).toEqual([3, 0, 1])
  })
  it('OFF: the world-aligned projection is applied again (the texture stays put in the world)', () => {
    const b = makeBox('a', [0, 0, 0], [2, 2, 2], 'wall/a')
    const withUv = { ...b, faces: b.faces.map((f) => ({ ...f, uvs: projectFace(b, f) })) }
    const moved = mapBrushPoints(withUv, translateBy([3, 0, 1]), false, false)
    const face = moved.faces.find((f) => f.id === 5)!
    expect(face.uvs).toEqual(projectFace(moved, face))
    expect(face.uvs).not.toEqual(withUv.faces.find((f) => f.id === 5)!.uvs)
    // a face without stored UVs simply uses the draw-time projection
    expect(mapBrushPoints(b, translateBy([3, 0, 1]), false, false).faces.every((f) => f.uvs === undefined)).toBe(true)
  })
  it('ON survives rotation and mirroring: UVs follow their corners', () => {
    const b = makeBox('a', [0, 0, 0], [1, 2, 3], 'wall/a')
    const rot = mapBrushPoints(b, rotateAbout([0, 0, 0], [0, 1, 0], 0.7))
    expect(rot.faces.find((f) => f.id === 5)!.uvs).toEqual(faceUv(b, 5))
    const mir = mapBrushPoints(b, scaleAbout([0, 0, 0], [-1, 1, 1]), true)
    const orig = faceUv(b, 5), got = mir.faces.find((f) => f.id === 5)!.uvs!
    expect(got).toEqual([...orig].reverse())
  })
})

describe('entity properties follow rotate and scale', () => {
  const ent = (classname: string, keyvalues: Record<string, string> = {}, angles?: [number, number, number]) =>
    ({ id: 'e', classname, origin: [0, 0, 0], ...(angles ? { angles } : {}), keyvalues }) as never
  const d = (e: unknown) => ({ schemaVersion: '0.2', entities: [e], prefabInstances: [] }) as never
  const run = (e: unknown, map: (p: [number, number, number]) => [number, number, number]) => (transformSelections(d(e), [{ entityId: 'e' }], map).entities[0] as { angles?: number[]; keyvalues: Record<string, string> })

  it('a pitch / roll rotation turns the angles of a spot light, not just yaw', () => {
    const r = run(ent('light_spot', {}, [90, 0, 0]), rotateAbout([0, 0, 0], [0, 1, 0], Math.PI / 2))
    // pointing down stays pointing down when turned about the vertical axis; the frame turns with it
    const R = rotationFromAngles(r.angles as [number, number, number])
    near(apply3(R, [0, 0, 1]), [0, -1, 0], 1e-6)
    const x = run(ent('light_spot', {}, [0, 0, 0]), rotateAbout([0, 0, 0], [1, 0, 0], Math.PI / 2))
    near(apply3(rotationFromAngles(x.angles as [number, number, number]), [0, 0, 1]), [0, -1, 0], 1e-6) // +Z turned about X by 90: (0,0,1) -> (0,-1,0)
  })
  it('scaling a point light scales its attenuation radius and source size', () => {
    const r = run(ent('light_point', { radius: '10', source_size: '0.5' }), scaleAbout([0, 0, 0], [2, 2, 2]))
    expect(r.keyvalues.radius).toBe('20')
    expect(r.keyvalues.source_size).toBe('1')
    expect(run(ent('light_point', {}), scaleAbout([0, 0, 0], [0.5, 0.5, 0.5])).keyvalues.radius).toBe('5') // the .ent default of 10
    expect(run(ent('light_point', { radius: '10' }), scaleAbout([0, 0, 0], [1, 2, 1])).keyvalues.radius).toBe('12.59921') // cbrt of the volume change
  })
  it('a rect light scales width / height along its own axes, a decal along width / height / depth', () => {
    const r = run(ent('light_rect', { width: '2', height: '1' }, [0, 0, 0]), scaleAbout([0, 0, 0], [3, 1, 1])) // right = +X
    expect(r.keyvalues.width).toBe('6')
    expect(r.keyvalues.height).toBe('1')
    const dc = run(ent('decal', { width: '1', height: '1', depth: '0.5' }), scaleAbout([0, 0, 0], [2, 4, 1])) // zero angles: right +X, up -Z, dir -Y
    expect(dc.keyvalues.width).toBe('2')
    expect(dc.keyvalues.height).toBe('1')
    expect(dc.keyvalues.depth).toBe('2')
  })
  it('models get modelscale_vec, back to nothing at 1', () => {
    const r = run(ent('misc_model', {}), scaleAbout([0, 0, 0], [2, 1, 3]))
    expect(r.keyvalues.modelscale_vec).toBe('2 1 3')
    const back = run({ ...(ent('misc_model', { modelscale_vec: '2 1 3' }) as object) }, scaleAbout([0, 0, 0], [0.5, 1, 1 / 3]))
    expect(back.keyvalues.modelscale_vec).toBeUndefined()
  })
  it('a plain move changes nothing but the origin', () => {
    const e = ent('light_point', { radius: '3' })
    const r = transformSelections(d(e), [{ entityId: 'e' }], translateBy([1, 2, 3])).entities[0]
    expect(r.keyvalues).toBe((e as { keyvalues: unknown }).keyvalues)
  })
})
