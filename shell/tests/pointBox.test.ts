import { describe, expect, it } from 'vitest'
import { directionArrow } from '../src/model/arrows'
import { decalBoundsLines } from '../src/model/annotations'
import { decalLocalBox } from '../src/model/decal'
import { emptyDocument } from '../src/model/document'
import type { Entity, MapDocument } from '../src/model/document'
import type { Box } from '../src/model/entityBox'
import { makeBox } from '../src/model/geometry'
import { boundsOfBox, drawsBox, pointBox, worldCorners } from '../src/model/pointBox'
import { pick } from '../src/render/picking'
import { cornerLines, boxLines } from '../src/render/renderer'
import { soleBrushOf, wholeEntityIfSole } from '../src/state/store'

const long: Box = { min: [-1, -0.1, -0.1], max: [1, 0.1, 0.1] } // 2 m along x, thin
const boxOf = () => long
const ent = (over: Partial<Entity> = {}): Entity => ({ id: 'e1', classname: 'info_x', origin: [0, 0, 0], ...over })
const near = (a: number[], b: number[], d = 1e-6) => a.every((v, i) => Math.abs(v - b[i]) < d)
const sameSet = (a: number[][], b: number[][]) => a.every((p) => b.some((q) => near(p, q, 1e-5))) && b.every((q) => a.some((p) => near(p, q, 1e-5)))

describe('point entity box follows angles', () => {
  it('with no angles it is the class box at the origin', () => {
    const b = boundsOfBox(pointBox(ent({ origin: [1, 2, 3] }), boxOf))
    expect(b.min).toEqual([0, 1.9, 2.9])
    expect(b.max).toEqual([2, 2.1, 3.1])
  })

  it('a yaw of 90 degrees turns the long side from x to z', () => {
    const b = boundsOfBox(pointBox(ent({ angles: [0, 90, 0] }), boxOf))
    expect(b.max[2] - b.min[2]).toBeCloseTo(2)
    expect(b.max[0] - b.min[0]).toBeCloseTo(0.2)
  })

  it('an off-centre box swings round the origin, and the direction arrow starts at its turned centre', () => {
    const off: Box = { min: [0, 0, 0], max: [2, 2, 2] }
    const e = ent({ angles: [0, 90, 0] })
    const c = directionArrow(e, () => off).slice(0, 3)
    expect(near(c, [1, 1, -1])).toBe(true) // R(yaw 90) * (1,1,1)
  })

  it('is picked where it really is: a ray along the turned long side hits, one along the old side misses', () => {
    const turned = ent({ angles: [0, 90, 0] })
    const opts = { entityBox: boxOf }
    expect(pick({ origin: [5, 0, 0.8], dir: [-1, 0, 0] }, [turned], opts)?.t).toBeCloseTo(4.9) // z = 0.8 is inside the turned box (+-1 in z)
    const hit = pick({ origin: [0, 0, 5], dir: [0, 0, -1] }, [turned], opts)
    expect(hit?.t).toBeCloseTo(4)
    expect(pick({ origin: [5, 0, 0.8], dir: [-1, 0, 0] }, [ent()], opts)).toBeNull() // unturned: z = 0.8 is outside +-0.1
    expect(pick({ origin: [5, 0, 0], dir: [-1, 0, 0] }, [ent()], opts)?.t).toBeCloseTo(4)
  })

  it('the drawn corners and the drawn edges agree with the axis-aligned helpers when nothing is turned', () => {
    const corners = worldCorners(pointBox(ent(), boxOf))
    expect(cornerLines(corners)).toEqual(boxLines(long.min, long.max))
  })
})

describe('lights and decals have no drawn box but stay selectable', () => {
  it('only plain point entities draw one', () => {
    expect(drawsBox(ent())).toBe(true)
    expect(drawsBox(ent({ classname: 'light_point' }))).toBe(false)
    expect(drawsBox(ent({ classname: 'light_spot' }))).toBe(false)
    expect(drawsBox(ent({ classname: 'decal' }))).toBe(false)
  })

  it('a light is still picked through its class box', () => {
    const light = ent({ classname: 'light_point', origin: [0, 3, 0] })
    expect(pick({ origin: [0, 3, 5], dir: [0, 0, -1] }, [light], { entityBox: () => ({ min: [-0.25, -0.25, -0.25], max: [0.25, 0.25, 0.25] }) })?.entityId).toBe('e1')
  })

  it('a decal is picked by its projector bounds (width x, depth y, height z before angles)', () => {
    const d = ent({ classname: 'decal', keyvalues: { width: '2', height: '1', depth: '0.5' } })
    expect(decalLocalBox(d)).toEqual({ min: [-1, -0.25, -0.5], max: [1, 0.25, 0.5] })
    expect(pick({ origin: [0, 5, 0.4], dir: [0, -1, 0] }, [d])?.t).toBeCloseTo(4.75)
    expect(pick({ origin: [0, 5, 0.7], dir: [0, -1, 0] }, [d])).toBeNull()
    expect(pick({ origin: [1.5, 5, 0], dir: [0, -1, 0] }, [d])).toBeNull()
  })

  it('the picked bounds are the very box the editor draws for the decal, whatever its angles', () => {
    for (const angles of [[0, 0, 0], [90, 0, 0], [30, 50, 20], [-70, 140, 10]] as [number, number, number][]) {
      const d = ent({ classname: 'decal', origin: [1, 2, 3], angles, keyvalues: { width: '2', height: '1.2', depth: '0.6' } })
      const lines = decalBoundsLines(d)
      const drawn: number[][] = []
      for (let i = 0; i < lines.length; i += 3) drawn.push(lines.slice(i, i + 3))
      expect(sameSet(worldCorners(pointBox(d, () => long)), drawn)).toBe(true)
    }
  })
})

describe('a brush that is the only one of its entity selects the entity', () => {
  const brush = (id: string) => makeBox(id, [0, 0, 0], [1, 1, 1])
  const doc = (): MapDocument => ({
    ...emptyDocument(),
    entities: [
      { id: 'w', classname: 'worldspawn', brushes: [brush('a')] },
      { id: 'door', classname: 'func_door', brushes: [brush('b')] },
      { id: 'grp', classname: 'func_group', brushes: [brush('c'), brush('d')] },
      { id: 'pt', classname: 'info_x', origin: [0, 0, 0] },
    ],
  })

  it('a lone brush of a non-world entity becomes the entity', () => {
    expect(wholeEntityIfSole(doc(), { entityId: 'door', brushId: 'b' })).toEqual({ entityId: 'door' })
  })
  it('worldspawn brushes, brushes of a multi-brush entity and faces keep their brush', () => {
    expect(wholeEntityIfSole(doc(), { entityId: 'w', brushId: 'a' })).toEqual({ entityId: 'w', brushId: 'a' })
    expect(wholeEntityIfSole(doc(), { entityId: 'grp', brushId: 'c' })).toEqual({ entityId: 'grp', brushId: 'c' })
    expect(wholeEntityIfSole(doc(), { entityId: 'door', brushId: 'b', faceId: 2 })).toEqual({ entityId: 'door', brushId: 'b', faceId: 2 })
  })
  it('mesh editing finds the brush behind a selected entity of one brush', () => {
    expect(soleBrushOf(doc(), { entityId: 'door' })).toEqual({ entityId: 'door', brushId: 'b' })
    expect(soleBrushOf(doc(), { entityId: 'grp' })).toEqual({ entityId: 'grp' })
    expect(soleBrushOf(doc(), { entityId: 'pt' })).toEqual({ entityId: 'pt' })
  })
})
