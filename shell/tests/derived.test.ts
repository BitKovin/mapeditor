import { describe, expect, it } from 'vitest'
import { boundaryEdges, buildCarvedMesh, buildDecalMesh, carvedKey, DecalEntry, DerivedEntry, DerivedResult, usableCarved, usableDecals, World } from '../src/model/derived'
import { MapDocument, mapEntity } from '../src/model/document'
import { makeBox } from '../src/model/geometry'
import { parseInbound } from '../src/net/protocol'
import { makeApp } from './helpers'

// a 4x4 face at z=0 (normal +z) with a 2x2 hole in the middle, as the four convex strips Core produces,
// deliberately with T-junctions (the top/bottom strips span the whole width, the sides only the hole's height)
const quad = (pts: number[][]): number[] => pts.flat()
const strips = [
  quad([[0, 0, 0], [4, 0, 0], [4, 1, 0], [0, 1, 0]]),
  quad([[0, 3, 0], [4, 3, 0], [4, 4, 0], [0, 4, 0]]),
  quad([[0, 1, 0], [1, 1, 0], [1, 3, 0], [0, 3, 0]]),
  quad([[3, 1, 0], [4, 1, 0], [4, 3, 0], [3, 3, 0]]),
]

const docWith = (extra: Parameters<typeof makeBox>[0][] = []): MapDocument => ({
  schemaVersion: '0.2', prefabInstances: [],
  entities: [{ id: 'w', classname: 'worldspawn', brushes: [makeBox('wall', [0, 0, 0], [4, 4, 4], 'wall/a'), { ...makeBox('hole', [1, 1, -1], [3, 3, 5], 'cut/b'), op: 'subtract' as const }, ...extra.map((id) => makeBox(id, [9, 0, 0], [10, 1, 1]))] }],
})
const W = (doc: MapDocument, prefabs: World['prefabs'] = []): World => ({ doc, prefabs })
const entry = (over: Partial<DerivedEntry> = {}): DerivedEntry => ({
  entityId: 'w', instance: '', brushId: 'wall', subtractors: ['hole'],
  fragments: [
    { brush: 'wall', face: 1, v: strips[0] }, { brush: 'wall', face: 1, v: strips[1] }, { brush: 'wall', face: 1, v: strips[2] }, { brush: 'wall', face: 1, v: strips[3] },
    { brush: 'hole', face: 2, v: quad([[1, 1, 0], [1, 1, 4], [1, 3, 4], [1, 3, 0]]) },
  ], ...over,
})

describe('boundaryEdges', () => {
  it('a lone polygon keeps all its edges', () => {
    expect(boundaryEdges([{ brush: 'b', face: 0, v: strips[0] }]).length / 6).toBe(4)
  })
  it('hides the seams between fragments of one face and keeps only the outline, even with T-junctions', () => {
    const edges = boundaryEdges(strips.map((v) => ({ brush: 'wall', face: 1, v })))
    const segs = edges.length / 6
    // outer square: 4 sides (split where the strips meet is fine only if it stays on the outline) + inner hole: 4 sides
    const lengths: number[] = []
    for (let i = 0; i < segs; i++) lengths.push(Math.hypot(edges[i * 6] - edges[i * 6 + 3], edges[i * 6 + 1] - edges[i * 6 + 4], edges[i * 6 + 2] - edges[i * 6 + 5]))
    expect(lengths.reduce((a, b) => a + b, 0)).toBeCloseTo(16 + 8) // perimeter of the square + perimeter of the hole
    // no edge may lie strictly inside the face, e.g. the seam y=1 between x in (0,1) or (3,4)
    for (let i = 0; i < segs; i++) {
      const [ax, ay, bx, by] = [edges[i * 6], edges[i * 6 + 1], edges[i * 6 + 3], edges[i * 6 + 4]]
      const onOuter = (ax === 0 && bx === 0) || (ax === 4 && bx === 4) || (ay === 0 && by === 0) || (ay === 4 && by === 4)
      const onHole = (ax >= 1 && ax <= 3 && bx >= 1 && bx <= 3 && ay >= 1 && ay <= 3 && by >= 1 && by <= 3)
      expect(onOuter || onHole).toBe(true)
    }
  })
  it('different authored faces never cancel each other', () => {
    const a = { brush: 'b', face: 0, v: quad([[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0]]) }
    const b = { brush: 'b', face: 1, v: quad([[1, 0, 0], [0, 0, 0], [0, 1, 0], [1, 1, 0]]) }
    expect(boundaryEdges([a, b]).length / 6).toBe(8)
  })
})

