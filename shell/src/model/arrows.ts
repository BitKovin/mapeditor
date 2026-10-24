// Entity arrows (REQUIREMENTS §17 items 1 and 2). Pure: everything returns flat line endpoints
// (x,y,z, x,y,z, ...) grouped by colour, so it can be tested without GL.
//
//  * direction arrow: every point entity points where it looks, from its box centre along
//    forward = R(angles) * (0,0,1) (the convention of DESIGN 18.1b);
//  * link arrows: from an entity to the entity one of its reference keys (`target`, `killtarget`, ...) names,
//    arrowhead at the target. References are resolved exactly like the picker stores them: a scope-relative
//    path of ids (`refs`), plus the older plain-name form (`keyvalues.target` = a targetname).
import type { Entity } from './document'
import { classOf, isBrushEntity, refPaths } from './document'
import type { Box } from './entityBox'
import type { ResolvedEntity } from './expand'
import { Vec3, add, cross, normalize, scale, sub } from './math'
import { boundsOfBox, pointBox, toWorld } from './pointBox'
import { apply3 } from './rotation'

/** An entity as the viewport sees it: the document's own (`ids` empty) or prefab content (`ids` = instance ids from the root). */
export interface Placed { entity: Entity; ids: string[]; sourceId: string; refScopes?: Record<string, number> }
export interface Overlay { color: [number, number, number]; lines: number[] }
export interface Link { from: Placed; key: string; to: Placed }

export const placedEntities = (docEntities: readonly Entity[], prefab: readonly ResolvedEntity[]): Placed[] => [
  ...docEntities.map((entity) => ({ entity, ids: [] as string[], sourceId: entity.id })),
  ...prefab.map((r) => ({ entity: r.entity, ids: r.instancePath.slice(1), sourceId: r.sourceEntityId, refScopes: r.refScopes })),
]
/** Root-relative path of an entity: the key references resolve to. */
export const pathOfPlaced = (p: Placed): string => [...p.ids, p.sourceId].join('/')

// ---- geometry -------------------------------------------------------------------------------------

/** Four-barbed head (reads as a cone from any side) at `tip`, pointing along `dir`, plus the shaft from `from`. */
export function arrowLines(from: Vec3, tip: Vec3, headLen: number): number[] {
  const d = sub(tip, from), len = Math.hypot(d[0], d[1], d[2])
  if (len < 1e-9) return []
  const dir = scale(d, 1 / len)
  const u = normalize(Math.abs(dir[1]) < 0.9 ? cross(dir, [0, 1, 0]) : cross(dir, [1, 0, 0]))
  const v = cross(dir, u)
  const back = sub(tip, scale(dir, headLen)), w = headLen * 0.4
  const out: number[] = [...from, ...tip]
  for (const s of [u, v, scale(u, -1), scale(v, -1)]) out.push(...tip, ...add(back, scale(s, w)))
  return out
}

const centreOf = (b: Box): Vec3 => [(b.min[0] + b.max[0]) / 2, (b.min[1] + b.max[1]) / 2, (b.min[2] + b.max[2]) / 2]

/** Distances along the ray o + t*dir where it enters and leaves the box, or null when it misses. */
function rayBoxSpan(o: Vec3, dir: Vec3, b: Box): { tin: number; tout: number } | null {
  let tin = -Infinity, tout = Infinity
  for (let i = 0; i < 3; i++) {
    if (Math.abs(dir[i]) < 1e-12) { if (o[i] < b.min[i] || o[i] > b.max[i]) return null; continue }
    const a = (b.min[i] - o[i]) / dir[i], c = (b.max[i] - o[i]) / dir[i]
    tin = Math.max(tin, Math.min(a, c)); tout = Math.min(tout, Math.max(a, c))
  }
  return tin <= tout ? { tin, tout } : null
}

/** Axis-aligned box of an entity in root space: a point entity's class box turned with its angles (the box around it), a brush entity's brushes' bounds. */
export function boundsOf(e: Entity, boxOf: (classname: string) => Box): Box {
  if (isBrushEntity(e)) {
    let min: Vec3 = [Infinity, Infinity, Infinity], max: Vec3 = [-Infinity, -Infinity, -Infinity]
    for (const b of e.brushes ?? []) for (const v of b.vertices) { min = [Math.min(min[0], v[0]), Math.min(min[1], v[1]), Math.min(min[2], v[2])]; max = [Math.max(max[0], v[0]), Math.max(max[1], v[1]), Math.max(max[2], v[2])] }
    if (min[0] <= max[0]) return { min, max }
  }
  return boundsOfBox(pointBox(e, boxOf))
}

/** Where the direction arrow of a point entity runs: from the centre of its (turned) box along `dir`, out through the box, a little beyond it. */
export function directionSpan(e: Entity, boxOf: (classname: string) => Box): { from: Vec3; dir: Vec3; len: number } {
  const ob = pointBox(e, boxOf)
  const local: Box = { min: ob.min, max: ob.max }
  const lc = centreOf(local), c = toWorld(ob, lc)
  // forward = R * (0,0,1) is +Z in the box's own frame, so the span through the box is measured there
  const span = rayBoxSpan(lc, [0, 0, 1], local)
  const dir = apply3(ob.rot, [0, 0, 1])
  const size = Math.max(ob.max[0] - ob.min[0], ob.max[1] - ob.min[1], ob.max[2] - ob.min[2])
  return { from: c, dir, len: Math.max((span ? span.tout : 0) + 0.6 * size, 0.3) }
}

