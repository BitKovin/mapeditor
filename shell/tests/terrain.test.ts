import { describe, expect, it } from 'vitest'
import type { Brush, Entity, MapDocument } from '../src/model/document'
import { translateEntity } from '../src/model/document'
import { signedVolume, validateBrush } from '../src/model/geometry'
import { smoothEdgeKeys } from '../src/model/smooth'
import {
  BrushParams, CreateParams, DEFAULT_BRUSH, TerrainStroke, brushWeight, circleOnTerrain, createTerrain, gridFor, heightAt, rayTerrain, reconcileTerrain, specOf, stamp,
  terrainOf, tileOf, tilesAcross, tilesOfRect, cellTexture,
} from '../src/model/terrain'
import { Vec3 } from '../src/model/math'

let n = 0
const bid = () => `b${n++}`
const params = (over: Partial<CreateParams> = {}): CreateParams => ({ sizeX: 10, sizeZ: 6, cell: 1, tile: 4, origin: [-5, 2, -3], thickness: 4, texture: 'dev/grass', ...over })
const make = (over: Partial<CreateParams> = {}): Entity => {
  const e = createTerrain(params(over), 'terrain1', bid)
  if ('error' in e) throw new Error(e.error)
  return e.entity
}
const docOf = (...entities: Entity[]): MapDocument => ({ schemaVersion: '0.2', prefabInstances: [], entities })
const brush = (e: Entity, tx: number, ty: number): Brush => e.brushes!.find((b) => { const t = tileOf(b); return t && t[0] === tx && t[1] === ty })!
const hf = (e: Entity) => terrainOf(e)!

/** Every directed edge of the brush appears once and its reverse once: a closed 2-manifold. */
function closedManifold(b: Brush): boolean {
  const edges = new Map<string, number>()
  for (const f of b.faces) for (let i = 0; i < f.loop.length; i++) { const k = `${f.loop[i]}>${f.loop[(i + 1) % f.loop.length]}`; edges.set(k, (edges.get(k) ?? 0) + 1) }
  for (const [k, c] of edges) { const [a, z] = k.split('>'); if (c !== 1 || !edges.has(`${z}>${a}`)) return false }
  return true
}

describe('creating a terrain', () => {
  it('is one func_detail entity of closed, planar slab tiles (what Core validates, lights and exports)', () => {
    const e = make()
    expect(e.classname).toBe('func_detail')
    expect(e.terrain).toEqual({ cols: 10, rows: 6, cell: 1, tile: 4 })
    expect(e.brushes).toHaveLength(3 * 2) // ceil(10/4) x ceil(6/4)
    expect(new Set(e.brushes!.map((b) => b.id)).size).toBe(6)
    for (const b of e.brushes!) {
      expect(validateBrush(b, false)).toBeNull() // planar, valid indices
      expect(closedManifold(b)).toBe(true)
      expect(signedVolume(b)).toBeGreaterThan(0) // outward wound
      expect(b.faces.every((f) => f.loop.length === 3 || f.loop.length >= 4)).toBe(true)
    }
  })

  it('an edge tile holds the leftover cells', () => {
    const e = make()
    const last = brush(e, 2, 1) // 2 x 2 cells
    expect(last.vertices.length).toBe(9 + 8)
    expect(last.faces.length).toBe(2 * 4 + 8 + 1)
  })

  it('the volume of a flat terrain is its area times its thickness; the top faces up, the walls and underside are caulk', () => {
    const e = make()
    const vol = e.brushes!.reduce((s, b) => s + signedVolume(b), 0)
    expect(vol).toBeCloseTo(10 * 6 * 4, 6)
    const b = brush(e, 0, 0)
    const tops = b.faces.filter((f) => f.id < 2 * 16), sides = b.faces.filter((f) => f.id >= 2 * 16)
    expect(tops.every((f) => f.texture === 'dev/grass')).toBe(true)
    expect(sides.every((f) => f.texture === 'common/caulk')).toBe(true)
  })

  it('smooth edges join the top triangles only, so the whole surface shades as one', () => {
    const b = brush(make(), 0, 0)
    const top = new Set(b.faces.filter((f) => f.id < 32).map((f) => f.id))
    const pairs = b.smooth as [number, number][]
    for (const [a, c] of pairs) { expect(top.has(a) && top.has(c)).toBe(true) }
    // 16 cells -> 32 triangles: 3 edges each, every interior edge is shared once; the border has 16 edges
    expect(pairs.length).toBe((32 * 3 - 16) / 2)
    expect(smoothEdgeKeys(b).size).toBe(pairs.length)
  })

  it('rejects sizes the Core and the Shell would choke on', () => {
    expect('error' in gridFor(params({ cell: 0.01 }))).toBe(true)
    expect('error' in gridFor(params({ sizeX: 4000 }))).toBe(true)
    expect('error' in gridFor(params({ thickness: 0 }))).toBe(true)
    expect('error' in gridFor(params({ tile: 1 }))).toBe(true)
    expect(createTerrain(params({ sizeX: 0 }), 'x', bid)).toHaveProperty('error')
    expect(gridFor(params({ sizeX: 256, sizeZ: 256 }))).toMatchObject({ cols: 256, rows: 256 })
  })

  it('terrainOf reads the heights back from the tiles', () => {
    const e = make()
    const f = hf(e)
    expect(f.spec).toEqual({ cols: 10, rows: 6, cell: 1, tile: 4 })
    expect([f.ox, f.oz, f.base]).toEqual([-5, -3, -2])
    expect(f.h.length).toBe(11 * 7)
    expect(f.h.every((y) => y === 2)).toBe(true)
    expect(terrainOf(e)).toBe(f) // cached
  })
})

