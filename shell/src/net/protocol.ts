// Mirrors core/include/core/protocol.h. See DESIGN.md §10.
import type { MapDocument } from '../model/document'
import type { DecalEntry, DerivedEntry } from '../model/derived'
import type { Library } from '../model/expand'

export interface GameProfile {
  name: string; textureRoot: string; textureExtensions: string[]; modelExtensions: string[]
  /** All relative to the assets root. */
  entityDefinitions: string; mapRoot: string; prefabRoot: string; texGroupRoot?: string
  /** Texture pixels per meter when a texture has no properties of its own (default 64). */
  pixelsPerMeter?: number
}
/** One class from the game's .ent file (see core/include/core/entdef.h). */
/** `default` / `min` / `max` / `step` are the .ent's typed-editor hints: the game's value when the key is absent and the numeric range. */
export interface EntityKeyDef { key: string; type: string; name: string; description: string; items?: { name: string; value: string }[]; default?: string; min?: number; max?: number; step?: number }
export interface EntityFlagDef { bit: number; key: string; name: string; description: string }
export interface EntityClassDef {
  name: string; kind: 'point' | 'group'
  color: [number, number, number]
  /** Game units: min xyz, max xyz. */
  box: [number, number, number, number, number, number] | null
  description: string; keys: EntityKeyDef[]; flags: EntityFlagDef[]
}
export interface DirEntry { name: string; dir: boolean }
/** How File > Play map starts the game (per game folder; see core/include/core/settings.h GameLaunch). */
export interface GameLaunch { exe: string; args: string; workingDir: string }
/** What Core keeps in settings.json for the Shell (the `settings` message). `editor` is the Shell's own preferences (state/editorPrefs.ts). */
export interface CoreSettings {
  assetsRoot: string; recentMaps: string[]; game: GameLaunch; editor: Record<string, unknown>
  /** Core can show the operating system's file dialogs (native_dialog) */
  nativeDialogs: boolean; settingsFile: string; lightingFile: string
}
export interface DialogFilter { name: string; pattern: string }
export interface FolderEntry { name: string; path: string }
/** rgba: base64 of little endian float16 x 4 per pixel (linear, as baked; alpha 0 = no value). */
export interface LightmapTile { page: number; x: number; y: number; w: number; h: number; rgba: string }
export type RequestId = number | string | null
/** One surface piece of the lit mesh: the authored face it lies on and how many triangles it has (in vertex order). */
/** A face's lightmap mapping (sent with its first piece): lightmap uv = (dot(p, u.xyz) + u.w, dot(p, v.xyz) + v.w) on page `pg`, style layers as a bit mask `k`. */
export interface LitFaceMap { u: [number, number, number, number]; v: [number, number, number, number]; pg: number; k: [number, number, number] }
export interface LitFace { i: string; e: string; bi: string; b: string; f: number; t: string; n: number; m?: LitFaceMap }
/** What a brush entity that is not lightmapped is lit with, per light style layer (linear; the x2 range already decoded): sum over styles of value(style) * (ambient + directed * max(0, n . dir)).
 *  ONE direction per entity, shared by every layer (REQUIREMENTS §27 G4: the engine's light grid has one direction per point). `dir` on a layer is the entity's, filled in by the client. */
export interface EntityStyleLight { s: number; ambient: [number, number, number]; directed: [number, number, number]; dir: [number, number, number] }
export interface EntityLightEntry { i: string; e: string; dir: [number, number, number]; lights: EntityStyleLight[] }

