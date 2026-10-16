import { describe, expect, it } from 'vitest'
import { mat4 } from 'gl-matrix'
import { DEFAULT_QUAKE_BOX, boxAt, boxFromQuake, boxSize, editorToQuake, originOnSurface, quakeToEditor } from '../src/model/entityBox'
import { entityOrigin, prefabOrigin } from '../src/model/placement'
import { cross } from '../src/model/math'
import { pick } from '../src/render/picking'
import { boxLines, boxTriangles, translatedMatrix } from '../src/render/renderer'
import { isoBox, iconColor } from '../src/render/icons'
import { isOutsideWindow } from '../src/ui/windowGeometry'
import { parseInbound, msg } from '../src/net/protocol'
import { makeApp } from './helpers'
import { emptyDocument } from '../src/model/document'

const close = (a: number[], b: number[]) => a.forEach((v, i) => expect(v).toBeCloseTo(b[i], 9))

describe('Quake units and axes -> editor metres, Y-up', () => {
  it('32 Quake units are 1 metre and Quake Z (up) becomes editor Y (up)', () => {
    close(quakeToEditor([32, 0, 0]), [1, 0, 0])
    close(quakeToEditor([0, 0, 32]), [0, 1, 0])
    close(quakeToEditor([0, 32, 0]), [0, 0, -1]) // Quake's +Y (forward) is the editor's -Z (forward)
    close(quakeToEditor([16, -64, 48]), [0.5, 1.5, 2])
  })
  it('keeps handedness (a rotation, not a mirror) and round-trips', () => {
    const x = quakeToEditor([32, 0, 0]), y = quakeToEditor([0, 32, 0]), z = quakeToEditor([0, 0, 32])
    close(cross(x, y), z) // Quake X x Y = Z must still hold
    for (const v of [[1, 2, 3], [-40, 7, 100]] as [number, number, number][]) close(editorToQuake(quakeToEditor(v)), v)
  })
  it('never produces -0', () => {
    expect(Object.is(quakeToEditor([0, 0, 0])[2], 0)).toBe(true)
    expect(Object.is(quakeToEditor([0, 0, 0])[0], 0)).toBe(true)
  })
})

describe('entity bounding box from the definition', () => {
  it('converts a typical Quake box (-16 -16 -24, 16 16 32)', () => {
    const b = boxFromQuake([-16, -16, -24, 16, 16, 32])
    close(b.min, [-0.5, -0.75, -0.5]); close(b.max, [0.5, 1, 0.5])
    close(boxSize(b), [1, 1.75, 1])
  })
  it('re-sorts the corners: Quake +Y becomes editor -Z, so min and max swap on that axis', () => {
    const b = boxFromQuake([-16, -8, 0, 16, 24, 32]) // asymmetric in Quake Y
    close(b.min, [-0.5, 0, -0.75]); close(b.max, [0.5, 1, 0.25])
  })
  it('falls back to a half-metre cube for a missing or broken box', () => {
    const d = boxFromQuake(null)
    close(d.min, [-0.25, -0.25, -0.25]); close(d.max, [0.25, 0.25, 0.25])
    expect(boxFromQuake([1, 2, 3])).toEqual(d)
    expect(boxFromQuake([4, 0, 0, -4, 1, 1])).toEqual(d) // max below min
    expect(boxFromQuake([NaN, 0, 0, 1, 1, 1])).toEqual(d)
    expect(DEFAULT_QUAKE_BOX).toHaveLength(6)
  })
  it('moves with the origin', () => {
    const b = boxAt(boxFromQuake([-16, -16, -16, 16, 16, 16]), [10, 2, -3])
    close(b.min, [9.5, 1.5, -3.5]); close(b.max, [10.5, 2.5, -2.5])
  })
})

describe('where a dropped entity lands', () => {
  const box = boxFromQuake([-16, -16, -24, 16, 16, 32]) // bottom 0.75 m below the origin
  it('rests on a floor, hangs from a ceiling, sits against a wall', () => {
    close(originOnSurface([4, 0, 2], [0, 1, 0], box), [4, 0.75, 2])
    close(originOnSurface([4, 3, 2], [0, -1, 0], box), [4, 2, 2])
    close(originOnSurface([4, 1, 2], [1, 0, 0], box), [4.5, 1, 2])
    close(originOnSurface([4, 1, 2], [0, 0, -1], box), [4, 1, 1.5]) // a wall facing -z: the box is on the -z side
  })
  it('snaps the point first, then offsets, so the box still touches the surface', () => {
    close(entityOrigin([4.13, 0, 2.4], [0, 1, 0], box, 0.5), [4, 0.75, 2.5])
    close(entityOrigin([4.13, 0.2, 2.4], [0, 1, 0], box, 0), [4.13, 0.95, 2.4]) // snap off
  })
  it('prefabs go to the snapped point', () => close(prefabOrigin([1.2, 0.1, 3.9], 0.5), [1, 0, 4]))
})

