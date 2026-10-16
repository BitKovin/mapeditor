// Texture groups (REQUIREMENTS §20): a reusable rule set that lays textures out over a face by rules (a split grammar) instead of repeating one texture.
// Stored by Core as `<assets>/texgroups/<name>.texgroup.json`; this file is the format: types, a lenient reader and the reference a face keeps (`face.layout`).
//
// File format (format 1). Every size is in metres. A `v` split runs bottom to top, a `u` split left to right (as the picture reads from the front).
//   { "format": 1, "name": "...", "description": "...",
//     "params": { "floorHeight": { "type": "number", "default": 3, "min": 2, "max": 6, "step": 0.1, "label": "Floor height" }, "balconies": { "type": "bool", "default": true }, ... },
//     "slots":  { "wall": "bldg/brick", "window": { "texture": "bldg/window" } },     named textures; the rules never name a texture path directly
//     "fallback": "wall",                                                              slot that fills gaps / stands in while loading (default: the first slot)
//     "rules":  { "bay": NODE },                                                       named sub-trees, used by { "ref": "bay" }
//     "root": NODE }
//
// NODE (one of):
//   "wall" or { "tex": "wall", "fit": "tile|stretch|width|height", "origin": "face|cell", "alignU": "start|center|end", "alignV": ..., "flipU": false, "flipV": false, "rotate": 0 }
//   { "split": "u|v", "parts": [PART...], "leftover": "stretch|start|center|end", "fill": NODE }
//   { "choice": [OPTION...], "mode": "first|random|cycle", "by": "floor.index" }
//   { "ref": "name" }
// PART   = a NODE (inline, or under "node") plus: "size": SIZE, "when": COND, "optional": true, "priority": n, "repeat": { "size": SIZE, "fit": "round|floor|ceil", "min": 1, "max": 256, "as": "floor" }
// OPTION = a NODE (inline, or under "node") plus: "when": COND, "weight": n
// SIZE   = 3 (metres) | "*" | "2*" (flexible, weight) | "25%" (of the parent) | { "flex": 1, "min": 0, "max": 9 } | { "frac": 0.25 } | { "param": "floorHeight", "mul": 1 } | { "natural": "slot" }
// COND   = true | false | "first" (variable is true / non zero) | { "all": [COND] } | { "any": [COND] } | { "not": COND } | { "var": "w", ">=": 2.5 }
//          comparisons: "==" "!=" "<" "<=" ">" ">=" "in" (array) "mod" ([m, r]: value mod m == r); a value is a literal, { "param": "x" } or { "var": "x" }
//
// The reader is lenient (a hand-edited or damaged file must never break the editor): unusable parts are dropped and listed in `warnings`; no usable `root` is an error.
import { safeTexturePath } from './texprops'
import { Kit, kitTextures, parseKit } from './tileset'

export const GROUP_FORMAT = 1
export const GROUP_MAX_DEPTH = 24, GROUP_MAX_NODES = 4000, GROUP_MAX_LIST = 64, GROUP_MAX_SLOTS = 64, GROUP_MAX_PARAMS = 32, GROUP_MAX_RULES = 64

export type ParamValue = number | boolean | string
export type ParamDef =
  | { type: 'number' | 'int'; default: number; min: number; max: number; step: number; label: string }
  | { type: 'bool'; default: boolean; label: string }
  | { type: 'choice'; default: string; options: string[]; label: string }

export type CmpOp = '==' | '!=' | '<' | '<=' | '>' | '>=' | 'in' | 'mod'
export const CMP_OPS: CmpOp[] = ['==', '!=', '<', '<=', '>', '>=', 'in', 'mod']
export type Val = number | string | boolean | { param: string } | { v: string }
export type Cond =
  | { k: 'const'; value: boolean }
  | { k: 'all' | 'any'; items: Cond[] }
  | { k: 'not'; item: Cond }
  | { k: 'truthy'; v: string }
  | { k: 'cmp'; v: string; op: CmpOp; value: Val | Val[] }

export type SizeSpec =
  | { k: 'fixed'; m: number }
  | { k: 'param'; name: string; mul: number }
  | { k: 'frac'; f: number }
  | { k: 'natural'; slot: string }
  | { k: 'flex'; weight: number; min: number; max: number }

