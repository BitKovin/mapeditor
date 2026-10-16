import { describe, expect, it } from 'vitest'
import { advanceVirtualCursor, describeModal, stepSegments, grabDelta, metresPerPixel, planePoint, rotateAngle, scaleFactor, scaleVector, typeInto, typedValue, widthFromMouse } from '../src/tools/modal'
import { boxSelect, combineSel, pickEdge, pickFaceId, pickVertex, Project } from '../src/tools/meshPick'
import { makeBox } from '../src/model/geometry'
import { selFromFaces } from '../src/model/meshTopology'

describe('numeric entry', () => {
  it('builds numbers key by key', () => {
    let b = ''
    for (const k of ['1', '.', '5']) b = typeInto(b, k)!
    expect(b).toBe('1.5'); expect(typedValue(b)).toBe(1.5)
    expect(typeInto('1.5', '.')).toBe('1.5')             // one decimal point only
    expect(typeInto('', '.')).toBe('0.')
    expect(typeInto('2', '-')).toBe('-2'); expect(typeInto('-2', '-')).toBe('2')
    expect(typeInto('12', 'Backspace')).toBe('1')
    expect(typeInto('1', 'x')).toBeNull()                // not numeric: the caller treats it as an axis key etc.
    expect(typedValue('')).toBeNull(); expect(typedValue('-')).toBeNull()
  })
})

describe('mouse to parameter', () => {
  const ray = (x: number, y: number) => ({ origin: [x, y, 10] as [number, number, number], dir: [0, 0, -1] as [number, number, number] })
  it('grab on the view plane follows the mouse, with grid snapping', () => {
    const d = grabDelta(ray(3.2, 1.1), [1, 1, 0], [1, 1, 0], [0, 0, -1], null, 0.5)
    expect(d).toEqual([2, 0, 0])
    expect(grabDelta(ray(3.2, 1.1), [1, 1, 0], [1, 1, 0], [0, 0, -1], null, 0)[0]).toBeCloseTo(2.2)
  })
  it('grab along an axis only moves along it', () => {
    const d = grabDelta(ray(4, 7), [1, 0, 0], [1, 0, 0], [0, 0, -1], 0, 0)
    expect(d[1]).toBe(0); expect(d[2]).toBe(0); expect(d[0]).toBeCloseTo(3)
  })
  it('the plane through a point facing the camera', () => {
    expect(planePoint(ray(2, 3), [0, 0, 0], [0, 0, 1])).toEqual([2, 3, 0])
    expect(planePoint({ origin: [0, 0, 1], dir: [1, 0, 0] }, [0, 0, 0], [0, 0, 1])).toBeNull()
  })
  it('rotation follows the angle around the pivot, snapping to steps', () => {
    const a = rotateAngle({ x: 0, y: 0 }, { x: 10, y: 0 }, { x: 0, y: -10 }, true, 0)   // mouse moved counter-clockwise on screen
    expect(a).toBeCloseTo(Math.PI / 2)
    expect(rotateAngle({ x: 0, y: 0 }, { x: 10, y: 0 }, { x: 0, y: -10 }, false, 0)).toBeCloseTo(-Math.PI / 2)
    expect(rotateAngle({ x: 0, y: 0 }, { x: 10, y: 0 }, { x: 10, y: -1 }, true, Math.PI / 12)).toBeCloseTo(0)
  })
  it('scale is the ratio of distances, vectors respect the axis', () => {
    expect(scaleFactor({ x: 0, y: 0 }, { x: 10, y: 0 }, { x: 20, y: 0 }, 0)).toBeCloseTo(2)
    expect(scaleFactor({ x: 0, y: 0 }, { x: 1, y: 0 }, { x: 50, y: 0 }, 0)).toBe(1)   // started on the pivot: no scaling
    expect(scaleVector(2, null)).toEqual([2, 2, 2]); expect(scaleVector(2, 1)).toEqual([1, 2, 1])
  })
  it('width for inset/bevel never goes negative and snaps', () => {
    expect(widthFromMouse({ x: 0, y: 0 }, { x: 30, y: 40 }, 0.01, 0)).toBeCloseTo(0.5)
    expect(widthFromMouse({ x: 0, y: 0 }, { x: 30, y: 40 }, 0.01, 0.25)).toBeCloseTo(0.5)
    expect(metresPerPixel(10, Math.PI / 2, 1000)).toBeCloseTo(0.02)
  })
  it('wheel steps bevel segments within 1..max', () => {
    expect(stepSegments(1, -100, 32)).toBe(2)   // wheel up adds
    expect(stepSegments(2, 100, 32)).toBe(1)    // wheel down removes
    expect(stepSegments(1, 100, 32)).toBe(1)    // never below 1 (1 = chamfer)
    expect(stepSegments(32, -100, 32)).toBe(32) // never above the maximum
    expect(stepSegments(5, 0, 32)).toBe(5)
  })
  it('a locked-pointer modal keeps a virtual cursor that may leave the canvas', () => {
    let p = { x: 10, y: 10 }
    p = advanceVirtualCursor(p, -50, 7); p = advanceVirtualCursor(p, -50, 7)
    expect(p).toEqual({ x: -90, y: 24 })
  })
  it('bevel status line shows the segment count', () => {
    expect(describeModal('bevel', 0.1, null, '', 4)).toMatch(/4 segments \(wheel\)/)
    expect(describeModal('bevel', 0.1, null, '', 1)).toMatch(/1 segment \(wheel\)/)
    expect(describeModal('inset', 0.1, null, '')).not.toMatch(/segment/)
  })
  it('describes the modal for the status bar', () => {
    expect(describeModal('grab', 1.5, 0, '')).toMatch(/Move X: 1.500 m/)
    expect(describeModal('rotate', Math.PI / 2, null, '')).toMatch(/90.0°/)
    expect(describeModal('scale', 2, 2, '')).toMatch(/Scale Z: ×2.000/)
    expect(describeModal('extrude', 0, null, '1.5')).toMatch(/1.5_/)
  })
})

