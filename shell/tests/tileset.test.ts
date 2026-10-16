// Simple texture sets (REQUIREMENTS §20 simple sets): wall kits (lanes of pieces) and 9-slice frames.
import { describe, expect, it } from 'vitest'
import { solveLayout } from '../src/model/layout'
import { parseGroup, groupTextures } from '../src/model/texgroup'
import { Kit, WallKit, emptyWallKit, kitTile, kitToJson, newPiece, parseKit, solveKit, suggestWallKit, emptyFrameKit } from '../src/model/tileset'

const solve = (k: Kit, w: number, h: number, seed = 1, info?: Parameters<typeof solveKit>[1]['info']) => solveKit({ ...k, tile: k.tile ?? [1, 1] } as Kit, { w, h, seed, info })
const area = (cells: { x0: number; y0: number; x1: number; y1: number }[]) => cells.reduce((s, c) => s + (c.x1 - c.x0) * (c.y1 - c.y0), 0)
const at = (cells: ReturnType<typeof solve>['cells'], x: number, y: number) => cells.filter((c) => c.x0 <= x && x < c.x1 && c.y0 <= y && y < c.y1)
const kit = (j: object): WallKit => { const w: string[] = []; const k = parseKit(j, (m) => w.push(m)); if (!k || k.kind !== 'wall') throw new Error(w.join()); return k }

describe('reading and writing', () => {
  it('compact forms: a texture, a block of rows, weights and places', () => {
    const k = kit({ kind: 'wall', middle: { pieces: ['a/wall', { tex: 'a/win', weight: 0.3 }, { cells: [['a/l', 'a/r']], place: 'once' }] } })
    expect(k.middle.pieces).toHaveLength(3); expect(k.middle.pieces[2].cells).toEqual([['a/l', 'a/r']]); expect(k.middle.pieces[2].place).toBe('once'); expect(k.middle.pieces[1].weight).toBe(0.3)
  })
  it('round trips and leaves defaults out', () => {
    const k = kit({ kind: 'wall', tile: [3, 3], bottom: { pieces: [{ tex: 'a/d', place: 'once' }, { tex: 'a/w', weight: 3 }] }, middle: { rows: 2, pieces: [{ cells: [['a/u'], ['a/l']] }] } })
    expect(kitToJson(k)).toEqual({ kind: 'wall', tile: [3, 3], top: { pieces: [] }, middle: { rows: 2, pieces: [{ cells: [['a/u'], ['a/l']] }] }, bottom: { pieces: [{ tex: 'a/d', place: 'once' }, { tex: 'a/w', weight: 3 }] } })
    expect(parseKit(kitToJson(k), () => {})).toEqual(k)
  })
  it('bad parts are dropped with a warning; a lane keeps one height', () => {
    const w: string[] = []
    const k = parseKit({ kind: 'wall', middle: { rows: 1, pieces: [{ tex: '../x' }, { cells: [['a'], ['b']] }, { cells: [['a', 'b'], ['c']] }, 'ok/tex'] } }, (m) => w.push(m)) as WallKit
    // an unusable path keeps the piece as 'not chosen yet' (the editor shows an empty cell); odd shapes are dropped
    expect(k.middle.pieces.map((p) => p.cells)).toEqual([[[null]], [['ok/tex']]]); expect(w.join()).toMatch(/not a valid texture path/); expect(w.length).toBeGreaterThan(2)
    expect(parseKit({ kind: 'nope' }, () => {})).toBeNull(); expect(parseKit(5, () => {})).toBeNull()
  })
  it('a kit is a group: textures listed, solved through solveLayout', () => {
    const r = parseGroup({ format: 1, name: 'K', kit: { kind: 'wall', tile: [1, 1], middle: { pieces: ['a/w', 'a/v'] } } })
    expect(r.errors).toEqual([]); expect(groupTextures(r.group!).sort()).toEqual(['a/v', 'a/w'])
    const s = solveLayout({ group: r.group!, w: 3, h: 2, seed: 1 })
    expect(area(s.cells)).toBeCloseTo(6, 6); expect(s.truncated).toBe(false)
    expect(parseGroup({ format: 1, kit: { kind: 'wall' } }).group).not.toBeNull() // an empty set is still a group (the editor starts from it)
  })
})