export type Fit = 'tile' | 'stretch' | 'width' | 'height'
export type Align = 'start' | 'center' | 'end'
export type Leftover = 'stretch' | 'start' | 'center' | 'end'
export type Rotate = 0 | 90 | 180 | 270

export interface TexNode { k: 'tex'; slot: string; fit: Fit; origin: 'face' | 'cell'; alignU: Align; alignV: Align; flipU: boolean; flipV: boolean; rotate: Rotate }
export interface SplitNode { k: 'split'; axis: 'u' | 'v'; parts: Part[]; leftover: Leftover; fill: Node | null }
export interface ChoiceNode { k: 'choice'; mode: 'first' | 'random' | 'cycle'; by: string | null; options: Option[] }
export interface RefNode { k: 'ref'; name: string }
export type Node = TexNode | SplitNode | ChoiceNode | RefNode

export interface RepeatSpec { size: SizeSpec; fit: 'round' | 'floor' | 'ceil'; min: number; max: number; as: string }
export interface Part { size: SizeSpec; node: Node; when: Cond | null; optional: boolean; priority: number; repeat: RepeatSpec | null }
export interface Option { when: Cond | null; weight: number; node: Node }

export interface TexGroup {
  format: number
  name: string
  description: string
  params: Record<string, ParamDef>
  /** slot name -> texture path */
  slots: Record<string, string>
  /** the slot that fills gaps and stands in for the group (null = none usable) */
  fallback: string | null
  rules: Record<string, Node>
  root: Node
  /** a simple texture set (model/tileset.ts); then `root` is only a placeholder and `slots` lists the textures of the set */
  kit: Kit | null
}

export interface GroupParse { group: TexGroup | null; errors: string[]; warnings: string[] }

const fin = (v: unknown): v is number => typeof v === 'number' && Number.isFinite(v)
const clamp = (v: number, lo: number, hi: number): number => Math.min(hi, Math.max(lo, v))
const isObj = (v: unknown): v is Record<string, unknown> => !!v && typeof v === 'object' && !Array.isArray(v)
const NAME_RE = /^[A-Za-z_][A-Za-z0-9_]{0,47}$/
export const isName = (s: unknown): s is string => typeof s === 'string' && NAME_RE.test(s)
/** Variable names: dotted (`floor.index`, `param.balconies`). */
const VAR_RE = /^[A-Za-z_][A-Za-z0-9_]{0,47}(\.[A-Za-z_][A-Za-z0-9_]{0,47})?$/
export const isVarName = (s: unknown): s is string => typeof s === 'string' && VAR_RE.test(s)

/** Variables the rules can read besides parameters and named repeats (see model/layout.ts). */
export const BUILTIN_VARS = ['w', 'h', 'x', 'y', 'fromTop', 'fromBottom', 'fromLeft', 'fromRight', 'worldY', 'faceW', 'faceH', 'facing', 'tilt', 'rand', 'index', 'count', 'first', 'last', 'fromEnd'] as const

// ---- the reader ------------------------------------------------------------------------------------------------------------------------------------
class Reader {
  nodes = 0
  warnings: string[] = []
  warn(where: string, what: string): void { if (this.warnings.length < 200) this.warnings.push(`${where}: ${what}`) }
}

function parseVal(j: unknown): Val | null {
  if (fin(j) || typeof j === 'boolean' || typeof j === 'string') return j
  if (isObj(j) && isName(j.param)) return { param: j.param }
  if (isObj(j) && isVarName(j.var)) return { v: j.var }
  return null
}

