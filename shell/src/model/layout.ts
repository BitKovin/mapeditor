// The texture group solver (REQUIREMENTS §20): a group (model/texgroup.ts), a face size and parameter values in; a list of CELLS out. A cell is a rectangle of
// the face (metres, origin at the face's bottom-left, y up) with the texture slot that fills it and how the texture sits in it. Pure and deterministic: the
// same input always gives the same cells (randomness comes from a seeded hash of the place, never from a clock or Math.random).
//
// Used twice: model/layoutPlan.ts turns the cells of a real face into UV pieces for every renderer, and the group editor draws the same cells as a preview.
import { Align, Cond, Fit, Node, ParamValue, Part, RepeatSpec, Rotate, SizeSpec, SplitNode, TexGroup, TexNode, Val, resolveParams } from './texgroup'
import { TexInfo, naturalSize } from './texprops'
import { solveKit } from './tileset'

/** More cells than this and the face is not laid out (the plain texture is drawn): the same kind of cap as MAX_CELLS of repeat behaviour. */
export const MAX_LAYOUT_CELLS = 2048
/** Work limit of one solve (rules calling rules calling rules must end). */
export const MAX_LAYOUT_VISITS = 40000
const MAX_REF_DEPTH = 40
const EPS = 1e-9

export type Facing = 'wall' | 'up' | 'down'
/** `wall` = the face stands (|normal.y| <= 0.5), `up` = it looks up (floor, roof), `down` = it looks down (ceiling, soffit). World Y is up. */
export const facingOf = (normalY: number): Facing => (normalY > 0.5 ? 'up' : normalY < -0.5 ? 'down' : 'wall')
/** Degrees between the face normal and world up: 0 = floor, 90 = wall, 180 = ceiling. */
export const tiltOf = (normalY: number): number => (Math.acos(Math.max(-1, Math.min(1, normalY))) * 180) / Math.PI

export interface Cell {
  x0: number; y0: number; x1: number; y1: number
  /** texture path */
  texture: string
  /** slot name ('' = the fallback fills this cell) */
  slot: string
  fit: Fit
  origin: 'face' | 'cell'
  alignU: Align; alignV: Align
  flipU: boolean; flipV: boolean
  rotate: Rotate
  /** only part of the texture is shown, from its left (cropU) and its bottom (cropV): 1 = the whole texture (default). Used by texture sets so a tile keeps its size at the end of a wall. Only with fit 'stretch'. */
  cropU?: number; cropV?: number
  /** where in the rule tree it came from (stable; shown in the editor, feeds the random numbers) */
  path: string
}

export interface LayoutInput {
  group: TexGroup
  /** overrides of the group's parameters (checked and completed by `resolveParams`) */
  params?: Record<string, ParamValue>
  /** face size in metres along the layout's u (left to right) and v (bottom to top) */
  w: number
  h: number
  facing?: Facing
  tilt?: number
  seed: number
  /** world height of a point of the face given in layout metres (rules may read `worldY`); default: the layout y */
  worldY?: (x: number, y: number) => number
  /** texture sizes (for natural sizes); without it a natural size counts as 1 m */
  info?: (path: string) => TexInfo | undefined
  /** what fills gaps and failed choices when the group has no usable fallback slot */
  fallbackTexture?: string
}

export interface LayoutResult {
  cells: Cell[]
  /** things the author should know: missing names, parts that did not fit, caps hit */
  warnings: string[]
  /** a cap stopped the solve: `cells` is incomplete and must not be used */
  truncated: boolean
}

// ---- seeded random (model/rng.ts, re-exported: tests and callers import it from here)
export { faceSeed, hash32, rand01 } from './rng'
import { rand01 } from './rng'

// ---- the solver ------------------------------------------------------------------------------------------------------------------------------------
interface Rect { x: number; y: number; w: number; h: number }
interface Rep { name: string; index: number; count: number }
interface Ctx { path: string; reps: readonly Rep[] }
type Value = number | string | boolean | undefined

