import { afterEach, describe, expect, it } from 'vitest'
import { carvedKey } from '../src/model/derived'
import type { DerivedEntry, World } from '../src/model/derived'
import { makeBox, faceNormal } from '../src/model/geometry'
import type { MapDocument } from '../src/model/document'
import { buildPreset } from '../src/model/relief'
import { reliefFaces, reliefMessage, reliefSignature } from '../src/model/reliefBake'
import { defaultProps, texEnv } from '../src/model/texprops'

const TEX = 'wall/relief'
const withRelief = (kind: Parameters<typeof buildPreset>[0] | null): void => {
  texEnv.setProvider((p) => (p === TEX ? { path: p, width: 64, height: 64, ppm: 64, props: { ...defaultProps(), relief: kind ? buildPreset(kind, 1, 1) : null } } : undefined))
}
afterEach(() => texEnv.setProvider(null))
const worldOf = (...brushes: ReturnType<typeof makeBox>[]): World => ({ doc: { entities: [{ id: 'w', brushes }] } as unknown as MapDocument, prefabs: [] })

describe('relief geometry for the bake', () => {
  it('a brush with a relief texture sends displaced triangles per face, keyed like the Core keys its surface pieces', () => {
    withRelief('recess')
    const b = makeBox('a', [0, 0, 0], [2, 2, 2], TEX)
    const faces = reliefFaces(worldOf(b), [])
    expect(faces).toHaveLength(6)
    for (const f of faces) {
      expect(f.back).toBe(false)
      expect(f.entityId).toBe('w'); expect(f.brushId).toBe('a'); expect(f.instance).toBe(''); expect(f.brushInstance).toBe('')
      expect(f.tris.length % 9).toBe(0)
      expect(f.tris.length).toBeGreaterThan(2 * 9) // more than the flat face's two triangles
      // the plane the relief stands out of, and no vertex above it (a recess only sinks)
      for (let i = 0; i < f.tris.length; i += 3) {
        const h = f.normal[0] * f.tris[i] + f.normal[1] * f.tris[i + 1] + f.normal[2] * f.tris[i + 2] - f.offset
        expect(h).toBeLessThanOrEqual(1e-6)
      }
      expect(f.tris.some((_, i) => i % 3 === 0 && f.normal[0] * f.tris[i] + f.normal[1] * f.tris[i + 1] + f.normal[2] * f.tris[i + 2] - f.offset < -1e-3)).toBe(true)
    }
  })

  it('nothing is sent for a texture without a relief, nor for a subtractor itself', () => {
    const b = makeBox('a', [0, 0, 0], [2, 2, 2], TEX)
    expect(reliefFaces(worldOf(b), [])).toEqual([])
    withRelief('recess')
    expect(reliefFaces(worldOf({ ...b, op: 'subtract' }), [])).toEqual([])
  })

  it('a carved brush uses the Core fragments; the walls a subtractor cut are a back side keyed by the subtractor', () => {
    withRelief('recess')
    const wall = makeBox('wall', [0, 0, 0], [4, 4, 1], TEX)
    const cut = { ...makeBox('cut', [1, 1, -1], [2, 2, 2], TEX), op: 'subtract' as const }
    const w = worldOf(wall, cut)
    const top = cut.faces.find((f) => faceNormal(cut, f)[0] > 0.9)! // the cutter's +x face: the cut wall at x = 2 faces -x (into the opening)
    const verts = top.loop.flatMap((i) => cut.vertices[i])
    const entry = {
      entityId: 'w', brushId: 'wall', instance: '', subtractors: ['cut'],
      fragments: [{ brush: 'cut', instance: '', face: top.id, v: [...verts].reverse().reduce<number[]>((a, _, i, r) => (i % 3 === 0 ? [...a, r[i + 2], r[i + 1], r[i]] : a), []) }],
    } as unknown as DerivedEntry
    const faces = reliefFaces(w, [entry])
    expect(faces).toHaveLength(1)
    expect(faces[0].brushId).toBe('cut')
    expect(faces[0].back).toBe(true)
    expect(faces[0].normal[0]).toBeLessThan(-0.9)
  })

  it('a brush the Core carved but whose result is out of date is left out instead of covered by a whole-face relief', () => {
    withRelief('recess')
    const b = makeBox('a', [0, 0, 0], [2, 2, 2], TEX)
    expect(reliefFaces(worldOf(b), [], new Set([carvedKey('w', 'a', '')]))).toEqual([]) // known to Core as carved, no usable entry
  })

  it('the signature changes with the geometry and the message round trips the vertices', () => {
    withRelief('recess')
    const b = makeBox('a', [0, 0, 0], [2, 2, 2], TEX)
    const a = reliefFaces(worldOf(b), [])
    expect(reliefSignature(a)).toBe(reliefSignature(reliefFaces(worldOf(b), [])))
    withRelief('raise')
    expect(reliefSignature(reliefFaces(worldOf(b), []))).not.toBe(reliefSignature(a))
    const m = JSON.parse(reliefMessage(a)) as { type: string; faces: { v: string; i: string; e: string; f: number; s: number; n: number[]; d: number }[] }
    expect(m.type).toBe('relief_geometry')
    expect(m.faces).toHaveLength(6)
    const bytes = Uint8Array.from(atob(m.faces[0].v), (c) => c.charCodeAt(0))
    const floats = new Float32Array(bytes.buffer)
    expect(floats.length).toBe(a[0].tris.length)
    expect(floats[0]).toBeCloseTo(a[0].tris[0], 5)
  })
})
