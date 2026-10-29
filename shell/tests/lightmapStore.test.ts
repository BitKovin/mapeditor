import { describe, expect, it } from 'vitest'
import { LightmapStore, LIT_VERTEX_FLOATS, packRect } from '../src/render/lightmapStore'
import { parseInbound } from '../src/net/protocol'
import { floatToHalf, halfToFloat } from '../src/render/half'

const b64 = (bytes: Uint8Array) => btoa(String.fromCharCode(...bytes))
const mesh = (rev: number, triangles = 1, atlas = 8, pages = 1, keepPages = false) => ({
  type: 'lit_mesh' as const, rev, atlas, pageCount: pages, triangleCount: triangles, keepPages,
  vertices: b64(new Uint8Array(new Float32Array(triangles * 3 * LIT_VERTEX_FLOATS).fill(0.5).buffer)),
})
// tiles carry linear float16 pixels: `rgba` here are plain numbers, converted like the Core does
const halves = (rgba: number[]) => b64(new Uint8Array(new Uint16Array(rgba.map(floatToHalf)).buffer))
const tile = (page: number, x: number, y: number, w: number, h: number, rgba: number[]) => ({ page, x, y, w, h, rgba: halves(rgba) })

describe('lightmap store', () => {
  it('decodes the lit mesh and allocates one page per page of the atlas', () => {
    const s = new LightmapStore()
    expect(s.ready).toBe(false)
    s.applyMesh(mesh(3, 2, 8, 2))
    expect(s.ready).toBe(true)
    expect(s.mesh!.length).toBe(2 * 3 * LIT_VERTEX_FLOATS)
    expect(s.mesh![0]).toBeCloseTo(0.5)
    expect(s.pages.length).toBe(2)
    expect(s.pages[0].length).toBe(8 * 8 * 4)
    expect(s.rev).toBe(3)
  })

  it('rejects a mesh whose size does not match the triangle count', () => {
    const s = new LightmapStore()
    s.applyMesh({ ...mesh(1, 2), triangleCount: 3 })
    expect(s.ready).toBe(false)
  })

  it('writes tiles into the page, skips pixels that were never baked and reports dirty rectangles', () => {
    const s = new LightmapStore()
    s.applyMesh(mesh(1))
    // 2 x 1 tile: first pixel baked red, second empty (alpha 0)
    expect(s.applyTiles({ type: 'lightmap_tiles', rev: 1, atlas: 8, tiles: [tile(0, 3, 2, 2, 1, [255, 0, 0, 255, 9, 9, 9, 0])] })).toBe(1)
    const at = (x: number, y: number) => Array.from(s.pages[0].slice((y * 8 + x) * 4, (y * 8 + x) * 4 + 4))
    expect(at(3, 2).map(halfToFloat)).toEqual([255, 0, 0, 1])  // alpha is stored as 1.0
    expect(at(4, 2)).toEqual([0, 0, 0, 0])
    const d = s.dirtySince(0)
    expect(d.rects).toEqual([{ page: 0, x: 3, y: 2, w: 2, h: 1, style: 0 }])
    expect(d.full).toBe(false)
    expect(s.dirtySince(d.cursor).rects).toEqual([])  // nothing new for a renderer that is up to date
    expect(s.dirtySince(0).rects.length).toBe(1)      // a second viewport with its own cursor still sees it
    // a later tile does not erase what an earlier one wrote where it has nothing
    s.applyTiles({ type: 'lightmap_tiles', rev: 1, atlas: 8, tiles: [tile(0, 3, 2, 1, 1, [0, 0, 0, 0])] })
    expect(at(3, 2).map(halfToFloat)).toEqual([255, 0, 0, 1])
  })

  it('ignores tiles of another revision, outside the atlas, or with the wrong size', () => {
    const s = new LightmapStore()
    s.applyMesh(mesh(5))
    const px = [1, 2, 3, 255]
    expect(s.applyTiles({ type: 'lightmap_tiles', rev: 4, atlas: 8, tiles: [tile(0, 0, 0, 1, 1, px)] })).toBe(0)
    expect(s.applyTiles({ type: 'lightmap_tiles', rev: 5, atlas: 8, tiles: [tile(0, 8, 0, 1, 1, px), tile(1, 0, 0, 1, 1, px), tile(0, 0, 0, 2, 1, px)] })).toBe(0)
    expect(s.dirtySince(0).rects).toEqual([])
  })

  it('a new mesh starts from empty pages and bumps the serial; disabling clears everything', () => {
    const s = new LightmapStore()
    s.applyMesh(mesh(1))
    const first = s.serial
    s.applyTiles({ type: 'lightmap_tiles', rev: 1, atlas: 8, tiles: [tile(0, 0, 0, 1, 1, [9, 9, 9, 255])] })
    s.applyMesh(mesh(2))
    expect(s.serial).toBeGreaterThan(first)
    expect(s.pages[0][0]).toBe(0)
    expect(s.dirtySince(0).rects).toEqual([])
    s.applyStatus({ type: 'lighting_status', enabled: false, rev: 2, chunks: 0, stale: 0, minPasses: 0, maxPasses: 8, converged: false, texels: 0, pages: 0 })
    expect(s.ready).toBe(false)
  })

  it('asks for a full re-upload when a renderer fell too far behind the dirty log', () => {
    const s = new LightmapStore()
    s.applyMesh(mesh(1, 1, 64))
    let cursor = 0
    for (let i = 0; i < 5000; i++) s.applyTiles({ type: 'lightmap_tiles', rev: 1, atlas: 64, tiles: [tile(0, i % 60, 0, 1, 1, [1, 1, 1, 255])] })
    cursor = s.dirtySince(cursor).cursor
    for (let i = 0; i < 5000; i++) s.applyTiles({ type: 'lightmap_tiles', rev: 1, atlas: 64, tiles: [tile(0, i % 60, 1, 1, 1, [1, 1, 1, 255])] })
    expect(s.dirtySince(0).full).toBe(true)
    expect(s.dirtySince(s.dirtySince(0).cursor).full).toBe(false)
  })

  it('reports progress from the status', () => {
    const s = new LightmapStore()
    expect(s.progress()).toBe(0)
    const st = { type: 'lighting_status' as const, enabled: true, rev: 1, chunks: 10, stale: 10, minPasses: 0, maxPasses: 4, converged: false, texels: 100, pages: 1 }
    s.applyStatus(st)
    expect(s.progress()).toBe(0)
    s.applyStatus({ ...st, stale: 0, minPasses: 2 })
    expect(s.progress()).toBeGreaterThan(0.4)
    expect(s.progress()).toBeLessThan(1)
    s.applyStatus({ ...st, stale: 0, minPasses: 4, converged: true })
    expect(s.progress()).toBe(1)
  })
})

