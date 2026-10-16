// Types for document schema 0.2 (mirrors core/include/core/document.h) plus the pure,
// immutable edit helpers the Shell uses. Every edit returns a NEW document that shares
// all untouched parts with the old one, so identity comparisons are cheap (undo history,
// GPU mesh caches keyed by brush object, "did anything change?").
//
// The index signatures are deliberate: Core preserves fields it doesn't know about, and
// so must we -- never rebuild an object from known fields only.
import { Vec3, add } from './math'
import { brushBounds, translateBrush } from './geometry'

export type { Vec3 }
export const SCHEMA_VERSION = '0.2'

export interface Face { id: number; loop: number[]; uvs?: [number, number][]; texture?: string; /** lightmap texel density relative to the level's (absent = 1); see lightmapScaleOf */ lightmapScale?: number; [k: string]: unknown }
export interface Carve { self?: boolean; structural?: boolean; nonStructural?: boolean }
export interface Brush {
  id: string
  op?: 'add' | 'subtract'
  carve?: Carve
  vertices: Vec3[]
  faces: Face[]
  [k: string]: unknown
}
export interface Entity {
  id: string
  classname?: string
  keyvalues?: Record<string, string>
  origin?: Vec3
  angles?: Vec3
  /** Reference keys (target, killtarget...) -> scope-relative path "entityId" | "childInstanceId/entityId" (DESIGN 18.1). */
  refs?: Record<string, string>
  brushes?: Brush[]
  [k: string]: unknown
}
/** What an instance stores against ONE entity of its prefab file; only differences are stored (DESIGN 18.1a). */
export interface EntityOverride {
  keyvalues?: Record<string, string>
  refs?: Record<string, string>
  origin?: Vec3
  angles?: Vec3
  scale?: Vec3
  deleted?: boolean
}
export interface PrefabInstance {
  id: string
  /** Optional; missing = derived from the file name (see instanceNames). Unique among siblings. */
  name?: string
  file: string
  origin?: Vec3
  angles?: Vec3
  scale?: Vec3
  /** key = entity path relative to this instance: "entityId" | "childInstanceId/entityId"... */
  overrides?: Record<string, EntityOverride>
  [k: string]: unknown
}
export interface MapDocument {
  schemaVersion: string
  entities: Entity[]
  prefabInstances: PrefabInstance[]
  [k: string]: unknown
}

// --- class rules (mirror core/src/entity_class.cpp) -------------------------------
const STRUCTURAL = new Set(['worldspawn', 'func_detail', 'func_group'])
const CONVEX = new Set(['worldspawn', 'func_group'])
export const classOf = (e: Entity): string => e.classname ?? 'point_entity'
export const isStructural = (c: string): boolean => STRUCTURAL.has(c)
export const requiresConvexBrushes = (c: string): boolean => CONVEX.has(c)
export const isBrushEntity = (e: Entity): boolean => classOf(e) === 'worldspawn' || (e.brushes?.length ?? 0) > 0
export const showsCustomOrigin = (e: Entity): boolean => isBrushEntity(e) && !isStructural(classOf(e))
/** Centre of the bounding box of all the given brushes (subtractive ones included); null when there is no geometry. The default origin of a new brush entity. */
export function brushesCentre(brushes: Brush[]): Vec3 | null {
  let min: Vec3 = [Infinity, Infinity, Infinity], max: Vec3 = [-Infinity, -Infinity, -Infinity], any = false
  for (const b of brushes) {
    const bb = brushBounds(b)
    if (!bb) continue
    any = true
    for (let i = 0; i < 3; i++) { min[i] = Math.min(min[i], bb.min[i]); max[i] = Math.max(max[i], bb.max[i]) }
  }
  return any ? [(min[0] + max[0]) / 2, (min[1] + max[1]) / 2, (min[2] + max[2]) / 2] : null
}
export const originOf = (e: Entity): Vec3 => e.origin ?? [0, 0, 0]

export const emptyDocument = (): MapDocument => ({ schemaVersion: SCHEMA_VERSION, entities: [], prefabInstances: [] })

export function newId(prefix: string): string {
  const b = new Uint8Array(4)
  crypto.getRandomValues(b)
  return `${prefix}_${[...b].map((x) => x.toString(16).padStart(2, '0')).join('')}`
}

