import { describe, expect, it } from 'vitest'
import { validateBrush, brushBounds, signedVolume } from '../src/model/geometry'
import { fallbackPlane, planeThrough, rayPlane, rectFromDrag, heightFor, buildBrush, snapToPlane, previewLines } from '../src/tools/boxTool'
import { fitProjection, planarCoords, projectFace, projectionOf, defaultProjection, uvsForDraw } from '../src/model/uv'
import { makeBox } from '../src/model/geometry'
import { emptyDocument, findEntity } from '../src/model/document'
import { addDrawnBrush, applyTexture, editLightmapScale, editProjection, faceTargets, resetProjection, fitTexture } from '../src/state/actions'
import { GRID_SIZES, defaultViewportSettings, snapStep } from '../src/render/viewportSettings'
import { makeApp } from './helpers'

describe('grid settings', () => {
  it('offers exactly the requested sizes; snapping can be switched off', () => {
    expect(GRID_SIZES).toEqual([0.1, 0.25, 0.5, 1, 2, 4, 5, 10, 15, 20, 50, 100, 250, 500])
    const s = defaultViewportSettings()
    expect(snapStep(s)).toBe(0.5)
    s.snapEnabled = false
    expect(snapStep(s)).toBe(0)
  })
})

describe('box tool: planes', () => {
  it('a hit on an axis-aligned face gives a world-axis basis', () => {
    const p = planeThrough([1, 2, 3], [0, 1, 0])
    expect(p.u).toEqual([0, 0, 1]); expect(p.v).toEqual([1, 0, 0]); expect(p.normal).toEqual([0, 1, 0])
  })
  it('a tilted face gets an orthonormal basis', () => {
    const p = planeThrough([0, 0, 0], [1, 1, 0])
    const d = (a: number[], b: number[]) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
    expect(d(p.u, p.v)).toBeCloseTo(0); expect(d(p.u, p.normal)).toBeCloseTo(0); expect(d(p.v, p.normal)).toBeCloseTo(0)
    expect(Math.hypot(...p.u)).toBeCloseTo(1)
  })
  it('with no surface hit: ground plane if the ray reaches it, else a horizontal plane at a set distance', () => {
    const down = fallbackPlane({ origin: [0, 5, 0], dir: [0, -Math.SQRT1_2, Math.SQRT1_2] })
    expect(down.origin[1]).toBeCloseTo(0); expect(down.origin[2]).toBeCloseTo(5)
    const up = fallbackPlane({ origin: [0, 5, 0], dir: [0, 0, 1] }, 10)
    expect(up.origin).toEqual([0, 5, 10]); expect(up.normal).toEqual([0, 1, 0])
    const far = fallbackPlane({ origin: [0, 1, 0], dir: [0, -0.001, 1] }, 10) // would hit the ground ~1000 m away
    expect(far.origin[2]).toBeCloseTo(10, 0)
  })
  it('rayPlane intersects in front of the ray only', () => {
    const p = planeThrough([0, 0, 0], [0, 1, 0])
    expect(rayPlane({ origin: [1, 4, 1], dir: [0, -1, 0] }, p)).toEqual([1, 0, 1])
    expect(rayPlane({ origin: [1, 4, 1], dir: [0, 1, 0] }, p)).toBeNull()
  })
  it('snaps in-plane components only, and only on axis-aligned planes', () => {
    const p = planeThrough([0, 0, 0], [0, 1, 0])
    expect(snapToPlane([1.26, 3.3, -0.74], p, 0.5)).toEqual([1.5, 3.3, -0.5]) // y (the plane's own axis) untouched
    expect(snapToPlane([1.26, 3.3, -0.74], p, 0)).toEqual([1.26, 3.3, -0.74])
  })
})

