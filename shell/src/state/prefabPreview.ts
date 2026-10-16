// Prefab content for previews (icons and the drag ghost). The document's own prefab library only holds
// files the map already uses, so a prefab you are about to place is read from Core on demand, together
// with whatever it nests, and expanded at the origin exactly like a real instance would be.
import type { Entity, MapDocument } from '../model/document'
import { Library, expandPrefabs, normalizePath } from '../model/expand'

export type ReadPrefab = (path: string) => Promise<MapDocument>
type Slot = MapDocument | { error: string } | 'loading'
type Status = 'ready' | 'loading' | 'error'

export class PrefabPreviews {
  private slots = new Map<string, Slot>()
  private built = new Map<string, Entity[]>()
  private listeners = new Set<() => void>()
  /** `known` = files the open map already has loaded (used first, so those never cost a round trip). */
  constructor(private read: ReadPrefab, private known: () => Library = () => ({})) {}

  onChange(fn: () => void): () => void { this.listeners.add(fn); return () => void this.listeners.delete(fn) }

  /** Forget everything (after the assets folder changed, or on request when files changed on disk). */
  clear(): void { this.slots.clear(); this.built.clear(); this.listeners.forEach((f) => f()) }

  /** The prefab's content at the origin, or null while it (or something it nests) is still loading, or when it cannot be read. */
  entities(file: string): Entity[] | null {
    const key = normalizePath(file)
    const have = this.built.get(key)
    if (have) return have
    if (this.resolve(key, new Set()) !== 'ready') return null
    const lib: Library = {}
    for (const [k, v] of this.slots) if (v !== 'loading') lib[k] = v
    const doc: MapDocument = { schemaVersion: '0.2', entities: [], prefabInstances: [{ id: 'preview', file: key, origin: [0, 0, 0], angles: [0, 0, 0] }] }
    const out = expandPrefabs(doc, lib).entities.map((r) => r.entity)
    this.built.set(key, out)
    return out
  }

  /** Why a prefab cannot be previewed, if it cannot. */
  error(file: string): string | null {
    const s = this.slots.get(normalizePath(file))
    return s && s !== 'loading' && 'error' in s ? String(s.error) : null
  }

  private resolve(key: string, seen: Set<string>): Status {
    let slot = this.slots.get(key)
    if (slot === undefined) {
      const k = this.known()[key]
      if (k && !('error' in k)) { slot = k; this.slots.set(key, k) }
      else {
        this.slots.set(key, 'loading')
        this.read(key).then(
          (doc) => { if (this.slots.get(key) === 'loading') { this.slots.set(key, doc); this.resolve(key, new Set()); this.listeners.forEach((f) => f()) } }, // resolve: start reading what it nests
          (e: unknown) => { if (this.slots.get(key) === 'loading') { this.slots.set(key, { error: (e as Error).message }); this.listeners.forEach((f) => f()) } },
        )
        return 'loading'
      }
    }
    if (slot === 'loading') return 'loading'
    if ('error' in slot) return 'error'
    seen.add(key)
    let status: Status = 'ready'
    for (const inst of slot.prefabInstances ?? []) {
      const child = normalizePath(inst.file)
      if (seen.has(child)) continue // a cycle: expandPrefabs reports it, the preview just skips it
      if (this.resolve(child, seen) === 'loading') status = 'loading'
    }
    return status
  }
}
