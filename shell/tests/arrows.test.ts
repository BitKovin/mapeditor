import { describe, expect, it } from 'vitest'
import { Placed, boundsOf, directionArrow, entityArrows, linkArrow, placedEntities, resolveLinks } from '../src/model/arrows'
import type { Entity, MapDocument } from '../src/model/document'
import { boxFromQuake } from '../src/model/entityBox'
import { expandPrefabs } from '../src/model/expand'
import { makeBox } from '../src/model/geometry'

const box = boxFromQuake([-16, -16, -16, 16, 16, 16]) // 1 m cube, centred
const boxOf = () => box
const ent = (id: string, over: Partial<Entity> = {}): Entity => ({ id, classname: 'info_x', origin: [0, 0, 0], ...over })
const P = (e: Entity, ids: string[] = [], over: Partial<Placed> = {}): Placed => ({ entity: e, ids, sourceId: e.id, ...over })
const seg = (a: number[], i: number) => [a.slice(i * 6, i * 6 + 3), a.slice(i * 6 + 3, i * 6 + 6)]
const keys = () => ['target', 'killtarget']

describe('direction arrow', () => {
  it('starts at the box centre, points along R(angles)*(0,0,1) and is longer than the box half-size', () => {
    const a = directionArrow(ent('p', { origin: [1, 2, 3] }), boxOf)
    const [from, tip] = seg(a, 0)
    expect(from).toEqual([1, 2, 3])
    expect(tip[0]).toBeCloseTo(1); expect(tip[1]).toBeCloseTo(2); expect(tip[2]).toBeGreaterThan(3 + 0.5) // yaw 0: +Z, past the box face
    expect(a.length / 6).toBe(1 + 4)                                   // shaft + four barbs
  })
  it('yaw 90 points +X, pitch changes the elevation', () => {
    const [, tip] = seg(directionArrow(ent('p', { angles: [0, 90, 0] }), boxOf), 0)
    expect(tip[0]).toBeGreaterThan(0.5); expect(tip[2]).toBeCloseTo(0)
    const [, up] = seg(directionArrow(ent('p', { angles: [-90, 0, 0] }), boxOf), 0)
    expect(up[1]).toBeGreaterThan(0.5); expect(Math.abs(up[2])).toBeLessThan(1e-9)
  })
  it('scales with the box: a bigger box gives a longer arrow', () => {
    const big = boxFromQuake([-64, -64, -64, 64, 64, 64])
    const len = (a: number[]) => Math.hypot(a[3] - a[0], a[4] - a[1], a[5] - a[2])
    expect(len(directionArrow(ent('p'), () => big))).toBeGreaterThan(len(directionArrow(ent('p'), boxOf)) * 2)
  })
})

describe('link arrow', () => {
  it('runs between the boxes, head at the target\'s surface', () => {
    const a = boundsOf(ent('a', { origin: [0, 0, 0] }), boxOf), b = boundsOf(ent('b', { origin: [10, 0, 0] }), boxOf)
    const [from, tip] = seg(linkArrow(a, b), 0)
    expect(from[0]).toBeCloseTo(0.5); expect(tip[0]).toBeCloseTo(9.5)  // trimmed to the faces of the 1 m boxes
    expect(from[1]).toBeCloseTo(0)
  })
  it('falls back to centre-to-centre when the boxes overlap, and draws nothing for one point', () => {
    const a = boundsOf(ent('a'), boxOf), b = boundsOf(ent('b', { origin: [0.5, 0, 0] }), boxOf)
    const [from, tip] = seg(linkArrow(a, b), 0)
    expect(from[0]).toBeCloseTo(0); expect(tip[0]).toBeCloseTo(0.5)
    expect(linkArrow(a, a)).toEqual([])
  })
  it('a brush entity is bounded by its brushes', () => {
    const e: Entity = { id: 'd', classname: 'func_door', brushes: [makeBox('b', [0, 0, 0], [2, 4, 6])] }
    expect(boundsOf(e, boxOf)).toEqual({ min: [0, 0, 0], max: [2, 4, 6] })
  })
})