describe('box tool: drawing', () => {
  const plane = planeThrough([0, 0, 0], [0, 1, 0])
  it('a free rectangle then a height makes a valid convex box of that size', () => {
    const rect = rectFromDrag(plane, [0, 0, 0], [2, 0, 3], false)
    const b = buildBrush('b', plane, rect, 1.5)!
    expect(validateBrush(b, true)).toBeNull()
    const bb = brushBounds(b)!
    expect(bb.max.map((v, i) => v - bb.min[i]).map((v) => Math.round(v * 1000) / 1000).sort()).toEqual([1.5, 2, 3])
    expect(signedVolume(b)).toBeCloseTo(9)
  })
  it('Shift in phase 1 makes a square, keeping the drag direction', () => {
    const r = rectFromDrag(plane, [0, 0, 0], [-1, 0, 4], true)
    expect(Math.abs(r.du)).toBeCloseTo(4); expect(Math.abs(r.dv)).toBeCloseTo(4)
    expect(Math.sign(r.du) * Math.sign(r.dv)).toBe(-1) // still drawn toward (-x, +z)
  })
  it('Shift in phase 2 makes a cube; the mouse only chooses up or down', () => {
    const r = rectFromDrag(plane, [0, 0, 0], [2, 0, 1], true)
    expect(heightFor(0.3, r, true, 0.5)).toBe(2)
    expect(heightFor(-7, r, true, 0.5)).toBe(-2)
    const cube = buildBrush('c', plane, r, heightFor(1, r, true, 0.5))!
    const bb = brushBounds(cube)!
    expect(bb.max.map((v, i) => v - bb.min[i])).toEqual([2, 2, 2])
  })
  it('height snaps to the grid without Shift', () => {
    const r = rectFromDrag(plane, [0, 0, 0], [1, 0, 1], false)
    expect(heightFor(1.37, r, false, 0.5)).toBe(1.5)
    expect(heightFor(1.37, r, false, 0)).toBe(1.37)
  })
  it('extruding downward or drawing backwards still gives an outward-wound solid', () => {
    for (const [du, dz, h] of [[2, 2, -1], [-2, 2, 1], [-2, -3, -1]]) {
      const rect = rectFromDrag(plane, [0, 0, 0], [du, 0, dz], false)
      const b = buildBrush('b', plane, rect, h)!
      expect(validateBrush(b, true)).toBeNull()
    }
  })
  it('drawing on a wall extrudes along the wall normal (a box "grown" from the surface)', () => {
    const wall = planeThrough([0, 1, 5], [0, 0, -1])
    const b = buildBrush('w', wall, rectFromDrag(wall, [0, 1, 5], [1, 2, 5], false), 0.5)!
    expect(validateBrush(b, true)).toBeNull()
    const bb = brushBounds(b)!
    expect(bb.max[2]).toBeCloseTo(5); expect(bb.min[2]).toBeCloseTo(4.5)
  })
  it('degenerate sizes produce no brush; the preview shows the flat rectangle first', () => {
    expect(buildBrush('z', plane, { origin: [0, 0, 0], du: 0, dv: 2 }, 1)).toBeNull()
    expect(buildBrush('z', plane, { origin: [0, 0, 0], du: 1, dv: 2 }, 0)).toBeNull()
    expect(previewLines(plane, { origin: [0, 0, 0], du: 1, dv: 2 }, null)).toHaveLength(4 * 6)
    expect(previewLines(plane, { origin: [0, 0, 0], du: 1, dv: 2 }, 1)).toHaveLength(12 * 6)
  })
})