describe('first guess from file names (the building blocks)', () => {
  const names = ['brick_wall_pt_5', 'brick_wall_pt_5_door', 'brick_wall_pt_5_window', 'brick_wall_pt_8', 'building_pt_1', 'building_pt_1_bottom_wall_1', 'building_pt_1_bottom_window_1', 'building_pt_1_bottom_window_2', 'building_pt_1_top_wall_1', 'building_pt_1_top_window_1', 'building_pt_1_top_window_2'].map((n) => `bb/${n}`)
  const k = suggestWallKit(names)
  const lane = (l: 'top' | 'middle' | 'bottom') => k[l].pieces.map((p) => p.cells[0][0]!.replace('bb/', ''))
  it('bottom_* go to the ground lane, doors there too and placed once, the rest fill the middle', () => {
    expect(lane('bottom')).toContain('building_pt_1_bottom_wall_1'); expect(lane('bottom')).toContain('brick_wall_pt_5_door')
    expect(k.bottom.pieces.find((p) => p.cells[0][0]!.endsWith('_door'))!.place).toBe('once')
    expect(lane('middle')).toEqual(expect.arrayContaining(['brick_wall_pt_5', 'brick_wall_pt_5_window', 'building_pt_1_top_wall_1', 'building_pt_1_top_window_2']))
    expect(lane('top')).toEqual([])
  })
  it('plain walls are Often, windows Sometimes; roofs go to the top lane', () => {
    expect(k.middle.pieces.find((p) => p.cells[0][0]!.endsWith('pt_5'))!.weight).toBe(3); expect(k.middle.pieces.find((p) => p.cells[0][0]!.endsWith('pt_5_window'))!.weight).toBe(1)
    expect(suggestWallKit(['x/roof_edge', 'x/brick']).top.pieces).toHaveLength(1)
  })
  it('a ground lane with only a door gets plain walls to fill with', () => {
    const s = suggestWallKit(['x/wall', 'x/door']); expect(s.bottom.pieces.map((p) => p.place)).toContain('any')
    const out = solve(s, 6, 3); expect(out.warnings).toEqual([])
  })
})

describe('wall kit: lanes, rows and columns', () => {
  const k = kit({ kind: 'wall', tile: [1, 1], bottom: { pieces: [{ tex: 'a/door', place: 'once' }, { tex: 'a/base', weight: 3 }] }, middle: { pieces: [{ tex: 'a/wall', weight: 3 }, { tex: 'a/win' }] }, top: { pieces: [{ tex: 'a/roof' }] } })
  it('bottom row at the bottom, rooftop row on top of everything, middle repeats between; exact cover', () => {
    const r = solve(k, 6, 5)
    expect(area(r.cells)).toBeCloseTo(30, 6)
    for (const c of at(r.cells, 2.5, 0.5)) expect(['a/door', 'a/base']).toContain(c.texture)
    expect(at(r.cells, 3.5, 4.5).map((c) => c.texture)).toEqual(['a/roof'])
    for (const y of [1.5, 2.5, 3.5]) for (const x of [0.5, 2.5, 5.5]) expect(['a/wall', 'a/win']).toContain(at(r.cells, x, y)[0].texture)
    expect(r.warnings).toEqual([])
  })
  it('a door is placed exactly once per ground row; a taller wall gets more middle rows', () => {
    for (let seed = 1; seed <= 20; seed++) expect(solve(k, 8, 5, seed).cells.filter((c) => c.texture === 'a/door')).toHaveLength(1)
    const rows = (h: number) => new Set(solve(k, 3, h).cells.map((c) => c.y0.toFixed(3))).size
    expect(rows(5)).toBe(5); expect(rows(9)).toBe(9)
  })
  it('short walls drop the middle lane, never overlap, and still cover the face', () => {
    const r = solve(k, 4, 1.2); expect(area(r.cells)).toBeCloseTo(4.8, 6)
    const tiny = solve(k, 0.4, 0.4); expect(area(tiny.cells)).toBeCloseTo(0.16, 6)
  })
  it('tile size: the natural size of the first texture when the set has none', () => {
    const kk = { ...k, tile: null } as Kit
    expect(kitTile(kk, () => ({ path: 'a/door', width: 204, height: 204, ppm: 64, props: undefined as never }))).toEqual([3.1875, 3.1875])
    expect(kitTile(kk)).toEqual([1, 1]); expect(kitTile(k)).toEqual([1, 1])
  })
  it('is deterministic for a seed and varies with it', () => {
    const wide = (seed: number) => JSON.stringify(solve(k, 30, 3, seed).cells.map((c) => c.texture))
    expect(wide(4)).toBe(wide(4))
    expect(new Set([1, 2, 3, 4, 5, 6, 7, 8].map(wide)).size).toBeGreaterThan(5) // different seeds give different walls
  })
})