class Solver {
  cells: Cell[] = []
  warnings: string[] = []
  private warned = new Set<string>()
  private visits = 0
  truncated = false
  private params: Record<string, ParamValue>
  private fallbackPath: string

  constructor(private i: LayoutInput) {
    this.params = resolveParams(i.group, i.params)
    const fb = i.group.fallback ? i.group.slots[i.group.fallback] : undefined
    this.fallbackPath = fb ?? i.fallbackTexture ?? 'dev/grey'
  }

  warn(msg: string): void { if (!this.warned.has(msg) && this.warnings.length < 100) { this.warned.add(msg); this.warnings.push(msg) } }
  private stop(why: string): void { if (!this.truncated) { this.truncated = true; this.warn(why) } }

  run(): void {
    const { w, h } = this.i
    if (!(w > EPS && h > EPS)) return
    this.node(this.i.group.root, { x: 0, y: 0, w, h }, { path: 'r', reps: [] }, 0)
  }

  // ---- variables and conditions
  private variable(name: string, r: Rect, c: Ctx): Value {
    const i = this.i
    switch (name) {
      case 'w': return r.w
      case 'h': return r.h
      case 'x': case 'fromLeft': return r.x
      case 'y': case 'fromBottom': return r.y
      case 'fromTop': return i.h - (r.y + r.h)
      case 'fromRight': return i.w - (r.x + r.w)
      case 'worldY': return i.worldY ? i.worldY(r.x, r.y) : r.y
      case 'faceW': return i.w
      case 'faceH': return i.h
      case 'facing': return i.facing ?? 'wall'
      case 'tilt': return i.tilt ?? 90
      case 'rand': return rand01(i.seed, c.path)
    }
    const dot = name.indexOf('.')
    const head = dot < 0 ? name : name.slice(0, dot), tail = dot < 0 ? '' : name.slice(dot + 1)
    if (head === 'param' && tail) return this.params[tail]
    const rep = dot < 0 ? c.reps[c.reps.length - 1] : [...c.reps].reverse().find((x) => x.name === head)
    const field = dot < 0 ? head : tail
    if (!rep) {
      // outside any repeat the plain names read as a single item (so a rule written for a repeat still works when used once)
      if (dot >= 0) return undefined
      return field === 'index' || field === 'fromEnd' ? 0 : field === 'count' ? 1 : field === 'first' || field === 'last' ? true : undefined
    }
    switch (field) {
      case 'index': return rep.index
      case 'count': return rep.count
      case 'first': return rep.index === 0
      case 'last': return rep.index === rep.count - 1
      case 'fromEnd': return rep.count - 1 - rep.index
    }
    return undefined
  }

  private val(v: Val, r: Rect, c: Ctx): Value {
    if (typeof v === 'object') return 'param' in v ? this.params[v.param] : this.variable(v.v, r, c)
    return v
  }

  private cond(k: Cond, r: Rect, c: Ctx): boolean {
    switch (k.k) {
      case 'const': return k.value
      case 'all': return k.items.every((x) => this.cond(x, r, c))
      case 'any': return k.items.some((x) => this.cond(x, r, c))
      case 'not': return !this.cond(k.item, r, c)
      case 'truthy': { const v = this.variable(k.v, r, c); if (v === undefined) this.warn(`unknown variable '${k.v}'`); return v === true || (typeof v === 'number' && v !== 0) || (typeof v === 'string' && v !== '') }
      case 'cmp': {
        const a = this.variable(k.v, r, c)
        if (a === undefined) { this.warn(`unknown variable '${k.v}'`); return false }
        const list = Array.isArray(k.value) ? k.value.map((x) => this.val(x, r, c)) : [this.val(k.value, r, c)]
        if (k.op === 'in') return list.some((b) => same(a, b))
        if (k.op === 'mod') {
          const m = num(list[0]), rem = num(list[1])
          if (!(m !== null && m > 0 && rem !== null)) return false
          const x = num(a)
          return x !== null && Math.abs((((x % m) + m) % m) - rem) < 1e-6
        }
        const b = list[0]
        if (k.op === '==') return same(a, b)
        if (k.op === '!=') return !same(a, b)
        const x = num(a), y = num(b)
        if (x === null || y === null) return false
        return k.op === '<' ? x < y - 1e-9 : k.op === '<=' ? x <= y + 1e-9 : k.op === '>' ? x > y + 1e-9 : x >= y - 1e-9
      }
    }
  }