describe('UV projection', () => {
  const box = makeBox('b', [0, 0, 0], [2, 2, 2])
  it('walls read upright: v decreases with height, on all four sides', () => {
    for (const n of [[1, 0, 0], [-1, 0, 0], [0, 0, 1], [0, 0, -1]] as [number, number, number][]) {
      const lo = planarCoords(n, [0.3, 0.2, 0.4]), hi = planarCoords(n, [0.3, 1.2, 0.4])
      expect(hi[1]).toBeLessThan(lo[1])
    }
  })
  it('the same texture direction is not mirrored between opposite walls (u follows the viewer\'s right)', () => {
    // +Z wall seen from outside: moving along +x moves right; -Z wall: moving along -x moves right
    expect(planarCoords([0, 0, 1], [1, 0, 0])[0]).toBeGreaterThan(planarCoords([0, 0, 1], [0, 0, 0])[0])
    expect(planarCoords([0, 0, -1], [-1, 0, 0])[0]).toBeGreaterThan(planarCoords([0, 0, -1], [0, 0, 0])[0])
  })
  it('default projection tiles every 2 m; scale, shift and rotation apply in that order', () => {
    const f = box.faces[1] // +Y face, 2 m wide
    const uv = projectFace(box, f)
    const span = (i: 0 | 1) => Math.max(...uv.map((q) => q[i])) - Math.min(...uv.map((q) => q[i]))
    expect(span(0)).toBeCloseTo(1); expect(span(1)).toBeCloseTo(1)
    const half = projectFace(box, f, { ...defaultProjection(), scale: [1, 1] })
    expect(Math.max(...half.map((q) => q[0])) - Math.min(...half.map((q) => q[0]))).toBeCloseTo(2)
    const shifted = projectFace(box, f, { ...defaultProjection(), shift: [0.25, 0] })
    expect(shifted[0][0] - uv[0][0]).toBeCloseTo(0.25)
    const rotated = projectFace(box, f, { ...defaultProjection(), rotate: 90 })
    expect(rotated[2]).not.toEqual(uv[2]) // (vertex 0 is the origin, which rotation can't move)
  })
  it('fit stretches the texture to cover the face exactly once', () => {
    const f = box.faces[3]
    const p = fitProjection(box, f)
    const uv = projectFace(box, f, p)
    for (const i of [0, 1] as const) { expect(Math.min(...uv.map((q) => q[i]))).toBeCloseTo(0); expect(Math.max(...uv.map((q) => q[i]))).toBeCloseTo(1) }
  })
  it('bad stored projections fall back to defaults instead of producing NaN', () => {
    const p = projectionOf({ id: 0, loop: [], projection: { scale: [0, 'x'], shift: [NaN, 1], rotate: 'no' } } as any)
    expect(p).toEqual({ scale: [2, 2], shift: [0, 0], rotate: 0 }.scale ? p : p)
    expect(p.scale).toEqual([2, 2]); expect(p.shift).toEqual([0, 0]); expect(p.rotate).toBe(0)
  })
  it('stored uvs win over the projection at draw time', () => {
    const f = { ...box.faces[0], uvs: [[0, 0], [1, 0], [1, 1], [0, 1]] as [number, number][] }
    expect(uvsForDraw(box, f)).toBe(f.uvs)
    expect(uvsForDraw(box, box.faces[0])).toHaveLength(4)
  })
})

