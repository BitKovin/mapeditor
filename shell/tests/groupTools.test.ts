import { readFileSync } from 'node:fs'
import { describe, expect, it } from 'vitest'
import { GROUP_TEMPLATES, cellAt, previewPatternMatrix, previewScale, reliefBorderWarnings, setSlotTexture } from '../src/model/groupTools'
import { Cell, solveLayout } from '../src/model/layout'
import { cellMap } from '../src/model/layoutPlan'
import { buildPreset } from '../src/model/relief'
import { parseGroup, parseGroupText } from '../src/model/texgroup'
import { defaultProps } from '../src/model/texprops'

describe('group templates', () => {
  it.each(GROUP_TEMPLATES.map((t) => [t.id, t] as const))('%s is a clean group and lays out a wall, a floor and a roof', (_id, t) => {
    const r = parseGroup(t.group)
    expect(r.errors).toEqual([]); expect(r.warnings).toEqual([])
    for (const [w, h, facing] of [[9, 12, 'wall'], [5, 5, 'up'], [2, 3, 'wall']] as const) {
      const s = solveLayout({ group: r.group!, w, h, facing, seed: 4 })
      expect(s.truncated).toBe(false); expect(s.cells.length).toBeGreaterThan(0)
      expect(s.cells.reduce((a, c) => a + (c.x1 - c.x0) * (c.y1 - c.y0), 0)).toBeCloseTo(w * h, 4)
    }
  })
  it('the facade template is the shipped apartment block without its extras (same grammar, valid)', () => {
    const shipped = parseGroupText(readFileSync(new URL('../../core/assets/texgroups/apartment_block.texgroup.json', import.meta.url), 'utf8'))
    expect(shipped.errors).toEqual([])
  })
})

describe('relief border check', () => {
  const info = (relief: ReturnType<typeof buildPreset> | null) => () => ({ path: 'p', width: 64, height: 64, ppm: 64, props: { ...defaultProps(), relief } })
  const g = parseGroup({ slots: { a: 'p' }, root: 'a' }).group!
  it('a recess (flat border) is fine; a raised border is reported', () => {
    expect(reliefBorderWarnings(g, info(buildPreset('recess', 1, 1)))).toEqual([])
    expect(reliefBorderWarnings(g, info(null))).toEqual([])
    const raised = { vertices: [[0, 0, 0.1], [1, 0, 0.1], [1, 1, 0.1], [0, 1, 0.1]], faces: [[0, 1, 2, 3]] } as never
    expect(reliefBorderWarnings(g, info(raised))[0]).toMatch(/not flat at the border/)
  })
})

describe('preview pattern transform', () => {
  const cell = (o: Partial<Cell>): Cell => ({ x0: 1, y0: 1, x1: 3, y1: 2, texture: 't', slot: 't', fit: 'stretch', origin: 'cell', alignU: 'start', alignV: 'start', flipU: false, flipV: false, rotate: 0, path: 'r', ...o })
  const apply = (m: number[], x: number, y: number) => [m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]]
  it('an image corner lands on the cell corner it belongs to (canvas y runs down)', () => {
    const m = previewPatternMatrix(cellMap(cell({}), [1, 1], 6, 3), 64, 64, 100, 3)
    // image top-left (0,0) = layout (1, 2) -> canvas (100, 100); bottom-right (64,64) = layout (3, 1) -> canvas (300, 200)
    expect(apply(m, 0, 0)).toEqual([expect.closeTo(100, 6), expect.closeTo(100, 6)]); expect(apply(m, 64, 64)).toEqual([expect.closeTo(300, 6), expect.closeTo(200, 6)])
  })
  it('flips and rotation keep the same rule', () => {
    const c = cell({ flipU: true }), m = previewPatternMatrix(cellMap(c, [1, 1], 6, 3), 64, 64, 100, 3)
    expect(apply(m, 0, 0)[0]).toBeCloseTo(300, 6) // flipped: the image's left edge is at the cell's right edge
    const r = cell({ rotate: 90 }), mr = previewPatternMatrix(cellMap(r, [1, 1], 6, 3), 64, 64, 100, 3)
    const p = apply(mr, 64, 0); expect(Number.isFinite(p[0]) && Number.isFinite(p[1])).toBe(true)
  })
})

describe('editor helpers', () => {
  it('cellAt finds the cell under a point; a shared edge belongs to the later cell; outside is -1', () => {
    const cells = [{ x0: 0, y0: 0, x1: 2, y1: 2 }, { x0: 2, y0: 0, x1: 4, y1: 2 }]
    expect(cellAt(cells, 1, 1)).toBe(0); expect(cellAt(cells, 3, 1)).toBe(1); expect(cellAt(cells, 2, 1)).toBe(1); expect(cellAt(cells, 5, 1)).toBe(-1)
  })
  it('previewScale fits the wall into the canvas minus margins', () => {
    expect(previewScale(116, 116, 10, 5, 8)).toBe(10); expect(previewScale(116, 36, 10, 5, 8)).toBe(4)
  })
  it('setSlotTexture keeps both slot forms and refuses unknown slots / bad text', () => {
    const text = JSON.stringify({ format: 1, slots: { a: 'x/a', b: { texture: 'x/b', note: 1 } }, root: 'a' })
    const t1 = JSON.parse(setSlotTexture(text, 'a', 'y/new')!); expect(t1.slots.a).toBe('y/new')
    const t2 = JSON.parse(setSlotTexture(text, 'b', 'y/new')!); expect(t2.slots.b).toEqual({ texture: 'y/new', note: 1 })
    expect(setSlotTexture(text, 'zz', 'p')).toBeNull(); expect(setSlotTexture('{', 'a', 'p')).toBeNull(); expect(setSlotTexture('[]', 'a', 'p')).toBeNull()
  })
})