describe('the height brushes', () => {
  const P: BrushParams = { radius: 2, strength: 1, falloff: 0 }
  const fresh = () => ({ ...hf(make()), h: hf(make()).h.slice() })
  const at = (f: ReturnType<typeof fresh>, gx: number, gz: number) => f.h[gz * (f.spec.cols + 1) + gx]

  it('weight: full inside, a smooth drop to nothing at the radius', () => {
    expect(brushWeight(0, 4, 0.5)).toBe(1)
    expect(brushWeight(2, 4, 0.5)).toBe(1)
    expect(brushWeight(4, 4, 0.5)).toBe(0)
    expect(brushWeight(3, 4, 0.5)).toBeCloseTo(0.5, 9)
    expect(brushWeight(5, 4, 0.5)).toBe(0)
    expect(brushWeight(1, 4, 0)).toBe(1)
    expect(brushWeight(3.99, 4, 0)).toBe(1) // a hard edge
    expect(brushWeight(0, 4, 1)).toBe(1)
    expect(brushWeight(2, 4, 1)).toBeCloseTo(0.5, 9)
  })

  it('raise adds strength x 0.5 m under the brush and nothing outside; lower and Shift-invert reverse it', () => {
    const f = fresh()
    const r = stamp(f, 'raise', f.ox + 5, f.oz + 3, P)!
    expect(at(f, 5, 3)).toBeCloseTo(2.5, 9)
    expect(at(f, 4, 3)).toBeCloseTo(2.5, 9) // inside the radius with a hard edge
    expect(at(f, 7, 3)).toBe(2) // exactly at the radius: untouched
    expect(at(f, 0, 0)).toBe(2)
    expect(r).toEqual({ gx0: 4, gx1: 6, gz0: 2, gz1: 4 }) // the rim (distance 2) has weight 0
    stamp(f, 'lower', f.ox + 5, f.oz + 3, P)
    expect(at(f, 5, 3)).toBeCloseTo(2, 9)
    stamp(f, 'raise', f.ox + 5, f.oz + 3, P, { invert: true })
    expect(at(f, 5, 3)).toBeCloseTo(1.5, 9)
  })

  it('never digs through the bottom', () => {
    const f = fresh()
    for (let k = 0; k < 40; k++) stamp(f, 'lower', f.ox + 5, f.oz + 3, P)
    expect(at(f, 5, 3)).toBeCloseTo(f.base + 0.25, 9)
  })

  it('smooth pulls a spike toward its neighbours and keeps the mean in range; flatten pulls toward the target', () => {
    const f = fresh()
    f.h[3 * 11 + 5] = 4 // a spike of 2 m
    stamp(f, 'smooth', f.ox + 5, f.oz + 3, { radius: 1.5, strength: 1, falloff: 0 })
    expect(at(f, 5, 3)).toBeCloseTo(2 + 2 / 9, 4) // the 3x3 mean
    expect(at(f, 5, 3)).toBeLessThan(4)
    const g = fresh()
    stamp(g, 'raise', g.ox + 5, g.oz + 3, P)
    stamp(g, 'flatten', g.ox + 5, g.oz + 3, { ...P, strength: 0.5 }, { target: 2 })
    expect(at(g, 5, 3)).toBeCloseTo(2.25, 9)
    stamp(g, 'flatten', g.ox + 5, g.oz + 3, { ...P, strength: 1 }, { target: 2 })
    expect(at(g, 5, 3)).toBeCloseTo(2, 9)
  })

  it('a brush at the border or outside the grid clips instead of failing', () => {
    const f = fresh()
    expect(stamp(f, 'raise', f.ox - 100, f.oz, P)).toBeNull()
    const r = stamp(f, 'raise', f.ox, f.oz, P)!
    expect(r.gx0).toBe(0); expect(r.gz0).toBe(0)
  })

  it('tilesOfRect: a point on a tile border belongs to both tiles', () => {
    const s = hf(make()).spec // tile 4: tiles across = 3
    expect(tilesOfRect(s, { gx0: 4, gx1: 4, gz0: 1, gz1: 1 }).sort()).toEqual([0, 1])
    expect(tilesOfRect(s, { gx0: 4, gx1: 4, gz0: 4, gz1: 4 }).sort()).toEqual([0, 1, 3, 4])
    expect(tilesOfRect(s, { gx0: 10, gx1: 10, gz0: 6, gz1: 6 })).toEqual([5]) // the far corner is in the last tile only
    expect(tilesOfRect(s, { gx0: 0, gx1: 0, gz0: 0, gz1: 0 })).toEqual([0])
  })
})