export type Inbound =
  | { type: 'document_sync'; rev: number; path: string; doc: MapDocument; dirty?: boolean }
  | { type: 'update_ack'; rev: number }
  | { type: 'prefab_library'; prefabs: Library }
  | { type: 'profile'; profile: GameProfile; exporter?: string; exporters?: { id: string; name: string; extension: string }[] }
  | { type: 'entity_defs'; classes: EntityClassDef[]; warnings: string[] }
  | ({ type: 'settings' } & CoreSettings)
  | { type: 'lit_mesh'; rev: number; atlas: number; pageCount: number; triangleCount: number; /** the Core mirrors what we hold: only differing tiles follow and the pages stay */ keepPages?: boolean; vertices: string; faces?: LitFace[] }
  | { type: 'light_styles'; patterns: string[]; hz: number }
  | { type: 'lightvol'; rev: number; origin: [number, number, number]; cell: number; dims: [number, number, number]; probes: string; layers?: { s: number; probes: string }[] }
  | { type: 'entity_light'; rev: number; entities: EntityLightEntry[] }
  | { type: 'lightmap_tiles'; rev: number; atlas: number; /** light style layer, 0 = base */ style?: number; tiles: LightmapTile[] }
  | { type: 'lighting_status'; enabled: boolean; rev: number; chunks: number; stale: number; minPasses: number; maxPasses: number; converged: boolean; texels: number; pages: number; /** the light style layers that exist (0 = base first) */ styles?: number[]; styleWarnings?: string[]; /** display multiplier (Level settings exposure); absent = 1 */ exposure?: number; /** display offset added after the exposure (Level settings brightness); absent = 0 */ brightness?: number }
  | { type: 'derived_geometry'; rev: number; carved: DerivedEntry[]; skipped: { entityId: string; instance?: string; brushId: string }[]; decals: DecalEntry[] }
  | { type: 'response'; requestId: RequestId; ok: boolean; path?: string; rev?: number; error?: string; ext?: string; data?: string; props?: unknown; textures?: string[]; texgroups?: string[]; group?: unknown
      // read_texture of a skybox (REQUIREMENTS §28): its faces (lf rt up dn ft bk) or its panorama, as the engine loads them from env/
      sky?: { panorama: boolean; images: { ext: string; data: string }[] }
      // list_directory
      entries?: DirEntry[]
      // browse_filesystem (parent: null = top, '' = the Windows drive list)
      dirs?: FolderEntry[]; parent?: string | null; drives?: string[]; hasProfile?: boolean
      // read_prefab
      doc?: MapDocument
      // set_assets_root
      assetsRoot?: string; persisted?: boolean
      // native_dialog
      cancelled?: boolean; absolutePath?: string; unsupported?: boolean
      // read/write_game_profile (file = as written), read/write_lighting_config
      file?: unknown; profile?: GameProfile; config?: Record<string, number | boolean>; defaults?: Record<string, number | boolean>; warning?: string
      // launch_game
      commandLine?: string }
  | { type: 'error'; message: string }
  /** BSP export (REQUIREMENTS §25): progress about once a second, then the result. */
  | { type: 'export_progress'; stage: string; fraction: number }
  | { type: 'export_result'; ok: boolean; cancelled: boolean; error: string; warnings: string[]; bsp: string; lightmaps: string; seconds: number; bakeSeconds: number; device: string
      stats: { surfaces: number; droppedSurfaces: number; brushes: number; models: number; leafs: number; clusters: number; lightmapPages: number; gridPoints: number; palette: number; visCell: number; visSeconds: number } }

function isClassDef(c: any): c is EntityClassDef {
  return !!c && typeof c.name === 'string' && (c.kind === 'point' || c.kind === 'group') && Array.isArray(c.color) && c.color.length === 3
    && Array.isArray(c.keys) && Array.isArray(c.flags)
}

