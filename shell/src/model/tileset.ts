// Simple texture sets (REQUIREMENTS §20 "simple sets"): the everyday way to texture a wall or a frame from a few ready-made pieces, without rules.
// A set is stored in the same group file as the advanced groups (`<assets>/texgroups/<name>.texgroup.json`), under the key "kit". It produces the same
// `Cell[]` as the advanced solver (model/layout.ts), so the face plan, relief, carved fragments, Lit view and bake need nothing new.
//
// Two kinds:
//   "wall"  a wall built from square tiles in three lanes: Bottom (ground row), Middle (repeats to fill the height), Top (rooftop / cap row, always above everything).
//           A lane holds PIECES. A piece is one tile or a block of tiles that always stay together (a door of two halves side by side, a window bay of two
//           storeys stacked): `cells` = rows top to bottom, each a list of texture paths. Pieces are placed left to right along the wall.
//           weight: how often (Rare 0.3 / Sometimes 1 / Often 3). place: any | left | right | middle | once (once = exactly one per row, at a random spot).
//   "frame" 9-slice from nine textures: corners keep their size, edges repeat along their side, the middle repeats in both directions (nothing is stretched
//           beyond the small scaling that makes a whole number of tiles fit exactly).
//
//   { "format": 1, "name": "Brick building", "kit": { "kind": "wall", "tile": [3.2, 3.2], "avoidRepeat": true,
//       "bottom": { "pieces": [ { "tex": "bb/brick_door", "place": "once" }, { "tex": "bb/brick_wall", "weight": 3 } ] },
//       "middle": { "pieces": [ { "cells": [["bb/win_a", "bb/win_b"]] }, { "tex": "bb/brick_wall", "weight": 3 } ] },
//       "top":    { "pieces": [ { "tex": "bb/cap" } ] } } }
//   { "format": 1, "kit": { "kind": "frame", "tile": [1, 1], "tl": "...", "t": "...", "tr": "...", "l": "...", "c": "...", "r": "...", "bl": "...", "b": "...", "br": "..." } }
//
// Tile size: `tile` in metres, or (absent) the natural size of the first texture (its pixels / pixels per meter). Every texture is fitted to one tile cell.
import type { Cell } from './layout'
import { rand01 } from './rng'
import { TexInfo, naturalSize, safeTexturePath } from './texprops'

export type Place = 'any' | 'left' | 'right' | 'middle' | 'once'
export const PLACES: Place[] = ['any', 'left', 'right', 'middle', 'once']
export type LaneId = 'top' | 'middle' | 'bottom'
export const LANES: LaneId[] = ['top', 'middle', 'bottom']
export const WEIGHTS: { label: string; value: number }[] = [{ label: 'Rare', value: 0.3 }, { label: 'Sometimes', value: 1 }, { label: 'Often', value: 3 }]
export const MAX_PIECE = 6, MAX_PIECES = 48

export interface KitPiece {
  /** rows top to bottom, each `cols` long; null = not chosen yet (the piece is not used) */
  cells: (string | null)[][]
  weight: number
  place: Place
  /** Match name: pieces with different names never sit directly on each other; a piece without a name sits on anything (and anything sits on it) */
  match: string | null
}
export interface KitLane { rows: number; pieces: KitPiece[] }
export interface WallKit { kind: 'wall'; tile: [number, number] | null; /** how far a tile may be scaled to make a whole number fit, as a fraction (0.25 = 25 %, 0 = no limit) */ maxStretch: number; avoidRepeat: boolean; top: KitLane; middle: KitLane; bottom: KitLane }
export const FRAME_SLOTS = ['tl', 't', 'tr', 'l', 'c', 'r', 'bl', 'b', 'br'] as const
export type FrameSlot = (typeof FRAME_SLOTS)[number]
export interface FrameKit { kind: 'frame'; tile: [number, number] | null; maxStretch: number; cells: Record<FrameSlot, string | null> }
export type Kit = WallKit | FrameKit

export const pieceCols = (p: KitPiece): number => p.cells[0]?.length ?? 0
export const pieceRows = (p: KitPiece): number => p.cells.length
const usable = (p: KitPiece): boolean => p.cells.length > 0 && p.cells.every((r) => r.length === p.cells[0].length && r.every((t) => t !== null))