describe('wall kit: pieces that line up', () => {
  it('horizontal blocks stay together, left to right', () => {
    const k = kit({ kind: 'wall', tile: [1, 1], middle: { pieces: [{ cells: [['a/L', 'a/R']], weight: 2 }, 'a/plain'] } })
    for (let seed = 1; seed <= 10; seed++) {
      const r = solve(k, 11, 1, seed)
      for (const c of r.cells) if (c.texture === 'a/L') { const next = at(r.cells, c.x1 + 0.01, 0.5)[0]; expect(next?.texture).toBe('a/R') }
      for (const c of r.cells) if (c.texture === 'a/R') { const prev = at(r.cells, c.x0 - 0.01, 0.5)[0]; expect(prev?.texture).toBe('a/L') }
    }
  })
  it('vertical blocks stay stacked: the first row of the piece is the upper one', () => {
    const k = kit({ kind: 'wall', tile: [1, 1], middle: { rows: 2, pieces: [{ cells: [['a/upper'], ['a/lower']] }] } })
    const r = solve(k, 3, 4)
    expect(area(r.cells)).toBeCloseTo(12, 6)
    for (const c of r.cells) if (c.texture === 'a/upper') { const below = at(r.cells, (c.x0 + c.x1) / 2, c.y0 - 0.01)[0]; expect(below.texture).toBe('a/lower'); expect(below.x0).toBeCloseTo(c.x0, 9) }
  })
  it('left / right / middle places and a gap no piece fits (shown with the fallback and said)', () => {
    const k = kit({ kind: 'wall', tile: [1, 1], middle: { pieces: [{ tex: 'a/L', place: 'left' }, { tex: 'a/R', place: 'right' }, { tex: 'a/M', place: 'middle' }, 'a/fill'] } })
    const r = solve(k, 7, 1)
    expect(at(r.cells, 0.5, 0.5)[0].texture).toBe('a/L'); expect(at(r.cells, 6.5, 0.5)[0].texture).toBe('a/R'); expect(at(r.cells, 3.5, 0.5)[0].texture).toBe('a/M')
    const wide = kit({ kind: 'wall', tile: [1, 1], middle: { pieces: [{ cells: [['a/x', 'a/y']] }] } })
    const g = solve(wide, 5, 1); expect(g.warnings.join()).toMatch(/narrow enough/); expect(area(g.cells)).toBeCloseTo(5, 6); expect(g.cells.some((c) => c.path === 'gap')).toBe(true)
  })
  it('never puts the same piece twice in a row when there is a choice (Avoid repeats)', () => {
    const k = kit({ kind: 'wall', tile: [1, 1], middle: { pieces: ['a/a', 'a/b', 'a/c'] } })
    for (let seed = 1; seed <= 10; seed++) { const t = solve(k, 30, 1, seed).cells.map((c) => c.texture); for (let i = 1; i < t.length; i++) expect(t[i]).not.toBe(t[i - 1]) }
    const off = solve({ ...k, avoidRepeat: false }, 60, 1, 3).cells.map((c) => c.texture); expect(off.some((t, i) => i && t === off[i - 1])).toBe(true)
  })
  it('pieces missing a texture are not used; an empty set says so', () => {
    const k = emptyWallKit(); k.middle.pieces.push(newPiece(null))
    const r = solve(k, 2, 2); expect(r.warnings.join()).toMatch(/no usable piece/)
  })
})