describe('resolveLinks', () => {
  it('follows refs (id paths) in the document', () => {
    const items = [P(ent('btn', { refs: { target: 'door' } })), P(ent('door'))]
    const l = resolveLinks(items, keys)
    expect(l).toHaveLength(1)
    expect(l[0].from.entity.id).toBe('btn'); expect(l[0].to.entity.id).toBe('door'); expect(l[0].key).toBe('target')
  })
  it('a ref with several targets links to each of them', () => {
    const items = [P(ent('btn', { refs: { target: ['d1', 'nope', 'd2'] } })), P(ent('d1')), P(ent('d2'))]
    expect(resolveLinks(items, keys).map((l) => l.to.entity.id)).toEqual(['d1', 'd2'])
  })
  it('a plain name reaches every entity that has it (names may be shared)', () => {
    const items = [P(ent('btn', { keyvalues: { target: 'gate' } })), P(ent('g1', { keyvalues: { targetname: 'gate' } })), P(ent('g2', { keyvalues: { targetname: 'gate' } })), P(ent('x', { keyvalues: { targetname: 'other' } }))]
    expect(resolveLinks(items, keys).map((l) => l.to.entity.id)).toEqual(['g1', 'g2'])
  })
  it('ignores dangling refs and self refs', () => {
    expect(resolveLinks([P(ent('a', { refs: { target: 'nope' } })), P(ent('b', { refs: { target: 'b' } }))], keys)).toEqual([])
  })
  it('resolves several reference keys of one entity', () => {
    const items = [P(ent('a', { refs: { target: 'b', killtarget: 'c' } })), P(ent('b')), P(ent('c'))]
    expect(resolveLinks(items, keys).map((l) => l.key + '>' + l.to.entity.id)).toEqual(['target>b', 'killtarget>c'])
  })
  it('a ref inside a prefab file is relative to that instance; one overridden in the root to the root', () => {
    const items = [
      P(ent('inner', { refs: { target: 'other' } }), ['i1']), P(ent('other'), ['i1']),                  // file ref: scope = the instance (1 id)
      P(ent('o2', { refs: { target: 'world' } }), ['i2'], { refScopes: { target: 0 } }), P(ent('world')), // override written in i2: relative to the root
    ]
    const l = resolveLinks(items, keys)
    expect(l.map((x) => `${x.from.sourceId}>${x.to.sourceId}`)).toEqual(['inner>other', 'o2>world'])
    expect(l[0].to.ids).toEqual(['i1']); expect(l[1].to.ids).toEqual([])
  })
  it('a root entity can reach into a prefab (instance/child/entity path)', () => {
    const items = [P(ent('btn', { refs: { target: 'i1/c1/door' } })), P(ent('door'), ['i1', 'c1'])]
    expect(resolveLinks(items, keys)[0].to.ids).toEqual(['i1', 'c1'])
  })
  it('the plain-name form: targetname in the same scope first, then a resolved global name', () => {
    const items = [
      P(ent('btn', { keyvalues: { target: 'door01' } })), P(ent('d', { keyvalues: { targetname: 'door01' } })),
      P(ent('in', { keyvalues: { target: 'lamp' } }), ['i1']), P(ent('lamp', { keyvalues: { targetname: 'lamp' } }), ['i1']), P(ent('lamp', { keyvalues: { targetname: 'lamp' } }), ['i2']),
    ]
    const l = resolveLinks(items, keys)
    expect(l.map((x) => `${x.from.sourceId}>${x.to.sourceId}@${x.to.ids.join('/')}`)).toEqual(['btn>d@', 'in>lamp@i1'])
    const viaName = resolveLinks([P(ent('btn', { keyvalues: { target: 'door|lock' } })), P(ent('lock', { keyvalues: { targetname: 'lock' } }), ['i9'])], keys, (p) => (p.ids.length ? `door|${p.entity.keyvalues?.targetname}` : ''))
    expect(viaName).toHaveLength(1)
  })
  it('only reference keys count: targetname itself is not a link', () => {
    expect(resolveLinks([P(ent('a', { keyvalues: { targetname: 'b' } })), P(ent('b', { keyvalues: { targetname: 'b' } }))], keys)).toEqual([])
  })
})