export const emptyLane = (): KitLane => ({ rows: 1, pieces: [] })
export const newPiece = (tex: string | null = null, weight = 1, place: Place = 'any', match: string | null = null): KitPiece => ({ cells: [[tex]], weight, place, match })
export const DEFAULT_MAX_STRETCH = 0.25
export const emptyWallKit = (): WallKit => ({ kind: 'wall', tile: null, maxStretch: DEFAULT_MAX_STRETCH, avoidRepeat: true, top: emptyLane(), middle: emptyLane(), bottom: emptyLane() })
export const emptyFrameKit = (): FrameKit => ({ kind: 'frame', tile: null, maxStretch: DEFAULT_MAX_STRETCH, cells: { tl: null, t: null, tr: null, l: null, c: null, r: null, bl: null, b: null, br: null } })

/** Every texture a set uses (for loading and for cache signatures). */
export function kitTextures(k: Kit): string[] {
  const out = new Set<string>()
  if (k.kind === 'frame') for (const s of FRAME_SLOTS) { const t = k.cells[s]; if (t) out.add(t) }
  else for (const l of LANES) for (const p of k[l].pieces) for (const r of p.cells) for (const t of r) if (t) out.add(t)
  return [...out]
}

// ---- reading and writing ---------------------------------------------------------------------------------------------------------------------------
const fin = (v: unknown): v is number => typeof v === 'number' && Number.isFinite(v)
const isObj = (v: unknown): v is Record<string, unknown> => !!v && typeof v === 'object' && !Array.isArray(v)

function parsePiece(j: unknown, warn: (m: string) => void, at: string): KitPiece | null {
  if (typeof j === 'string') j = { tex: j }
  if (!isObj(j)) { warn(`${at}: a piece must be a texture or an object`); return null }
  let grid: unknown = j.cells
  if (grid === undefined && typeof j.tex === 'string') grid = [[j.tex]]
  if (grid === undefined && Array.isArray(j.tex)) grid = j.tex
  if (!Array.isArray(grid) || grid.length === 0 || grid.length > MAX_PIECE) { warn(`${at}: a piece needs "tex" or "cells" (at most ${MAX_PIECE} rows)`); return null }
  const rows: (string | null)[][] = []
  for (const row of grid) {
    const list = Array.isArray(row) ? row : [row]
    if (list.length === 0 || list.length > MAX_PIECE) { warn(`${at}: a row of a piece needs 1 to ${MAX_PIECE} textures`); return null }
    rows.push(list.map((t) => { const safe = t === null ? null : safeTexturePath(t); if (t !== null && !safe) warn(`${at}: '${String(t)}' is not a valid texture path (the piece is kept without it)`); return safe }))
  }
  if (rows.some((r) => r.length !== rows[0].length)) { warn(`${at}: all rows of a piece must have the same number of textures`); return null }
  const place = PLACES.includes(j.place as Place) ? (j.place as Place) : 'any'
  const match = typeof j.match === 'string' && /^[A-Za-z0-9 _\-]{1,32}$/.test(j.match.trim()) ? j.match.trim() : null
  if (j.match !== undefined && j.match !== null && !match) warn(`${at}: 'match' must be a short name (letters, digits, space, _ -)`)
  return { cells: rows, weight: fin(j.weight) && j.weight > 0 ? Math.min(j.weight, 1000) : 1, place, match }
}

function parseLane(j: unknown, warn: (m: string) => void, at: string): KitLane {
  const lane = emptyLane()
  if (!isObj(j)) return lane
  const list = Array.isArray(j.pieces) ? j.pieces.slice(0, MAX_PIECES) : []
  for (let i = 0; i < list.length; i++) { const p = parsePiece(list[i], warn, `${at}.pieces[${i}]`); if (p) lane.pieces.push(p) }
  const rows = lane.pieces.length ? Math.max(...lane.pieces.map(pieceRows)) : 1
  lane.rows = fin(j.rows) ? Math.max(1, Math.min(MAX_PIECE, Math.round(j.rows))) : rows
  // all pieces of a lane are as tall as the lane: a different height cannot be placed
  const before = lane.pieces.length
  lane.pieces = lane.pieces.filter((p) => pieceRows(p) === lane.rows)
  if (lane.pieces.length < before) warn(`${at}: pieces with another height than the lane (${lane.rows} row${lane.rows === 1 ? '' : 's'}) were left out`)
  return lane
}

/** `maxStretch` in the file is a percentage (25), 0 = no limit. */
const parseStretch = (j: unknown): number => (typeof j === 'number' && Number.isFinite(j) && j >= 0 ? Math.min(j, 500) / 100 : DEFAULT_MAX_STRETCH)
const parseTile = (j: unknown): [number, number] | null => (Array.isArray(j) && j.length === 2 && fin(j[0]) && fin(j[1]) && j[0] > 0.05 && j[1] > 0.05 ? [Math.min(j[0], 1000), Math.min(j[1], 1000)] : null)