describe('buildCarvedMesh', () => {
  it('textures fragments from their authored parent face, cut walls from the subtractor', () => {
    const mesh = buildCarvedMesh(entry(), W(docWith()))
    expect([...mesh.textures.keys()].sort()).toEqual(['cut/b', 'wall/a'])
    expect(mesh.textures.get('wall/a')!.length / 8).toBe(4 * 2 * 3) // 4 quads -> 2 triangles each
    expect(mesh.textures.get('cut/b')!.length / 8).toBe(6)
    const n = mesh.textures.get('wall/a')!.slice(3, 6)
    expect(n[2]).toBeCloseTo(1)                                     // normal of the fragments = +z
  })
  it('falls back to a default texture when the parent has disappeared', () => {
    const mesh = buildCarvedMesh(entry({ fragments: [{ brush: 'gone', face: 0, v: strips[0] }] }), W(docWith()))
    expect([...mesh.textures.keys()]).toEqual(['dev/grey'])
  })
})

describe('usableCarved: authored geometry takes over while an edit is unconfirmed', () => {
  const result = (): DerivedResult => ({ rev: 2, carved: [entry()], skipped: [], decals: [] })
  const cache = () => new WeakMap()

  it('uses the result while nothing it depends on has changed', () => {
    const doc = docWith()
    expect(usableCarved(result(), W(doc), W(doc), cache()).has(carvedKey('w', 'wall'))).toBe(true)
  })
  it('keeps it when an unrelated brush is edited', () => {
    const basis = docWith(['other'])
    const now = mapEntity(basis, 'w', (e) => ({ ...e, brushes: e.brushes!.map((b) => (b.id === 'other' ? { ...b, vertices: b.vertices.map((v) => [v[0] + 1, v[1], v[2]] as [number, number, number]) } : b)) }))
    expect(now).not.toBe(basis)
    expect(usableCarved(result(), W(basis), W(now), cache()).size).toBe(1)
  })
  it('drops it when the carved brush, or a subtractor, changed', () => {
    const basis = docWith()
    for (const id of ['wall', 'hole']) {
      const now = mapEntity(basis, 'w', (e) => ({ ...e, brushes: e.brushes!.map((b) => (b.id === id ? { ...b, vertices: b.vertices.map((v) => [v[0] + 0.5, v[1], v[2]] as [number, number, number]) } : b)) }))
      expect(usableCarved(result(), W(basis), W(now), cache()).size).toBe(0)
    }
  })
  it('drops it when the owner changes class (carve scope depends on it) or a brush is gone', () => {
    const basis = docWith()
    expect(usableCarved(result(), W(basis), W(mapEntity(basis, 'w', (e) => ({ ...e, classname: 'func_door' }))), cache()).size).toBe(0)
    expect(usableCarved(result(), W(basis), W(mapEntity(basis, 'w', (e) => ({ ...e, brushes: e.brushes!.filter((b) => b.id !== 'hole') }))), cache()).size).toBe(0)
  })
  it('rebuilds the mesh when a parent face is retextured, and reuses it otherwise', () => {
    const basis = docWith(), c = cache(), r = result()  // one result object, as the Store keeps it
    const first = usableCarved(r, W(basis), W(basis), c).get('w/wall')
    expect(usableCarved(r, W(basis), W(basis), c).get('w/wall')).toBe(first)
    const retextured = mapEntity(basis, 'w', (e) => ({ ...e, brushes: e.brushes!.map((b) => (b.id === 'wall' ? { ...b, faces: b.faces.map((f) => ({ ...f, texture: 'new/tex' })) } : b)) }))
    // 'wall' itself changed, so it is not usable at all until Core recomputes -- and the cache must not hand back the stale mesh
    expect(usableCarved(r, W(basis), W(retextured), c).size).toBe(0)
  })
})