describe('frame (9-slice) with repeating edges and middle', () => {
  const f = (() => { const k = emptyFrameKit(); (['tl', 't', 'tr', 'l', 'c', 'r', 'bl', 'b', 'br'] as const).forEach((s) => { k.cells[s] = `f/${s}` }); k.tile = [1, 1]; return k })()
  it('corners keep their size, edges repeat along their side, the middle repeats both ways, exact cover', () => {
    const r = solve(f, 5, 4)
    expect(area(r.cells)).toBeCloseTo(20, 6)
    expect(at(r.cells, 0.5, 0.5)[0].texture).toBe('f/bl'); expect(at(r.cells, 4.5, 3.5)[0].texture).toBe('f/tr'); expect(at(r.cells, 0.5, 3.5)[0].texture).toBe('f/tl'); expect(at(r.cells, 4.5, 0.5)[0].texture).toBe('f/br')
    expect(r.cells.filter((c) => c.texture === 'f/t')).toHaveLength(3); expect(r.cells.filter((c) => c.texture === 'f/l')).toHaveLength(2); expect(r.cells.filter((c) => c.texture === 'f/c')).toHaveLength(6)
    for (const c of r.cells.filter((x) => x.texture === 'f/bl')) { expect(c.x1 - c.x0).toBeCloseTo(1, 9); expect(c.y1 - c.y0).toBeCloseTo(1, 9) }
  })
  it('edges are whole copies: a 5.4 m side has 4 edge tiles, each a little wider than a tile', () => {
    const r = solve(f, 6.4, 3), tops = r.cells.filter((c) => c.texture === 'f/t')
    expect(tops).toHaveLength(4); expect(tops[0].x1 - tops[0].x0).toBeCloseTo(4.4 / 4, 9)
  })
  it('a face smaller than two corners shrinks them, with no edges or middle; a missing slot is filled and said', () => {
    const r = solve(f, 1.2, 1.2); expect(area(r.cells)).toBeCloseTo(1.44, 6); expect(r.cells).toHaveLength(4); expect(r.cells[0].x1).toBeCloseTo(0.6, 9)
    const k = { ...f, cells: { ...f.cells, c: null } } as Kit
    const m = solve(k, 4, 4); expect(m.warnings.join()).toMatch(/slot 'c'/); expect(area(m.cells)).toBeCloseTo(16, 6)
  })
})

import { addPiece, addTextures, duplicatePiece, fitPieceCols, movePiece, pieceCols, pieceRows, removePiece, setLaneRows, setPieceCell } from '../src/model/tileset'

describe('editing operations', () => {
  it('adding pieces takes the lane height; setting the lane height resizes every piece', () => {
    const k = emptyWallKit()
    const a = addPiece(k, 'middle', 'a/wall')!, b = addPiece(k, 'middle', 'a/win')!
    setLaneRows(k, 'middle', 2)
    expect(pieceRows(a)).toBe(2); expect(pieceRows(b)).toBe(2); expect(a.cells).toEqual([['a/wall'], ['a/wall']])
    const c = addPiece(k, 'middle', 'a/new')!; expect(pieceRows(c)).toBe(2)
    setCell(c)
    expect(c.cells).toEqual([['a/top'], ['a/new']])
    setLaneRows(k, 'middle', 1); expect(c.cells).toEqual([['a/top']]) // shrinking keeps the top rows
    function setCell(p: typeof c) { setPieceCell(p, 0, 0, 'a/top') }
  })
  it('widening a piece repeats its last column, narrowing keeps the left ones; bad cells are ignored', () => {
    const p = addPiece(emptyWallKit(), 'top', 'a/x')!
    fitPieceCols(p, 3); expect(p.cells).toEqual([['a/x', 'a/x', 'a/x']]); setPieceCell(p, 0, 2, 'a/y'); fitPieceCols(p, 2); expect(p.cells).toEqual([['a/x', 'a/x']]); expect(pieceCols(p)).toBe(2)
    setPieceCell(p, 5, 5, 'zz'); setPieceCell(p, 0, -1, 'zz'); expect(p.cells).toEqual([['a/x', 'a/x']])
  })
  it('moving between lanes: a piece takes the target lane height; an empty lane takes the piece height', () => {
    const k = emptyWallKit(); addPiece(k, 'middle', 'a/w'); setLaneRows(k, 'middle', 2)
    const t = addPiece(k, 'top', 'a/roof')!
    expect(movePiece(k, 'top', 0, 'middle')).toBe(1); expect(pieceRows(t)).toBe(2); expect(k.top.pieces).toHaveLength(0)
    expect(movePiece(k, 'middle', 0, 'bottom')).toBe(0); expect(k.bottom.rows).toBe(2) // empty lane takes the height
    expect(movePiece(k, 'middle', 9, 'top')).toBe(-1); expect(movePiece(k, 'bottom', 0, 'bottom')).toBe(0)
  })
  it('duplicate and delete', () => {
    const k = emptyWallKit(); const p = addPiece(k, 'middle', 'a/w', 'once', 3)!
    const d = duplicatePiece(k, 'middle', 0)!; d.cells[0][0] = 'a/other'
    expect(p.cells[0][0]).toBe('a/w'); expect(d.weight).toBe(3); expect(d.place).toBe('once'); expect(k.middle.pieces).toHaveLength(2)
    removePiece(k, 'middle', 0); expect(k.middle.pieces[0]).toBe(d)
  })
  it('add textures sorts them into lanes by name and fits them to existing lane heights', () => {
    const k = emptyWallKit(); addPiece(k, 'bottom', 'a/base'); setLaneRows(k, 'bottom', 2)
    expect(addTextures(k, ['a/wall_bottom', 'a/wall_window'])).toBeGreaterThanOrEqual(2)
    expect(k.bottom.pieces.every((p) => pieceRows(p) === 2)).toBe(true); expect(k.middle.pieces.map((p) => p.cells[0][0])).toContain('a/wall_window')
  })
})