describe('a stroke', () => {
  const P: BrushParams = { radius: 2, strength: 0.4, falloff: 0.5 }
  const stroke = (e: Entity, tool: 'raise' | 'smooth' | 'flatten' | 'paint' | 'lower' = 'raise', extra = {}) => new TerrainStroke(e, hf(e), tool, P, extra)

  it('changes only the tiles it touched; the others stay the same objects; the result is still a valid terrain', () => {
    const e = make(), doc = docOf(e)
    const s = stroke(e)
    s.moveTo(-5 + 1, -3 + 1) // inside tile (0, 0), clear of every border
    const next = s.document(doc)
    const ne = next.entities[0]
    expect(ne.brushes!.filter((b, i) => b !== e.brushes![i])).toHaveLength(1)
    expect(brush(ne, 1, 0)).toBe(brush(e, 1, 0))
    expect(reconcileTerrain(doc, next)).toBe(next) // built by the terrain code: trusted
    // not trusted, same content (as after a save and reload): reconcile re-checks it and keeps it
    const reloaded = JSON.parse(JSON.stringify(next)) as MapDocument
    expect(reconcileTerrain(null, reloaded)).toBe(reloaded)
    expect(terrainOf(reloaded.entities[0])).not.toBeNull()
    for (const b of ne.brushes!) { expect(validateBrush(b, false)).toBeNull(); expect(closedManifold(b)).toBe(true) }
    expect(Math.max(...hf(ne).h)).toBeGreaterThan(2)
  })

  it('a brush over a tile border moves the shared edge of both tiles the same way (no crack)', () => {
    const e = make(), s = stroke(e)
    s.moveTo(-5 + 4, -3 + 2) // on the border between tile (0,0) and (1,0)
    const ne = s.document(docOf(e)).entities[0]
    const f = hf(ne) // terrainOf is null when a seam does not close
    expect(f).not.toBeNull()
    expect(Math.max(...f.h)).toBeGreaterThan(2)
    for (const b of [brush(ne, 0, 0), brush(ne, 1, 0)]) expect(closedManifold(b)).toBe(true)
  })

  it('the stamps follow the path at a fixed spacing: many small moves = one long move', () => {
    const e = make()
    const a = stroke(e), b = stroke(e)
    a.moveTo(-3, 0); a.moveTo(3, 0)
    b.moveTo(-3, 0)
    for (let x = -3; x <= 3; x += 0.1) b.moveTo(x, 0)
    b.moveTo(3, 0)
    const ha = a.heights.h, hb = b.heights.h
    let worst = 0
    for (let k = 0; k < ha.length; k++) worst = Math.max(worst, Math.abs(ha[k] - hb[k]))
    expect(worst).toBeLessThan(1e-3)
    expect(Math.max(...ha)).toBeGreaterThan(2.3)
  })

  it('flatten aims at the height under the first stamp (or the fixed height)', () => {
    const e = make()
    const up = new TerrainStroke(e, hf(e), 'raise', { radius: 3, strength: 1, falloff: 0 })
    up.moveTo(-5 + 2, -3 + 3)
    const hill = up.document(docOf(e)).entities[0]
    const flat = new TerrainStroke(hill, hf(hill), 'flatten', { radius: 6, strength: 1, falloff: 0 }, { flattenHeight: 2 })
    flat.moveTo(-5 + 5, -3 + 3)
    expect(Math.max(...flat.heights.h)).toBeCloseTo(2, 9)
    const sample = new TerrainStroke(hill, hf(hill), 'flatten', { radius: 1, strength: 1, falloff: 0 })
    const top = heightAt(hf(hill), -5 + 2, -3 + 3)
    sample.moveTo(-5 + 2, -3 + 3)
    expect(heightAt(sample.heights, -5 + 2, -3 + 3)).toBeCloseTo(top, 9) // flattening at the top keeps the top
  })

  it('paint changes the texture of the top faces of the cells under the brush, and no geometry', () => {
    const e = make(), doc = docOf(e)
    const s = stroke(e, 'paint', { texture: 'dev/rock' })
    s.moveTo(-5 + 1.5, -3 + 1.5)
    const ne = s.document(doc).entities[0]
    const sp = specOf(ne)!
    expect(cellTexture(ne, sp, 1, 1)).toBe('dev/rock')
    expect(cellTexture(ne, sp, 8, 5)).toBe('dev/grass')
    expect(brush(ne, 0, 0).vertices).toBe(brush(e, 0, 0).vertices)
    expect(brush(ne, 0, 0).faces.filter((f) => f.texture === 'dev/rock').length).toBeGreaterThan(0)
    expect(brush(ne, 0, 0).faces.filter((f) => f.id >= 32).every((f) => f.texture === 'common/caulk')).toBe(true)
    expect(Math.max(...hf(ne).h)).toBe(2)
  })

  it('moves that add no stamp keep what the stroke already did (the document is the whole stroke so far, not the last move)', () => {
    const e = make(), doc = docOf(e)
    const s = stroke(e)
    s.moveTo(-3, 0)
    const first = s.document(doc)
    s.moveTo(-3.05, 0) // far less than the spacing: no new stamp
    const again = s.document(doc)
    expect(again.entities[0].brushes).toEqual(first.entities[0].brushes)
    expect(again.entities[0]).not.toBe(e)
  })

  it('does nothing before the first stamp', () => {
    const e = make(), doc = docOf(e)
    expect(stroke(e).document(doc)).toBe(doc)
  })
})