describe('a prefab subtractor that cuts the world (door frame in a wall)', () => {
  // root: worldspawn "wall"; prefab instance "door": entity "e_frame" with the subtractive "b_opening"
  const prefabEntity = { id: 'e_frame', classname: 'func_group', brushes: [{ ...makeBox('b_opening', [1, 1, -1], [3, 3, 5], 'cut/door'), op: 'subtract' as const }] }
  const prefabs = (): World['prefabs'] => [{ entity: prefabEntity, instancePath: ['root', 'door'], sourceEntityId: 'e_frame' }]
  const doc = (): MapDocument => ({ schemaVersion: '0.2', prefabInstances: [], entities: [{ id: 'w', classname: 'worldspawn', brushes: [makeBox('wall', [0, 0, 0], [4, 4, 4], 'wall/a')] }] })
  const cut: DerivedEntry = {
    entityId: 'w', instance: '', brushId: 'wall', subtractors: ['b_opening'], subtractorInstances: ['door'],
    fragments: [
      { brush: 'wall', instance: '', face: 1, v: strips[0] },
      { brush: 'b_opening', instance: 'door', face: 2, v: quad([[1, 1, 0], [1, 1, 4], [1, 3, 4], [1, 3, 0]]) },
    ],
  }
  const result = (e: DerivedEntry = cut): DerivedResult => ({ rev: 1, carved: [e], skipped: [], decals: [] })

  it('textures the cut walls from the prefab brush and names its entity and instance for picking', () => {
    const mesh = buildCarvedMesh(cut, W(doc(), prefabs()))
    expect([...mesh.textures.keys()].sort()).toEqual(['cut/door', 'wall/a'])
    const wallTri = mesh.pickTris.find((t) => t.brushId === 'b_opening')!
    expect(wallTri).toMatchObject({ entityId: 'e_frame', instance: 'door' })
    expect(mesh.pickTris.find((t) => t.brushId === 'wall')).toMatchObject({ entityId: 'w', instance: '' })
  })
  it('is used while the prefab is unchanged, and dropped when the prefab subtractor changes', () => {
    const basis = W(doc(), prefabs())
    expect(usableCarved(result(), basis, basis, new WeakMap()).has(carvedKey('w', 'wall'))).toBe(true)
    // the same prefab content, re-expanded (a new array with new objects): not trusted for the document's own brush
    const moved = { ...prefabEntity, brushes: [{ ...prefabEntity.brushes[0], vertices: prefabEntity.brushes[0].vertices.map((v) => [v[0] + 1, v[1], v[2]] as [number, number, number]) }] }
    const now = W(doc(), [{ entity: moved, instancePath: ['root', 'door'], sourceEntityId: 'e_frame' }])
    expect(usableCarved(result(), basis, now, new WeakMap()).size).toBe(0)
    // the prefab is gone entirely
    expect(usableCarved(result(), basis, W(doc(), []), new WeakMap()).size).toBe(0)
  })
  it('without subtractorInstances (older Core) subtractors are looked up in the entry\'s own instance', () => {
    const m = parseInbound(JSON.stringify({ type: 'derived_geometry', rev: 1, skipped: [], decals: [], carved: [{ ...cut, subtractorInstances: ['a', 'b'] }] })) as any
    expect(m.carved[0].subtractorInstances).toBeUndefined()   // mismatched length is discarded
    const ok = parseInbound(JSON.stringify({ type: 'derived_geometry', rev: 1, skipped: [], decals: [], carved: [cut] })) as any
    expect(ok.carved[0].subtractorInstances).toEqual(['door'])
    expect(ok.carved[0].fragments[1].instance).toBe('door')
  })
})