  // ---- sizes
  /** Metres of a non flexible size along `axis` inside a parent of `extent` metres. */
  private metres(s: SizeSpec, extent: number, axis: 'u' | 'v'): number {
    switch (s.k) {
      case 'fixed': return s.m
      case 'frac': return s.f * extent
      case 'param': { const v = this.params[s.name]; if (typeof v !== 'number') { this.warn(`size uses parameter '${s.name}' which is not a number`); return 0 } return v * s.mul }
      case 'natural': {
        const path = this.i.group.slots[s.slot]
        const info = path ? this.i.info?.(path) : undefined
        if (!info) { this.warn(`natural size of slot '${s.slot}' is not known (texture not loaded?): 1 m used`); return 1 }
        return naturalSize(info)[axis === 'u' ? 0 : 1]
      }
      case 'flex': return 0
    }
  }

  // ---- nodes
  private fallbackCell(r: Rect, c: Ctx, why: string): void {
    this.warn(why)
    this.add(r, c, { k: 'tex', slot: '', fit: 'tile', origin: 'face', alignU: 'start', alignV: 'start', flipU: false, flipV: false, rotate: 0 }, this.fallbackPath)
  }

  private add(r: Rect, c: Ctx, n: TexNode, texture: string): void {
    if (this.cells.length >= MAX_LAYOUT_CELLS) { this.stop(`more than ${MAX_LAYOUT_CELLS} cells: the face is not laid out (use bigger parts or fewer repeats)`); return }
    this.cells.push({ x0: r.x, y0: r.y, x1: r.x + r.w, y1: r.y + r.h, texture, slot: n.slot, fit: n.fit, origin: n.origin, alignU: n.alignU, alignV: n.alignV, flipU: n.flipU, flipV: n.flipV, rotate: n.rotate, path: c.path })
  }

  private node(n: Node, r: Rect, c: Ctx, depth: number): void {
    if (this.truncated) return
    if (++this.visits > MAX_LAYOUT_VISITS) { this.stop('the rules need too many steps (a rule may call itself)'); return }
    if (!(r.w > 1e-6 && r.h > 1e-6)) return
    if (depth > MAX_REF_DEPTH) { this.fallbackCell(r, c, 'rules nested too deep (does a rule call itself?)'); return }
    switch (n.k) {
      case 'tex': {
        const path = this.i.group.slots[n.slot]
        if (!path) { this.fallbackCell(r, c, `unknown slot '${n.slot}'`); return }
        this.add(r, c, n, path)
        return
      }
      case 'ref': {
        const target = this.i.group.rules[n.name]
        if (!target) { this.fallbackCell(r, c, `unknown rule '${n.name}'`); return }
        this.node(target, r, { path: `${c.path}>${n.name}`, reps: c.reps }, depth + 1)
        return
      }
      case 'choice': return this.choice(n, r, c, depth)
      case 'split': return this.split(n, r, c, depth)
    }
  }