describe('mesh picking', () => {
  const b = makeBox('b', [0, 0, 0], [1, 1, 1])
  // orthographic top-down projection: x,z -> screen, 100 px per metre
  const project: Project = (p) => ({ x: p[0] * 100, y: p[2] * 100, depth: p[1] })
  it('nearest vertex within the radius', () => {
    expect(pickVertex(b, project, 1, 1, 10)).not.toBeNull()
    expect(pickVertex(b, project, 50, 50, 10)).toBeNull()
    expect(b.vertices[pickVertex(b, project, 99, 2, 10)!].slice(0, 3)).toEqual(expect.arrayContaining([1]))
  })
  it('nearest edge', () => {
    const k = pickEdge(b, project, 50, 2, 8)
    expect(k).not.toBeNull()
    expect(pickEdge(b, project, 50, 50, 8)).toBeNull()
  })
  it('front-most face along a ray', () => {
    expect(pickFaceId(b, { origin: [0.5, 5, 0.5], dir: [0, -1, 0] })).toBe(5)      // the top face
    expect(pickFaceId(b, { origin: [0.5, -5, 0.5], dir: [0, 1, 0] })).toBe(4)      // the bottom one
    expect(pickFaceId(b, { origin: [5, 5, 5], dir: [0, 1, 0] })).toBeNull()
  })
  it('box select picks by element kind', () => {
    const all = boxSelect(b, project, 'vertex', -10, -10, 110, 110)
    expect(all.verts.length).toBe(8)
    expect(boxSelect(b, project, 'vertex', -10, -10, 50, 110).verts.length).toBe(4)          // the x = 0 half
    expect(boxSelect(b, project, 'edge', -10, -10, 50, 110).edges.length).toBe(4)
    expect(boxSelect(b, project, 'face', -10, -10, 110, 110).faces.length).toBe(6)
    expect(boxSelect(b, project, 'face', -10, -10, 50, 110).faces).toEqual([2])              // only the -X face is fully inside
  })
  it('shift toggles, plain adds', () => {
    const top = selFromFaces(b, [5]), bottom = selFromFaces(b, [4])
    expect(combineSel(b, top, bottom, 'face', false).faces.sort()).toEqual([4, 5])
    expect(combineSel(b, selFromFaces(b, [4, 5]), bottom, 'face', true).faces).toEqual([5])
  })
})