describe('Match names: pieces that line up between rows', () => {
  const tagOf: Record<string, string | null> = { 'a/w1': 'window 1', 'a/w2': 'window 2', 'a/wall': null, 'a/roof': null }
  const stacked = kit({
    kind: 'wall', tile: [1, 1],
    bottom: { pieces: [{ tex: 'a/w1', match: 'window 1' }, { tex: 'a/w2', match: 'window 2' }, { tex: 'a/wall', weight: 3 }] },
    middle: { pieces: [{ tex: 'a/w1', match: 'window 1' }, { tex: 'a/w2', match: 'window 2' }, { tex: 'a/wall', weight: 3 }] },
    top: { pieces: [{ tex: 'a/roof' }] },
  })
  it('never puts a window of one style on a window of another; plain walls and windows still mix freely', () => {
    let windowOnPlain = 0, plainOnWindow = 0, sameStyle = 0
    for (let seed = 1; seed <= 60; seed++) {
      const r = solve(stacked, 16, 4, seed)
      expect(r.warnings).toEqual([])
      for (const c of r.cells) {
        const below = at(r.cells, (c.x0 + c.x1) / 2, c.y0 - 0.01)[0]
        if (!below) continue
        const a = tagOf[c.texture], b = tagOf[below.texture]
        if (a && b) { expect(a).toBe(b); sameStyle++ }
        if (a && !b) windowOnPlain++
        if (!a && b && c.texture === 'a/wall') plainOnWindow++
      }
    }
    expect(windowOnPlain).toBeGreaterThan(0); expect(plainOnWindow).toBeGreaterThan(0); expect(sameStyle).toBeGreaterThan(0)
  })
  it('without Match names the same set does mix styles (the rule is what prevents it)', () => {
    const plain = kit({ kind: 'wall', tile: [1, 1], bottom: { pieces: [{ tex: 'a/w1' }, { tex: 'a/w2' }] }, middle: { pieces: [{ tex: 'a/w1' }, { tex: 'a/w2' }] } })
    let mixed = 0
    for (let seed = 1; seed <= 30; seed++) { const r = solve(plain, 12, 2, seed); for (const c of r.cells) { const b = at(r.cells, (c.x0 + c.x1) / 2, c.y0 - 0.01)[0]; if (b && b.texture !== c.texture) mixed++ } }
    expect(mixed).toBeGreaterThan(0)
  })
  it('a block keeps its name over all its width; an impossible request is said, not looped', () => {
    const k = kit({ kind: 'wall', tile: [1, 1], bottom: { pieces: [{ tex: 'a/w1', match: 'window 1' }] }, middle: { pieces: [{ tex: 'a/w2', match: 'window 2' }] } })
    const r = solve(k, 4, 2); expect(area(r.cells)).toBeCloseTo(8, 6); expect(r.warnings.join()).toMatch(/Match name/)
  })
  it('is stored, copied and read back; a bad name is dropped with a warning; file names suggest it', () => {
    const k = kit({ kind: 'wall', middle: { pieces: [{ tex: 'a/w', match: 'window 2' }] } })
    expect(k.middle.pieces[0].match).toBe('window 2'); expect(kitToJson(k)).toMatchObject({ middle: { pieces: [{ tex: 'a/w', match: 'window 2' }] } })
    const w: string[] = []; const bad = parseKit({ kind: 'wall', middle: { pieces: [{ tex: 'a/w', match: '<<x>>' }] } }, (m) => w.push(m)) as WallKit
    expect(bad.middle.pieces[0].match).toBeNull(); expect(w.join()).toMatch(/'match' must be a short name/)
    const kk = emptyWallKit(); addPiece(kk, 'middle', 'a/x', 'any', 3)!.match = 'k'; expect(duplicatePiece(kk, 'middle', 0)!.match).toBe('k')
    const s = suggestWallKit(['bb/building_pt_1_bottom_window_2', 'bb/building_pt_1_top_window_2', 'bb/building_pt_1_top_window_1', 'bb/brick_wall_pt_5_window', 'bb/brick_wall_pt_5_door', 'bb/wall_door_3'])
    const m = (n: string) => [...s.bottom.pieces, ...s.middle.pieces].find((p) => p.cells[0][0]!.endsWith(n))!.match
    expect(m('bottom_window_2')).toBe('window 2'); expect(m('top_window_2')).toBe('window 2'); expect(m('top_window_1')).toBe('window 1'); expect(m('pt_5_window')).toBeNull(); expect(m('pt_5_door')).toBeNull(); expect(m('wall_door_3')).toBe('door 3')
  })
})