describe('lighting protocol messages', () => {
  it('parses lit_mesh, lightmap_tiles and lighting_status and rejects malformed ones', () => {
    expect(parseInbound(JSON.stringify(mesh(1)))).toMatchObject({ type: 'lit_mesh', rev: 1 })
    expect(parseInbound(JSON.stringify({ ...mesh(1), atlas: 0 }))).toBeNull()
    expect(parseInbound(JSON.stringify({ ...mesh(1), vertices: 5 }))).toBeNull()
    const t = { type: 'lightmap_tiles', rev: 1, atlas: 8, tiles: [tile(0, 0, 0, 1, 1, [1, 2, 3, 255]), { page: 0, x: -1, y: 0, w: 1, h: 1, rgba: '' }] }
    const parsed = parseInbound(JSON.stringify(t)) as any
    expect(parsed.tiles.length).toBe(1) // the malformed tile is dropped, the good one survives
    expect(parseInbound(JSON.stringify({ type: 'lightmap_tiles', rev: 1, atlas: 8 }))).toBeNull()
    const st = { type: 'lighting_status', enabled: true, rev: 1, chunks: 2, stale: 1, minPasses: 0, maxPasses: 4, converged: false, texels: 9, pages: 1 }
    expect(parseInbound(JSON.stringify(st))).toMatchObject({ type: 'lighting_status', stale: 1 })
    expect(parseInbound(JSON.stringify({ ...st, enabled: 'yes' }))).toBeNull()
  })

  it('fullRects covers every page, also after a second mesh reset the change log (the renderer must not rely on dirtySince(0) then)', () => {
    const s = new LightmapStore()
    s.applyMesh(mesh(1, 1, 8, 2))
    s.applyTiles({ type: 'lightmap_tiles', rev: 1, atlas: 8, tiles: [tile(0, 0, 0, 1, 1, [1, 2, 3, 255])] })
    s.applyMesh(mesh(2, 1, 8, 2))
    s.applyTiles({ type: 'lightmap_tiles', rev: 2, atlas: 8, tiles: [tile(1, 0, 0, 1, 1, [1, 2, 3, 255])] })
    const stale = s.dirtySince(0)
    expect(stale.full).toBe(true)
    expect(stale.rects).toEqual([])  // this is why a fresh texture must use fullRects()
    expect(s.fullRects()).toEqual([{ page: 0, x: 0, y: 0, w: 8, h: 8, style: 0 }, { page: 1, x: 0, y: 0, w: 8, h: 8, style: 0 }])
  })
})