/** Reads the "kit" of a group file. Never throws; unusable parts are dropped and reported through `warn`. */
export function parseKit(j: unknown, warn: (m: string) => void): Kit | null {
  if (!isObj(j)) { warn('kit: must be an object'); return null }
  if (j.kind === 'frame') {
    const k = emptyFrameKit()
    k.tile = parseTile(j.tile); k.maxStretch = parseStretch(j.maxStretch)
    for (const s of FRAME_SLOTS) { const v = j[s]; k.cells[s] = v === null || v === undefined ? null : safeTexturePath(v); if (v !== undefined && v !== null && !k.cells[s]) warn(`kit.${s}: not a valid texture path`) }
    return k
  }
  if (j.kind !== 'wall') { warn(`kit: unknown kind '${String(j.kind)}' (wall or frame)`); return null }
  return { kind: 'wall', tile: parseTile(j.tile), maxStretch: parseStretch(j.maxStretch), avoidRepeat: j.avoidRepeat !== false, top: parseLane(j.top, warn, 'kit.top'), middle: parseLane(j.middle, warn, 'kit.middle'), bottom: parseLane(j.bottom, warn, 'kit.bottom') }
}

/** The stored form (compact: a 1 x 1 piece is `{ "tex": ... }`, defaults are left out). */
export function kitToJson(k: Kit): Record<string, unknown> {
  const tile = { ...(k.tile ? { tile: k.tile } : {}), ...(Math.abs(k.maxStretch - DEFAULT_MAX_STRETCH) > 1e-9 ? { maxStretch: Math.round(k.maxStretch * 1000) / 10 } : {}) }
  if (k.kind === 'frame') { const o: Record<string, unknown> = { kind: 'frame', ...tile }; for (const s of FRAME_SLOTS) o[s] = k.cells[s]; return o }
  const lane = (l: KitLane): unknown => ({
    ...(l.rows > 1 ? { rows: l.rows } : {}),
    pieces: l.pieces.map((p) => ({
      ...(p.cells.length === 1 && p.cells[0].length === 1 ? { tex: p.cells[0][0] } : { cells: p.cells }),
      ...(p.weight !== 1 ? { weight: p.weight } : {}), ...(p.place !== 'any' ? { place: p.place } : {}), ...(p.match ? { match: p.match } : {}),
    })),
  })
  return { kind: 'wall', ...tile, ...(k.avoidRepeat ? {} : { avoidRepeat: false }), top: lane(k.top), middle: lane(k.middle), bottom: lane(k.bottom) }
}

// ---- first guess from file names ---------------------------------------------------------------------------------------------------------------------
const tokens = (path: string): string[] => (path.split('/').pop() ?? path).toLowerCase().split(/[^a-z0-9]+/).filter(Boolean)
const has = (t: string[], words: string[]): boolean => t.some((x) => words.includes(x))

/**
 * Sorts textures into lanes by what their names say: roof / cap / cornice -> Top; bottom / base / ground / plinth -> Bottom; top (upper floors) and everything
 * else -> Middle; a door goes to the Bottom lane and is placed once; windows are Sometimes, plain walls Often. It is only a first guess the author corrects.
 */
export function suggestWallKit(paths: string[]): WallKit {
  const k = emptyWallKit()
  for (const path of paths) {
    const t = tokens(path)
    const door = has(t, ['door', 'gate', 'entrance']), win = has(t, ['window', 'win'])
    const lane: LaneId = has(t, ['roof', 'rooftop', 'cap', 'cornice', 'parapet', 'crown', 'eave']) ? 'top' : has(t, ['bottom', 'base', 'ground', 'plinth', 'basement']) || door ? 'bottom' : 'middle'
    // 'window_2' / 'door_1': the number is the style; the same style lines up between rows (bottom_window_2 under top_window_2)
    const style = /(window|win|door)[_ \-]?(\d+)$/.exec((path.split('/').pop() ?? path).toLowerCase())
    k[lane].pieces.push(newPiece(path, door ? 1 : win ? 1 : 3, door ? 'once' : 'any', style ? `${style[1] === 'door' ? 'door' : 'window'} ${style[2]}` : null))
  }
  // a lane that only has doors / windows still needs something plain to fill with: the middle lane's plain walls also fill the bottom lane
  const plainOf = (l: KitLane): KitPiece[] => l.pieces.filter((p) => p.place === 'any' && p.weight === 3)
  if (k.bottom.pieces.length && !plainOf(k.bottom).length) for (const p of plainOf(k.middle)) k.bottom.pieces.push({ ...p, cells: p.cells.map((r) => [...r]) })
  return k
}

