// "Turn into prefab": the selection becomes a prefab file (a map document of its own, centred on the selection's pivot)
// and is replaced in the level by ONE instance of that file at the pivot, so nothing moves on screen.
// Pure document functions; the file picking and writing is in app.ts.
import {
  Brush, Entity, MapDocument, PrefabInstance, RefValue, Vec3, refPaths, refValueOf, addInstance, classOf, findEntity, findInstance, newId, newInstanceName, removeBrush, removeEntity, removeInstance,
} from '../model/document'
import { translateBrush } from '../model/geometry'
import { selectionPivot } from '../model/transform'
import type { Selection } from './store'

export interface PrefabExtract { prefab: MapDocument; pivot: Vec3 }

const r3 = (n: number): number => Math.round(n * 1000) / 1000
const clone = <T>(v: T): T => JSON.parse(JSON.stringify(v)) as T
const shift = (p: Vec3 | undefined, pivot: Vec3): Vec3 => [r3((p?.[0] ?? 0) - pivot[0]), r3((p?.[1] ?? 0) - pivot[1]), r3((p?.[2] ?? 0) - pivot[2])]
const moveBrush = (b: Brush, pivot: Vec3): Brush => translateBrush(clone(b), [-pivot[0], -pivot[1], -pivot[2]])

/** The selection as a prefab document with its pivot at the origin; null when nothing selected exists. Entity ids are kept (so refs between the selected entities still work); refs to things left behind are dropped. */
export function extractPrefab(doc: MapDocument, sels: readonly Selection[]): PrefabExtract | null {
  if (!sels.length) return null
  const c = selectionPivot(doc, [...sels])
  if (!c) return null
  const pivot: Vec3 = [r3(c[0]), r3(c[1]), r3(c[2])]
  const whole = new Set(sels.filter((s) => s.instanceId === undefined && !s.brushId).map((s) => s.entityId))
  const parts = new Map<string, Set<string>>() // entity id -> picked brush ids (entities that are not selected as a whole)
  for (const s of sels) if (s.instanceId === undefined && s.brushId && !whole.has(s.entityId)) (parts.get(s.entityId) ?? parts.set(s.entityId, new Set()).get(s.entityId)!).add(s.brushId)
  const entities: Entity[] = []
  for (const e of doc.entities) {
    const picked = whole.has(e.id) ? null : parts.get(e.id)
    if (!whole.has(e.id) && !picked) continue
    const brushes = (e.brushes ?? []).filter((b) => !picked || picked.has(b.id)).map((b) => moveBrush(b, pivot))
    if (picked && !brushes.length) continue
    const out: Entity = { ...clone(e), ...(e.origin || classOf(e) !== 'worldspawn' ? { origin: shift(e.origin, pivot) } : {}), ...(e.brushes ? { brushes } : {}) }
    entities.push(out)
  }
  const instances: PrefabInstance[] = []
  for (const s of sels) {
    if (s.instanceId === undefined) continue
    const p = findInstance(doc, s.instanceId)
    if (p && !instances.some((x) => x.id === p.id)) instances.push({ ...clone(p), origin: shift(p.origin, pivot) })
  }
  if (!entities.length && !instances.length) return null
  const ids = new Set(entities.map((e) => e.id))
  for (const e of entities) {
    if (!e.refs) continue
    const refs: Record<string, RefValue> = {}
    for (const [k, v] of Object.entries(e.refs)) { const kept = refValueOf(refPaths(v).filter((t) => ids.has(t))); if (kept !== undefined) refs[k] = kept }
    if (Object.keys(refs).length) e.refs = refs; else delete e.refs
  }
  const { entities: _e, prefabInstances: _p, ...rest } = doc
  void _e; void _p
  return { prefab: { ...rest, entities, prefabInstances: instances }, pivot }
}

/** The level after the swap: the selection removed, one instance of `file` at `pivot` added. */
export function replaceWithInstance(doc: MapDocument, sels: readonly Selection[], file: string, pivot: Vec3): { doc: MapDocument; instanceId: string } {
  const whole = new Set(sels.filter((s) => s.instanceId === undefined && !s.brushId).map((s) => s.entityId))
  let out = doc
  for (const s of sels) {
    if (s.instanceId !== undefined) { out = removeInstance(out, s.instanceId); continue }
    if (s.brushId && !whole.has(s.entityId)) out = removeBrush(out, s.entityId, s.brushId)
  }
  for (const id of whole) if (findEntity(out, id)) out = removeEntity(out, id)
  const instanceId = newId('pi')
  out = addInstance(out, { id: instanceId, name: newInstanceName(out.prefabInstances, file), file, origin: pivot, angles: [0, 0, 0] })
  return { doc: out, instanceId }
}