function parseCond(j: unknown, r: Reader, at: string, depth = 0): Cond | null {
  if (depth > GROUP_MAX_DEPTH) { r.warn(at, 'condition nested too deep'); return null }
  if (typeof j === 'boolean') return { k: 'const', value: j }
  if (typeof j === 'string') { if (isVarName(j)) return { k: 'truthy', v: j }; r.warn(at, `'${j}' is not a variable name`); return null }
  if (!isObj(j)) { r.warn(at, 'a condition must be true / false, a variable name or an object'); return null }
  if (Array.isArray(j.all) || Array.isArray(j.any)) {
    const key = Array.isArray(j.all) ? 'all' : 'any'
    const items = (j[key] as unknown[]).slice(0, GROUP_MAX_LIST).map((c, i) => parseCond(c, r, `${at}.${key}[${i}]`, depth + 1))
    if (items.some((c) => c === null)) return null // a condition that lost a part would mean something else: drop it whole
    return { k: key, items: items as Cond[] }
  }
  if ('not' in j) { const c = parseCond(j.not, r, `${at}.not`, depth + 1); return c ? { k: 'not', item: c } : null }
  if (isVarName(j.var)) {
    const ops = CMP_OPS.filter((o) => o in j)
    if (ops.length === 0) return { k: 'truthy', v: j.var }
    if (ops.length > 1) { r.warn(at, `one comparison per condition (found ${ops.join(', ')})`); return null }
    const op = ops[0], raw = j[op]
    if (op === 'in' || op === 'mod') {
      if (!Array.isArray(raw) || raw.length === 0 || raw.length > GROUP_MAX_LIST) { r.warn(at, `'${op}' needs a list`); return null }
      const vals = raw.map(parseVal)
      if (vals.some((v) => v === null)) { r.warn(at, `'${op}' list has an unusable value`); return null }
      if (op === 'mod' && vals.length !== 2) { r.warn(at, "'mod' needs [m, r]"); return null }
      return { k: 'cmp', v: j.var, op, value: vals as Val[] }
    }
    const v = parseVal(raw)
    if (v === null) { r.warn(at, `'${op}' has an unusable value`); return null }
    return { k: 'cmp', v: j.var, op, value: v }
  }
  r.warn(at, "unknown condition (use { var, op: value }, all, any, not, a variable name or true / false)")
  return null
}

function parseSize(j: unknown, r: Reader, at: string, dflt: SizeSpec): SizeSpec {
  if (j === undefined || j === null) return dflt
  if (fin(j)) { if (j > 0) return { k: 'fixed', m: Math.min(j, 10000) }; r.warn(at, 'a size must be positive'); return dflt }
  if (typeof j === 'string') {
    const t = j.trim()
    let m = /^(\d*\.?\d*)\*$/.exec(t)
    if (m) return { k: 'flex', weight: m[1] === '' ? 1 : Math.max(1e-6, Number(m[1])), min: 0, max: Infinity }
    m = /^(\d*\.?\d+)%$/.exec(t)
    if (m) return { k: 'frac', f: clamp(Number(m[1]) / 100, 0, 1) }
    r.warn(at, `size '${j}' not understood (use 3, "*", "2*" or "25%")`)
    return dflt
  }
  if (isObj(j)) {
    if (fin(j.flex) || 'min' in j || 'max' in j) {
      const w = fin(j.flex) && j.flex > 0 ? j.flex : 1
      const min = fin(j.min) && j.min > 0 ? j.min : 0
      const max = fin(j.max) && j.max > 0 ? Math.max(j.max, min) : Infinity
      return { k: 'flex', weight: w, min, max }
    }
    if (fin(j.frac)) return { k: 'frac', f: clamp(j.frac, 0, 1) }
    if (isName(j.param)) return { k: 'param', name: j.param, mul: fin(j.mul) ? j.mul : 1 }
    if (isName(j.natural)) return { k: 'natural', slot: j.natural }
  }
  r.warn(at, 'size not understood')
  return dflt
}

const FLEX1: SizeSpec = { k: 'flex', weight: 1, min: 0, max: Infinity }

function parseTex(o: Record<string, unknown>, r: Reader, at: string): TexNode | null {
  if (!isName(o.tex)) { r.warn(at, 'a texture part needs a slot name in "tex"'); return null }
  const fit = o.fit === 'stretch' || o.fit === 'width' || o.fit === 'height' ? o.fit : 'tile'
  if (o.fit !== undefined && o.fit !== fit && o.fit !== 'tile') r.warn(at, `unknown fit '${String(o.fit)}', using tile`)
  const al = (v: unknown): Align => (v === 'center' || v === 'end' ? v : 'start')
  const rot = o.rotate === 90 || o.rotate === 180 || o.rotate === 270 ? o.rotate : 0
  return { k: 'tex', slot: o.tex, fit, origin: o.origin === 'cell' ? 'cell' : o.origin === 'face' ? 'face' : fit === 'tile' ? 'face' : 'cell', alignU: al(o.alignU), alignV: al(o.alignV), flipU: o.flipU === true, flipV: o.flipV === true, rotate: rot }
}

