// Hide / Unhide (REQUIREMENTS §24 C4): a view setting, not part of the map (not saved, not undone). Hidden things are neither drawn nor picked.
// Keys: `e:<entityId>` hides a whole entity, `b:<entityId>|<brushId>` one brush.
import type { Entity } from './document'

export const entityKey = (entityId: string): string => `e:${entityId}`
export const brushKey = (entityId: string, brushId: string): string => `b:${entityId}|${brushId}`

/** The entities as the views see them: hidden entities left out, hidden brushes removed (an entity whose brushes are all hidden is left out too). */
export function visibleEntities(entities: Entity[], hidden: readonly string[]): Entity[] {
  if (!hidden.length) return entities
  const h = new Set(hidden)
  const out: Entity[] = []
  for (const e of entities) {
    if (h.has(entityKey(e.id))) continue
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
    if (s.instanceId !== undefined || !s.entityId) continue  // prefab instances: not yet
    out.push(s.brushId ? brushKey(s.entityId, s.brushId) : entityKey(s.entityId))
  }
  return out
}