export function parseInbound(text: string): Inbound | null {
  let m: any
  try { m = JSON.parse(text) } catch { return null }
  if (!m || typeof m !== 'object') return null
  switch (m.type) {
    case 'document_sync':
      return typeof m.rev === 'number' && m.doc && Array.isArray(m.doc.entities) ? m : null
    case 'update_ack': return typeof m.rev === 'number' ? m : null
    case 'prefab_library': return m.prefabs && typeof m.prefabs === 'object' ? m : null
    case 'profile': return m.profile && typeof m.profile === 'object' ? m : null
    case 'entity_defs': return Array.isArray(m.classes) ? { type: 'entity_defs', classes: m.classes.filter(isClassDef), warnings: Array.isArray(m.warnings) ? m.warnings.map(String) : [] } : null
    case 'derived_geometry': {
      if (typeof m.rev !== 'number' || !Array.isArray(m.carved)) return null
      const okVerts = (v: unknown, min = 9) => Array.isArray(v) && v.length >= min && v.length % 3 === 0 && v.every((x) => typeof x === 'number' && Number.isFinite(x))
      const okEntry = (c: any) => c && typeof c.entityId === 'string' && typeof c.brushId === 'string' && Array.isArray(c.subtractors) && Array.isArray(c.fragments)
        && c.fragments.every((f: any) => f && typeof f.brush === 'string' && typeof f.face === 'number' && okVerts(f.v))
      const okDecal = (d: any) => d && typeof d.decalId === 'string' && typeof d.texture === 'string' && Array.isArray(d.fragments)
        && d.fragments.every((f: any) => f && typeof f.brush === 'string' && typeof f.entityId === 'string' && typeof f.face === 'number' && okVerts(f.v) && Array.isArray(f.uv) && f.uv.length === (f.v.length / 3) * 2 && f.uv.every((x: unknown) => typeof x === 'number' && Number.isFinite(x)))
      const inst = (x: any) => ({ ...x, instance: typeof x.instance === 'string' ? x.instance : '' })
      // per-subtractor instances only when they line up with the ids; a fragment's `instance` only when it is a string
      const entry = (c: any) => {
        const out = inst(c)
        if (!(Array.isArray(c.subtractorInstances) && c.subtractorInstances.length === c.subtractors.length && c.subtractorInstances.every((i: unknown) => typeof i === 'string'))) delete out.subtractorInstances
        out.fragments = c.fragments.map((f: any) => (typeof f.instance === 'string' ? f : (({ instance: _i, ...r }) => r)(f)))
        return out
      }
      return {
        type: 'derived_geometry', rev: m.rev,
        carved: m.carved.filter(okEntry).map(entry),
        skipped: Array.isArray(m.skipped) ? m.skipped : [],
        decals: Array.isArray(m.decals) ? m.decals.filter(okDecal).map((d: any) => ({ ...inst(d), fragments: d.fragments.map(inst) })) : [],
      }
    }
    case 'lit_mesh': {
      const ok = ['rev', 'atlas', 'pageCount', 'triangleCount'].every((k) => Number.isInteger(m[k]) && m[k] >= 0) && m.atlas > 0 && typeof m.vertices === 'string'
      const okFace = (f: any) => f && typeof f.i === 'string' && typeof f.e === 'string' && typeof f.bi === 'string' && typeof f.b === 'string' && Number.isInteger(f.f) && typeof f.t === 'string' && Number.isInteger(f.n) && f.n > 0
      const okMap = (x: any) => x && Array.isArray(x.u) && x.u.length === 4 && x.u.every(Number.isFinite) && Array.isArray(x.v) && x.v.length === 4 && x.v.every(Number.isFinite) && Number.isInteger(x.pg) && Array.isArray(x.k) && x.k.length === 3 && x.k.every(Number.isFinite)
      const faces: LitFace[] = Array.isArray(m.faces) ? m.faces.filter(okFace).map((f: any) => (f.m !== undefined && !okMap(f.m) ? { ...f, m: undefined } : f)) : []
      return ok ? { type: 'lit_mesh', rev: m.rev, atlas: m.atlas, pageCount: m.pageCount, triangleCount: m.triangleCount, keepPages: m.keepPages === true, vertices: m.vertices, faces } : null
    }
    case 'light_styles': {
      if (!Array.isArray(m.patterns) || m.patterns.length > 64 || !m.patterns.every((p: any) => typeof p === 'string' && /^[a-z]*$/.test(p))) return null
      return { type: 'light_styles', patterns: m.patterns, hz: Number.isFinite(m.hz) && m.hz > 0 ? m.hz : 10 }
    }
    case 'lightvol': {
      const vec3 = (v: any) => Array.isArray(v) && v.length === 3 && v.every((x) => Number.isFinite(x))
      if (!Number.isInteger(m.rev) || !vec3(m.origin) || !vec3(m.dims) || !(m.cell > 0) || typeof m.probes !== 'string') return null
      const layers = Array.isArray(m.layers) ? m.layers.filter((l: any) => l && Number.isInteger(l.s) && typeof l.probes === 'string') : []
      return { type: 'lightvol', rev: m.rev, origin: m.origin, cell: m.cell, dims: m.dims, probes: m.probes, layers }
    }
    case 'entity_light': {
      const vec3 = (v: any) => Array.isArray(v) && v.length === 3 && v.every((x) => Number.isFinite(x))
      const okLight = (l: any) => l && Number.isInteger(l.s) && vec3(l.ambient) && vec3(l.directed)
      const ok = (e: any) => e && typeof e.i === 'string' && typeof e.e === 'string' && vec3(e.dir) && Array.isArray(e.lights) && e.lights.length > 0 && e.lights.every(okLight)
      if (!Number.isInteger(m.rev) || !Array.isArray(m.entities)) return null
      // one direction per entity: every layer uses it
      const entities = m.entities.filter(ok).map((e: any) => ({ i: e.i, e: e.e, dir: e.dir, lights: e.lights.map((l: any) => ({ s: l.s, ambient: l.ambient, directed: l.directed, dir: e.dir })) }))
      return { type: 'entity_light', rev: m.rev, entities }
    }
    case 'lightmap_tiles': {
      if (!Number.isInteger(m.rev) || !Number.isInteger(m.atlas) || m.atlas <= 0 || !Array.isArray(m.tiles)) return null
      const okTile = (t: any) => t && ['page', 'x', 'y', 'w', 'h'].every((k) => Number.isInteger(t[k]) && t[k] >= 0) && t.w > 0 && t.h > 0 && typeof t.rgba === 'string'
      return { type: 'lightmap_tiles', rev: m.rev, atlas: m.atlas, style: Number.isInteger(m.style) && m.style >= 0 && m.style <= 63 ? m.style : 0, tiles: m.tiles.filter(okTile) }
    }
    case 'lighting_status': {
      const nums = ['rev', 'chunks', 'stale', 'minPasses', 'maxPasses', 'texels', 'pages']
      if (typeof m.enabled !== 'boolean' || typeof m.converged !== 'boolean' || !nums.every((k) => Number.isFinite(m[k]))) return null
      return { type: 'lighting_status', enabled: m.enabled, rev: m.rev, chunks: m.chunks, stale: m.stale, minPasses: m.minPasses, maxPasses: m.maxPasses, converged: m.converged, texels: m.texels, pages: m.pages, exposure: Number.isFinite(m.exposure) && m.exposure > 0 ? m.exposure : 1, brightness: Number.isFinite(m.brightness) ? m.brightness : 0, styles: Array.isArray(m.styles) ? m.styles.filter(Number.isInteger) : [0], styleWarnings: Array.isArray(m.styleWarnings) ? m.styleWarnings.filter((w: any) => typeof w === 'string') : [] }
    }
    case 'settings': {
      if (typeof m.assetsRoot !== 'string') return null
      const str = (v: any) => (typeof v === 'string' ? v : '')
      const g = m.game && typeof m.game === 'object' ? m.game : {}
      return {
        type: 'settings', assetsRoot: m.assetsRoot, recentMaps: Array.isArray(m.recentMaps) ? m.recentMaps.filter((p: any) => typeof p === 'string') : [],
        game: { exe: str(g.exe), args: typeof g.args === 'string' ? g.args : '+map {mapname}', workingDir: str(g.workingDir) },
        editor: m.editor && typeof m.editor === 'object' && !Array.isArray(m.editor) ? m.editor : {},
        nativeDialogs: m.nativeDialogs === true, settingsFile: str(m.settingsFile), lightingFile: str(m.lightingFile),
      }
    }
    case 'response': return typeof m.ok === 'boolean' ? m : null
    case 'error': return typeof m.message === 'string' ? m : null
    case 'export_progress': return typeof m.stage === 'string' && Number.isFinite(m.fraction) ? { type: 'export_progress', stage: m.stage, fraction: m.fraction } : null
    case 'export_result': {
      if (typeof m.ok !== 'boolean') return null
      const st = m.stats && typeof m.stats === 'object' ? m.stats : {}
      const num = (k: string) => (Number.isFinite(st[k]) ? st[k] : 0)
      const stats = { surfaces: num('surfaces'), droppedSurfaces: num('droppedSurfaces'), brushes: num('brushes'), models: num('models'), leafs: num('leafs'), clusters: num('clusters'), lightmapPages: num('lightmapPages'), gridPoints: num('gridPoints'), palette: num('palette'), visCell: num('visCell'), visSeconds: num('visSeconds') }
      const str = (v: any) => (typeof v === 'string' ? v : '')
      return { type: 'export_result', ok: m.ok, cancelled: m.cancelled === true, error: str(m.error), warnings: Array.isArray(m.warnings) ? m.warnings.filter((w: any) => typeof w === 'string') : [], bsp: str(m.bsp), lightmaps: str(m.lightmaps), seconds: Number.isFinite(m.seconds) ? m.seconds : 0, bakeSeconds: Number.isFinite(m.bakeSeconds) ? m.bakeSeconds : 0, device: str(m.device), stats }
    }
    default: return null
  }
}