function parseNode(j: unknown, r: Reader, at: string, depth: number): Node | null {
  if (depth > GROUP_MAX_DEPTH) { r.warn(at, `nested deeper than ${GROUP_MAX_DEPTH} levels`); return null }
  if (++r.nodes > GROUP_MAX_NODES) { if (r.nodes === GROUP_MAX_NODES + 1) r.warn(at, `more than ${GROUP_MAX_NODES} nodes: the rest is ignored`); return null }
  if (typeof j === 'string') return isName(j) ? parseTex({ tex: j }, r, at) : (r.warn(at, `'${j}' is not a slot name`), null)
  if (!isObj(j)) { r.warn(at, 'a node must be a slot name or an object'); return null }
  if (j.node !== undefined && j.tex === undefined && j.split === undefined && j.choice === undefined && j.ref === undefined) return parseNode(j.node, r, at, depth + 1)
  if (j.tex !== undefined) return parseTex(j, r, at)
  if (j.ref !== undefined) { if (isName(j.ref)) return { k: 'ref', name: j.ref }; r.warn(at, 'ref needs a rule name'); return null }
  if (j.split !== undefined) {
    if (j.split !== 'u' && j.split !== 'v') { r.warn(at, "split must be 'u' or 'v'"); return null }
    if (!Array.isArray(j.parts)) { r.warn(at, 'a split needs a list of parts'); return null }
    const parts: Part[] = []
    j.parts.slice(0, GROUP_MAX_LIST).forEach((p, i) => { const pt = parseSplitPart(p, r, `${at}.parts[${i}]`, depth + 1); if (pt) parts.push(pt) })
    if (j.parts.length > GROUP_MAX_LIST) r.warn(at, `more than ${GROUP_MAX_LIST} parts: the rest is ignored`)
    if (parts.length === 0) { r.warn(at, 'a split without usable parts'); return null }
    const leftover: Leftover = j.leftover === 'start' || j.leftover === 'center' || j.leftover === 'end' ? j.leftover : 'stretch'
    const fill = j.fill !== undefined ? parseNode(j.fill, r, `${at}.fill`, depth + 1) : null
    return { k: 'split', axis: j.split, parts, leftover, fill }
  }
  if (j.choice !== undefined) {
    if (!Array.isArray(j.choice)) { r.warn(at, 'choice needs a list of options'); return null }
    const options: Option[] = []
    j.choice.slice(0, GROUP_MAX_LIST).forEach((o, i) => { const op = parseOption(o, r, `${at}.choice[${i}]`, depth + 1); if (op) options.push(op) })
    if (options.length === 0) { r.warn(at, 'a choice without usable options'); return null }
    const mode = j.mode === 'random' || j.mode === 'cycle' ? j.mode : 'first'
    return { k: 'choice', mode, by: isVarName(j.by) ? j.by : null, options }
  }
  r.warn(at, 'node has none of tex / split / choice / ref')
  return null
}

function parseSplitPart(j: unknown, r: Reader, at: string, depth: number): Part | null {
  const o = typeof j === 'string' ? { tex: j } : j
  if (!isObj(o)) { r.warn(at, 'a part must be an object'); return null }
  const node = parseNode(o.node !== undefined ? o.node : o, r, at, depth)
  if (!node) return null
  let repeat: RepeatSpec | null = null
  if (o.repeat !== undefined && o.repeat !== null && o.repeat !== false) {
    const rr = o.repeat === true ? {} : o.repeat
    if (!isObj(rr)) r.warn(at, 'repeat must be an object')
    else {
      const size = parseSize(rr.size, r, `${at}.repeat.size`, { k: 'fixed', m: 1 })
      if (size.k === 'flex' || size.k === 'frac') r.warn(at, 'a repeat size must be metres, a parameter or a natural size; using 1 m')
      repeat = {
        size: size.k === 'flex' || size.k === 'frac' ? { k: 'fixed', m: 1 } : size,
        fit: rr.fit === 'floor' || rr.fit === 'ceil' ? rr.fit : 'round',
        min: fin(rr.min) ? clamp(Math.floor(rr.min), 0, 4096) : 1,
        max: fin(rr.max) ? clamp(Math.floor(rr.max), 1, 4096) : 256,
        as: isName(rr.as) ? rr.as : '',
      }
      if (repeat.max < repeat.min) repeat.max = repeat.min
    }
  }
  const when = o.when !== undefined ? parseCond(o.when, r, `${at}.when`) : null
  if (o.when !== undefined && when === null) return null // a part whose condition is unusable is dropped, never made unconditional
  return { size: parseSize(o.size, r, `${at}.size`, FLEX1), node, when, optional: o.optional === true, priority: fin(o.priority) ? clamp(Math.round(o.priority), -1000, 1000) : 0, repeat }
}

