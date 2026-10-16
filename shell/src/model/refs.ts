// Reference targets for the picker (DESIGN 18.1): a reference is a scope-relative PATH of ids, never a typed name.
// All paths here are relative to the document being edited ("root").
import type { Entity, MapDocument, PrefabInstance } from './document'
import { instanceNames } from './document'
import { Library, ResolvedEntity, normalizePath } from './expand'

export interface RefTarget {
  /** "entityId" for the document's own entities, "inst/child/entityId" for prefab content. */
  path: string
  /** What the user sees: the entity's name (or class) plus its id. */
  label: string
  /** "" for the document's own entities, otherwise the instance chain "door_frame|hinge". */
  group: string
  /** The name the compiler will give it (`group|targetname`), or '' when unnamed. */
  resolvedName: string
}

const nameOf = (e: Entity): string => e.keyvalues?.targetname ?? ''
const labelOf = (e: Entity, id: string): string => `${nameOf(e) || e.classname || 'entity'}  (${id})`

/** Effective names of the instance chain `ids` (instance ids below the document root), walking into the library for nested ones. */
export function chainNames(doc: MapDocument, library: Library, ids: readonly string[]): string[] {
  const out: string[] = []
  let cur: MapDocument | undefined = doc
  for (const id of ids) {
    if (!cur) { out.push(id); continue }
    const list: PrefabInstance[] = cur.prefabInstances
    const i = list.findIndex((p) => p.id === id)
    if (i < 0) { out.push(id); cur = undefined; continue }
    out.push(instanceNames(list)[i])
    const lib: MapDocument | { error: string } | undefined = library[normalizePath(list[i].file)]
    cur = lib && !('error' in lib) ? lib : undefined
  }
  return out
}

/** Every entity a reference can point at: the document's own, then prefab content grouped by instance. */
export function refTargets(doc: MapDocument, prefab: readonly ResolvedEntity[], library: Library): RefTarget[] {
  const out: RefTarget[] = doc.entities.map((e) => ({ path: e.id, label: labelOf(e, e.id), group: '', resolvedName: nameOf(e) }))
  for (const r of prefab) {
    const ids = r.instancePath.slice(1)
    const names = chainNames(doc, library, ids)
    const group = names.join('|')
    const own = nameOf(r.entity)
    out.push({ path: [...ids, r.sourceEntityId].join('/'), label: labelOf(r.entity, r.sourceEntityId), group, resolvedName: own ? `${group}|${own}` : '' })
  }
  return out
}

/** The root-relative path of the entity a viewport hit belongs to (the pipette). */
export function pathOfHit(entity: Entity, doc: MapDocument, prefab: readonly ResolvedEntity[]): string | null {
  if (doc.entities.includes(entity)) return entity.id
  const r = prefab.find((p) => p.entity === entity)
  return r ? [...r.instancePath.slice(1), r.sourceEntityId].join('/') : null
}

/** Reference keys of an entity: the built-in ones plus any key its class definition types as a target. */
export function refKeysOf(def: { keys: { key: string; type: string }[] } | undefined, present: Iterable<string> = []): string[] {
  const keys = new Set<string>(['target', 'killtarget'])
  for (const k of def?.keys ?? []) if (k.type === 'target_destination' || k.type === 'target') keys.add(k.key)
  for (const k of present) keys.add(k)
  return [...keys]
}
