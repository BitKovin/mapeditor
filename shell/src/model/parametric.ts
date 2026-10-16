// Parametric shapes (REQUIREMENTS §22 P1). Every brush of a placed cylinder / wedge / arch / stairs carries a `shape` record naming its group,
// its part and how the whole shape was made (kind, parameters, the drawn box, orientation). The Details panel edits the record and every part
// is rebuilt in place; transforms carry the box along (transform.ts mapBrushPoints); any other change to a part's geometry turns the group into
// ordinary brushes (reconcileShapes, run on every edit by the store). The Core keeps the record as it keeps any key it does not know.
import type { Brush, Entity, Face, MapDocument } from './document'
import { Vec3, add, length, scale, sub } from './math'
import { Frame, ShapeId, ShapeParams, Turn, buildInFrame, defaultShapeParams, turned } from './shapes'
import { alignBrush } from './surface'

export type ParamShape = Exclude<ShapeId, 'box'>
export interface ShapeSpec { group: string; kind: ParamShape; part: number; parts: number; frame: Frame; params: Partial<ShapeParams>; turn: Turn }

const KINDS: ParamShape[] = ['cylinder', 'wedge', 'arch', 'stairs']
const isVec = (x: unknown): x is Vec3 => Array.isArray(x) && x.length === 3 && x.every((n) => typeof n === 'number' && Number.isFinite(n))
const isNum = (x: unknown): x is number => typeof x === 'number' && Number.isFinite(x)

/** The parameters that matter for a kind (the rest of ShapeParams is not stored). */
export function paramsFor(kind: ParamShape, p: ShapeParams): Partial<ShapeParams> {
  return kind === 'cylinder' ? { sides: p.sides } : kind === 'stairs' ? { steps: p.steps } : kind === 'arch' ? { archSegments: p.archSegments, archThickness: p.archThickness } : {}
}

/** The brush's shape record, or null when it has none (or a damaged one). */
export function shapeOf(b: Brush): ShapeSpec | null {
  const s = b.shape as Partial<ShapeSpec> | undefined
  if (!s || typeof s !== 'object' || typeof s.group !== 'string' || !KINDS.includes(s.kind as ParamShape)) return null
  if (!Number.isInteger(s.part) || !Number.isInteger(s.parts) || s.part! < 0 || s.part! >= s.parts!) return null
  const f = s.frame as Partial<Frame> | undefined
  if (!f || !isVec(f.o) || !isVec(f.u) || !isVec(f.v) || !isVec(f.n) || !isNum(f.du) || !isNum(f.dv) || !isNum(f.h)) return null
  const turn = ([0, 1, 2, 3] as const).find((t) => t === s.turn) ?? 0
  return { group: s.group, kind: s.kind as ParamShape, part: s.part!, parts: s.parts!, frame: f as Frame, params: typeof s.params === 'object' && s.params ? s.params : {}, turn }
}

const withoutShape = (b: Brush): Brush => { const { shape: _drop, ...rest } = b; void _drop; return rest as Brush }

/** Marks freshly built parts as one shape. */
export function attachShape(brushes: Brush[], kind: ParamShape, frame: Frame, params: ShapeParams, turn: Turn, group: string): Brush[] {
  return brushes.map((b, part) => ({ ...b, shape: { group, kind, part, parts: brushes.length, frame, params: paramsFor(kind, params), turn } satisfies ShapeSpec }))
}

/** The parts a spec describes (geometry only: texture '', ids `p0`, `p1`...). */
function partsOf(kind: ParamShape, frame: Frame, params: Partial<ShapeParams>, turn: Turn): Brush[] | { error: string } {
  let i = 0
  const r = buildInFrame(kind, turned(frame, turn), '', { ...defaultShapeParams(), ...params }, () => `p${i++}`)
  return 'error' in r ? r : r.brushes
}

/** The box after a point map (affine maps keep every generated point where the mapped brush has it, so a moved shape still matches). */
export function mapFrame(f: Frame, map: (p: Vec3) => Vec3): Frame {
  const o = map(f.o)
  const axis = (dir: Vec3, len: number): [Vec3, number] => {
    const d = sub(map(add(f.o, scale(dir, len))), o), l = length(d)
    return l > 1e-12 ? [scale(d, 1 / l), l] : [dir, len]
  }
  const [u, du] = axis(f.u, f.du), [v, dv] = axis(f.v, f.dv), [n, h] = axis(f.n, f.h)
  return { o, u, v, n, du, dv, h }
}