describe('packRect (what the renderer hands to texSubImage3D)', () => {
  const atlas = 8
  const page = new Uint16Array(atlas * atlas * 4)
  for (let i = 0; i < atlas * atlas; i++) page.set([i, i + 100, i + 150, 255], i * 4) // texel index encoded in the colour

  it('copies an inner rectangle row by row into a tight buffer', () => {
    const out = packRect(page, atlas, { page: 0, x: 2, y: 3, w: 3, h: 2 })
    expect(out.length).toBe(3 * 2 * 4)
    for (let y = 0; y < 2; y++) for (let x = 0; x < 3; x++) expect(out[(y * 3 + x) * 4]).toBe((3 + y) * atlas + 2 + x)
  })
  it('a rectangle touching the right edge and one on the first row work too', () => {
    expect(packRect(page, atlas, { page: 0, x: 5, y: 7, w: 3, h: 1 })[0]).toBe(7 * atlas + 5)
    expect(packRect(page, atlas, { page: 0, x: 0, y: 0, w: 2, h: 2 })[(1 * 2 + 1) * 4]).toBe(atlas + 1)
  })
  it('full-width rectangles are a contiguous slice of the page', () => {
    const out = packRect(page, atlas, { page: 0, x: 0, y: 2, w: atlas, h: 3 })
    expect(out.length).toBe(atlas * 3 * 4)
    expect(out[0]).toBe(2 * atlas)
  })

  it('keepPages: a new mesh over the same atlas keeps pixels, change log and serial, and only the mesh serial moves', () => {
    const s = new LightmapStore()
    s.applyMesh(mesh(1))
    s.applyTiles({ type: 'lightmap_tiles', rev: 1, atlas: 8, tiles: [tile(0, 1, 1, 1, 1, [10, 20, 30, 1])] })
    const serial = s.serial, meshSerial = s.meshSerial
    const cursor = s.dirtySince(0).cursor
    s.applyMesh(mesh(2, 3, 8, 1, true))
    expect(s.serial).toBe(serial)               // the renderer keeps its texture
    expect(s.meshSerial).toBe(meshSerial + 1)   // but takes the new vertices
    expect(s.rev).toBe(2)
    expect(s.triangleCount).toBe(3)
    expect(Array.from(s.pages[0].slice((1 * 8 + 1) * 4, (1 * 8 + 1) * 4 + 4)).map(halfToFloat)).toEqual([10, 20, 30, 1])
    // tiles of the old revision no longer apply, tiles of the new one do and show up as dirty for a renderer that was up to date
    expect(s.applyTiles({ type: 'lightmap_tiles', rev: 1, atlas: 8, tiles: [tile(0, 2, 2, 1, 1, [1, 1, 1, 1])] })).toBe(0)
    expect(s.applyTiles({ type: 'lightmap_tiles', rev: 2, atlas: 8, tiles: [tile(0, 2, 2, 1, 1, [1, 1, 1, 1])] })).toBe(1)
    const d = s.dirtySince(cursor)
    expect(d.full).toBe(false)
    expect(d.rects).toEqual([{ page: 0, x: 2, y: 2, w: 1, h: 1, style: 0 }])
  })

  it('keepPages keeps the style layers too, and a mesh that arrives over kept pages replaces the faces and drops the stale analysis', () => {
    const s = new LightmapStore()
    s.applyMesh(mesh(1))
    s.applyTiles({ type: 'lightmap_tiles', rev: 1, atlas: 8, style: 5, tiles: [tile(0, 1, 1, 1, 1, [7, 7, 7, 1])] })
    expect(s.styles).toEqual([0, 5])
    const serial = s.serial
    const face = (b: string) => ({ i: '', e: 'w', bi: '', b, f: 0, t: 't', n: 1 })
    s.applyMesh({ ...mesh(2, 1, 8, 1, true), faces: [face('a')] })
    expect(s.serial).toBe(serial)                 // the texture array keeps its layers
    expect(s.styles).toEqual([0, 5])
    expect(Array.from(s.pagesOf(5)![0].slice((1 * 8 + 1) * 4, (1 * 8 + 1) * 4 + 4)).map(halfToFloat)).toEqual([7, 7, 7, 1])
    expect(s.faces.map((f) => f.b)).toEqual(['a'])
    s.applyMesh({ ...mesh(3, 1, 8, 1, true), faces: [face('b')] })
    expect(s.faces.map((f) => f.b)).toEqual(['b'])  // the face table of the new mesh, not of the kept pages
    expect(s.litEntities.has('|w')).toBe(true)
    s.applyMesh(mesh(4, 1, 8, 1, false))           // the Core says start over: layers go too
    expect(s.styles).toEqual([0])
  })

  it('keepPages is only honoured when the atlas has the same shape and pages exist', () => {
    const s = new LightmapStore()
    s.applyMesh(mesh(1, 1, 8, 1, true))         // nothing to keep yet
    expect(s.serial).toBe(1)
    s.applyTiles({ type: 'lightmap_tiles', rev: 1, atlas: 8, tiles: [tile(0, 0, 0, 1, 1, [9, 9, 9, 1])] })
    s.applyMesh(mesh(2, 1, 16, 1, true))        // other atlas size
    expect(s.serial).toBe(2)
    expect(s.atlas).toBe(16)
    expect(s.pages[0].length).toBe(16 * 16 * 4)
    expect(s.pages[0][0]).toBe(0)
    s.applyMesh(mesh(3, 1, 16, 2, true))        // other page count
    expect(s.serial).toBe(3)
    expect(s.pages.length).toBe(2)
    s.applyMesh(mesh(4, 1, 16, 2, false))       // the Core says we have to start over
    expect(s.serial).toBe(4)
  })

  it('parses keepPages from the wire (absent means false)', () => {
    const base = { type: 'lit_mesh', rev: 1, atlas: 8, pageCount: 1, triangleCount: 0, vertices: '' }
    expect((parseInbound(JSON.stringify(base)) as any).keepPages).toBe(false)
    expect((parseInbound(JSON.stringify({ ...base, keepPages: true })) as any).keepPages).toBe(true)
  })
})

