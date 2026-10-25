// Hide / Unhide (REQUIREMENTS §24 C4): a view setting, not part of the map (not saved, not undone). Hidden things are neither drawn nor picked.
// Keys: `e:<entityId>` hides a whole entity, `b:<entityId>|<brushId>` one brush, `i:<instanceId>` a prefab instance with everything in it.
import type { Entity } from './document'
import { isBrushEntity } from './document'

export const entityKey = (entityId: string): string => `e:${entityId}`
export const instanceKey = (instanceId: string): string => `i:${instanceId}`
export const brushKey = (entityId: string, brushId: string): string => `b:${entityId}|${brushId}`

// ---- hide rules: "hide every entity whose class matches" (view setting like the keys above) -----------------------------
/** `pattern` is a class name; `*` matches any run of characters (`trigger_*` = every class starting with trigger_). Case-insensitive. */
export interface HideRule { pattern: string; enabled: boolean; /** added by the user (can be removed); presets only toggle */ custom?: boolean }

export const PRESET_RULES: readonly HideRule[] = [
  { pattern: 'trigger_*', enabled: false },
  { pattern: 'editor_playable_area', enabled: false },
]
export const defaultHideRules = (): HideRule[] => PRESET_RULES.map((r) => ({ ...r }))

export const normalizePattern = (s: string): string => s.trim().toLowerCase().replace(/\s+/g, '').replace(/\*+/g, '*')

/** A function telling whether a class name is matched by one of the enabled rules. worldspawn (the map itself) is never matched by a rule. */
export function ruleMatcher(rules: readonly HideRule[] | undefined): (classname: string | undefined) => boolean {
  const on = (rules ?? []).filter((r) => r.enabled && normalizePattern(r.pattern))
  if (!on.length) return () => false
  const res = on.map((r) => new RegExp(`^${normalizePattern(r.pattern).split('*').map((x) => x.replace(/[.+?^${}()|[\]\\]/g, '\\$&')).join('.*')}$`))
  return (c) => { const n = (c ?? '').toLowerCase(); return n !== 'worldspawn' && res.some((re) => re.test(n)) }
}

/** Rules from storage (anything may be in there): the presets are always present (their on/off kept), custom rules kept when they are well-formed and not duplicated. */
export function restoreHideRules(raw: unknown): HideRule[] {
  const list = Array.isArray(raw) ? raw : []
  const rules = defaultHideRules()
  for (const x of list) {
    if (typeof x !== 'object' || x === null) continue
    const { pattern, enabled } = x as { pattern?: unknown; enabled?: unknown }
    if (typeof pattern !== 'string' || typeof enabled !== 'boolean') continue
    const p = normalizePattern(pattern)
    if (!p || p.length > 80) continue
    const own = rules.find((r) => normalizePattern(r.pattern) === p)
    if (own) own.enabled = enabled
    else rules.push({ pattern: p, enabled, custom: true })
  }
  return rules
}

/** The patterns of the rules, normalized, in order: what the game profile's `hideGroups` holds (whether a rule is on stays each developer's own). */
export const rulePatterns = (rules: readonly HideRule[]): string[] => rules.map((r) => normalizePattern(r.pattern))

/** The rules after the game profile's shared groups arrived: its list is the list (presets always stay), on/off of a pattern we already had is kept, new ones start off. */
export function rulesFromGroups(groups: readonly string[], current: readonly HideRule[]): HideRule[] {
  const on = new Map(current.map((r) => [normalizePattern(r.pattern), r.enabled]))
  return restoreHideRules(groups.map((pattern) => ({ pattern, enabled: on.get(normalizePattern(pattern)) ?? false })))
}

/** The rule that hides `classname` (first enabled match), for the menu's "hidden by ..." note. */
export function matchingRule(classname: string, rules: readonly HideRule[]): HideRule | undefined {
  return rules.find((r) => ruleMatcher([r])(classname))
}