describe('keeping the record honest', () => {
  it('moving the whole entity keeps the terrain (the grid is read from the tiles)', () => {
    const e = make(), doc = docOf(e)
    const moved = translateEntity(doc, 'terrain1', [3, 1, -2])
    expect(reconcileTerrain(doc, moved)).toBe(moved)
    const f = terrainOf(moved.entities[0])!
    expect([f.ox, f.oz, f.base]).toEqual([-2, -5, -1])
    expect(f.h.every((y) => Math.abs(y - 3) < 1e-9)).toBe(true)
  })

  it('a mesh edit that moves a top vertex sideways turns the terrain into ordinary brushes', () => {
    const e = make(), doc = docOf(e)
    const b = brush(e, 0, 0)
    const edited: Brush = { ...b, vertices: b.vertices.map((p, i) => (i === 6 ? [p[0] + 0.3, p[1], p[2]] as Vec3 : p)) }
    const next: MapDocument = { ...doc, entities: [{ ...e, brushes: e.brushes!.map((x) => (x === b ? edited : x)) }] }
    const r = reconcileTerrain(doc, next)
    expect(r).not.toBe(next)
    expect(r.entities[0].terrain).toBeUndefined()
    expect(r.entities[0].brushes!.every((x) => x.terrainTile === undefined)).toBe(true)
    expect(r.entities[0].brushes).toHaveLength(6) // the geometry is all still there
  })

  it('a mesh edit that only changes the height of an inner vertex is a sculpt: still a terrain; on a tile border the seam must still close', () => {
    const e = make(), doc = docOf(e)
    const b = brush(e, 0, 0)
    const lift = (idx: number): MapDocument => ({ ...doc, entities: [{ ...e, brushes: e.brushes!.map((x) => (x === b ? { ...b, vertices: b.vertices.map((p, i) => (i === idx ? [p[0], p[1] + 1, p[2]] as Vec3 : p)) } : x)) }] })
    const inner = lift(6)
    expect(reconcileTerrain(doc, inner)).toBe(inner)
    expect(terrainOf(inner.entities[0])).not.toBeNull()
    const border = lift(4) // grid point (4, 0): shared with tile (1, 0), which did not move
    expect(reconcileTerrain(doc, border).entities[0].terrain).toBeUndefined()
  })

  it('moving one tile alone, deleting one, or splitting a face also ends it; editing a texture does not', () => {
    const e = make(), doc = docOf(e)
    const b = brush(e, 1, 1)
    const swap = (nb: Brush | null): MapDocument => ({ ...doc, entities: [{ ...e, brushes: e.brushes!.flatMap((x) => (x === b ? (nb ? [nb] : []) : [x])) }] })
    expect(reconcileTerrain(doc, swap({ ...b, vertices: b.vertices.map((p) => [p[0] + 1, p[1], p[2]] as Vec3) })).entities[0].terrain).toBeUndefined()
    expect(reconcileTerrain(doc, swap(null)).entities[0].terrain).toBeUndefined()
    const textured = swap({ ...b, faces: b.faces.map((f) => ({ ...f, texture: 'dev/red' })) })
    expect(reconcileTerrain(doc, textured)).toBe(textured)
  })

  it('a tile that ends up in another entity (or a pasted copy of it) loses its mark', () => {
    const e = make()
    const lone: Entity = { id: 'other', classname: 'func_detail', brushes: [brush(e, 0, 0)] }
    const next = docOf({ ...e, brushes: e.brushes!.slice(1), terrain: undefined } as Entity, lone)
    const r = reconcileTerrain(null, next)
    expect(r.entities[1].brushes![0].terrainTile).toBeUndefined()
  })

  it('a duplicated terrain (new ids, same content) is still a terrain', () => {
    const e = make()
    const copy: Entity = { ...e, id: 'copy', brushes: e.brushes!.map((b) => ({ ...b, id: b.id + 'c' })) }
    const next = docOf(e, copy)
    expect(reconcileTerrain(docOf(e), next)).toBe(next)
  })

  it('a damaged record is not a terrain', () => {
    const e = make()
    expect(terrainOf({ ...e, terrain: { cols: 3, rows: 3, cell: 1, tile: 4 } })).toBeNull()
    expect(terrainOf({ ...e, terrain: { cols: 'x' } })).toBeNull()
    expect(terrainOf({ id: 'x' })).toBeNull()
  })
})

