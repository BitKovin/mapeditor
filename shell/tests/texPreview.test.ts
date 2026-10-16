import { describe, expect, it } from 'vitest'
import { buildPreview, pieceTransform, splitFromPointer, anchorFromEdgeClick, dragAltOffset } from '../src/model/texPreview'
import { TexInfo, defaultProps } from '../src/model/texprops'

const A = (props = defaultProps()): TexInfo => ({ path: 'wall/pt_1', width: 204, height: 204, ppm: 64, props })
const wallpaper = () => { const p = defaultProps(); p.repeat.v = { mode: 'anchor', anchor: 'end', split: 0.4, fill: 'mirror', alt: null, edge: 'auto' }; return p }
const area = (poly: [number, number][]) => { let s = 0; poly.forEach((a, i) => { const b = poly[(i + 1) % poly.length]; s += a[0] * b[1] - b[0] * a[1] }); return Math.abs(s) / 2 }

describe('texture properties preview', () => {
  it('plain properties: two triangles, the whole wall, not cut', () => {
    const p = buildPreview(A(), () => undefined, 4, 3)
    expect(p.cut).toBe(false)
    expect(p.pieces.reduce((s, q) => s + area(q.poly), 0)).toBeCloseTo(12, 9)
  })
  it('a wallpaper preview is cut and still covers exactly the wall', () => {
    const p = buildPreview(A(wallpaper()), () => undefined, 4, 7)
    expect(p.cut).toBe(true)
    expect(p.pieces.length).toBeGreaterThan(2)
    expect(p.pieces.reduce((s, q) => s + area(q.poly), 0)).toBeCloseTo(28, 7)
  })
  it('the alternate texture appears above the first one', () => {
    const p = wallpaper(); p.repeat.v.alt = { texture: 'wall/pt_1_1', offset: [0, 0] }
    const B: TexInfo = { path: 'wall/pt_1_1', width: 204, height: 204, ppm: 64, props: defaultProps() }
    const pr = buildPreview(A(p), (q) => (q === B.path ? B : undefined), 4, 7)
    const ys = (tex: string) => pr.pieces.filter((q) => q.texture === tex).flatMap((q) => q.poly.map((v) => v[1]))
    expect(Math.min(...ys('wall/pt_1_1'))).toBeGreaterThanOrEqual(204 / 64 - 1e-9)
    expect(Math.max(...ys('wall/pt_1'))).toBeLessThanOrEqual(204 / 64 + 1e-9)
  })
  it('too many cells fall back to a plain preview', () => {
    const p = defaultProps(); p.repeat.u = { mode: 'mirror', anchor: 'start', split: 0.5, fill: 'mirror', alt: null, edge: 'auto' }; p.repeat.v = { ...p.repeat.u }
    expect(buildPreview(A(p), () => undefined, 4000, 4000, 0.01).cut).toBe(false)
  })
  it('piece transform maps texture pixels onto the piece corners', () => {
    const q = buildPreview(A(wallpaper()), () => undefined, 4, 3).pieces[0]
    const toCanvas = (v: [number, number]): [number, number] => [v[0] * 50, (3 - v[1]) * 50]
    const m = pieceTransform(q, toCanvas, 204, 204)!
    q.poly.forEach((pt, i) => {
      const px = q.uv[i][0] * 204, py = q.uv[i][1] * 204
      const c = toCanvas(pt)
      expect(m[0] * px + m[2] * py + m[4]).toBeCloseTo(c[0], 6)
      expect(m[1] * px + m[3] * py + m[5]).toBeCloseTo(c[1], 6)
    })
  })
  it('pointer helpers', () => {
    expect(splitFromPointer(-1)).toBe(0.02); expect(splitFromPointer(2)).toBe(0.98); expect(splitFromPointer(0.4)).toBe(0.4)
    expect(anchorFromEdgeClick(0.02)).toBe('start'); expect(anchorFromEdgeClick(0.97)).toBe('end'); expect(anchorFromEdgeClick(0.5)).toBeNull()
    const o = dragAltOffset([0, 0], 1, 0, { width: 128, height: 128, ppm: 64 })
    expect(o[0]).toBeCloseTo(-0.5, 9)
    expect(dragAltOffset([0, 0], 0, 1, { width: 128, height: 128, ppm: 64 })[1]).toBeCloseTo(0.5, 9)
  })
})