  private choice(n: Extract<Node, { k: 'choice' }>, r: Rect, c: Ctx, depth: number): void {
    const matching = n.options.map((o, i) => ({ o, i })).filter(({ o }) => !o.when || this.cond(o.when, r, c))
    if (matching.length === 0) { this.fallbackCell(r, c, `no option of a choice matched at ${c.path}`); return }
    let pick = matching[0]
    if (n.mode === 'random') {
      const total = matching.reduce((s, m) => s + m.o.weight, 0)
      let t = rand01(this.i.seed, `${c.path}?`) * total
      pick = matching[matching.length - 1]
      for (const m of matching) { if (t < m.o.weight) { pick = m; break } t -= m.o.weight }
    } else if (n.mode === 'cycle') {
      const by = n.by ? num(this.variable(n.by, r, c)) : num(this.variable('index', r, c))
      const k = Math.floor(by ?? 0)
      pick = matching[((k % matching.length) + matching.length) % matching.length]
    }
    this.node(pick.o.node, r, { path: `${c.path}?${pick.i}`, reps: c.reps }, depth + 1)
  }

  private split(n: SplitNode, r: Rect, c: Ctx, depth: number): void {
    const horizontal = n.axis === 'u'
    const E = horizontal ? r.w : r.h
    interface Item { part: Part; idx: number; fixed: number | null; weight: number; min: number; max: number; size: number; dropped: boolean }
    const items: Item[] = []
    n.parts.forEach((part, idx) => {
      if (part.when && !this.cond(part.when, r, c)) return
      const s = part.size
      const flex = s.k === 'flex'
      items.push({ part, idx, fixed: flex ? null : Math.max(0, this.metres(s, E, n.axis)), weight: flex ? s.weight : 0, min: flex ? s.min : 0, max: flex ? s.max : Infinity, size: 0, dropped: false })
    })
    if (items.length === 0) return

    const need = (): number => items.reduce((a, it) => a + (it.dropped ? 0 : it.fixed ?? it.min), 0)
    // too little room: optional parts go first, lowest priority first (later parts before earlier ones of the same priority)
    if (need() > E + 1e-6) {
      const order = items.filter((it) => it.part.optional).sort((a, b) => a.part.priority - b.part.priority || b.idx - a.idx)
      for (const it of order) { if (need() <= E + 1e-6) break; it.dropped = true; this.warn(`part ${it.idx} of ${c.path} left out: not enough room (${fmt(E)} m)`) }
    }
    const active = items.filter((it) => !it.dropped)
    if (active.length === 0) return
    let total = need()
    if (total > E + 1e-6) {
      // still too much: everything shrinks by the same factor
      this.warn(`parts of ${c.path} need ${fmt(total)} m but only ${fmt(E)} m are there: shrunk to fit`)
      const f = E / total
      for (const it of active) it.size = (it.fixed ?? it.min) * f
    } else {
      const flexes = active.filter((it) => it.fixed === null)
      for (const it of active) if (it.fixed !== null) it.size = it.fixed
      let remaining = E - active.reduce((a, it) => a + (it.fixed ?? 0), 0)
      let open = flexes.slice()
      // water filling: share by weight, pin the parts that break their min (or max), share the rest again
      for (let pass = 0; pass <= flexes.length && open.length; pass++) {
        const wsum = open.reduce((a, it) => a + it.weight, 0)
        const shares = open.map((it) => (remaining * it.weight) / wsum)
        const low = open.filter((it, k) => shares[k] < it.min - 1e-9), high = open.filter((it, k) => shares[k] > it.max + 1e-9)
        const pin = low.length ? low : high
        if (pin.length === 0) { open.forEach((it, k) => { it.size = shares[k] }); open = []; break }
        for (const it of pin) { it.size = low.length ? it.min : it.max; remaining -= it.size }
        open = open.filter((it) => !pin.includes(it))
      }
      total = active.reduce((a, it) => a + it.size, 0)
    }
    // leftover: nothing flexible took the rest
    const used = active.reduce((a, it) => a + it.size, 0)
    let cursor = 0
    const gap = E - used
    if (gap > 1e-6) {
      if (n.leftover === 'stretch' && used > EPS) { const f = E / used; for (const it of active) it.size *= f }
      else if (n.leftover === 'stretch') { /* nothing to scale: the first part takes it */ active[0].size += gap }
      else {
        const before = n.leftover === 'end' ? gap : n.leftover === 'center' ? gap / 2 : 0
        const after = gap - before
        if (before > 1e-6) this.gap(n, r, c, depth, horizontal, 0, before)
        cursor = before
        this.emit(n, r, c, depth, active, horizontal, cursor)
        if (after > 1e-6) this.gap(n, r, c, depth, horizontal, E - after, after)
        return
      }
    }
    this.emit(n, r, c, depth, active, horizontal, cursor)
  }