describe('looking at the surface', () => {
  const bumpy = (): Entity => {
    const e = make({ sizeX: 12, sizeZ: 12, cell: 1, tile: 5 })
    const s = new TerrainStroke(e, hf(e), 'raise', { radius: 3, strength: 1, falloff: 0.3 })
    s.moveTo(-5, -3); s.moveTo(-1, 0); s.moveTo(1, 2)
    return s.document(docOf(e)).entities[0]
  }

  it('heightAt agrees with the triangles of the tiles (a vertex gives its height, a ray down hits the same height)', () => {
    const f = hf(bumpy())
    expect(heightAt(f, f.ox + 3, f.oz + 4)).toBeCloseTo(f.h[4 * 13 + 3], 9)
    for (const [x, z] of [[-4.3, -2.2], [0.1, 0.7], [1.5, 2.5], [-0.2, 3.9], [3.33, -1.1]]) {
      const hit = rayTerrain(f, [x, 50, z], [0, -1, 0])!
      expect(hit.point[1]).toBeCloseTo(heightAt(f, x, z), 6)
    }
  })

  it('rays at an angle find the nearest triangle, also a ray that starts under a hill', () => {
    const f = hf(bumpy())
    const origin: Vec3 = [f.ox - 5, 30, f.oz - 5], tgt: Vec3 = [f.ox + 6, heightAt(f, f.ox + 6, f.oz + 6), f.oz + 6]
    const dir: Vec3 = [tgt[0] - origin[0], tgt[1] - origin[1], tgt[2] - origin[2]]
    const l = Math.hypot(...dir)
    const hit = rayTerrain(f, origin, [dir[0] / l, dir[1] / l, dir[2] / l])!
    expect(hit.point[0]).toBeCloseTo(tgt[0], 5); expect(hit.point[2]).toBeCloseTo(tgt[2], 5)
    expect(rayTerrain(f, [f.ox - 5, 30, f.oz - 5], [0, 1, 0])).toBeNull()
    expect(rayTerrain(f, [f.ox + 100, 30, f.oz], [0, -1, 0])).toBeNull()
    // a ray along the ground that dips below the hill top hits it
    const sideways = rayTerrain(f, [f.ox - 2, 2.4, f.oz + 5], [1, 0, 0])
    expect(sideways === null || Math.abs(sideways.point[1] - 2.4) < 1e-9).toBe(true)
  })

  it('the cursor circle lies on the ground', () => {
    const f = hf(bumpy())
    const lines = circleOnTerrain(f, f.ox + 6, f.oz + 6, 3, 16, 0.05)
    expect(lines.length).toBe(16 * 6)
    for (let k = 0; k < lines.length; k += 3) expect(lines[k + 1]).toBeCloseTo(heightAt(f, lines[k], lines[k + 2]) + 0.05, 9)
  })
})