describe('picking uses the definition box', () => {
  const ent = { id: 'e1', classname: 'item', origin: [0, 0, 0] as [number, number, number] }
  const tall = { min: [-0.25, 0, -0.25] as [number, number, number], max: [0.25, 2, 0.25] as [number, number, number] }
  it('hits a tall box high above the origin and misses where the default box would not reach', () => {
    const ray = { origin: [3, 1.5, 0] as [number, number, number], dir: [-1, 0, 0] as [number, number, number] }
    expect(pick(ray, [ent], { entityBox: () => tall })?.entityId).toBe('e1')
    expect(pick(ray, [ent])).toBeNull() // the default half-metre box is only 0.25 high
  })
})

describe('draw helpers', () => {
  it('a box has 12 edges and 12 triangles', () => {
    expect(boxLines([0, 0, 0], [1, 1, 1])).toHaveLength(12 * 2 * 3)
    expect(boxTriangles([0, 0, 0], [1, 1, 1])).toHaveLength(12 * 3 * 3)
  })
  it('translatedMatrix equals viewProj * translation', () => {
    const vp = mat4.perspective(mat4.create(), 1, 1.5, 0.1, 100); mat4.multiply(vp, vp, mat4.lookAt(mat4.create(), [3, 4, 5], [0, 1, 0], [0, 1, 0]))
    const want = mat4.multiply(mat4.create(), vp, mat4.fromTranslation(mat4.create(), [1.5, -2, 0.25]))
    close([...translatedMatrix(new Float32Array(vp), [1.5, -2, 0.25])], [...want])
  })
  it('the isometric icon box fits the icon, whatever the proportions', () => {
    for (const size of [[1, 1, 1], [0.5, 4, 0.5], [6, 0.1, 3]] as [number, number, number][]) {
      const b = isoBox(size, 64, 6)
      const pts = [...b.top, ...b.left, ...b.right]
      for (const [x, y] of pts) { expect(x).toBeGreaterThanOrEqual(5.99); expect(x).toBeLessThanOrEqual(58.01); expect(y).toBeGreaterThanOrEqual(5.99); expect(y).toBeLessThanOrEqual(58.01) }
    }
  })
  it('lifts near-black class colours', () => {
    expect(iconColor([0, 0, 0])).toEqual([0.6, 0.6, 0.6])
    expect(iconColor([1, 0, 0])).toEqual([1, 0, 0])
    expect(iconColor(undefined)).toHaveLength(3)
  })
})

describe('store and protocol', () => {
  it('serves the converted box per class, and the default for unknown classes', () => {
    const { store, sock, sync } = makeApp(); sync()
    sock().receive({ type: 'entity_defs', warnings: [], classes: [{ name: 'item_health', kind: 'point', color: [1, 0, 0], box: [-16, -16, -24, 16, 16, 32], description: '', keys: [], flags: [] }, { name: 'info_x', kind: 'point', color: [0, 1, 0], box: null, description: '', keys: [], flags: [] }] })
    close(store.entityBox('item_health').max, [0.5, 1, 0.5])
    close(store.entityBox('info_x').max, [0.25, 0.25, 0.25])
    close(store.entityBox('nope').max, [0.25, 0.25, 0.25])
  })
  it('read_prefab request and a response carrying the document', () => {
    expect(JSON.parse(msg.readPrefab(5, 'prefabs/a.map.json'))).toEqual({ type: 'read_prefab', requestId: 5, path: 'prefabs/a.map.json' })
    const r = parseInbound(JSON.stringify({ type: 'response', requestId: 5, ok: true, path: 'prefabs/a.map.json', doc: emptyDocument() }))
    expect(r && r.type === 'response' && r.doc?.entities).toEqual([])
  })
})

describe('dropping a tab outside the browser window', () => {
  const win = { screenX: 100, screenY: 50, outerWidth: 1000, outerHeight: 700 }
  it('is outside only when the cursor is beyond the window rectangle', () => {
    expect(isOutsideWindow(500, 300, win)).toBe(false)
    expect(isOutsideWindow(1500, 300, win)).toBe(true) // second monitor to the right
    expect(isOutsideWindow(-300, 300, win)).toBe(true) // second monitor to the left
    expect(isOutsideWindow(500, 900, win)).toBe(true)
  })
  it('does not guess when the browser reports 0,0', () => expect(isOutsideWindow(0, 0, win)).toBe(false))
})