/** A brush moved / rotated / scaled with `map` keeps its shape record, with the box mapped too. */
export function mapShape(b: Brush, map: (p: Vec3) => Vec3): Brush {
  const s = shapeOf(b)
  return s ? { ...b, shape: { ...s, frame: mapFrame(s.frame, map) } } : b
}

/** A face of the rebuilt part: the old face with the same id keeps its look (stored UVs dropped, so its projection applies); a new face copies face 0. */
function restyle(fresh: Brush, old: Brush | undefined): Brush {
  const ref = old?.faces[0]
  const faces = fresh.faces.map((f): Face => {
    const was = old?.faces.find((x) => x.id === f.id) ?? ref
    if (!was) return f
    const { uvs: _u, loop: _l, id: _i, ...look } = was
    void _u; void _l; void _i
    return { ...look, id: f.id, loop: f.loop }
  })
  return alignBrush({ ...fresh, faces })
}

export interface ShapePatch { params?: Partial<ShapeParams>; size?: { du?: number; dv?: number; h?: number }; turn?: Turn }

/**
 * Rebuilds the shape `group` of entity `entityId` with `patch` applied: part i keeps the id of the old part i (extra parts get `newId()`,
 * surplus parts are removed), faces keep their look by face id. Returns the new document (`doc`) or an error (the old shape is then untouched).
 */
export function regenerateShape(doc: MapDocument, entityId: string, group: string, patch: ShapePatch, newId: () => string): { doc: MapDocument } | { error: string } {
  const e = doc.entities.find((x) => x.id === entityId)
  const old = (e?.brushes ?? []).map((b) => ({ b, s: shapeOf(b) })).filter((x) => x.s?.group === group).sort((a, c) => a.s!.part - c.s!.part)
  if (!e || !old.length) return { error: 'That shape no longer exists' }
  const s = old[0].s!
  const frame: Frame = { ...s.frame, ...(patch.size ?? {}) }
  if (!(frame.du > 1e-4 && frame.dv > 1e-4 && frame.h > 1e-4)) return { error: 'A shape needs a size above zero on every side' }
  const params = { ...s.params, ...(patch.params ?? {}) }, turn = patch.turn ?? s.turn
  const built = partsOf(s.kind, frame, params, turn)
  if ('error' in built) return built
  const parts = attachShape(built, s.kind, frame, { ...defaultShapeParams(), ...params }, turn, group).map((p, i) => {
    const was = old[i]?.b
    const b = restyle({ ...p, id: was?.id ?? newId(), ...(was?.op ? { op: was.op } : {}), ...(was?.carve ? { carve: was.carve } : {}) }, was)
    return b
  })
  // the rebuilt parts take the place of the first old part; the other old parts go
  const oldIds = new Set(old.map((x) => x.b.id))
  const brushes: Brush[] = []
  let placed = false
  for (const b of e.brushes ?? []) {
    if (!oldIds.has(b.id)) { brushes.push(b); continue }
    if (!placed) { brushes.push(...parts); placed = true }
  }
  const ne: Entity = { ...e, brushes }
  return { doc: { ...doc, entities: doc.entities.map((x) => (x.id === entityId ? ne : x)) } }
}

const close = (a: number, b: number, eps = 1e-6): boolean => Math.abs(a - b) < eps
const sameFrame = (a: Frame, b: Frame): boolean =>
  [0, 1, 2].every((k) => close(a.o[k], b.o[k]) && close(a.u[k], b.u[k]) && close(a.v[k], b.v[k]) && close(a.n[k], b.n[k])) && close(a.du, b.du) && close(a.dv, b.dv) && close(a.h, b.h)

const sameGeometry = (a: Brush, b: Brush, eps = 1e-6): boolean =>
  a.vertices.length === b.vertices.length && a.vertices.every((p, i) => Math.abs(p[0] - b.vertices[i][0]) < eps && Math.abs(p[1] - b.vertices[i][1]) < eps && Math.abs(p[2] - b.vertices[i][2]) < eps)