function parseOption(j: unknown, r: Reader, at: string, depth: number): Option | null {
  const o = typeof j === 'string' ? { tex: j } : j
  if (!isObj(o)) { r.warn(at, 'an option must be an object'); return null }
  const node = parseNode(o.node !== undefined ? o.node : o, r, at, depth)
  if (!node) return null
  const when = o.when !== undefined ? parseCond(o.when, r, `${at}.when`) : null
  if (o.when !== undefined && when === null) return null
  return { when, weight: fin(o.weight) && o.weight > 0 ? Math.min(o.weight, 1e6) : 1, node }
}

function parseParams(j: unknown, r: Reader): Record<string, ParamDef> {
  const out: Record<string, ParamDef> = {}
  if (j === undefined) return out
  if (!isObj(j)) { r.warn('params', 'must be an object'); return out }
  for (const [name, d] of Object.entries(j).slice(0, GROUP_MAX_PARAMS)) {
    const at = `params.${name}`
    if (!isName(name) || !isObj(d)) { r.warn(at, 'unusable parameter'); continue }
    const label = typeof d.label === 'string' && d.label ? d.label.slice(0, 80) : name
    if (d.type === 'bool') { out[name] = { type: 'bool', default: d.default === true, label }; continue }
    if (d.type === 'choice') {
      const options = Array.isArray(d.options) ? d.options.filter((o): o is string => typeof o === 'string' && o.length > 0 && o.length <= 64).slice(0, GROUP_MAX_LIST) : []
      if (options.length === 0) { r.warn(at, 'a choice parameter needs options'); continue }
      out[name] = { type: 'choice', default: typeof d.default === 'string' && options.includes(d.default) ? d.default : options[0], options, label }
      continue
    }
    if (d.type === 'number' || d.type === 'int') {
      const min = fin(d.min) ? d.min : 0
      const max = fin(d.max) && d.max >= min ? d.max : Math.max(min, 1000)
      const step = fin(d.step) && d.step > 0 ? d.step : d.type === 'int' ? 1 : 0.1
      const dflt = clamp(fin(d.default) ? d.default : min, min, max)
      out[name] = { type: d.type, default: d.type === 'int' ? Math.round(dflt) : dflt, min, max, step, label }
      continue
    }
    r.warn(at, "type must be number, int, bool or choice")
  }
  return out
}