describe('cap on the number of tiles', () => {
  it('a tiny tile on a big face is refused fast with a message, for walls and frames', () => {
    const wall = kit({ kind: 'wall', tile: [0.1, 0.1], middle: { pieces: ['a/w'] } })
    const t0 = Date.now()
    const r = solve(wall, 500, 500); expect(r.truncated).toBe(true); expect(r.cells).toEqual([]); expect(r.warnings.join()).toMatch(/more than 2048 tiles/)
    const f = emptyFrameKit(); f.tile = [0.1, 0.1]; for (const s of ['tl', 't', 'tr', 'l', 'c', 'r', 'bl', 'b', 'br'] as const) f.cells[s] = 'a/x'
    expect(solve(f, 500, 500).truncated).toBe(true)
    expect(Date.now() - t0).toBeLessThan(500)
  })
  it('through solveLayout the face is then drawn plainly (truncated), and a normal wall is not affected', () => {
    const g = parseGroup({ format: 1, kit: { kind: 'wall', tile: [0.1, 0.1], middle: { pieces: ['a/w'] } } }).group!
    expect(solveLayout({ group: g, w: 500, h: 500, seed: 1 }).truncated).toBe(true)
    expect(solveLayout({ group: g, w: 4, h: 4, seed: 1 }).truncated).toBe(false)
  })
})

import { DEFAULT_MAX_STRETCH, axisTiles, bestCount, stretchOf } from '../src/model/tileset'