/** Direction arrow of a point entity (see directionSpan). */
export function directionArrow(e: Entity, boxOf: (classname: string) => Box): number[] {
  const { from, dir, len } = directionSpan(e, boxOf)
  return arrowLines(from, add(from, scale(dir, len)), Math.max(len * 0.25, 0.08))
}

/** Link arrow between two entities: centre to centre, trimmed to the surfaces of both boxes (the head touches the target's box). */
export function linkArrow(from: Box, to: Box): number[] {
  const a = centreOf(from), b = centreOf(to)
  const d = sub(b, a), dist = Math.hypot(d[0], d[1], d[2])
  if (dist < 1e-6) return []
  const dir = scale(d, 1 / dist)
  const leave = rayBoxSpan(a, dir, from), enter = rayBoxSpan(a, dir, to)
  let t0 = leave ? Math.max(leave.tout, 0) : 0, t1 = enter ? enter.tin : dist
  if (!(t1 > t0 + 1e-6)) { t0 = 0; t1 = dist } // overlapping or touching boxes: centre to centre
  const len = t1 - t0
  return arrowLines(add(a, scale(dir, t0)), add(a, scale(dir, t1)), Math.min(0.4, Math.max(0.1, len * 0.2)))
}

// ---- references -----------------------------------------------------------------------------------

/** Every link between entities: `refKeys(classname)` names the reference keys of a class. `resolvedName` (default: the targetname) is what a plain-name key matches. */
export function resolveLinks(items: readonly Placed[], refKeys: (classname: string) => readonly string[], resolvedName?: (p: Placed) => string): Link[] {
  const byPath = new Map<string, Placed>()
  for (const p of items) byPath.set(pathOfPlaced(p), p)
  const nameOf = resolvedName ?? ((p: Placed) => p.entity.keyvalues?.targetname ?? '')
  let byName: Map<string, Placed[]> | null = null
  const nameIndex = (): Map<string, Placed[]> => {
    if (!byName) { byName = new Map(); for (const p of items) { const n = nameOf(p); if (n) byName.set(n, [...(byName.get(n) ?? []), p]) } }
    return byName
  }
  const out: Link[] = []
  for (const from of items) {
    const e = from.entity
    for (const key of refKeys(classOf(e))) {
      const refs = refPaths(e.refs?.[key])
      if (refs.length) {
        const scope = Math.min(from.refScopes?.[key] ?? from.ids.length, from.ids.length)
        for (const ref of refs) {
          const to = byPath.get([...from.ids.slice(0, scope), ref].join('/'))
          if (to && to !== from) out.push({ from, key, to })
        }
      } else {
        const name = e.keyvalues?.[key]
        if (name) {
          // a plain name reaches EVERY entity with that name: those of the same scope first (prefab-local names), else the ones with that global resolved name
          const sameScope = items.filter((q) => q.ids.length === from.ids.length && q.ids.every((x, i) => x === from.ids[i]) && q.entity.keyvalues?.targetname === name)
          for (const to of sameScope.length ? sameScope : nameIndex().get(name) ?? []) if (to !== from) out.push({ from, key, to })
        }
      }
    }
  }
  return out
}

// ---- everything the viewport draws ---------------------------------------------------------------------

export interface ArrowOptions {
  boxOf: (classname: string) => Box
  colorOf: (classname: string) => [number, number, number] | undefined
  isSelected: (p: Placed) => boolean
  refKeys: (classname: string) => readonly string[]
  resolvedName?: (p: Placed) => string
  showDirections: boolean
  /** false = only the links that touch a selected entity */
  showAllLinks: boolean
}
const DEFAULT_COLOR: [number, number, number] = [1, 0.85, 0.25]
export const LINK_COLOR: [number, number, number] = [0.72, 0.5, 0.95]
export const LINK_COLOR_SELECTED: [number, number, number] = [1, 0.95, 0.35]
const brighten = (c: [number, number, number]): [number, number, number] => [c[0] + (1 - c[0]) * 0.6, c[1] + (1 - c[1]) * 0.6, c[2] + (1 - c[2]) * 0.6]

export function entityArrows(items: readonly Placed[], o: ArrowOptions): Overlay[] {
  const groups = new Map<string, Overlay>()
  const add2 = (color: [number, number, number], lines: number[]) => {
    if (!lines.length) return
    const k = color.map((x) => x.toFixed(3)).join(',')
    let g = groups.get(k)
    if (!g) groups.set(k, (g = { color, lines: [] }))
    g.lines.push(...lines)
  }
  if (o.showDirections) {
    for (const p of items) {
      const e = p.entity
      if (isBrushEntity(e) || classOf(e) === 'worldspawn' || classOf(e) === 'decal') continue
      const c = o.colorOf(classOf(e)) ?? DEFAULT_COLOR
      add2(o.isSelected(p) ? brighten(c) : c, directionArrow(e, o.boxOf))
    }
  }
  for (const l of resolveLinks(items, o.refKeys, o.resolvedName)) {
    const sel = o.isSelected(l.from) || o.isSelected(l.to)
    if (!sel && !o.showAllLinks) continue
    add2(sel ? LINK_COLOR_SELECTED : LINK_COLOR, linkArrow(boundsOf(l.from.entity, o.boxOf), boundsOf(l.to.entity, o.boxOf)))
  }
  return [...groups.values()]
}