// ---- solving ----------------------------------------------------------------------------------------------------------------------------------------
export interface KitInput {
  w: number; h: number; seed: number
  info?: (path: string) => TexInfo | undefined
  fallbackTexture?: string
}
export interface KitResult { cells: Cell[]; warnings: string[]; /** more cells than the cap: `cells` is empty and the face is drawn plainly */ truncated?: boolean }
/** Same cap as the advanced solver (layout.ts MAX_LAYOUT_CELLS): a face needing more tiles is not laid out. */
export const MAX_KIT_CELLS = 2048

/** The tile size in metres: the set's own, else the natural size of the first texture, else 1 m. */
export function kitTile(k: Kit, info?: (path: string) => TexInfo | undefined): [number, number] {
  if (k.tile) return k.tile
  const first = kitTextures(k)[0]
  const ti = first ? info?.(first) : undefined
  return ti ? naturalSize(ti) : [1, 1]
}

/** How many whole tiles fit `q` tiles of room with the least scaling: the count n that minimises |ln(q / n)| (never below 1). */
export function bestCount(q: number): number {
  if (!(q > 1)) return 1
  const f = Math.floor(q)
  return q < Math.sqrt(f * (f + 1)) ? f : f + 1
}
/** How much a tile is scaled: 0.25 = 25 % bigger or smaller (a tile at 0.8 of its size counts as 25 %, like 1.25). */
export const stretchOf = (scale: number): number => Math.max(scale, 1 / scale) - 1

export interface Seg { a0: number; a1: number; /** fraction of the tile that is shown (1 = whole) */ crop: number }
/**
 * Divides `len` metres into tiles of `tile` metres. Scaled: the whole number of equal tiles with the least scaling, when that is within `limit` (a fraction; 0 = no
 * limit, always scaled). Otherwise (or when `natural`) the tiles keep their size and the LAST one is cut off. A leftover under 3 % of a tile is absorbed by scaling.
 */
export function axisTiles(len: number, tile: number, limit: number, natural = false): { segs: Seg[]; cropped: boolean } {
  const even = (n: number): Seg[] => Array.from({ length: n }, (_, k) => ({ a0: (len * k) / n, a1: (len * (k + 1)) / n, crop: 1 }))
  const q = len / tile, n = bestCount(q)
  if (!natural && (limit <= 0 || stretchOf(len / (n * tile)) <= limit + 1e-9)) return { segs: even(n), cropped: false }
  const full = Math.floor(q + 1e-9), rem = len - full * tile
  if (full >= 1 && rem <= 0.03 * tile) return { segs: even(full), cropped: false }
  const segs: Seg[] = Array.from({ length: full }, (_, k) => ({ a0: k * tile, a1: (k + 1) * tile, crop: 1 }))
  if (rem > 1e-9) segs.push({ a0: full * tile, a1: len, crop: rem / tile })
  return { segs, cropped: rem > 1e-9 }
}

const cell = (x0: number, y0: number, x1: number, y1: number, texture: string, path: string, cropU = 1, cropV = 1): Cell => ({ x0, y0, x1, y1, texture, slot: texture, fit: 'stretch', origin: 'cell', alignU: 'start', alignV: 'start', flipU: false, flipV: false, rotate: 0, path, ...(cropU < 1 - 1e-9 ? { cropU } : {}), ...(cropV < 1 - 1e-9 ? { cropV } : {}) })

interface Placement { piece: KitPiece; col: number }