describe('protocol + Store: derived_geometry', () => {
  it('parses, and drops malformed entries', () => {
    const good = entry()
    const m = parseInbound(JSON.stringify({ type: 'derived_geometry', rev: 3, carved: [good, { entityId: 'w' }, { ...good, fragments: [{ brush: 'x', face: 0, v: [1, 2] }] }], skipped: [] }))
    expect(m).toMatchObject({ type: 'derived_geometry', rev: 3 })
    expect((m as any).carved).toHaveLength(1)
    expect(parseInbound('{"type":"derived_geometry","rev":"x","carved":[]}')).toBeNull()
  })

  it('is used once the ack for the same revision arrives, and only while the brushes are unchanged', () => {
    const { store, sock, sync } = makeApp()
    sync(docWith(), 1)
    expect(store.carved().size).toBe(0)
    // an edit is acknowledged as rev 2; Core's derived_geometry for rev 2 is delivered BEFORE the ack
    store.edit('move other', (d) => mapEntity(d, 'w', (e) => ({ ...e, keyvalues: { a: '1' } })))
    sock().receive({ type: 'derived_geometry', rev: 2, carved: [entry()], skipped: [], decals: [] })
    expect(store.carved().size).toBe(0)             // not confirmed yet: authored geometry
    sock().receive({ type: 'update_ack', rev: 2 })
    expect(store.carved().size).toBe(1)
    // now the carved brush itself is edited: back to authored until Core answers
    store.edit('move wall', (d) => mapEntity(d, 'w', (e) => ({ ...e, brushes: e.brushes!.map((b) => (b.id === 'wall' ? { ...b, vertices: b.vertices.map((v) => [v[0] + 1, v[1], v[2]] as [number, number, number]) } : b)) })))
    expect(store.carved().size).toBe(0)
    sock().receive({ type: 'update_ack', rev: 3 })
    expect(store.carved().size).toBe(0)             // rev 3 has no derived message yet: still the old one, which is now stale
  })

  it('applies to a freshly opened document when derived_geometry follows the sync', () => {
    const { store, sock } = makeApp(); sock().open()
    sock().receive({ type: 'document_sync', rev: 5, path: 'm.map.json', doc: docWith() })
    sock().receive({ type: 'derived_geometry', rev: 5, carved: [entry()], skipped: [], decals: [] })
    expect(store.carved().size).toBe(1)
  })

  it('applies when the sync follows the derived message (another window edited)', () => {
    const { store, sock } = makeApp(); sock().open()
    sock().receive({ type: 'derived_geometry', rev: 6, carved: [entry()], skipped: [], decals: [] })
    sock().receive({ type: 'document_sync', rev: 6, path: 'm.map.json', doc: docWith() })
    expect(store.carved().size).toBe(1)
  })

  it('reports carvings Core gave up on', () => {
    const { store, sock, sync } = makeApp(); sync(docWith(), 1)
    sock().receive({ type: 'derived_geometry', rev: 1, carved: [], skipped: [{ entityId: 'w', brushId: 'wall' }], decals: [] })
    expect(store.state.message).toMatch(/too complex/)
  })
})