describe('entityArrows (what the viewport draws)', () => {
  const opts = (over: Partial<Parameters<typeof entityArrows>[1]> = {}) => ({
    boxOf, colorOf: (c: string) => (c === 'info_x' ? ([0.2, 0.4, 0.8] as [number, number, number]) : undefined), isSelected: () => false, refKeys: keys,
    showDirections: true, showAllLinks: true, ...over,
  })
  const items = [P(ent('a', { refs: { target: 'b' } })), P(ent('b', { origin: [5, 0, 0] })), P({ id: 'w', classname: 'worldspawn', brushes: [makeBox('x', [0, 0, 0], [1, 1, 1])] }), P({ id: 'dc', classname: 'decal', origin: [9, 9, 9] })]

  it('every point entity gets an arrow in its class colour; worldspawn, brush entities and decals do not', () => {
    const o = entityArrows(items, opts({ showAllLinks: false }))
    expect(o).toHaveLength(1)
    expect(o[0].color).toEqual([0.2, 0.4, 0.8])
    expect(o[0].lines.length / 6).toBe(2 * 5)           // a and b
  })
  it('a selected entity\'s arrow is brighter', () => {
    const o = entityArrows(items, opts({ showAllLinks: false, isSelected: (p) => p.entity.id === 'a' }))
    expect(o).toHaveLength(3)                            // plain arrow, bright arrow, and a's link (touches the selection)
    const plain = o.find((x) => x.color[0] === 0.2)!, bright = o.find((x) => x.color[0] > 0.2)!
    expect(plain.lines.length / 6).toBe(5); expect(bright.lines.length / 6).toBe(5)
    expect(bright.color.every((c, i) => c >= plain.color[i])).toBe(true)
  })
  it('links: all of them when asked, only the selection\'s otherwise; the direction option is independent', () => {
    expect(entityArrows(items, opts({ showDirections: false }))).toHaveLength(1)                              // one link, violet
    expect(entityArrows(items, opts({ showDirections: false, showAllLinks: false }))).toHaveLength(0)
    const sel = entityArrows(items, opts({ showDirections: false, showAllLinks: false, isSelected: (p) => p.entity.id === 'b' })) // the target being selected counts too
    expect(sel).toHaveLength(1)
    expect(sel[0].color).not.toEqual(entityArrows(items, opts({ showDirections: false }))[0].color)
  })
})

describe('prefab content', () => {
  it('expansion keeps the scope of every ref, so cross-instance links resolve like the picker', () => {
    const file: MapDocument = { schemaVersion: '0.2', prefabInstances: [], entities: [ent('btn', { refs: { target: 'door' } }), ent('door')] }
    const doc: MapDocument = {
      schemaVersion: '0.2', entities: [ent('world_btn')],
      prefabInstances: [{ id: 'i1', file: 'p.map.json', origin: [10, 0, 0], overrides: { door: { refs: { target: 'world_btn' } } } }],
    }
    const r = expandPrefabs(doc, { 'p.map.json': file })
    const btn = r.entities.find((x) => x.sourceEntityId === 'btn')!
    expect(btn.refScopes).toEqual({ target: 1 })
    const door = r.entities.find((x) => x.sourceEntityId === 'door')!
    expect(door.refScopes).toEqual({ target: 0 })           // overridden in the root's instance: relative to the root
    const links = resolveLinks(placedEntities(doc.entities, r.entities), keys)
    expect(links.map((l) => `${l.from.sourceId}>${l.to.sourceId}`).sort()).toEqual(['btn>door', 'door>world_btn'])
    expect(links.find((l) => l.from.sourceId === 'btn')!.to.entity.origin).toEqual([10, 0, 0]) // the arrow ends at the placed (root-space) door
  })
})
