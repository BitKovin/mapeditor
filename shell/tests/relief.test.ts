import { describe, expect, it } from 'vitest'
import { PRESETS, RELIEF_MAX_DEPTH, brushToRelief, buildPreset, confineBrush, flatRelief, isFlatRelief, parseRelief, reliefEqual, reliefReport, reliefToBrush, reliefToJson, reliefTriangles } from '../src/model/relief'
import { ReliefSession, faceIdsTouching } from '../src/model/reliefSession'
import { extrudeFaces, insetFaces, isError, transformVerts } from '../src/model/meshOps'

describe('relief stored form', () => {
  it('flat relief is one quad at depth 0', () => {
    const r = flatRelief()
    expect(r.faces).toEqual([[0, 1, 2, 3]])
    expect(isFlatRelief(r)).toBe(true)
  })
  it('parse rejects junk and keeps good data', () => {
    expect(parseRelief(null)).toBeNull()
    expect(parseRelief({ vertices: [], faces: [] })).toBeNull()
    expect(parseRelief({ vertices: [[0, 0, 0]], faces: [[0, 0, 0]] })).toBeNull()
    const r = parseRelief(reliefToJson(buildPreset('window', 2, 3)))
    expect(r).not.toBeNull()
    expect(reliefEqual(r, buildPreset('window', 2, 3))).toBe(true)
  })
  it('parse clamps coordinates and drops bad faces', () => {
    const r = parseRelief({ vertices: [[-1, 0, 100], [2, 0, 0], [1, 1, -100], [0, 1, 0], ['x', 1, 1]], faces: [[0, 1, 2, 3], [0, 1, 9], [0, 1]] })!
    expect(r.faces.length).toBe(1)
    expect(r.vertices[0]).toEqual([0, 0, RELIEF_MAX_DEPTH])
    expect(r.vertices[1][0]).toBe(1)
    expect(r.vertices[2][2]).toBe(-RELIEF_MAX_DEPTH)
  })
  it('unused vertices are dropped', () => {
    const r = parseRelief({ vertices: [[0, 0, 0], [1, 1, 1], [1, 0, 0], [0, 1, 0], [1, 1, 0]], faces: [[0, 2, 4, 3]] })!
    expect(r.vertices.length).toBe(4)
  })
  it('brush round trip keeps the relief (any texture size)', () => {
    for (const [w, h] of [[1, 1], [2, 0.5], [4, 3]]) {
      const r = buildPreset('window', w, h)
      expect(reliefEqual(brushToRelief(reliefToBrush(r, w, h), w, h), r)).toBe(true)
    }
  })
})

describe('presets', () => {
  it('every preset covers the tile, keeps the border flat and has no warnings', () => {
    for (const p of PRESETS) {
      const r = buildPreset(p.id, 2, 3)
      const rep = reliefReport(reliefToBrush(r, 2, 3), 2, 3)
      expect(rep.warnings, p.id).toEqual([])
      if (p.id === 'flat') expect(isFlatRelief(r)).toBe(true)
      else expect(isFlatRelief(r)).toBe(false)
    }
  })
  it('recess sinks, raise stands out', () => {
    const rc = reliefReport(reliefToBrush(buildPreset('recess', 1, 1), 1, 1), 1, 1)
    const ra = reliefReport(reliefToBrush(buildPreset('raise', 1, 1), 1, 1), 1, 1)
    expect(rc.minZ).toBeLessThan(0); expect(rc.maxZ).toBe(0)
    expect(ra.maxZ).toBeGreaterThan(0); expect(ra.minZ).toBe(0)
  })
  it('window has a pane set forward of the recess', () => {
    const rep = reliefReport(reliefToBrush(buildPreset('window', 2, 2), 2, 2), 2, 2)
    expect(rep.minZ).toBeLessThan(0)
  })
})

describe('report', () => {
  it('warns about a raised border, a hole and back faces', () => {
    const b = reliefToBrush(flatRelief(), 1, 1)
    const up = transformVerts(b, [0], (p) => [p[0], p[1], 0.2])
    if (isError(up)) throw new Error(up.error)
    expect(reliefReport(up.brush, 1, 1).warnings.some((t) => t.includes('border'))).toBe(true)
    const small = { ...b, vertices: b.vertices.map((v) => [v[0] * 0.5, v[1], v[2]] as [number, number, number]) }
    expect(reliefReport(small, 1, 1).warnings.some((t) => t.includes('covers'))).toBe(true)
    const back = { ...b, faces: b.faces.map((f) => ({ ...f, loop: [...f.loop].reverse() })) }
    expect(reliefReport(back, 1, 1).warnings.some((t) => t.includes('look into'))).toBe(true)
  })
  it('triangle data has 8 floats per vertex and uv in the picture', () => {
    const t = reliefTriangles(reliefToBrush(flatRelief(), 2, 4), 2, 4)
    expect(t.length).toBe(2 * 3 * 8)
    for (let i = 0; i < t.length; i += 8) { expect(t[i + 6]).toBeGreaterThanOrEqual(0); expect(t[i + 6]).toBeLessThanOrEqual(1); expect(t[i + 7]).toBeGreaterThanOrEqual(0); expect(t[i + 7]).toBeLessThanOrEqual(1) }
  })
  it('confineBrush pulls vertices back into the tile', () => {
    const b = reliefToBrush(flatRelief(), 1, 1)
    const r = transformVerts(b, [2], () => [5, 5, 50])
    if (isError(r)) throw new Error(r.error)
    const c = confineBrush(r.brush, 1, 1)
    for (const v of c.vertices) { expect(v[0]).toBeLessThanOrEqual(1); expect(v[1]).toBeLessThanOrEqual(1); expect(Math.abs(v[2])).toBeLessThanOrEqual(RELIEF_MAX_DEPTH) }
  })
})