/** Reads a parsed JSON value. Never throws. */
export function parseGroup(json: unknown): GroupParse {
  const r = new Reader()
  if (!isObj(json)) return { group: null, errors: ['the file is not a JSON object'], warnings: [] }
  if (json.format !== undefined && json.format !== GROUP_FORMAT) return { group: null, errors: [`unsupported format ${String(json.format)} (this editor reads format ${GROUP_FORMAT})`], warnings: [] }
  if (json.kit !== undefined) {
    const kw: string[] = []
    const kit = parseKit(json.kit, (m) => kw.push(m))
    if (!kit) return { group: null, errors: ['the "kit" is not usable', ...kw], warnings: kw }
    const slots: Record<string, string> = {}
    kitTextures(kit).forEach((p, i) => { slots[`s${i}`] = p })
    const first = Object.keys(slots)[0] ?? null
    const root: Node = { k: 'tex', slot: first ?? '', fit: 'tile', origin: 'face', alignU: 'start', alignV: 'start', flipU: false, flipV: false, rotate: 0 }
    return { group: { format: GROUP_FORMAT, name: typeof json.name === 'string' ? json.name.slice(0, 120) : '', description: typeof json.description === 'string' ? json.description.slice(0, 2000) : '', params: {}, slots, fallback: first, rules: {}, root, kit }, errors: [], warnings: kw }
  }
  const slots: Record<string, string> = {}
  if (isObj(json.slots)) {
    for (const [name, v] of Object.entries(json.slots).slice(0, GROUP_MAX_SLOTS)) {
      const path = typeof v === 'string' ? v : isObj(v) ? v.texture : null
      const safe = safeTexturePath(path)
      if (isName(name) && safe) slots[name] = safe
      else r.warn(`slots.${name}`, 'needs a valid texture path')
    }
  } else r.warn('slots', 'missing: the rules have no textures to use')
  const rules: Record<string, Node> = {}
  if (isObj(json.rules)) {
    for (const [name, n] of Object.entries(json.rules).slice(0, GROUP_MAX_RULES)) {
      const node = isName(name) ? parseNode(n, r, `rules.${name}`, 1) : null
      if (node) rules[name] = node
      else if (!isName(name)) r.warn(`rules.${name}`, 'not a valid rule name')
    }
  }
  const root = json.root !== undefined ? parseNode(json.root, r, 'root', 0) : null
  if (!root) return { group: null, errors: ['no usable "root" node', ...r.warnings.filter((w) => w.startsWith('root'))], warnings: r.warnings }
  const group: TexGroup = {
    format: GROUP_FORMAT,
    name: typeof json.name === 'string' ? json.name.slice(0, 120) : '',
    description: typeof json.description === 'string' ? json.description.slice(0, 2000) : '',
    params: parseParams(json.params, r),
    slots,
    fallback: typeof json.fallback === 'string' && json.fallback in slots ? json.fallback : (Object.keys(slots)[0] ?? null),
    rules,
    root,
    kit: null,
  }
  const problems = checkReferences(group)
  return { group, errors: [], warnings: [...r.warnings, ...problems] }
}

/** Text -> group (the editor's entry: JSON syntax errors come back as errors). */
export function parseGroupText(text: string): GroupParse {
  let j: unknown
  try { j = JSON.parse(text) } catch (e) { return { group: null, errors: [`JSON: ${(e as Error).message}`], warnings: [] } }
  return parseGroup(j)
}

/** Names the rules use that do not exist: slots, rules, parameters, variables. They are not errors (the cell falls back), but the editor lists them. */
export function checkReferences(g: TexGroup): string[] {
  const out = new Set<string>()
  const ruleSeen = new Set<string>()
  const val = (v: Val | Val[], at: string): void => { for (const x of Array.isArray(v) ? v : [v]) if (typeof x === 'object' && 'param' in x && !(x.param in g.params)) out.add(`${at}: unknown parameter '${x.param}'`) }
  const varOk = (name: string, at: string): void => {
    if (name.startsWith('param.')) { if (!(name.slice(6) in g.params)) out.add(`${at}: unknown parameter '${name.slice(6)}'`) } else if (!name.includes('.') && !(BUILTIN_VARS as readonly string[]).includes(name)) out.add(`${at}: unknown variable '${name}'`)
  }
  const cond = (c: Cond | null, at: string): void => {
    if (!c) return
    if (c.k === 'all' || c.k === 'any') c.items.forEach((x) => cond(x, at))
    else if (c.k === 'not') cond(c.item, at)
    else if (c.k === 'truthy') varOk(c.v, at)
    else if (c.k === 'cmp') { varOk(c.v, at); val(c.value, at) }
  }
  const size = (s: SizeSpec, at: string): void => {
    if (s.k === 'param' && !(s.name in g.params)) out.add(`${at}: unknown parameter '${s.name}'`)
    if (s.k === 'natural' && !(s.slot in g.slots)) out.add(`${at}: unknown slot '${s.slot}'`)
  }
  const visit = (n: Node, at: string): void => {
    if (n.k === 'tex') { if (!(n.slot in g.slots)) out.add(`${at}: unknown slot '${n.slot}'`) }
    else if (n.k === 'ref') { if (!(n.name in g.rules)) out.add(`${at}: unknown rule '${n.name}'`); else if (!ruleSeen.has(n.name)) { ruleSeen.add(n.name); visit(g.rules[n.name], `rule ${n.name}`) } }
    else if (n.k === 'split') {
      n.parts.forEach((p, i) => { size(p.size, at); if (p.repeat) size(p.repeat.size, at); cond(p.when, `${at}.parts[${i}]`); visit(p.node, `${at}.parts[${i}]`) })
      if (n.fill) visit(n.fill, `${at}.fill`)
    } else {
      if (n.by) varOk(n.by, at)
      n.options.forEach((o, i) => { cond(o.when, `${at}.choice[${i}]`); visit(o.node, `${at}.choice[${i}]`) })
    }
  }
  visit(g.root, 'root')
  for (const [name, n] of Object.entries(g.rules)) if (!ruleSeen.has(name)) visit(n, `rule ${name}`)
  return [...out].slice(0, 100)
}