/**
 * Keeps shape records honest after an edit (`prev` -> `next`): a group must sit whole in one entity (every part index once, the same kind
 * and part count), and every part that changed must still be exactly what its record builds; otherwise the record is dropped from the whole
 * group (the shape becomes ordinary brushes). Parts that were not in the group before (a duplicate) get a new group id from `freshGroup`.
 * Returns `next` itself when nothing had to change.
 */
export function reconcileShapes(prev: MapDocument | null, next: MapDocument, freshGroup: () => string): MapDocument {
  const prevMembers = new Map<string, Set<string>>(), prevBrush = new Map<string, Brush>()
  for (const e of prev?.entities ?? []) for (const b of e.brushes ?? []) {
    prevBrush.set(`${e.id}|${b.id}`, b)
    const s = shapeOf(b)
    if (s) { const m = prevMembers.get(s.group) ?? new Set<string>(); m.add(`${e.id}|${b.id}`); prevMembers.set(s.group, m) }
  }
  let changed = false
  const entities = next.entities.map((e) => {
    const brushes = e.brushes ?? []
    if (!brushes.some((b) => b.shape !== undefined)) return e
    const groups = new Map<string, Brush[]>()
    const strip = new Set<string>(), regroup = new Map<string, string>()
    for (const b of brushes) {
      if (b.shape === undefined) continue
      const s = shapeOf(b)
      if (!s) { strip.add(b.id); continue }
      const l = groups.get(s.group) ?? []; l.push(b); groups.set(s.group, l)
    }
    const valid = (list: Brush[]): boolean => {
      const specs = list.map((b) => shapeOf(b)!), s0 = specs[0]
      if (specs.some((s) => s.kind !== s0.kind || s.parts !== s0.parts) || list.length !== s0.parts) return false
      if (new Set(specs.map((s) => s.part)).size !== list.length) return false
      // one box, one set of parameters, one orientation: a part moved or edited on its own breaks the shape
      if (specs.some((s) => s.turn !== s0.turn || !sameFrame(s.frame, s0.frame) || JSON.stringify(s.params) !== JSON.stringify(s0.params))) return false
      let expected: Brush[] | null = null
      for (const [i, b] of list.entries()) {
        if (prevBrush.get(`${e.id}|${b.id}`) === b) continue // untouched by this edit
        if (!expected) { const r = partsOf(specs[i].kind, specs[i].frame, specs[i].params, specs[i].turn); if ('error' in r) return false; expected = r }
        const want = expected[specs[i].part]
        if (!want || !sameGeometry(want, b)) return false
      }
      return true
    }
    for (const [g, list] of groups) {
      const was = prevMembers.get(g)
      const own = was ? list.filter((b) => was.has(`${e.id}|${b.id}`)) : list
      const copies = was ? list.filter((b) => !was.has(`${e.id}|${b.id}`)) : []
      // a group that is wholly new here (drawn, pasted, moved in from another entity) counts as its own
      const mine = own.length ? own : copies, extra = own.length ? copies : []
      if (!valid(mine)) for (const b of mine) strip.add(b.id)
      if (extra.length) {
        if (valid(extra)) { const ng = freshGroup(); for (const b of extra) regroup.set(b.id, ng) } else for (const b of extra) strip.add(b.id)
      }
    }
    if (!strip.size && !regroup.size) return e
    changed = true
    return { ...e, brushes: brushes.map((b) => (strip.has(b.id) ? withoutShape(b) : regroup.has(b.id) ? { ...b, shape: { ...shapeOf(b)!, group: regroup.get(b.id)! } } : b)) }
  })
  return changed ? { ...next, entities } : next
}

/** Ids of every part of the shape `brushId` belongs to (in part order), or just [brushId] when it is no shape. */
export function shapeMembers(e: Entity | undefined, brushId: string): string[] {
  const s = e?.brushes?.find((b) => b.id === brushId)
  const g = s ? shapeOf(s)?.group : undefined
  if (!e || !g) return [brushId]
  return (e.brushes ?? []).map((b) => ({ b, s: shapeOf(b) })).filter((x) => x.s?.group === g).sort((a, c) => a.s!.part - c.s!.part).map((x) => x.b.id)
}
