import { describe, expect, it } from 'vitest'
import { chooseHotspot, faceSize, hotspotAt, hotspotProjection, hotspotSize, moveRect, rectFromCorners } from '../src/model/hotspots'
import { faceNormal, makeBox } from '../src/model/geometry'
import { defaultProps, parseHotspots, parseProps, propsToJson } from '../src/model/texprops'
import { projectFace } from '../src/model/uv'
import { edgeInsetTexels } from '../src/model/uvmesh'

const info = { width: 512, height: 512, ppm: 64 } // 8 m per repeat
const wall = (w: number, h: number) => { const b = makeBox('b', [0, 0, 0], [w, h, 0.2], 't'); return { b, f: b.faces.find((x) => faceNormal(b, x)[2] > 0.9)! } }

describe('hotspots', () => {
  it('parse: valid rectangles clamped into the texture, junk dropped, capped', () => {
    expect(parseHotspots([{ x: -1, y: 0.5, w: 5, h: 0.2 }, { x: 'a' }, null, { x: 0.2, y: 0.2, w: 0.001, h: 0.3 }])).toEqual([{ x: 0, y: 0.5, w: 1, h: 0.2 }, { x: 0.2, y: 0.2, w: 0.01, h: 0.3 }])
    expect(parseHotspots('x')).toEqual([])
    expect(parseHotspots(Array.from({ length: 100 }, () => ({ x: 0, y: 0, w: 0.5, h: 0.5 })))).toHaveLength(64)
  })
  it('round-trips through the properties JSON', () => {
    const p = defaultProps(); p.hotspots = [{ x: 0.1, y: 0.2, w: 0.3, h: 0.4 }]
    expect(parseProps(JSON.parse(JSON.stringify(propsToJson(p)))).hotspots).toEqual(p.hotspots)
    expect(parseProps({}).hotspots).toEqual([])
  })
  it('face size follows the face axes', () => {
    expect(faceSize(wall(3, 2).b, wall(3, 2).f).map((v) => +v.toFixed(6))).toEqual([3, 2])
  })
  it('chooses the least distorted hotspot, then the closest size', () => {
    const spots = [{ x: 0, y: 0, w: 1, h: 0.25 }, { x: 0, y: 0.25, w: 0.25, h: 0.75 }, { x: 0.25, y: 0.25, w: 0.5, h: 0.5 }]
    expect(chooseHotspot(spots, info, [8, 2])).toBe(0)     // a wide strip
    expect(chooseHotspot(spots, info, [2, 6])).toBe(1)     // a tall strip
    expect(chooseHotspot(spots, info, [4, 4])).toBe(2)     // a square
    // two equally shaped: the nearer size wins
    const two = [{ x: 0, y: 0, w: 0.25, h: 0.25 }, { x: 0.5, y: 0.5, w: 0.5, h: 0.5 }]
    expect(chooseHotspot(two, info, [1.9, 1.9])).toBe(0)
    expect(chooseHotspot(two, info, [3.9, 3.9])).toBe(1)
    expect(chooseHotspot([], info, [1, 1])).toBe(-1)
    expect(hotspotSize(two[1], info)).toEqual([4, 4])
  })
  it('the projection shows exactly the hotspot (inset a little): the face corners map to the rectangle', () => {
    const { b, f } = wall(3, 2)
    const h = { x: 0.25, y: 0.5, w: 0.25, h: 0.25 }
    const uv = projectFace(b, f, hotspotProjection(b, f, h, info))
    const i = edgeInsetTexels(512) / 512
    expect(Math.min(...uv.map((q) => q[0]))).toBeCloseTo(h.x + i, 9); expect(Math.max(...uv.map((q) => q[0]))).toBeCloseTo(h.x + h.w - i, 9)
    expect(Math.min(...uv.map((q) => q[1]))).toBeCloseTo(h.y + i, 9); expect(Math.max(...uv.map((q) => q[1]))).toBeCloseTo(h.y + h.h - i, 9)
    // upright: v grows downwards the wall, like the texture
    const top = f.loop.map((vi, k) => ({ y: b.vertices[vi][1], v: uv[k][1] }))
    expect(Math.max(...top.map((t) => t.v))).toBeCloseTo(h.y + h.h - i, 9)
    expect(top.find((t) => t.y === 2)!.v).toBeCloseTo(h.y + i, 9)
  })
  it('rectangle helpers', () => {
    expect(rectFromCorners(0.8, 0.9, 0.2, 0.3)).toEqual({ x: 0.2, y: 0.3, w: 0.6000000000000001, h: 0.6000000000000001 })
    expect(rectFromCorners(0.5, 0.5, 0.5, 0.5)).toEqual({ x: 0.5, y: 0.5, w: 0.01, h: 0.01 })
    expect(rectFromCorners(0.99, 0.99, 1.2, 1.2).x + rectFromCorners(0.99, 0.99, 1.2, 1.2).w).toBeLessThanOrEqual(1 + 1e-12)
    expect(moveRect({ x: 0.5, y: 0.5, w: 0.4, h: 0.4 }, 1, -1)).toEqual({ x: 0.6, y: 0, w: 0.4, h: 0.4 })
    const hs = [{ x: 0, y: 0, w: 1, h: 1 }, { x: 0.4, y: 0.4, w: 0.2, h: 0.2 }]
    expect(hotspotAt(hs, 0.5, 0.5)).toBe(1); expect(hotspotAt(hs, 0.1, 0.1)).toBe(0); expect(hotspotAt([], 0.1, 0.1)).toBe(-1)
  })
})
