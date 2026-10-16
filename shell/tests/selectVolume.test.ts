import { describe, expect, it } from 'vitest'
import { emptyDocument } from '../src/model/document'
import { makeBox } from '../src/model/geometry'
import { clipBrush } from '../src/model/brushOps'
import { brushInside, brushesTouch } from '../src/model/selectVolume'
import { selectByVolume } from '../src/state/actions'
import { makeApp } from './helpers'

const box = (id: string, a: [number, number, number], b: [number, number, number]) => makeBox(id, a, b)

describe('brushInside / brushesTouch', () => {
  const big = box('big', [0, 0, 0], [10, 10, 10])
  it('inside: fully contained (also flush with the walls), but not partly outside', () => {
    expect(brushInside(big, box('a', [1, 1, 1], [2, 2, 2]))).toBe(true)
    expect(brushInside(big, box('b', [0, 0, 0], [10, 10, 10]))).toBe(true)
    expect(brushInside(big, box('c', [9, 9, 9], [11, 11, 11]))).toBe(false)
    expect(brushInside(big, box('d', [20, 0, 0], [21, 1, 1]))).toBe(false)
  })
  it('touching: overlap, face contact, edge contact and corner contact count; a gap does not', () => {
    expect(brushesTouch(big, box('o', [5, 5, 5], [15, 15, 15]))).toBe(true)
    expect(brushesTouch(big, box('f', [10, 0, 0], [12, 3, 3]))).toBe(true)
    expect(brushesTouch(big, box('e', [10, 10, 0], [12, 12, 3]))).toBe(true)
    expect(brushesTouch(big, box('k', [10, 10, 10], [12, 12, 12]))).toBe(true)
    expect(brushesTouch(big, box('g', [10.01, 0, 0], [12, 3, 3]))).toBe(false)
    expect(brushesTouch(big, box('n', [-3, -3, -3], [-1, -1, -1]))).toBe(false)
  })
  it('touching uses the real shape, not the bounding box (a wedge whose slanted face leaves the corner free)', () => {
    const wedge = clipBrush(box('w', [0, 0, 0], [10, 10, 10]), { n: [0.7071067811865476, 0.7071067811865476, 0], d: 0.7071067811865476 * 10 })!
    // the cut-away top-right corner region: inside the AABB of the wedge, but not touching the wedge
    expect(brushesTouch(wedge, box('corner', [9, 9, 4], [10, 10, 6]))).toBe(false)
    expect(brushesTouch(wedge, box('on', [4, 4, 4], [5, 5, 6]))).toBe(true)
  })
})

describe('selectByVolume', () => {
  const setup = () => {
    const d = emptyDocument()
    d.entities = [
      { id: 'w', classname: 'worldspawn', brushes: [box('room', [0, 0, 0], [10, 10, 10]), box('in1', [1, 1, 1], [2, 2, 2]), box('in2', [3, 3, 3], [4, 4, 4]), box('out', [20, 0, 0], [21, 1, 1]), box('edge', [9, 9, 9], [11, 11, 11])] },
      { id: 'f', classname: 'func_group', brushes: [box('grp', [5, 5, 5], [6, 6, 6])] },
    ]
    const a = makeApp(); a.sync(d)
    return a.store
  }
  it('inside selects every brush fully inside, across entities, and not the volume itself', () => {
    const s = setup()
    s.select({ entityId: 'w', brushId: 'room' })
    expect(selectByVolume(s, 'inside')).toBe(true)
    expect(s.state.multi.map((m) => m.brushId).sort()).toEqual(['grp', 'in1', 'in2'])
    expect(s.state.message).toMatch(/3 brushes inside/)
  })
  it('touching also takes the overlapping brush', () => {
    const s = setup()
    s.select({ entityId: 'w', brushId: 'room' })
    selectByVolume(s, 'touching')
    expect(s.state.multi.map((m) => m.brushId).sort()).toEqual(['edge', 'grp', 'in1', 'in2'])
  })
  it('reports when nothing matches or nothing is selected, and leaves the selection alone', () => {
    const s = setup()
    expect(selectByVolume(s, 'inside')).toBe(false)
    expect(s.state.message).toMatch(/select a convex brush/)
    s.select({ entityId: 'w', brushId: 'out' })
    expect(selectByVolume(s, 'touching')).toBe(false)
    expect(s.state.message).toMatch(/no other brush/)
    expect(s.state.selection).toEqual({ entityId: 'w', brushId: 'out' })
  })
})