describe('texturing actions', () => {
  const setup = () => {
    const app = makeApp(); app.sync()
    addDrawnBrush(app.store, makeBox('b1', [0, 0, 0], [1, 1, 1]))
    return app
  }
  it('drawing a brush creates worldspawn on demand, selects the brush, and is one undo step', () => {
    const { store } = setup()
    expect(store.state.selection).toMatchObject({ entityId: 'e_worldspawn', brushId: 'b1' })
    expect(store.state.history.map((h) => h.label)).toEqual(['Loaded', 'Draw box'])
  })
  it('applies to the selected face only in face mode, otherwise to the whole brush', () => {
    const { store } = setup()
    applyTexture(store, 'dev/wood')
    const brush = () => findEntity(store.state.doc, 'e_worldspawn')!.brushes![0]
    expect(brush().faces.every((f) => f.texture === 'dev/wood')).toBe(true)
    store.select({ entityId: 'e_worldspawn', brushId: 'b1', faceId: 2 })
    applyTexture(store, 'dev/metal')
    expect(brush().faces.map((f) => f.texture)).toEqual(['dev/wood', 'dev/wood', 'dev/metal', 'dev/wood', 'dev/wood', 'dev/wood'])
    expect(store.state.activeTexture).toBe('dev/metal')
  })
  it('re-applying the same texture is not an undo step', () => {
    const { store } = setup()
    applyTexture(store, 'dev/wood'); const n = store.state.history.length
    applyTexture(store, 'dev/wood')
    expect(store.state.history).toHaveLength(n)
  })
  it('projection edits write both the parameters and the resulting uvs, and undo restores them', () => {
    const { store } = setup()
    store.select({ entityId: 'e_worldspawn', brushId: 'b1', faceId: 1 })
    editProjection(store, 'Scale', (p) => ({ ...p, scale: [1, 1] }))
    const f = () => findEntity(store.state.doc, 'e_worldspawn')!.brushes![0].faces[1]
    expect(f().projection).toEqual({ scale: [1, 1], shift: [0, 0], rotate: 0 })
    expect(f().uvs).toHaveLength(4)
    fitTexture(store); expect(f().projection).not.toEqual({ scale: [1, 1], shift: [0, 0], rotate: 0 })
    resetProjection(store)
    expect(f().projection).toMatchObject({ scale: [2, 2], rotate: 0 })
    // the texture's bottom-left corner sits on the face's bottom-left corner
    expect(Math.min(...f().uvs!.map((q) => q[0]))).toBeCloseTo(0, 9)
    expect(Math.max(...f().uvs!.map((q) => q[1]))).toBeCloseTo(1, 9)
    store.undo(); store.undo(); store.undo()
    expect(f().projection).toBeUndefined()
  })
  it('lightmap density: stored on the faces, clamped, 1 is stored as absent, no-op edits are not undo steps, undo restores', () => {
    const { store } = setup()
    const faces = () => findEntity(store.state.doc, 'e_worldspawn')!.brushes![0].faces
    store.select({ entityId: 'e_worldspawn', brushId: 'b1', faceId: 1 })
    editLightmapScale(store, 'Lightmap density', (v) => v * 2)
    expect(faces()[1].lightmapScale).toBe(2)
    expect(faces()[0].lightmapScale).toBeUndefined()
    editLightmapScale(store, 'Lightmap density', () => 1000)
    expect(faces()[1].lightmapScale).toBe(16)
    editLightmapScale(store, 'Lightmap density', () => 0.001)
    expect(faces()[1].lightmapScale).toBe(0.0625)
    const n = store.state.history.length
    editLightmapScale(store, 'Lightmap density', () => 0.0625)
    expect(store.state.history).toHaveLength(n)
    editLightmapScale(store, 'Lightmap density', () => 1)
    expect('lightmapScale' in faces()[1]).toBe(false)
    store.undo()
    expect(faces()[1].lightmapScale).toBe(0.0625)
    // a whole brush: every face
    store.select({ entityId: 'e_worldspawn', brushId: 'b1' })
    editLightmapScale(store, 'Lightmap density', () => 4)
    expect(faces().every((f) => f.lightmapScale === 4)).toBe(true)
  })
  it('faceTargets: face, brush, or whole entity', () => {
    const { store } = setup()
    expect(faceTargets(store.state.doc, { entityId: 'e_worldspawn', brushId: 'b1', faceId: 3 })).toHaveLength(1)
    expect(faceTargets(store.state.doc, { entityId: 'e_worldspawn', brushId: 'b1' })).toHaveLength(6)
    expect(faceTargets(store.state.doc, { entityId: 'e_worldspawn' })).toHaveLength(6)
    expect(faceTargets(emptyDocument(), null)).toEqual([])
  })
})

describe('snapToPlane is anchored to the world grid, not to where you clicked', () => {
  it('axis-aligned plane: in-plane coordinates land on grid lines even when the plane origin is the click point', () => {
    const hit: [number, number, number] = [1.26, 3.3, -0.74]
    const p = planeThrough(hit, [0, 1, 0])
    const s = snapToPlane(hit, p, 0.5)
    expect(s[0]).toBeCloseTo(1.5); expect(s[2]).toBeCloseTo(-0.5); expect(s[1]).toBeCloseTo(3.3)
  })
  it('a slanted surface: the start point lands on a grid line of the surface and stays on the plane', () => {
    const n: [number, number, number] = [0.6, 0.8, 0]
    const p = planeThrough([1.1, 2.3, 0.7], n)
    const s = snapToPlane([1.1, 2.3, 0.7], p, 0.5)
    const a = s[0] * p.u[0] + s[1] * p.u[1] + s[2] * p.u[2], b = s[0] * p.v[0] + s[1] * p.v[1] + s[2] * p.v[2]
    expect(a / 0.5).toBeCloseTo(Math.round(a / 0.5)); expect(b / 0.5).toBeCloseTo(Math.round(b / 0.5))
    const off = (s[0] - 1.1) * n[0] + (s[1] - 2.3) * n[1] + (s[2] - 0.7) * n[2]
    expect(Math.abs(off)).toBeLessThan(1e-9)
  })
})