describe('ReliefSession', () => {
  it('starts flat and edits with undo / redo', () => {
    const s = new ReliefSession(null, 2, 2)
    expect(s.toRelief().faces.length).toBe(1)
    s.selectAll()
    expect(s.setDepth(0.3)).toBeNull()
    expect(s.toRelief().vertices.every((v) => v[2] === 0.3)).toBe(true)
    expect(s.canUndo).toBe(true)
    s.undo(); expect(isFlatRelief(s.toRelief())).toBe(true)
    s.redo(); expect(s.toRelief().vertices[0][2]).toBe(0.3)
  })
  it('a modal drag is one undo step', () => {
    const s = new ReliefSession(null, 1, 1)
    s.setSel(s.pick([0])); s.setMode('face'); s.setSel(s.pick([0]))
    s.beginModal()
    for (const d of [0.1, 0.2, 0.3]) expect(s.modalUpdate((b, sel) => extrudeFaces(b, sel.faces, d))).toBeNull()
    s.endModal(true)
    expect(s.toRelief().vertices.some((v) => Math.abs(v[2] - 0.3) < 1e-9)).toBe(true)
    s.undo()
    expect(isFlatRelief(s.toRelief())).toBe(true)
    expect(s.canUndo).toBe(false)
  })
  it('cancelling a modal restores the mesh', () => {
    const s = new ReliefSession(null, 1, 1)
    s.setSel(s.pick([0]))
    s.beginModal()
    s.modalUpdate((b, sel) => insetFaces(b, sel.faces, 0.1))
    expect(s.brush.faces.length).toBeGreaterThan(1)
    s.endModal(false)
    expect(s.brush.faces.length).toBe(1)
    expect(s.canUndo).toBe(false)
  })
  it('tools are refused while a modal is active and errors are reported', () => {
    const s = new ReliefSession(null, 1, 1)
    s.beginModal()
    expect(s.apply(() => ({ error: 'x' }))).toBe('A tool is active')
    s.endModal(false)
    expect(s.apply(() => ({ error: 'nope' }))).toBe('nope')
    expect(s.error).toBe('nope')
  })
  it('edits stay inside the tile', () => {
    const s = new ReliefSession(null, 1, 1)
    s.selectAll()
    s.apply((b, sel) => transformVerts(b, sel.verts, (p) => [p[0] + 5, p[1] - 5, 100]))
    for (const v of s.toRelief().vertices) { expect(v[0]).toBeLessThanOrEqual(1); expect(v[1]).toBeGreaterThanOrEqual(0); expect(Math.abs(v[2])).toBeLessThanOrEqual(RELIEF_MAX_DEPTH) }
  })
  it('deleting every face leaves the flat quad', () => {
    const s = new ReliefSession(null, 1, 1)
    s.setSel(s.pick([0]))
    s.deleteSelectedFaces()
    expect(s.brush.faces.length).toBeGreaterThan(0)
  })
  it('resize keeps the normalised relief and resets history', () => {
    const s = new ReliefSession(buildPreset('recess', 1, 1), 1, 1)
    const before = s.toRelief()
    s.resize(4, 2)
    expect(reliefEqual(s.toRelief(), before)).toBe(true)
    expect(s.canUndo).toBe(false)
    expect(Math.max(...s.brush.vertices.map((v) => v[0]))).toBeCloseTo(4)
  })
  it('load and reset are undoable', () => {
    const s = new ReliefSession(null, 1, 1)
    s.load(buildPreset('window', 1, 1))
    expect(isFlatRelief(s.toRelief())).toBe(false)
    s.reset(); expect(isFlatRelief(s.toRelief())).toBe(true)
    s.undo(); expect(isFlatRelief(s.toRelief())).toBe(false)
  })
  it('vertex selection drives face tools through faceIdsTouching', () => {
    const s = new ReliefSession(null, 1, 1)
    s.selectAll()
    expect(faceIdsTouching(s.brush, s.sel)).toEqual([0])
  })
})