describe('prefab content', () => {
  const prefabEntity = (id: string, brushes = docWith().entities[0].brushes!): import('../src/model/expand').ResolvedEntity =>
    ({ entity: { id: 'pe', classname: 'func_detail', brushes: brushes.map((b) => ({ ...b })) }, instancePath: ['root', id], sourceEntityId: 'pe' })
  const pentry = (): DerivedEntry => ({ ...entry({ entityId: 'pe', instance: 'inst1' }) })

  it('keys prefab brushes by instance so two instances of one prefab do not collide', () => {
    expect(carvedKey('pe', 'wall', 'inst1')).toBe('inst1|pe/wall')
    expect(carvedKey('pe', 'wall', 'inst2')).not.toBe(carvedKey('pe', 'wall', 'inst1'))
    expect(carvedKey('w', 'wall')).toBe('w/wall')
  })
  it('uses the result while the expansion is unchanged and drops it when it is re-expanded', () => {
    const prefabs = [prefabEntity('inst1')]
    const doc = docWith()
    const r: DerivedResult = { rev: 1, carved: [pentry()], skipped: [], decals: [] }
    const cache = new WeakMap()
    expect(usableCarved(r, W(doc, prefabs), W(doc, prefabs), cache).has('inst1|pe/wall')).toBe(true)
    // the store re-expands when an instance moves or the library changes: new array, new objects
    const again = [prefabEntity('inst1')]
    expect(usableCarved(r, W(doc, prefabs), W(doc, again), cache).size).toBe(0)
  })
  it('a subtractor of the root document that carved prefab content is found and used for parent faces', () => {
    const prefabs = [{ ...prefabEntity('inst1', [makeBox('wall', [0, 0, 0], [4, 4, 4], 'pf/wall')]) }]
    const doc = docWith()                                      // has the root subtractor "hole" (cut/b)
    const e = pentry()
    const mesh = buildCarvedMesh(e, W(doc, prefabs))
    expect([...mesh.textures.keys()].sort()).toEqual(['cut/b', 'pf/wall'])   // wall from the prefab, cut wall from the root's brush
  })
  it('pick triangles name the authored face they lie on and the entity that owns it', () => {
    const mesh = buildCarvedMesh(entry(), W(docWith()))
    expect(mesh.pickTris.length).toBe(4 * 2 + 2)
    const wall = mesh.pickTris.find((t) => t.brushId === 'wall')!
    expect(wall).toMatchObject({ entityId: 'w', faceId: 1 })
    expect(wall.normal[2]).toBeCloseTo(1)
    const cut = mesh.pickTris.find((t) => t.brushId === 'hole')!
    expect(cut).toMatchObject({ entityId: 'w', faceId: 2 })      // clicking a cut wall means the subtractor's face
  })
})