describe('limit on how much tiles may be stretched', () => {
  const three = (extra: object = {}) => kit({ kind: 'wall', tile: [3.2, 3.2], ...extra, bottom: { pieces: ['a/ground'] }, middle: { pieces: ['a/floor'] }, top: { pieces: ['a/roof'] } })
  const rowsOf = (r: ReturnType<typeof solve>) => [...new Set(r.cells.map((c) => (c.y1 - c.y0).toFixed(4)))]
  const tex = (r: ReturnType<typeof solve>) => [...new Set(r.cells.map((c) => c.texture))].sort()
  /** how much a cell scales its texture along height / width (1 = natural size of a 3.2 m tile) */
  const scaleV = (c: { y0: number; y1: number; cropV?: number }) => (c.y1 - c.y0) / (3.2 * (c.cropV ?? 1))
  const scaleU = (c: { x0: number; x1: number; cropU?: number }) => (c.x1 - c.x0) / (3.2 * (c.cropU ?? 1))

  it('bestCount and axisTiles: least scaling when within the limit, natural size with the last tile cut when not', () => {
    expect([bestCount(0.4), bestCount(1), bestCount(1.4), bestCount(1.42), bestCount(2.4), bestCount(2.5), bestCount(10.4)]).toEqual([1, 1, 1, 2, 2, 3, 10])
    expect(axisTiles(6.4, 3.2, 0.25)).toEqual({ segs: [{ a0: 0, a1: 3.2, crop: 1 }, { a0: 3.2, a1: 6.4, crop: 1 }], cropped: false })
    const e = axisTiles(7, 3.2, 0.25) // 2.19 tiles: scaled it would be 2 tiles of 3.5 (9 %): fine
    expect(e.cropped).toBe(false); expect(e.segs).toHaveLength(2)
    const c = axisTiles(4.8, 3.2, 0.25) // 1.5 tiles: scaling would be 33 % or 50 %: cut
    expect(c.cropped).toBe(true); expect(c.segs).toHaveLength(2); expect(c.segs[0]).toEqual({ a0: 0, a1: 3.2, crop: 1 }); expect(c.segs[1].a0).toBeCloseTo(3.2, 9); expect(c.segs[1].a1).toBeCloseTo(4.8, 9); expect(c.segs[1].crop).toBeCloseTo(0.5, 9)
    expect(axisTiles(4.8, 3.2, 0).segs).toHaveLength(2); expect(axisTiles(4.8, 3.2, 0).cropped).toBe(false) // no limit: scaled
    expect(axisTiles(6.45, 3.2, 0.25, true).segs).toHaveLength(2) // a 1.6 % leftover is absorbed, not a sliver
    expect(axisTiles(1, 3.2, 0.25).segs).toHaveLength(1); expect(axisTiles(1, 3.2, 0.25).segs[0].crop).toBeCloseTo(1 / 3.2, 9)
  })

  it('a two floor building: the tiles keep their size and nothing is squashed', () => {
    const r = solve(three(), 6.4, 6.4)
    expect(tex(r)).toEqual(['a/floor', 'a/ground']); expect(rowsOf(r)).toEqual(['3.2000']); expect(area(r.cells)).toBeCloseTo(6.4 * 6.4, 6)
    expect(r.warnings.join()).toMatch(/rooftop row is left out/)
    // a 5 m wall: no whole number of rows is within 25 %: the ground row keeps its size and the floor row above is cut off, nothing is squashed
    const five = solve(three(), 6.4, 5)
    expect(area(five.cells)).toBeCloseTo(32, 6); expect(five.cells.every((c) => Math.abs(scaleV(c) - 1) < 1e-9)).toBe(true)
    expect(five.cells.filter((c) => c.cropV).map((c) => +(c.cropV as number).toFixed(4))).toEqual([0.5625, 0.5625]) // 1.8 of 3.2 m
    expect(five.warnings.join()).toMatch(/cut off at 56 %/)
    // 'no limit' is the old behaviour: two rows scaled to 2.5 m (22 % squashed)
    const old = solve(three({ maxStretch: 0 }), 6.4, 5)
    expect(rowsOf(old)).toEqual(['2.5000']); expect(old.warnings).toEqual([])
  })

  it('whatever the height, no tile is scaled beyond the limit and the wall is always covered exactly', () => {
    for (let h = 0.4; h <= 40; h += 0.37) {
      const r = solve(three(), 6.4, h)
      expect(area(r.cells), `height ${h}`).toBeCloseTo(6.4 * h, 5)
      for (const c of r.cells) expect(stretchOf(scaleV(c)), `height ${h}`).toBeLessThanOrEqual(DEFAULT_MAX_STRETCH + 1e-9)
    }
    for (let w = 0.4; w <= 20; w += 0.41) {
      const r = solve(three(), w, 9.6)
      expect(area(r.cells), `width ${w}`).toBeCloseTo(w * 9.6, 5)
      for (const c of r.cells) expect(stretchOf(scaleU(c)), `width ${w}`).toBeLessThanOrEqual(DEFAULT_MAX_STRETCH + 1e-9)
    }
  })

  it('the rooftop stays when the wall is tall enough; another limit changes the choice; a very low wall shows part of a tile', () => {
    expect(tex(solve(three(), 6.4, 9.6))).toEqual(['a/floor', 'a/ground', 'a/roof'])
    expect(tex(solve(three(), 6.4, 8.8))).toContain('a/roof') // 8.8 / 9.6: 9 % scaling is fine
    expect(tex(solve(three({ maxStretch: 100 }), 6.4, 6.4))).toEqual(['a/floor', 'a/ground', 'a/roof']) // 50 % is inside 100 %
    expect(tex(solve(three({ maxStretch: 10 }), 6.4, 7))).not.toContain('a/roof')
    const tiny = solve(three(), 6.4, 1)
    expect(area(tiny.cells)).toBeCloseTo(6.4, 6); expect(tiny.cells.every((c) => Math.abs(scaleV(c) - 1) < 1e-9)).toBe(true); expect(tiny.warnings.join()).toMatch(/cut off/)
  })

  it('width: the last column is cut off at natural size when the wall is not a whole number of tiles within the limit', () => {
    const r = solve(three(), 4.8, 9.6)
    expect(new Set(r.cells.map((c) => c.x0.toFixed(3))).size).toBe(2); expect(r.cells.filter((c) => c.cropU).every((c) => Math.abs((c.cropU as number) - 0.5) < 1e-9)).toBe(true)
    expect(r.warnings.join()).toMatch(/last tile column is cut off at 50 %/)
    expect(solve(three(), 6.4, 9.6).warnings).toEqual([]); expect(solve(three({ maxStretch: 0 }), 4.8, 9.6).warnings).toEqual([])
    expect(solve(three(), 4.8, 9.6).cells.filter((c) => c.x0 > 3).every((c) => c.texture !== undefined)).toBe(true)
  })

  it('a block of two tiles is cut through, not shifted: the second tile is the one that is partial', () => {
    const k = kit({ kind: 'wall', tile: [2, 2], middle: { pieces: [{ cells: [['a/L', 'a/R']] }] } })
    const r = solve(k, 3, 2) // 1.5 tiles wide: [whole, half]
    expect(r.cells.find((c) => c.texture === 'a/L')).toMatchObject({ x0: 0, x1: 2 }); expect(r.cells.find((c) => c.texture === 'a/R')).toMatchObject({ x0: 2, x1: 3, cropU: 0.5 })
  })

  it('frames: edges and middle keep their size, the last tile is cut; no limit scales them as before', () => {
    const f = emptyFrameKit(); f.tile = [1, 1]; for (const s of ['tl', 't', 'tr', 'l', 'c', 'r', 'bl', 'b', 'br'] as const) f.cells[s] = `f/${s}`
    expect(solve(f, 5, 4).warnings).toEqual([])
    const r = solve(f, 3.5, 4) // inner 1.5 tiles
    expect(r.warnings.join()).toMatch(/cut off/); expect(area(r.cells)).toBeCloseTo(14, 6)
    expect(r.cells.filter((c) => c.texture === 'f/t').map((c) => [c.x0, c.x1, c.cropU ?? 1])).toEqual([[1, 2, 1], [2, 2.5, 0.5]])
    expect(solve({ ...f, maxStretch: 0 }, 3.5, 4).warnings).toEqual([])
  })

  it('is stored as a percentage; the default is not written; 0 means no limit; junk falls back to the default', () => {
    expect(kitToJson(three())).not.toHaveProperty('maxStretch')
    expect(kitToJson(three({ maxStretch: 40 }))).toMatchObject({ maxStretch: 40 }); expect(kitToJson(three({ maxStretch: 0 }))).toMatchObject({ maxStretch: 0 })
    expect(kit({ kind: 'wall', maxStretch: 0 }).maxStretch).toBe(0); expect(kit({ kind: 'wall', maxStretch: 40 }).maxStretch).toBeCloseTo(0.4)
    expect(kit({ kind: 'wall', maxStretch: -5 }).maxStretch).toBe(DEFAULT_MAX_STRETCH); expect(kit({ kind: 'wall', maxStretch: 'x' }).maxStretch).toBe(DEFAULT_MAX_STRETCH)
    expect(parseKit({ kind: 'frame', maxStretch: 15 }, () => {})).toMatchObject({ maxStretch: 0.15 })
  })
})