describe('HDR: lightmap values reach the page exactly as baked', () => {
  it('keeps brightness above 1 and tiny values (no clamping, no 8 bit rounding, no exposure applied)', () => {
    const s = new LightmapStore()
    s.applyMesh(mesh(1))
    s.applyTiles({ type: 'lightmap_tiles', rev: 1, atlas: 8, tiles: [tile(0, 0, 0, 3, 1, [3.5, 12.25, 0.0003, 1, 40, 0, 0.5, 1, 1, 2000, 0.01, 0.2])] })
    const px = (x: number) => Array.from(s.pages[0].slice(x * 4, x * 4 + 4)).map(halfToFloat)
    expect(px(0)[0]).toBeCloseTo(3.5, 2)
    expect(px(0)[1]).toBeCloseTo(12.25, 2)
    expect(px(0)[2]).toBeGreaterThan(0.0002)  // an 8 bit sRGB page would have flattened this to ~0
    expect(px(0)[2]).toBeLessThan(0.0004)
    expect(px(1)[0]).toBeCloseTo(40, 1)
    expect(px(2)[0]).toBeCloseTo(1, 3)
    expect(px(2)[1]).toBeCloseTo(2000, -1)
  })
  it('exposure is the level display multiplier from the status (1 when the Core did not say)', () => {
    const s = new LightmapStore()
    expect(s.exposure).toBe(1)
    s.applyStatus({ type: 'lighting_status', enabled: true, rev: 1, chunks: 1, stale: 0, minPasses: 1, maxPasses: 4, converged: false, texels: 1, pages: 1, exposure: 2.5 })
    expect(s.exposure).toBe(2.5)
    const p = parseInbound(JSON.stringify({ type: 'lighting_status', enabled: true, rev: 1, chunks: 1, stale: 0, minPasses: 1, maxPasses: 4, converged: false, texels: 1, pages: 1 })) as any
    expect(p.exposure).toBe(1)
    const q = parseInbound(JSON.stringify({ type: 'lighting_status', enabled: true, rev: 1, chunks: 1, stale: 0, minPasses: 1, maxPasses: 4, converged: false, texels: 1, pages: 1, exposure: 0.25 })) as any
    expect(q.exposure).toBe(0.25)
  })
  it('half float helpers agree with the Core on the values that matter', () => {
    expect(floatToHalf(1)).toBe(0x3c00)
    expect(floatToHalf(-3)).toBe(0)
    expect(floatToHalf(1e9)).toBe(0x7bff)
    expect(halfToFloat(0x7bff)).toBe(65504)
    for (const v of [0.01, 0.5, 1.5, 7.25, 100, 1000]) expect(Math.abs(halfToFloat(floatToHalf(v)) - v)).toBeLessThanOrEqual(v * 0.0006)
  })
})