/** Every texture path the group can use. */
export const groupTextures = (g: TexGroup): string[] => [...new Set(Object.values(g.slots))]

// ---- parameters ------------------------------------------------------------------------------------------------------------------------------------
/** The parameter values to use: the defaults with `overrides` applied, each checked against its definition (unknown names and bad values are ignored). */
export function resolveParams(g: TexGroup, overrides: Record<string, ParamValue> | undefined): Record<string, ParamValue> {
  const out: Record<string, ParamValue> = {}
  for (const [name, d] of Object.entries(g.params)) {
    const v = overrides?.[name]
    if (d.type === 'bool') out[name] = typeof v === 'boolean' ? v : d.default
    else if (d.type === 'choice') out[name] = typeof v === 'string' && d.options.includes(v) ? v : d.default
    else { const n = fin(v) ? clamp(v, d.min, d.max) : d.default; out[name] = d.type === 'int' ? Math.round(n) : n }
  }
  return out
}

// ---- what a face keeps -----------------------------------------------------------------------------------------------------------------------------
/** `face.layout`: which group lays the face out and the face's own choices. Everything but `group` is optional. */
export interface FaceLayout {
  group: string
  /** null = derived from the face's identity (stable) */
  seed: number | null
  /** only the parameters that differ from the group's defaults */
  params: Record<string, ParamValue>
  rotate: Rotate
  flipU: boolean
}

export function parseFaceLayout(j: unknown): FaceLayout | null {
  if (!isObj(j) || typeof j.group !== 'string') return null
  const group = safeTexturePath(j.group)
  if (!group) return null
  const params: Record<string, ParamValue> = {}
  if (isObj(j.params)) for (const [k, v] of Object.entries(j.params).slice(0, GROUP_MAX_PARAMS)) if (isName(k) && (fin(v) || typeof v === 'boolean' || (typeof v === 'string' && v.length <= 64))) params[k] = v
  const rotate = j.rotate === 90 || j.rotate === 180 || j.rotate === 270 ? j.rotate : 0
  return { group, seed: fin(j.seed) ? Math.trunc(j.seed) >>> 0 : null, params, rotate, flipU: j.flipU === true }
}

/** The stored form: defaults are left out so a plain application is just `{ group }`. */
export function faceLayoutToJson(l: FaceLayout): Record<string, unknown> {
  const o: Record<string, unknown> = { group: l.group }
  if (l.seed !== null) o.seed = l.seed
  if (Object.keys(l.params).length) o.params = { ...l.params }
  if (l.rotate) o.rotate = l.rotate
  if (l.flipU) o.flipU = true
  return o
}

// ---- the texture group environment (like texEnv for textures) --------------------------------------------------------------------------------------
/**
 * Where the pure functions look a group up. The TexGroupCache registers itself as the provider; `get` starts the load on first request and returns
 * undefined while it is loading (callers then draw the face plainly). `rev` increases whenever a group is loaded, edited or removed, and the cache
 * also bumps `texEnv` so every mesh cache that compares it rebuilds.
 */
class GroupEnv {
  rev = 0
  private provider: ((path: string) => TexGroup | undefined) | null = null
  private versions: ((path: string) => string) | null = null
  setProvider(p: ((path: string) => TexGroup | undefined) | null, versions: ((path: string) => string) | null = null): void { this.provider = p; this.versions = versions; this.rev++ }
  get(path: string): TexGroup | undefined { return this.provider?.(path) }
  /** changes exactly when the group's file content changed */
  version(path: string): string { return this.versions?.(path) ?? '' }
  bump(): void { this.rev++ }
}
export const groupEnv = new GroupEnv()