/** One row of a lane: which piece goes at which column. Deterministic for a seed + key. */
function placeRow(pieces: KitPiece[], nC: number, seed: number, key: string, avoid: boolean, below: (string | null)[], warn: (m: string) => void): { placements: Placement[]; gaps: number[]; tags: (string | null)[] } {
  let n = 0
  const rnd = (): number => rand01(seed, `${key}#${n++}`)
  const pick = (list: KitPiece[]): KitPiece => { const total = list.reduce((s, p) => s + p.weight, 0); let t = rnd() * total; for (const p of list) { if (t < p.weight) return p; t -= p.weight } return list[list.length - 1] }
  const taken = new Array<boolean>(nC).fill(false)
  const out: Placement[] = []
  const gaps: number[] = []
  const free = (c: number, w: number): boolean => c >= 0 && c + w <= nC && taken.slice(c, c + w).every((x) => !x)
  /** Match names: a named piece may not sit on a piece with another name */
  const matches = (p: KitPiece, c: number): boolean => { if (!p.match) return true; for (let i = 0; i < pieceCols(p); i++) { const b = below[c + i]; if (b && b !== p.match) return false } return true }
  let relaxed = false
  const put = (piece: KitPiece, col: number): void => { for (let i = 0; i < pieceCols(piece); i++) taken[col + i] = true; out.push({ piece, col }) }

  // pieces with a fixed place go first: one of the left / right / middle candidates each, every "once" piece
  for (const place of ['left', 'right', 'middle'] as Place[]) {
    const at = (p: KitPiece): number => (place === 'left' ? 0 : place === 'right' ? nC - pieceCols(p) : Math.floor((nC - pieceCols(p)) / 2))
    let fit = pieces.filter((p) => p.place === place && pieceCols(p) <= nC)
    const ok = fit.filter((p) => matches(p, at(p)))
    if (fit.length && !ok.length) relaxed = true
    fit = ok.length ? ok : fit
    if (!fit.length) continue
    const p = pick(fit)
    if (free(at(p), pieceCols(p))) put(p, at(p))
  }
  for (const p of pieces.filter((x) => x.place === 'once')) {
    const w = pieceCols(p)
    const spots: number[] = []
    for (let c = 0; c + w <= nC; c++) if (free(c, w) && matches(p, c)) spots.push(c)
    if (!spots.length) { for (let c = 0; c + w <= nC; c++) if (free(c, w)) spots.push(c); if (spots.length) relaxed = true }
    if (spots.length) put(p, spots[Math.min(spots.length - 1, Math.floor(rnd() * spots.length))])
  }
  // then the free runs are filled left to right with the ordinary pieces
  const ordinary = pieces.filter((p) => p.place === 'any')
  let prev: KitPiece | null = null
  for (let c = 0; c < nC;) {
    if (taken[c]) { c++; prev = null; continue }
    let run = 0
    while (c + run < nC && !taken[c + run]) run++
    let candidates = ordinary.filter((p) => pieceCols(p) <= run)
    const matching = candidates.filter((p) => matches(p, c))
    if (candidates.length && !matching.length) relaxed = true
    if (matching.length) candidates = matching
    if (!candidates.length) candidates = pieces.filter((p) => p.place === 'any' || p.place === 'once').filter((p) => pieceCols(p) <= run && matches(p, c))
    if (!candidates.length) candidates = pieces.filter((p) => p.place === 'any' || p.place === 'once').filter((p) => pieceCols(p) <= run)
    if (!candidates.length) { warn(`no piece is narrow enough for a gap of ${run} tile${run === 1 ? '' : 's'}: add a one tile wide piece`); for (let i = 0; i < run; i++) { taken[c + i] = true; gaps.push(c + i) } c += run; prev = null; continue }
    if (avoid && prev && candidates.length > 1) { const others = candidates.filter((p) => p !== prev); if (others.length) candidates = others }
    const p = pick(candidates)
    put(p, c); c += pieceCols(p); prev = p
  }
  if (relaxed) warn(`${key}: no piece with a matching Match name fits somewhere: a piece was put on a different one`)
  const tags: (string | null)[] = new Array<string | null>(nC).fill(null)
  for (const { piece, col } of out) for (let i = 0; i < pieceCols(piece); i++) tags[col + i] = piece.match
  return { placements: out.sort((a, b) => a.col - b.col), gaps, tags }
}