export const msg = {
  documentUpdate: (doc: MapDocument) => JSON.stringify({ type: 'document_update', doc }),
  openDocument: (requestId: number, path: string) => JSON.stringify({ type: 'open_document', requestId, path }),
  listTextures: (requestId: number) => JSON.stringify({ type: 'list_textures', requestId }),
  readTexture: (requestId: number, path: string) => JSON.stringify({ type: 'read_texture', requestId, path }),
  writeTextureProps: (requestId: number, path: string, props: unknown) => JSON.stringify({ type: 'write_texture_props', requestId, path, props }),
  listDirectory: (requestId: number, path = '') => JSON.stringify({ type: 'list_directory', requestId, path }),
  browseFilesystem: (requestId: number, path = '') => JSON.stringify({ type: 'browse_filesystem', requestId, ...(path ? { path } : {}) }),
  setAssetsRoot: (requestId: number, path: string) => JSON.stringify({ type: 'set_assets_root', requestId, path }),
  listTexGroups: (requestId: number) => JSON.stringify({ type: 'list_texgroups', requestId }),
  readTexGroup: (requestId: number, path: string) => JSON.stringify({ type: 'read_texgroup', requestId, path }),
  writeTexGroup: (requestId: number, path: string, group: unknown) => JSON.stringify({ type: 'write_texgroup', requestId, path, group }),
  deleteTexGroup: (requestId: number, path: string) => JSON.stringify({ type: 'delete_texgroup', requestId, path }),
  readPrefab: (requestId: number, path: string) => JSON.stringify({ type: 'read_prefab', requestId, path }),
  /** Live lighting (Core: lighting service). Any field may be left out; the camera lets the Core refine what you look at first. */
  lighting: (o: { enabled?: boolean; exposure?: number; camera?: [number, number, number] }) => JSON.stringify({ type: 'lighting', ...o }),
  /** BSP export of the map as it is now (REQUIREMENTS §25): `path` is an assets relative .bsp; bake false = geometry only. */
  exportBsp: (requestId: number, path: string, bake = true) => JSON.stringify({ type: 'export_bsp', requestId, path, bake }),
  /** Export with the game's format (REQUIREMENTS §31): the Core gives `path` the format's extension and answers { ok, path, format }. */
  exportMap: (requestId: number, path: string, bake = true) => JSON.stringify({ type: 'export_map', requestId, path, bake }),
  exportCancel: (requestId: number) => JSON.stringify({ type: 'export_cancel', requestId }),
  saveDocument: (requestId: number, path?: string) => JSON.stringify({ type: 'save_document', requestId, ...(path ? { path } : {}) }),
  /** The operating system's file / folder dialog, shown by Core; answered when the person closes it. relative: paths inside the assets root. */
  nativeDialog: (requestId: number, o: { kind: 'folder' | 'open' | 'save'; title?: string; startDir?: string; fileName?: string; filters?: DialogFilter[]; defaultExt?: string; relative?: boolean }) =>
    JSON.stringify({ type: 'native_dialog', requestId, ...o }),
  setSettings: (requestId: number, o: { game?: Partial<GameLaunch>; editor?: Record<string, unknown>; clearRecent?: boolean }) => JSON.stringify({ type: 'set_settings', requestId, ...o }),
  readGameProfile: (requestId: number) => JSON.stringify({ type: 'read_game_profile', requestId }),
  /** Only the keys to change (null removes one); Core validates, writes game_profile.json and reloads it. */
  writeGameProfile: (requestId: number, profile: Record<string, unknown>) => JSON.stringify({ type: 'write_game_profile', requestId, profile }),
  readLightingConfig: (requestId: number) => JSON.stringify({ type: 'read_lighting_config', requestId }),
  writeLightingConfig: (requestId: number, config: Record<string, number | boolean>) => JSON.stringify({ type: 'write_lighting_config', requestId, config }),
  /** Starts the game set in Settings > Game with an exported map (assets relative .bsp). */
  launchGame: (requestId: number, map: string) => JSON.stringify({ type: 'launch_game', requestId, map }),
}