describe('decals', () => {
  const decal = (over: Partial<DecalEntry> = {}): DecalEntry => ({
    decalId: 'd', instance: '', texture: 'decals/dirt',
    fragments: [{ entityId: 'w', instance: '', brush: 'wall', face: 1, v: strips[0], uv: [0, 0, 1, 0, 1, 1, 0, 1] }], ...over,
  })
  const withDecalEntity = (base: MapDocument): MapDocument => ({ ...base, entities: [...base.entities, { id: 'd', classname: 'decal', origin: [1, 0, 1], keyvalues: {} }] })

  it('builds triangles with the projector uv and no outline', () => {
    const m = buildDecalMesh(decal())
    expect([...m.textures.keys()]).toEqual(['decals/dirt'])
    const v = m.textures.get('decals/dirt')!
    expect(v.length / 8).toBe(6)
    expect([v[6], v[7]]).toEqual([0, 0])         // first corner's uv comes from Core, not from a re-projection
    expect(m.edges).toEqual([])
  })
  it('is trusted only while the projector and every surface it landed on are unchanged', () => {
    const basis = withDecalEntity(docWith())
    const r: DerivedResult = { rev: 1, carved: [], skipped: [], decals: [decal()] }
    const cache = new WeakMap()
    expect(usableDecals(r, W(basis), W(basis), cache).length).toBe(1)
    const unrelated = mapEntity(basis, 'w', (e) => ({ ...e, keyvalues: { x: '1' } }))    // entity object changed, brushes did not
    expect(usableDecals(r, W(basis), W(unrelated), cache).length).toBe(1)
    const moved = { ...basis, entities: basis.entities.map((e) => (e.id === 'd' ? { ...e, origin: [2, 0, 1] as [number, number, number] } : e)) }
    expect(usableDecals(r, W(basis), W(moved), cache).length).toBe(0)                    // projector edited: hidden until Core answers
    const surface = mapEntity(basis, 'w', (e) => ({ ...e, brushes: e.brushes!.map((b) => (b.id === 'wall' ? { ...b, vertices: b.vertices.map((p) => [p[0], p[1] + 1, p[2]] as [number, number, number]) } : b)) }))
    expect(usableDecals(r, W(basis), W(surface), cache).length).toBe(0)                  // the surface under it moved
    expect(usableDecals(r, W(basis), W({ ...basis, entities: basis.entities.filter((e) => e.id !== 'd') }), cache).length).toBe(0) // deleted
  })
  it('protocol keeps decals and defaults the instance', async () => {
    const { parseInbound } = await import('../src/net/protocol')
    const m = parseInbound(JSON.stringify({ type: 'derived_geometry', rev: 1, carved: [], decals: [{ decalId: 'd', texture: 't', fragments: [{ entityId: 'w', brush: 'b', face: 0, v: strips[0], uv: [0, 0, 1, 0, 1, 1, 0, 1] }] },
      { decalId: 'bad', texture: 't', fragments: [{ entityId: 'w', brush: 'b', face: 0, v: strips[0], uv: [0] }] }, { nope: 1 }] }))
    expect((m as any).decals).toHaveLength(1)                // a decal with a malformed fragment is dropped whole, as is junk
    expect((m as any).decals[0].instance).toBe('')
    expect((m as any).decals[0].fragments[0].instance).toBe('')
  })
})

describe('picking carved surfaces', () => {
  it('the hole you cut is clickable, and a cut wall names the subtractor', async () => {
    const { pick } = await import('../src/render/picking')
    const doc = docWith()
    const mesh = buildCarvedMesh(entry(), W(doc))
    const carved = new Map([['w/wall', mesh]])
    const ents = doc.entities
    // a ray straight down the +z axis through the hole (x=0.5,y=2 is inside the wall's strip at x<1; x=2,y=2 is in the hole)
    const through = { origin: [2, 2, 10] as [number, number, number], dir: [0, 0, -1] as [number, number, number] }
    const raw = pick(through, ents, { includeSubtractive: false })!                       // authored wall: the wall is solid, so the ray stops at its face at z=4
    expect(raw).toMatchObject({ entityId: 'w', brushId: 'wall' })
    const hit = pick(through, ents, { carved, includeSubtractive: false })
    expect(hit).toBeNull()                                  // the carved wall has a hole here (only fragment strips remain)
    const side = pick({ origin: [0.5, 2, 10], dir: [0, 0, -1] }, ents, { carved, includeSubtractive: false })!
    expect(side).toMatchObject({ entityId: 'w', brushId: 'wall', faceId: 1 })
    const cutWall = pick({ origin: [0, 2, 0.5], dir: [1, 0, 0] }, ents, { carved, includeSubtractive: false })!
    expect(cutWall).toMatchObject({ brushId: 'hole', faceId: 2 })
  })
  it('hidden subtractive shapes do not catch clicks', async () => {
    const { pick } = await import('../src/render/picking')
    const doc = docWith()
    const ray = { origin: [2, 2, 10] as [number, number, number], dir: [0, 0, -1] as [number, number, number] }
    const hit = pick(ray, doc.entities.map((e) => ({ ...e, brushes: e.brushes!.filter((b) => b.op === 'subtract') })))
    expect(hit?.brushId).toBe('hole')
    expect(pick(ray, doc.entities.map((e) => ({ ...e, brushes: e.brushes!.filter((b) => b.op === 'subtract') })), { includeSubtractive: false })).toBeNull()
  })
})
