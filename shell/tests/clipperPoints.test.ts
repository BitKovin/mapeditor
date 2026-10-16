import { describe, expect, it } from 'vitest'
import { clashes, depthAxis, hitClipPoint, rayPlaneHit, snapPoint } from '../src/tools/clipperPoints'

describe('clipper points', () => {
  it('rayPlaneHit meets the plane, refuses parallel rays', () => {
    const p = rayPlaneHit({ origin: [1, 5, 2], dir: [0, -1, 0] }, [0, 1, 0], [0, 1, 0])!
    expect(p).toEqual([1, 1, 2])
    expect(rayPlaneHit({ origin: [0, 5, 0], dir: [1, 0, 0] }, [0, 1, 0], [0, 1, 0])).toBeNull()
  })
  it('snapPoint snaps all axes but the kept depth axis', () => {
    expect(snapPoint([1.26, 3.3, 0.74], 0.5)).toEqual([1.5, 3.5, 0.5])
    expect(snapPoint([1.26, 3.3, 0.74], 0.5, 1)).toEqual([1.5, 3.3, 0.5])
    expect(snapPoint([1.26, 3.3, 0.74], 0)).toEqual([1.26, 3.3, 0.74])
  })
  it('hitClipPoint picks the nearest point within the radius', () => {
    const project = (p: [number, number, number]) => ({ x: p[0] * 10, y: p[2] * 10, depth: 1 })
    const pts = [{ p: [1, 0, 1] as [number, number, number] }, { p: [1.5, 0, 1] as [number, number, number] }]
    expect(hitClipPoint(pts, project, 14, 10)).toBe(1)
    expect(hitClipPoint(pts, project, 12, 10)).toBe(0)
    expect(hitClipPoint(pts, project, 200, 200)).toBeNull()
  })
  it('clashes ignores the point being moved; depthAxis reads ortho directions', () => {
    const pts = [{ p: [0, 0, 0] as [number, number, number] }, { p: [1, 0, 0] as [number, number, number] }]
    expect(clashes(pts, 0, [1, 0, 0])).toBe(true)
    expect(clashes(pts, 1, [1, 0, 0])).toBe(false)
    expect(depthAxis([0, -1, 0])).toBe(1)
    expect(depthAxis([-1, 0, 0])).toBe(0)
    expect(depthAxis(null)).toBeUndefined()
  })
})
