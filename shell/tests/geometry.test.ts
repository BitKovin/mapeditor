import { describe, expect, it } from 'vitest'
import { readFileSync } from 'node:fs'
import type { Brush } from '../src/model/document'
import { brushBounds, faceNormal, makeBox, signedVolume, translateBrush, triangulateFace, validateBrush } from '../src/model/geometry'
import { dot, sub } from '../src/model/math'

// The same vectors core/tests/test_brush.cpp runs: the two rule implementations must agree.
const vectors = JSON.parse(readFileSync(new URL('../../testdata/brush_vectors.json', import.meta.url), 'utf8'))

describe('shared brush vectors (Core and Shell must agree)', () => {
  for (const c of vectors.cases)
    it(c.name, () => {
      const err = validateBrush(c.brush as Brush, c.requireConvex)
      if (c.expect === null) expect(err).toBeNull()
      else expect(err).toContain(c.expect)
    })
})

describe('makeBox', () => {
  const b = makeBox('b', [-1, -1, -1], [1, 2, 3])
  it('is a valid convex solid with outward normals', () => {
    expect(validateBrush(b, true)).toBeNull()
    expect(signedVolume(b)).toBeCloseTo(24)
    const centre: [number, number, number] = [0, 0.5, 1]
    for (const f of b.faces) {
      const fc = f.loop.reduce((s, i) => [s[0] + b.vertices[i][0] / 4, s[1] + b.vertices[i][1] / 4, s[2] + b.vertices[i][2] / 4], [0, 0, 0])
      expect(dot(faceNormal(b, f), sub(fc as [number, number, number], centre))).toBeGreaterThan(0)
    }
  })
  it('translates without changing shape', () => {
    const t = translateBrush(b, [10, 0, 0])
    expect(brushBounds(t)!.min[0]).toBeCloseTo(9)
    expect(signedVolume(t)).toBeCloseTo(24)
    expect(t.id).toBe('b')
  })
})

describe('triangulateFace', () => {
  const area = (b: Brush, tris: number[][]) =>
    tris.reduce((s, [a, c, d]) => {
      const [p, q, r] = [b.vertices[a], b.vertices[c], b.vertices[d]]
      const u = sub(q, p), v = sub(r, p)
      return s + Math.hypot(u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]) / 2
    }, 0)

  it('splits a quad into two triangles with the same winding', () => {
    const b = makeBox('b', [0, 0, 0], [1, 1, 1])
    for (const f of b.faces) {
      const tris = triangulateFace(b, f)
      expect(tris).toHaveLength(2)
      expect(area(b, tris)).toBeCloseTo(1)
      for (const [a, c, d] of tris) {
        const n = faceNormal({ ...b, faces: [{ id: 0, loop: [a, c, d] }] }, { id: 0, loop: [a, c, d] })
        expect(dot(n, faceNormal(b, f))).toBeGreaterThan(0.99)
      }
    }
  })
  it('handles a concave (L-shaped) face without covering the notch', () => {
    const l = vectors.cases.find((c: any) => c.name.startsWith('L prism where free')).brush as Brush
    for (const f of l.faces.filter((f) => f.loop.length === 6)) {
      const tris = triangulateFace(l, f)
      expect(tris).toHaveLength(4)
      expect(area(l, tris)).toBeCloseTo(3) // the L has area 3; a fan would give 4
    }
  })
})