// --- prefab instance names (mirror core/src/document.cpp effectiveInstanceNames) ------------
/** `prefabs/sub/door.map.json` -> `door` */
export function prefabBaseName(file: string): string {
  let f = file.replace(/\\/g, '/')
  f = f.slice(f.lastIndexOf('/') + 1)
  for (const ext of ['.map.json', '.json']) if (f.length > ext.length && f.endsWith(ext)) { f = f.slice(0, -ext.length); break }
  return f || 'prefab'
}
/** Names of the instances, same order: the explicit name, or a derived base, base2, base3... that never collides. */
export function instanceNames(instances: readonly PrefabInstance[]): string[] {
  const taken = new Set(instances.filter((p) => p.name).map((p) => p.name as string))
  return instances.map((p) => {
    if (p.name) return p.name
    const base = prefabBaseName(p.file)
    let cand = base
    for (let n = 2; taken.has(cand); n++) cand = base + n
    taken.add(cand)
    return cand
  })
}
/** The name a NEW instance of `file` should be given: the first free base, base2, base3... */
export function newInstanceName(instances: readonly PrefabInstance[], file: string): string {
  const taken = new Set(instanceNames(instances))
  const base = prefabBaseName(file)
  let cand = base
  for (let n = 2; taken.has(cand); n++) cand = base + n
  return cand
}
/** Why `name` cannot be used for the instance `selfId` (null = fine). Names are unique among siblings. */
export function instanceNameProblem(instances: readonly PrefabInstance[], selfId: string, name: string): string | null {
  if (!name.trim()) return 'a name cannot be empty'
  if (/[|/]/.test(name)) return "a name may not contain '|' or '/'"
  const names = instanceNames(instances)
  const clash = instances.findIndex((p, i) => p.id !== selfId && names[i] === name)
  return clash >= 0 ? `another instance is already called '${name}'` : null
}

// --- immutable edits -------------------------------------------------------------
export const findEntity = (doc: MapDocument, id: string): Entity | undefined => doc.entities.find((e) => e.id === id)

export function mapEntity(doc: MapDocument, id: string, fn: (e: Entity) => Entity): MapDocument {
  let changed = false
  const entities = doc.entities.map((e) => {
    if (e.id !== id) return e
    const n = fn(e)
    changed ||= n !== e
    return n
  })
  return changed ? { ...doc, entities } : doc
}

export function mapBrush(doc: MapDocument, entityId: string, brushId: string, fn: (b: Brush) => Brush): MapDocument {
  return mapEntity(doc, entityId, (e) => {
    let changed = false
    const brushes = (e.brushes ?? []).map((b) => {
      if (b.id !== brushId) return b
      const n = fn(b)
      changed ||= n !== b
      return n
    })
    return changed ? { ...e, brushes } : e
  })
}

export const findInstance = (doc: MapDocument, id: string): PrefabInstance | undefined => doc.prefabInstances.find((p) => p.id === id)
export function mapInstance(doc: MapDocument, id: string, fn: (p: PrefabInstance) => PrefabInstance): MapDocument {
  let changed = false
  const prefabInstances = doc.prefabInstances.map((p) => {
    if (p.id !== id) return p
    const n = fn(p)
    changed ||= n !== p
    return n
  })
  return changed ? { ...doc, prefabInstances } : doc
}
export const addInstance = (doc: MapDocument, p: PrefabInstance): MapDocument => ({ ...doc, prefabInstances: [...doc.prefabInstances, p] })
export const removeInstance = (doc: MapDocument, id: string): MapDocument => ({ ...doc, prefabInstances: doc.prefabInstances.filter((p) => p.id !== id) })

export const addEntity = (doc: MapDocument, e: Entity): MapDocument => ({ ...doc, entities: [...doc.entities, e] })
export const removeEntity = (doc: MapDocument, id: string): MapDocument => ({
  ...doc,
  entities: doc.entities.filter((e) => e.id !== id),
})
export const addBrush = (doc: MapDocument, entityId: string, brush: Brush): MapDocument =>
  mapEntity(doc, entityId, (e) => ({ ...e, brushes: [...(e.brushes ?? []), brush] }))
export const removeBrush = (doc: MapDocument, entityId: string, brushId: string): MapDocument =>
  mapEntity(doc, entityId, (e) => ({ ...e, brushes: (e.brushes ?? []).filter((b) => b.id !== brushId) }))

/** Takes `brush` out of entity `entityId` and puts it into the func_detail entity `detailId` (created on first use). */
export function moveBrushToDetail(doc: MapDocument, entityId: string, brushId: string, brush: Brush, detailId: string): MapDocument {
  const removed = removeBrush(doc, entityId, brushId)
  if (findEntity(removed, detailId)) return mapEntity(removed, detailId, (e) => ({ ...e, brushes: [...(e.brushes ?? []), brush] }))
  return addEntity(removed, { id: detailId, classname: 'func_detail', brushes: [brush], origin: brushesCentre([brush]) ?? [0, 0, 0] })
}

// Moving an entity moves its origin and all of its brushes (mirrors the old Core semantics).
export function translateEntity(doc: MapDocument, id: string, delta: Vec3): MapDocument {
  return mapEntity(doc, id, (e) => ({
    ...e,
    origin: add(originOf(e), delta),
    ...(e.brushes ? { brushes: e.brushes.map((b) => translateBrush(b, delta)) } : {}),
  }))
}
