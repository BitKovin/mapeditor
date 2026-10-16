import { describe, expect, it } from 'vitest'
import { styleAnimates, styleValue } from '../src/render/lightStyles'
import { LightmapStore, LIT_VERTEX_FLOATS } from '../src/render/lightmapStore'
import { floatToHalf, halfToFloat } from '../src/render/half'
import { parseInbound } from '../src/net/protocol'

const patterns: string[] = Array(64).fill('')
patterns[9] = 'aaaaaaaazzzzzzzz' // slow strobe
patterns[2] = 'abcdefghijklmnopqrstuvwxyzyxwvutsrqponmlkjihgfedcba'
patterns[4] = 'mamamamamama'
const b64 = (u: Uint8Array) => btoa(String.fromCharCode(...u))
const halves = (v: number[]) => b64(new Uint8Array(new Uint16Array(v.map(floatToHalf)).buffer))

describe('light style playback (the engine model: 10 Hz, a = 0, m = 1, z = 25/12)', () => {
  it('plays the pattern one letter per 0.1 s and wraps', () => {
    expect(styleValue(patterns, 9, 0)).toBe(0)
    expect(styleValue(patterns, 9, 0.79)).toBe(0)
    expect(styleValue(patterns, 9, 0.8)).toBeCloseTo(25 / 12)
    expect(styleValue(patterns, 9, 1.6)).toBe(0) // wraps after 16 frames
    expect(styleValue(patterns, 2, 1.2)).toBeCloseTo(1)
    expect(styleValue(patterns, 4, 0.05)).toBeCloseTo(1)
    expect(styleValue(patterns, 4, 0.15)).toBe(0)
  })
  it('style 0, styles without a pattern, unknown ids are constant 1; negative time stays inside the pattern', () => {
    expect(styleValue(patterns, 0, 12.3)).toBe(1)
    expect(styleValue(patterns, 40, 3.3)).toBe(1)
    expect(styleValue(patterns, 64, 3.3)).toBe(1)
    expect(styleValue(patterns, -1, 3.3)).toBe(1)
    expect([0, 25 / 12]).toContainEqual(styleValue(patterns, 9, -0.05))
    expect(styleValue(patterns, 9, 0, 20)).toBe(0) // the rate comes from the Core
    expect(styleValue(patterns, 9, 0.4, 20)).toBeCloseTo(25 / 12)
  })
  it('knows which styles animate (to keep redrawing only when needed)', () => {
    expect(styleAnimates(patterns, 9)).toBe(true)
    expect(styleAnimates(patterns, 0)).toBe(false)
    expect(styleAnimates(patterns, 40)).toBe(false)
    patterns[11] = 'mmmm'
    expect(styleAnimates(patterns, 11)).toBe(false) // constant pattern
  })
})

describe('protocol: light styles', () => {
  it('parses the pattern table and rejects bad ones', () => {
    const ok = parseInbound(JSON.stringify({ type: 'light_styles', patterns: ['m', 'abc'], hz: 10 })) as any
    expect(ok.patterns).toEqual(['m', 'abc'])
    expect(parseInbound(JSON.stringify({ type: 'light_styles', patterns: ['mAm'], hz: 10 }))).toBeNull()
    expect(parseInbound(JSON.stringify({ type: 'light_styles', patterns: Array(65).fill('m'), hz: 10 }))).toBeNull()
    expect((parseInbound(JSON.stringify({ type: 'light_styles', patterns: [], hz: -1 })) as any).hz).toBe(10)
  })
  it('tiles carry their layer (default the base); status lists the styles', () => {
    const t = parseInbound(JSON.stringify({ type: 'lightmap_tiles', rev: 1, atlas: 8, style: 5, tiles: [] })) as any
    expect(t.style).toBe(5)
    expect((parseInbound(JSON.stringify({ type: 'lightmap_tiles', rev: 1, atlas: 8, tiles: [] })) as any).style).toBe(0)
    expect((parseInbound(JSON.stringify({ type: 'lightmap_tiles', rev: 1, atlas: 8, style: 99, tiles: [] })) as any).style).toBe(0)
    const st = parseInbound(JSON.stringify({ type: 'lighting_status', enabled: true, rev: 1, chunks: 1, stale: 0, minPasses: 1, maxPasses: 4, converged: false, texels: 1, pages: 1, styles: [0, 5], styleWarnings: ['w'] })) as any
    expect(st.styles).toEqual([0, 5])
    expect(st.styleWarnings).toEqual(['w'])
  })
})

describe('lightmap store: style layers', () => {
  const mesh = { type: 'lit_mesh' as const, rev: 1, atlas: 8, pageCount: 2, triangleCount: 1, vertices: b64(new Uint8Array(new Float32Array(3 * LIT_VERTEX_FLOATS).buffer)), faces: [] }
  const tile = (page: number, style: number, v: number[]) => ({ type: 'lightmap_tiles' as const, rev: 1, atlas: 8, style, tiles: [{ page, x: 1, y: 1, w: 1, h: 1, rgba: halves(v) }] })

  it('a layer appears with its first tile, has the same pages, and is independent of the base', () => {
    const s = new LightmapStore()
    s.applyMesh(mesh)
    expect(s.styles).toEqual([0])
    s.applyTiles(tile(0, 0, [1, 1, 1, 1]))
    const serial = s.serial
    expect(s.applyTiles(tile(1, 5, [3, 0, 0, 1]))).toBe(1)
    expect(s.styles).toEqual([0, 5])
    expect(s.serial).toBeGreaterThan(serial) // the renderer grows its texture array
    expect(s.pagesOf(5)).toHaveLength(2)
    const at = (pages: Uint16Array[], page: number) => halfToFloat(pages[page][(1 * 8 + 1) * 4])
    expect(at(s.pagesOf(0)!, 0)).toBe(1)
    expect(at(s.pagesOf(5)!, 1)).toBe(3)
    expect(at(s.pagesOf(5)!, 0)).toBe(0)   // never written
    expect(at(s.pagesOf(0)!, 1)).toBe(0)   // the base was not touched by the layer
    expect(s.pagesOf(9)).toBeUndefined()
  })
  it('dirty rectangles name their layer; fullRects covers every layer; a new mesh drops the layers', () => {
    const s = new LightmapStore()
    s.applyMesh(mesh)
    const c0 = s.dirtySince(Number.MAX_SAFE_INTEGER).cursor
    s.applyTiles(tile(0, 5, [2, 2, 2, 1]))
    s.applyTiles(tile(0, 0, [1, 1, 1, 1]))
    const d = s.dirtySince(c0)
    expect(d.rects.map((r) => r.style)).toEqual([5, 0])
    expect(s.fullRects().map((r) => [r.style, r.page])).toEqual([[0, 0], [0, 1], [5, 0], [5, 1]])
    s.applyMesh({ ...mesh, rev: 2 })
    expect(s.styles).toEqual([0])
  })
  it('keeps the style table from the Core', () => {
    const s = new LightmapStore()
    s.applyStyles({ type: 'light_styles', patterns: ['m', 'ab'], hz: 12 })
    expect(s.patterns).toEqual(['m', 'ab'])
    expect(s.styleHz).toBe(12)
  })
})