  private gap(n: SplitNode, r: Rect, c: Ctx, depth: number, horizontal: boolean, at: number, size: number): void {
    const rect: Rect = horizontal ? { x: r.x + at, y: r.y, w: size, h: r.h } : { x: r.x, y: r.y + at, w: r.w, h: size }
    const gc = { path: `${c.path}.gap${at > 0 ? 'e' : 's'}`, reps: c.reps }
    if (n.fill) this.node(n.fill, rect, gc, depth + 1)
    else this.fallbackCell(rect, gc, `leftover space of ${c.path} filled with the fallback texture`)
  }

  private emit(n: SplitNode, r: Rect, c: Ctx, depth: number, active: { part: Part; idx: number; size: number }[], horizontal: boolean, start: number): void {
    let at = start
    for (const it of active) {
      const size = it.size
      if (size > 1e-6) {
        const rect = (a: number, s: number): Rect => (horizontal ? { x: r.x + a, y: r.y, w: s, h: r.h } : { x: r.x, y: r.y + a, w: r.w, h: s })
        const rep = it.part.repeat
        if (!rep) this.node(it.part.node, rect(at, size), { path: `${c.path}/${it.idx}`, reps: c.reps }, depth + 1)
        else {
          const count = this.repeatCount(rep, size, n.axis, `${c.path}/${it.idx}`)
          for (let k = 0; k < count && !this.truncated; k++) {
            this.node(it.part.node, rect(at + (size * k) / count, size / count), { path: `${c.path}/${it.idx}.${k}`, reps: [...c.reps, { name: rep.as, index: k, count }] }, depth + 1)
          }
        }
      }
      at += size
    }
  }

  /** How many equal copies fill `extent` metres. */
  private repeatCount(rep: RepeatSpec, extent: number, axis: 'u' | 'v', where: string): number {
    let s = this.metres(rep.size, extent, axis)
    if (!(s >= 0.01)) { this.warn(`repeat at ${where} has a size below 1 cm: 1 cm used`); s = 0.01 }
    const q = extent / s
    let n = rep.fit === 'floor' ? Math.floor(q + 1e-9) : rep.fit === 'ceil' ? Math.ceil(q - 1e-9) : Math.round(q)
    if (n < rep.min) n = rep.min
    if (n > rep.max) { this.warn(`repeat at ${where} limited to ${rep.max} copies`); n = rep.max }
    return Math.max(0, n)
  }
}

const num = (v: Value): number | null => (typeof v === 'number' && Number.isFinite(v) ? v : typeof v === 'boolean' ? (v ? 1 : 0) : null)
const same = (a: Value, b: Value): boolean => {
  if (typeof a === 'string' || typeof b === 'string') return a === b
  const x = num(a), y = num(b)
  return x !== null && y !== null && Math.abs(x - y) < 1e-9
}
const fmt = (v: number): string => String(Math.round(v * 100) / 100)

/** Solves a group for a face of `w` x `h` metres. */
export function solveLayout(input: LayoutInput): LayoutResult {
  // a simple texture set (model/tileset.ts) has its own solver; both give the same cells
  if (input.group.kit) {
    const r = solveKit(input.group.kit, { w: input.w, h: input.h, seed: input.seed, info: input.info, fallbackTexture: input.fallbackTexture })
    return { cells: r.cells, warnings: r.warnings, truncated: r.truncated === true }
  }
  const s = new Solver(input)
  s.run()
  return { cells: s.truncated ? [] : s.cells, warnings: s.warnings, truncated: s.truncated }
}