describe('large grids', () => {
  it('a 256 x 256 terrain: creating, one stroke update and reading it back are fast, and a stroke leaves almost all tiles untouched', () => {
    const t0 = performance.now()
    const e = make({ sizeX: 256, sizeZ: 256, cell: 1, tile: 16, thickness: 10 })
    const created = performance.now() - t0
    expect(e.brushes).toHaveLength(256)
    const doc = docOf(e)
    const t1 = performance.now()
    const s = new TerrainStroke(e, terrainOf(e)!, 'raise', { radius: 8, strength: 0.5, falloff: 0.7 })
    s.moveTo(-100, 0)
    for (let x = -100; x < -60; x += 2) s.moveTo(x, 0)
    const next = s.document(doc)
    const stroked = performance.now() - t1
    expect(next.entities[0].brushes!.filter((b, i) => b !== e.brushes![i]).length).toBeLessThan(20)
    expect(created).toBeLessThan(3000); expect(stroked).toBeLessThan(500)
    expect(tilesAcross(terrainOf(next.entities[0])!.spec)).toBe(16)
  })
})

it('DEFAULT_BRUSH is usable', () => {
  expect(DEFAULT_BRUSH.radius).toBeGreaterThan(0)
  expect(DEFAULT_BRUSH.strength).toBeGreaterThan(0)
})