export function solveKit(k: Kit, i: KitInput): KitResult {
  const warnings: string[] = []
  const warn = (m: string): void => { if (!warnings.includes(m) && warnings.length < 50) warnings.push(m) }
  const [tw, th] = kitTile(k, i.info)
  const cells: Cell[] = []
  if (!(i.w > 1e-6 && i.h > 1e-6)) return { cells, warnings }
  const fallback = i.fallbackTexture ?? kitTextures(k)[0] ?? 'dev/grey'
  const fill = (x0: number, y0: number, x1: number, y1: number, why: string): void => { warn(why); cells.push(cell(x0, y0, x1, y1, fallback, 'fallback')) }
  const lim = k.maxStretch, pct = (v: number): number => Math.round(v * 100)
  const tooMany = (n: number): KitResult | null => (n > MAX_KIT_CELLS ? (warn(`more than ${MAX_KIT_CELLS} tiles: the face is not laid out (use a bigger tile size)`), { cells: [], warnings, truncated: true }) : null)

  if (k.kind === 'frame') {
    const t = k.cells
    const cw = Math.min(tw, i.w / 2), ch = Math.min(th, i.h / 2)
    const iw = i.w - 2 * cw, ih = i.h - 2 * ch
    const xs = iw > 1e-6 ? axisTiles(iw, tw, lim) : { segs: [] as Seg[], cropped: false }, ys = ih > 1e-6 ? axisTiles(ih, th, lim) : { segs: [] as Seg[], cropped: false }
    const over = tooMany((xs.segs.length + 2) * (ys.segs.length + 2)); if (over) return over
    if (xs.cropped || ys.cropped) warn(`the last edge and middle tiles are cut off (tiles keep their size because stretching them more than ${pct(lim)} % is not allowed): the size ${fmtM(i.w)} x ${fmtM(i.h)} m is not a whole number of tiles`)
    const add = (s: FrameSlot, x0: number, y0: number, x1: number, y1: number, cu = 1, cv = 1): void => { const p = t[s]; if (p) cells.push(cell(x0, y0, x1, y1, p, s, cu, cv)); else fill(x0, y0, x1, y1, `frame slot '${s}' has no texture`) }
    add('bl', 0, 0, cw, ch); add('br', i.w - cw, 0, i.w, ch); add('tl', 0, i.h - ch, cw, i.h); add('tr', i.w - cw, i.h - ch, i.w, i.h)
    for (const x of xs.segs) { add('b', cw + x.a0, 0, cw + x.a1, ch, x.crop); add('t', cw + x.a0, i.h - ch, cw + x.a1, i.h, x.crop) }
    for (const y of ys.segs) { add('l', 0, ch + y.a0, cw, ch + y.a1, 1, y.crop); add('r', i.w - cw, ch + y.a0, i.w, ch + y.a1, 1, y.crop) }
    for (const x of xs.segs) for (const y of ys.segs) add('c', cw + x.a0, ch + y.a0, cw + x.a1, ch + y.a1, x.crop, y.crop)
    return { cells, warnings }
  }

  // ---- wall: columns, then the rows of each lane
  const lanes = Object.fromEntries(LANES.map((l) => [l, { rows: k[l].rows, pieces: k[l].pieces.filter(usable) }])) as Record<LaneId, KitLane>
  for (const l of LANES) if (k[l].pieces.length > lanes[l].pieces.length) warn(`${l}: a piece without all its textures is not used`)
  const hasB = lanes.bottom.pieces.length > 0, hasT = lanes.top.pieces.length > 0, hasM = lanes.middle.pieces.length > 0
  const unit = hasM ? lanes.middle.rows : 0
  if (!hasB && !hasM && !hasT) { fill(0, 0, i.w, i.h, 'the set has no usable piece yet'); return { cells, warnings } }

  interface Band { y0: number; y1: number; crop: number }
  const bandsOf = (y0: number, count: number, h: number): Band[] => Array.from({ length: count }, (_, r) => ({ y0: y0 + r * h, y1: y0 + (r + 1) * h, crop: 1 }))
  // units: one entry per placement of a lane (bottom and top once, middle once per repeat), each a list of row bands from the bottom
  const units: { lane: LaneId; key: string; bands: Band[] }[] = []

  interface Cfg { b: boolean; m: boolean; t: boolean; bRows: number; nM: number; tRows: number; R: number; stretch: number }
  const make = (b: boolean, m: boolean, t: boolean): Cfg | null => {
    const bRows = b ? lanes.bottom.rows : 0, tRows = t ? lanes.top.rows : 0
    let nM = 0
    if (m) { const free = i.h - (bRows + tRows) * th; nM = free <= 0 ? 1 : bestCount(free / (unit * th)) }
    const R = bRows + nM * unit + tRows
    return R > 0 ? { b, m, t, bRows, nM, tRows, R, stretch: stretchOf(i.h / (R * th)) } : null
  }
  const order: [boolean, boolean, boolean][] = [[true, true, true], [true, true, false], [false, true, true], [false, true, false], [true, false, true], [true, false, false], [false, false, true]]
  const usableCfgs = order.filter(([b, m, t]) => (!b || hasB) && (!m || hasM) && (!t || hasT) && (b || m || t))
  const left = (c: { b: boolean; m: boolean; t: boolean }): void => {
    const l = [hasT && !c.t ? 'rooftop' : '', hasB && !c.b ? 'ground' : '', hasM && !c.m ? 'middle' : ''].filter(Boolean)
    if (l.length) warn(`the ${l.join(' and ')} row${l.length > 1 ? 's are' : ' is'} left out: this wall is too ${i.h < th * 3 ? 'low' : 'short'} for it without stretching the tiles more than ${pct(lim)} %`)
  }
  const scaled = (c: Cfg): void => {
    const rowH = i.h / c.R
    if (c.b) units.push({ lane: 'bottom', key: 'bottom', bands: bandsOf(0, c.bRows, rowH) })
    for (let m = 0; m < c.nM; m++) units.push({ lane: 'middle', key: `middle${m}`, bands: bandsOf((c.bRows + m * unit) * rowH, unit, rowH) })
    if (c.t) units.push({ lane: 'top', key: 'top', bands: bandsOf((c.bRows + c.nM * unit) * rowH, c.tRows, rowH) })
  }

  if (lim <= 0) {
    // no limit: every row the set has, the middle only when it gets half a unit; all scaled to fit
    const bRows = hasB ? lanes.bottom.rows : 0, tRows = hasT ? lanes.top.rows : 0, free = i.h - (bRows + tRows) * th
    const nM = unit ? (free >= unit * th * 0.5 ? Math.max(1, Math.round(free / (unit * th))) : 0) : 0
    const R = bRows + nM * unit + tRows
    scaled({ b: hasB, m: nM > 0, t: hasT, bRows, nM, tRows, R, stretch: stretchOf(i.h / (R * th)) })
  } else {
    const cfgs = usableCfgs.map(([b, m, t]) => make(b, m, t)).filter((c): c is Cfg => !!c)
    const ok = cfgs.find((c) => c.stretch <= lim + 1e-9)
    if (ok) { scaled(ok); left(ok) }
    else if (hasM) {
      // no whole number of rows fits within the limit: the tiles keep their size and the last middle row is cut off (the rooftop row stays on top)
      const c = usableCfgs.find(([b, m, t]) => m && ((b ? lanes.bottom.rows : 0) + (t ? lanes.top.rows : 0)) * th <= i.h + 1e-9) ?? [false, true, false]
      const [b, , t] = c, bRows = b ? lanes.bottom.rows : 0, tRows = t ? lanes.top.rows : 0
      if (b) units.push({ lane: 'bottom', key: 'bottom', bands: bandsOf(0, bRows, th) })
      const base = bRows * th, free = i.h - (bRows + tRows) * th
      const mid = axisTiles(free, unit * th, lim, true)
      mid.segs.forEach((seg, m) => {
        const bands: Band[] = []
        for (let r = 0; r < unit; r++) { const y0 = base + seg.a0 + r * th, y1 = Math.min(base + seg.a1, y0 + th); if (y1 - y0 > 1e-9) bands.push({ y0, y1, crop: (y1 - y0) / th }) }
        units.push({ lane: 'middle', key: `middle${m}`, bands })
      })
      if (t) units.push({ lane: 'top', key: 'top', bands: bandsOf(i.h - tRows * th, tRows, th) })
      left({ b, m: true, t })
      if (mid.cropped) warn(`the top middle row is cut off at ${pct(Math.min(...mid.segs.slice(-1).map((s) => s.crop)))} %: tiles keep their size (stretching them more than ${pct(lim)} % is not allowed)`)
    } else {
      // no middle row to cut: the least scaled layout, and a warning
      const best = cfgs.reduce<Cfg | null>((a, c) => (!a || c.stretch < a.stretch - 1e-9 ? c : a), null)
      if (best) { scaled(best); left(best); warn(`the tiles are scaled by ${pct(best.stretch)} % in height (limit ${pct(lim)} %): add a Middle row to the set so a wall of this height can be cut instead of stretched`) }
    }
  }
  const colsOf = axisTiles(i.w, tw, lim)
  const cols = colsOf.segs
  if (colsOf.cropped) warn(`the last tile column is cut off at ${pct(cols[cols.length - 1].crop)} %: tiles keep their size (stretching them more than ${pct(lim)} % is not allowed)`)
  const over = tooMany(cols.length * units.reduce((n, u) => n + u.bands.length, 0)); if (over) return over

  let below: (string | null)[] = new Array<string | null>(cols.length).fill(null) // Match names of the row underneath, per column
  for (const u of units) {
    const lane = lanes[u.lane]
    const { placements, gaps, tags } = placeRow(lane.pieces, cols.length, i.seed, u.key, k.avoidRepeat, below, warn)
    below = tags
    for (const { piece, col } of placements) {
      piece.cells.forEach((row, r) => row.forEach((tex, c) => {
        const band = u.bands[piece.cells.length - 1 - r] // rows are stored top to bottom, bands bottom to top; a cut-off row has fewer bands
        const seg = cols[col + c]
        if (band && seg) cells.push(cell(seg.a0, band.y0, seg.a1, band.y1, tex as string, u.key, seg.crop, band.crop))
      }))
    }
    for (const c of gaps) for (const band of u.bands) cells.push(cell(cols[c].a0, band.y0, cols[c].a1, band.y1, fallback, 'gap', cols[c].crop, band.crop))
  }
  return { cells, warnings }
}

