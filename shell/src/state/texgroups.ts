// Texture group cache (REQUIREMENTS §20). Like TextureCache: loaded on first request, shared by every viewport; the pure functions (model/uvmesh.ts, layoutPlan.ts)
// look groups up through `groupEnv`. A group that is loaded, saved or removed bumps the environment, which makes every cached mesh rebuild.
import { GroupParse, TexGroup, groupEnv, parseGroup } from '../model/texgroup'

export type GroupState =
  | { status: 'loading' }
  | { status: 'missing'; error: string }
  | { status: 'ready'; raw: unknown; parse: GroupParse; version: number }

export type FetchGroup = (path: string) => Promise<unknown>

export class TexGroupCache {
  private entries = new Map<string, GroupState>()
  private listeners = new Set<() => void>()
  private counter = 0

  /** `changed` is called after every change so the textures' listeners (viewports, panels) redraw and mesh caches rebuild (TextureCache.notify). */
  constructor(private fetchGroup: FetchGroup, private changed: () => void = () => {}) {
    groupEnv.setProvider((p) => this.group(p), (p) => { const e = this.entries.get(p); return e?.status === 'ready' ? String(e.version) : '' })
  }

  /** The usable group, or undefined while it loads / when it is missing or has no usable root (the lookup starts the load). */
  group(path: string): TexGroup | undefined {
    const st = this.state(path)
    return st.status === 'ready' ? st.parse.group ?? undefined : undefined
  }

  state(path: string): GroupState {
    let e = this.entries.get(path)
    if (!e) { e = { status: 'loading' }; this.entries.set(path, e); void this.load(path) }
    return e
  }

  private async load(path: string): Promise<void> {
    try {
      const raw = await this.fetchGroup(path)
      this.entries.set(path, { status: 'ready', raw, parse: parseGroup(raw), version: ++this.counter })
    } catch (e) {
      this.entries.set(path, { status: 'missing', error: (e as Error).message })
    }
    this.fire()
  }

  /** The editor saved a group (the caller wrote it to Core): kept in memory at once so every viewport shows it. */
  set(path: string, raw: unknown): void {
    this.entries.set(path, { status: 'ready', raw, parse: parseGroup(raw), version: ++this.counter })
    this.fire()
  }
  remove(path: string): void { this.entries.delete(path); this.fire() }
  /** Forget everything (the assets root changed). */
  clear(): void { this.entries.clear(); this.fire() }

  onChange(fn: () => void): () => void { this.listeners.add(fn); return () => void this.listeners.delete(fn) }
  private fire(): void { groupEnv.bump(); this.changed(); this.listeners.forEach((f) => f()) }
}