describe('lightmap store: the direct layer', () => {
  const status = (directLayer: boolean) => ({ type: 'lighting_status' as const, enabled: true, rev: 1, chunks: 1, stale: 0, minPasses: 1, maxPasses: 1, converged: true, texels: 1, pages: 1, directLayer })
  it('keeps the direct layer of a tile (alpha = the ambient occlusion factor, as sent) apart from the lightmap and logs it', () => {
    const s = new LightmapStore()
    s.applyMesh(mesh(1))
    const ao = floatToHalf(0.5)
    const t = { ...tile(0, 1, 1, 1, 1, [2, 3, 4, 1]), d: b64(new Uint8Array(new Uint16Array([floatToHalf(0.25), floatToHalf(0.5), 0xb000, ao]).buffer)) }
    expect(s.applyTiles({ type: 'lightmap_tiles', rev: 1, atlas: 8, tiles: [t] })).toBe(1)
    const at = (pg: Uint16Array) => Array.from(pg.slice((1 * 8 + 1) * 4, (1 * 8 + 1) * 4 + 4))
    expect(at(s.pages[0]).map(halfToFloat)).toEqual([2, 3, 4, 1])
    expect(at(s.directPages[0]).map(halfToFloat)).toEqual([0.25, 0.5, -0.125, 0.5])
    const rects = s.dirtySince(0).rects
    expect(rects.filter((r) => r.direct).length).toBe(1)
    expect(rects.filter((r) => !r.direct).length).toBe(1)
  })
  it('offers the layer only when the Core says so and every page has one', () => {
    const s = new LightmapStore()
    s.applyMesh(mesh(1, 1, 8, 2))
    expect(s.directLayer).toBe(false)
    s.applyStatus(status(true))
    expect(s.directLayer).toBe(true)
    s.applyStatus(status(false))
    expect(s.directLayer).toBe(false)
  })
  it('parses the new tile field and status fields, and drops a tile with a bad direct layer', () => {
    const m = parseInbound(JSON.stringify({ type: 'lightmap_tiles', rev: 1, atlas: 8, tiles: [{ page: 0, x: 0, y: 0, w: 1, h: 1, rgba: 'AAAAAAAAAAA=', d: 'AAAAAAAAAAA=' }, { page: 0, x: 1, y: 0, w: 1, h: 1, rgba: 'AA', d: 5 }] }))
    expect(m?.type === 'lightmap_tiles' && m.tiles.length).toBe(1)
    const st = parseInbound(JSON.stringify({ type: 'lighting_status', enabled: true, rev: 1, chunks: 1, stale: 0, minPasses: 1, maxPasses: 1, converged: true, texels: 1, pages: 1, directLayer: true, defaultSourceSize: 0.25, defaultSunSize: 0.5 }))
    expect(st?.type === 'lighting_status' && [st.directLayer, st.defaultSourceSize, st.defaultSunSize]).toEqual([true, 0.25, 0.5])
  })
})