const fmtM = (v: number): string => String(Math.round(v * 100) / 100)

// ---- editing operations (the visual editor calls these; pure data changes, tested) -----------------------------------------------------------------
const cloneCells = (c: (string | null)[][]): (string | null)[][] => c.map((r) => [...r])

/** Makes a piece `rows` tall: a taller piece repeats its last row, a shorter one keeps its top rows. */
export function fitPieceRows(p: KitPiece, rows: number): void {
  rows = Math.max(1, Math.min(MAX_PIECE, Math.round(rows)))
  while (p.cells.length < rows) p.cells.push([...p.cells[p.cells.length - 1]])
  p.cells.length = rows
}
/** Makes a piece `cols` wide: a wider piece repeats its last column, a narrower one keeps its left columns. */
export function fitPieceCols(p: KitPiece, cols: number): void {
  cols = Math.max(1, Math.min(MAX_PIECE, Math.round(cols)))
  for (const r of p.cells) { while (r.length < cols) r.push(r[r.length - 1] ?? null); r.length = cols }
}
/** All pieces of a lane are as tall as the lane: setting the lane height resizes every piece. */
export function setLaneRows(k: WallKit, lane: LaneId, rows: number): void {
  const l = k[lane]
  l.rows = Math.max(1, Math.min(MAX_PIECE, Math.round(rows)))
  for (const p of l.pieces) fitPieceRows(p, l.rows)
}
export function addPiece(k: WallKit, lane: LaneId, tex: string | null, place: Place = 'any', weight = 1): KitPiece | null {
  if (k[lane].pieces.length >= MAX_PIECES) return null
  const p = newPiece(tex, weight, place)
  fitPieceRows(p, k[lane].rows)
  k[lane].pieces.push(p)
  return p
}
export function removePiece(k: WallKit, lane: LaneId, index: number): void { k[lane].pieces.splice(index, 1) }
export function duplicatePiece(k: WallKit, lane: LaneId, index: number): KitPiece | null {
  const src = k[lane].pieces[index]
  if (!src || k[lane].pieces.length >= MAX_PIECES) return null
  const copy: KitPiece = { cells: cloneCells(src.cells), weight: src.weight, place: src.place, match: src.match }
  k[lane].pieces.splice(index + 1, 0, copy)
  return copy
}
/** Moves a piece to another lane (it takes that lane's height; into an empty lane the lane takes the piece's height). Returns the piece's new index. */
export function movePiece(k: WallKit, from: LaneId, index: number, to: LaneId): number {
  const p = k[from].pieces[index]
  if (!p) return -1
  if (from === to) return index
  k[from].pieces.splice(index, 1)
  if (!k[to].pieces.length) k[to].rows = pieceRows(p)
  else fitPieceRows(p, k[to].rows)
  k[to].pieces.push(p)
  return k[to].pieces.length - 1
}
/** Adds textures to a set, each into the lane its name suggests (see suggestWallKit). Returns how many were added. */
export function addTextures(k: WallKit, paths: string[]): number {
  const s = suggestWallKit(paths)
  let n = 0
  for (const lane of LANES) for (const p of s[lane].pieces) {
    if (k[lane].pieces.length >= MAX_PIECES) continue
    fitPieceRows(p, k[lane].pieces.length ? k[lane].rows : 1)
    k[lane].pieces.push(p); n++
  }
  return n
}
/** Sets one cell of a piece. */
export function setPieceCell(p: KitPiece, row: number, col: number, tex: string | null): void { if (p.cells[row] && col >= 0 && col < p.cells[row].length) p.cells[row][col] = tex }