/** Rules after adding `pattern` (enabled). An existing rule with that pattern is enabled instead of duplicated. Empty patterns are ignored. */
export function addRule(rules: readonly HideRule[], pattern: string): HideRule[] {
  const p = normalizePattern(pattern)
  if (!p) return [...rules]
  if (rules.some((r) => normalizePattern(r.pattern) === p)) return rules.map((r) => (normalizePattern(r.pattern) === p ? { ...r, enabled: true } : r))
  return [...rules, { pattern: p, enabled: true, custom: true }]
}

/** The distinct classes of brush entities (volumes, groups, ...) in the map with how many entities use each; worldspawn left out. Sorted: trigger_/editor_ first, then by name. */
export function volumeClasses(entities: readonly Entity[]): { classname: string; count: number }[] {
  const n = new Map<string, number>()
  for (const e of entities) {
    const c = e.classname ?? ''
    if (!c || c === 'worldspawn' || !isBrushEntity(e)) continue
    n.set(c, (n.get(c) ?? 0) + 1)
  }
  const rank = (c: string) => (c.startsWith('trigger_') ? 0 : c.startsWith('editor_') ? 1 : 2)
  return [...n].map(([classname, count]) => ({ classname, count })).sort((a, b) => rank(a.classname) - rank(b.classname) || a.classname.localeCompare(b.classname))
}

/** The entities as the views see them: hidden entities left out, hidden brushes removed (an entity whose brushes are all hidden is left out too); entities of a hidden class (rules) are left out. */
export function visibleEntities(entities: Entity[], hidden: readonly string[], rules?: readonly HideRule[]): Entity[] {
  const ruled = ruleMatcher(rules)
  if (!hidden.length && !rules?.some((r) => r.enabled)) return entities
  const h = new Set(hidden)
  const out: Entity[] = []
  for (const e of entities) {
    if (h.has(entityKey(e.id)) || ruled(e.classname)) continue
    if (!e.brushes?.length) { out.push(e); continue }
    const brushes = e.brushes.filter((b) => !h.has(brushKey(e.id, b.id)))
    if (brushes.length === e.brushes.length) out.push(e)
    else if (brushes.length) out.push({ ...e, brushes })
  }
  return out
}

/** Keys that hide a selection: a selected brush hides that brush, an entity selected as a whole hides the entity. */
export function hideKeys(sel: { entityId: string; brushId?: string; instanceId?: string }[]): string[] {
  const out: string[] = []
  for (const s of sel) {
    if (s.instanceId !== undefined) { out.push(instanceKey(s.instanceId)); continue }
    if (!s.entityId) continue
    out.push(s.brushId ? brushKey(s.entityId, s.brushId) : entityKey(s.entityId))
  }
  return out
}

/** The prefab content as the views see it: content of a hidden instance (outliner eye, Hide) and content of a hidden class (rules) left out. `instancePath[1]` is the top-level instance. */
export function visiblePrefabs<T extends { entity: Entity; instancePath: string[] }>(prefabs: readonly T[], hidden: readonly string[], rules?: readonly HideRule[]): T[] {
  const ruled = ruleMatcher(rules)
  const h = new Set(hidden)
  if (!h.size && !rules?.some((r) => r.enabled)) return prefabs as T[]
  return prefabs.filter((p) => !h.has(instanceKey(p.instancePath[1])) && !ruled(p.entity.classname))
}

/** What the outliner shows for an entity: hidden by its own key, by a class rule, or (for a brush) through its entity. */
export function hiddenState(hidden: ReadonlySet<string>, ruled: (c: string | undefined) => boolean, e: Entity, brushId?: string): 'no' | 'self' | 'rule' | 'parent' {
  if (brushId !== undefined) {
    if (hidden.has(entityKey(e.id)) || ruled(e.classname)) return 'parent'
    return hidden.has(brushKey(e.id, brushId)) ? 'self' : 'no'
  }
  if (hidden.has(entityKey(e.id))) return 'self'
  return ruled(e.classname) ? 'rule' : 'no'
}
