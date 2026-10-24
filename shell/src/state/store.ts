// The Shell's single source of UI truth: the working document, history, selection and sync
// state with Core. Rules (DESIGN.md §3/§10):
//  * Edits apply LOCALLY first (zero latency), then are submitted whole to Core.
//  * Editing is disabled until Core has synced and while disconnected.
//  * A remote document is adopted only when we have nothing in flight (last-writer-wins).
//  * A rejected update rolls the working document back to the last one Core accepted.
import type { MapDocument } from '../model/document'
import type { Brush, Entity } from '../model/document'
import { emptyDocument, findEntity } from '../model/document'
import { CarvedMesh, DecalEntry, DerivedEntry, DerivedResult, World, usableCarved, usableDecalEntries, usableDecals, usableEntries, carvedKey } from '../model/derived'
import { reliefFaces, reliefMessage, reliefSignature } from '../model/reliefBake'
import { classOf, mapBrush, moveBrushToDetail, newId, requiresConvexBrushes } from '../model/document'
import { reconcileShapes } from '../model/parametric'
import { BrushParams, CreateParams, DEFAULT_BRUSH, DEFAULT_CREATE, TerrainTool, reconcileTerrain } from '../model/terrain'
import { validateBrush } from '../model/geometry'
import { MeshSel, SelMode, cleanSel, convertSel, emptySel } from '../model/meshTopology'
import { OpResult, isError } from '../model/meshOps'
import { ShapeId, ShapeParams, defaultShapeParams } from '../model/shapes'
import { expandPrefabs, Library, ResolvedEntity } from '../model/expand'
import { Box, boxFromQuake } from '../model/entityBox'
import { msg } from '../net/protocol'
import type { CoreSettings, EntityClassDef, GameProfile, Inbound } from '../net/protocol'
import { LightmapStore } from '../render/lightmapStore'
import { LightvolStore } from '../render/lightvolStore'
import type { Status } from '../net/connection'
import { texEnv } from '../model/texprops'
import { progressMessage, resultMessage } from './exportStatus'
import { ExportFormat, RBSP_FORMAT, formatFromProfile } from './exportFormat'
import { editorPrefs } from './editorPrefs'

const DEFAULT_BOX: Box = boxFromQuake(null)

/** `instanceId` set = a prefab instance of the root document is selected (then `entityId` is ''). */
/** shift in texture repeats, rotate in degrees, scale as a fraction (0.1 = 10 % per press). */
/** shift / rotate / scale: the keyboard nudges (shift in repeats, scale as a factor). shiftU / shiftV (texels), stretchU / stretchV, and `rotate` (degrees, shared) are the Surface Inspector's spinner steps. */
export interface TexSteps { shift: number; rotate: number; scale: number; shiftU: number; shiftV: number; stretchU: number; stretchV: number; shearStep: number }
export const DEFAULT_TEX_STEPS: TexSteps = { shift: 0.125, rotate: 15, scale: 0.1, shiftU: 1, shiftV: 1, stretchU: 0.1, stretchV: 0.1, shearStep: 0.05 }

export interface Selection { entityId: string; brushId?: string; faceId?: number; instanceId?: string }

/**
 * A brush that is the only brush of its entity IS the entity (a door, a lift, a func_detail made of one brush): selecting it selects the entity, so
 * its keyvalues, angles and origin are what you edit. A face selection stays on its brush, and worldspawn stays a bag of separate brushes.
 */
export function wholeEntityIfSole(doc: MapDocument, sel: Selection): Selection {
  if (sel.brushId === undefined || sel.faceId !== undefined || sel.instanceId !== undefined) return sel
  const e = findEntity(doc, sel.entityId)
  return e && classOf(e) !== 'worldspawn' && e.brushes?.length === 1 && e.brushes[0].id === sel.brushId ? { entityId: sel.entityId } : sel
}
/** The reverse, for what needs a brush (mesh editing): an entity selected as a whole whose only brush is meant. */
export function soleBrushOf(doc: MapDocument, sel: Selection): Selection {
  if (sel.brushId !== undefined || sel.instanceId !== undefined) return sel
  const e = findEntity(doc, sel.entityId)
  return e && classOf(e) !== 'worldspawn' && e.brushes?.length === 1 ? { entityId: sel.entityId, brushId: e.brushes[0].id } : sel
}
export type Tool = 'select' | 'box' | 'decal' | 'terrain'
/** The Terrain tool's settings (REQUIREMENTS §39): the brush, and the size of the next terrain made in the Terrain panel. */
export interface TerrainSettings { tool: TerrainTool; brush: BrushParams; /** Flatten aims here; null = the height under the first stamp */ flattenHeight: number | null; create: Omit<CreateParams, 'origin' | 'texture'> }
/** Object = pick whole brushes/entities. Face = pick faces for texturing. Mesh = edit vertices/edges/faces of one brush. */
export type EditMode = 'object' | 'face' | 'mesh'
export interface HistoryEntry { label: string; doc: MapDocument }
export interface State {
  doc: MapDocument
  path: string
  rev: number
  status: Status
  synced: boolean
  library: Library
  prefabEntities: ResolvedEntity[]
  problems: string[]
  selection: Selection | null
  /** Everything selected (object mode), the primary `selection` included. */
  multi: Selection[]
  history: HistoryEntry[]
  historyIndex: number
  message: string | null
  tool: Tool
  editMode: EditMode
  /** Texture applied by 'apply' actions (chosen in the content browser). */
  activeTexture: string
  /** Global UV lock (toolbar toggle, default ON): moving/rotating/scaling brushes keeps their visible UV. */
  uvLock: boolean
  /** Pipette armed by a reference field: the next viewport click on an entity calls this with its root-relative path (null = cancelled). */
  pipette: ((path: string | null) => void) | null
  /** A brush that only fails the convex rule is moved into a func_detail entity instead of the edit being refused. */
  autoDetail: boolean
  /** Mesh mode proportional editing (O): unselected vertices within `propRadius` metres follow G / R / S with a smooth falloff */
  proportional: boolean
  /** Hidden things (model/visibility.ts keys): a view setting, not saved, not undone */
  hidden: string[]
  /** A BSP export runs in the Core (REQUIREMENTS §25 E9): the last progress, null when none runs. */
  exporting: { stage: string; fraction: number } | null
  /** Copy UV / Paste UV (right-click): the face whose texture and mapping Paste UV copies */
  uvClipboard: { entityId: string; brushId: string; faceId: number } | null
  propRadius: number
  /** Steps of the texture keys (Shift+arrows shift, Ctrl+Shift+arrows scale, Shift+PageUp/Down rotate). */
  texSteps: TexSteps
  /** What the Box tool draws, and its parameters (sides, steps, ...). */
  shape: ShapeId
  shapeParams: ShapeParams
  /** Decal projector size for the Decal tool. */
  decalParams: { width: number; height: number; depth: number }
  terrain: TerrainSettings
  /** Mesh edit mode: which element kind is being selected, and what is selected in the primary brush. */
  meshMode: SelMode
  meshSel: MeshSel
  /** True when the document differs from what is on disk (as far as this window knows). */
  dirty: boolean
  profile: GameProfile | null
  /** The game's export format (game_profile.json "exporter", REQUIREMENTS §31). */
  exportFormat: ExportFormat
  /** Classes from the game's .ent file; empty means none were found and the built-in lists are used. */
  entityDefs: EntityClassDef[]
  entityWarnings: string[]
  /** Absolute path of Core's assets folder. */
  assetsRoot: string
  /** Core's settings.json as far as the Shell uses it (recent maps, game launch, editor preferences, native dialogs); null until Core sent it */
  coreSettings: CoreSettings | null
  /** The last BSP export's outcome (File > Play map waits for it) */
  lastExport: { ok: boolean; cancelled: boolean; bsp: string; error: string } | null
}
export interface StoreConnection {
  submitDocument(doc: MapDocument): void
  readonly updateInFlight: boolean
  readonly hasPendingUpdate: boolean
  sendLighting?(text: string): void
}

const MAX_HISTORY = 200
const NO_CARVED: Map<string, CarvedMesh> = new Map()
const NO_DECALS: CarvedMesh[] = []
const NO_DECAL_ENTRIES: DecalEntry[] = []

export class Store {
  private s: State
  private listeners = new Set<() => void>()
  private lastAcked: MapDocument
  /** The document as last opened/saved; `dirty` is `doc !== savedDoc`. null = known to differ from disk. */
  private savedDoc: MapDocument | null
  private derived: DerivedResult | null = null
  /** The document Core computed `derived` from (null until the matching ack/sync tells us which it was). */
  private derivedBasis: World | null = null
  private carvedMemo: { world: World; derived: DerivedResult; basis: World; texRev: number; value: Map<string, CarvedMesh> } | null = null
  private decalMemo: { world: World; derived: DerivedResult; basis: World; value: CarvedMesh[] } | null = null
  private meshCache = new WeakMap<DerivedEntry, { deps: unknown[]; mesh: CarvedMesh; rev: number; sig: string }>()
  private decalCache = new WeakMap<DecalEntry, CarvedMesh>()
  private colors = new Map<string, [number, number, number]>()
  private boxes = new Map<string, Box>()
  private drag: { start: MapDocument; label: string } | null = null
  /** Live lighting (Core): pages, lit mesh and progress. Empty until a viewport asks for a lighting render mode. */
  readonly lightmap = new LightmapStore()
  readonly lightvol = new LightvolStore()
  private lightingWanters = new Set<unknown>()
  private lastCamera: { pos: [number, number, number]; at: number } | null = null
  /** What `syncRelief` last looked at, the pending send, and the signature of the relief Core holds (undefined = not known: send the next one). */
  private reliefSeen: { doc: unknown; prefabs: unknown; derived: DerivedResult | null; basis: World | null; texRev: number } | null = null
  private reliefTimer: ReturnType<typeof setTimeout> | null = null
  private reliefSent: string | undefined = undefined
  private expandKey: { instances: unknown; library: unknown; path: string } | null = null

  constructor(private conn: StoreConnection) {
    const doc = emptyDocument()
    this.lastAcked = doc
    this.savedDoc = doc
    this.s = {
      doc, path: '', rev: 0, status: 'disconnected', synced: false, library: {}, prefabEntities: [],
      problems: [], selection: null, multi: [], history: [{ label: 'Empty', doc }], historyIndex: 0, message: null,
      tool: 'select', editMode: 'object', activeTexture: 'dev/grey',
      uvLock: true, pipette: null, autoDetail: true, proportional: false, propRadius: 1, hidden: [], uvClipboard: null, exporting: null, texSteps: { ...DEFAULT_TEX_STEPS }, shape: 'box', shapeParams: defaultShapeParams(), decalParams: { width: 1, height: 1, depth: 0.5 }, terrain: { tool: 'raise', brush: { ...DEFAULT_BRUSH }, flattenHeight: null, create: { ...DEFAULT_CREATE } }, meshMode: 'vertex', meshSel: emptySel(),
      dirty: false, profile: null, exportFormat: RBSP_FORMAT, entityDefs: [], entityWarnings: [], assetsRoot: '', coreSettings: null, lastExport: null,
    }
  }

  get state(): State { return this.s }
  subscribe(fn: () => void): () => void {
    this.listeners.add(fn)
    return () => void this.listeners.delete(fn)
  }
  private set(patch: Partial<State>): void {
    this.s = { ...this.s, ...patch }
    this.listeners.forEach((f) => f())
  }

  get canEdit(): boolean { return this.s.status === 'connected' && this.s.synced }
  private guard(): boolean {
    if (this.canEdit) return true
    this.set({ message: 'Not connected to Core - editing is disabled' })
    return false
  }
  setMessage(message: string | null): void { this.set({ message }) }
  select(selection: Selection | null): void {
    const sameBrush = selection && this.s.selection && selection.entityId === this.s.selection.entityId && selection.brushId === this.s.selection.brushId
    this.set({ selection, multi: selection ? [selection] : [], ...(sameBrush ? {} : { meshSel: emptySel() }) })
  }
  /** Shift-click: adds to the selection, or removes it if already selected (object mode). */
  toggleSelect(sel: Selection): void {
    const same = (a: Selection) => a.entityId === sel.entityId && a.brushId === sel.brushId && a.faceId === sel.faceId && a.instanceId === sel.instanceId
    const has = this.s.multi.some(same)
    const multi = has ? this.s.multi.filter((a) => !same(a)) : [...this.s.multi, sel]
    this.set({ multi, selection: multi.length ? (has ? multi[multi.length - 1] : sel) : null })
  }
  selectMany(list: Selection[]): void { this.set({ multi: list, selection: list.length ? list[list.length - 1] : null, meshSel: emptySel() }) }
  setTool(tool: Tool): void { this.set({ tool }) }
  setAutoDetail(autoDetail: boolean): void { this.set({ autoDetail }) }
  setHidden(hidden: string[]): void { this.set({ hidden }) }
  setUvClipboard(uvClipboard: State['uvClipboard']): void { this.set({ uvClipboard }) }
  /** Asks the app to show a panel (e.g. 'inspector' for right-click > Properties); set by app.ts. */
  showPanel: (id: string) => void = () => {}
  setProportional(proportional: boolean): void { this.set({ proportional, message: `Proportional editing ${proportional ? 'on' : 'off'} (radius ${+this.s.propRadius.toFixed(3)} m; mouse wheel during G / R / S changes it)` }) }
  setPropRadius(r: number): void { this.set({ propRadius: Math.min(1000, Math.max(0.01, r)) }) }
  setTexStep(key: keyof TexSteps, value: number): void { if (Number.isFinite(value) && value > 0) this.set({ texSteps: { ...this.s.texSteps, [key]: value } }) }
  setPipette(pipette: ((path: string | null) => void) | null): void { this.set({ pipette, ...(pipette ? { message: 'Pipette: click an entity in the viewport (Esc cancels)' } : {}) }) }
  setUvLock(uvLock: boolean): void { this.set({ uvLock, message: uvLock ? 'UV lock on: textures stay on the faces when brushes move' : 'UV lock off: textures stay in the world when brushes move' }) }
  setShape(shape: ShapeId): void { this.set({ shape, tool: 'box' }) }
  setShapeParams(p: Partial<ShapeParams>): void { this.set({ shapeParams: { ...this.s.shapeParams, ...p } }) }
  setDecalParams(p: Partial<State['decalParams']>): void { this.set({ decalParams: { ...this.s.decalParams, ...p } }) }
  /** Terrain tool settings; picking a brush (a tool name) also switches to the Terrain tool. */
  setTerrain(p: { tool?: TerrainTool; brush?: Partial<BrushParams>; flattenHeight?: number | null; create?: Partial<TerrainSettings['create']> }): void {
    const t = this.s.terrain
    this.set({ terrain: { tool: p.tool ?? t.tool, brush: { ...t.brush, ...p.brush }, flattenHeight: p.flattenHeight !== undefined ? p.flattenHeight : t.flattenHeight, create: { ...t.create, ...p.create } }, ...(p.tool ? { tool: 'terrain' as const } : {}) })
  }

  setEditMode(editMode: EditMode): void {
    const picked = this.s.selection
    if (editMode === 'mesh') {
      const sel = picked ? soleBrushOf(this.s.doc, picked) : picked // an entity of one brush is edited through that brush
      if (!sel?.brushId) return this.set({ message: 'Select a brush first (click it), then press Tab to edit its mesh' })
      if (!this.guard()) return
      return this.set({ editMode, selection: sel, multi: [sel], meshSel: emptySel(), tool: 'select' })
    }
    // leaving face mode drops a face selection down to its brush; leaving mesh mode puts a sole brush back to its entity
    const down = editMode === 'object' && picked?.faceId !== undefined ? { entityId: picked.entityId, brushId: picked.brushId } : picked
    const selection = editMode === 'object' && this.s.editMode === 'mesh' && down ? wholeEntityIfSole(this.s.doc, down) : down
    this.set({ editMode, meshSel: emptySel(), selection, ...(selection !== picked && selection ? { multi: [selection] } : {}) })
  }

  // ---- mesh editing ------------------------------------------------------------------------
  /** The brush being mesh-edited (with its entity), if mesh mode is on. */
  meshTarget(doc: MapDocument = this.s.doc): { entity: Entity; brush: Brush } | null {
    const sel = this.s.selection
    if (this.s.editMode !== 'mesh' || !sel?.brushId) return null
    const entity = findEntity(doc, sel.entityId)
    const brush = entity?.brushes?.find((b) => b.id === sel.brushId)
    return entity && brush ? { entity, brush } : null
  }
  setMeshSel(meshSel: MeshSel): void { this.set({ meshSel }) }
  setMeshMode(meshMode: SelMode): void {
    const t = this.meshTarget()
    this.set({ meshMode, meshSel: t ? convertSel(t.brush, this.s.meshSel, this.s.meshMode, meshMode) : emptySel() })
  }

  /** Why `brush` may not be stored in `entity`: classes that need convex solids refuse anything else. */
  brushProblem(entity: Entity, brush: Brush): string | null {
    // the convex rule is for SOLIDS: a subtractive brush is never compiled as a solid, so it may be concave anywhere
    return validateBrush(brush, requiresConvexBrushes(classOf(entity)) && brush.op !== 'subtract')
  }
  /** True when `brush` is a valid solid except that it is concave in an entity that needs convex ones, and auto-convert is on. */
  needsDetail(entity: Entity, brush: Brush): boolean {
    return this.s.autoDetail && this.brushProblem(entity, brush) !== null && validateBrush(brush, false) === null
  }

  /** Runs an instant mesh operation on the primary brush as one undo step. Invalid results are refused with a message. */
  applyMeshOp(label: string, op: (b: Brush, sel: MeshSel) => OpResult): boolean {
    const t = this.meshTarget()
    if (!t || !this.guard()) return false
    const r = op(t.brush, this.s.meshSel)
    if (isError(r)) { this.set({ message: r.error }); return false }
    const problem = this.brushProblem(t.entity, r.brush)
    const sel = this.s.selection!
    if (problem && this.needsDetail(t.entity, r.brush)) {
      const detailId = newId('e')
      if (!this.edit(label, (d) => moveBrushToDetail(d, sel.entityId, sel.brushId!, r.brush, detailId))) return false
      this.select({ entityId: detailId, brushId: sel.brushId })
      this.set({ meshSel: cleanSel(r.brush, r.sel), message: 'The brush is concave, so it moved into a new func_detail entity' })
      return true
    }
    if (problem) { this.set({ message: `${label}: ${problem}` }); return false }
    if (r.brush === t.brush) return false
    this.edit(label, (d) => mapBrush(d, sel.entityId, sel.brushId!, () => r.brush))
    this.set({ meshSel: cleanSel(r.brush, r.sel), message: null })
    return true
  }
  setActiveTexture(activeTexture: string): void { this.set({ activeTexture }) }

  // ---- edits -----------------------------------------------------------------------
  /** One undoable step. `fn` must be pure and return the SAME doc if nothing changed. */
  edit(label: string, fn: (doc: MapDocument) => MapDocument): boolean {
    if (!this.guard()) return false
    const doc = this.reconciled(this.s.doc, fn(this.s.doc))
    if (doc === this.s.doc) return false
    this.applyDoc(doc)
    this.pushHistory(label, doc)
    this.conn.submitDocument(doc)
    return true
  }

  /** Continuous edit (gizmo drag): every update is shown and sent, but history gets ONE entry at the end. */
  beginDrag(label: string): boolean {
    if (!this.guard()) return false
    this.drag = { start: this.s.doc, label }
    return true
  }
  /**
   * `submit = false`: show the result but do not send it to Core yet (a stroke over a big terrain would stringify a megabyte per mouse move); the caller
   * sends it now and then with `submitCurrent`, and `endDrag` always sends the final document.
   */
  dragUpdate(fn: (start: MapDocument) => MapDocument, submit = true): void {
    if (!this.drag) return
    const doc = this.reconciled(this.drag.start, fn(this.drag.start))
    this.applyDoc(doc)
    if (submit) { this.conn.submitDocument(doc); this.unsent = false } else this.unsent = true
  }
  /** Sends the working document to Core now (after `dragUpdate(fn, false)`). */
  submitCurrent(): void { if (this.unsent) { this.unsent = false; this.conn.submitDocument(this.s.doc) } }
  private unsent = false
  /** The shape and terrain records brought in line with what the edit did to the geometry. */
  private reconciled(prev: MapDocument, next: MapDocument): MapDocument {
    return reconcileTerrain(prev, reconcileShapes(prev, next, () => newId('s')))
  }
  /** Abandons a continuous edit: the document goes back to how it was when `beginDrag` was called. */
  cancelDrag(): void {
    if (!this.drag) return
    const { start } = this.drag
    this.drag = null
    this.unsent = false
    if (this.s.doc !== start) { this.applyDoc(start); this.conn.submitDocument(start) }
  }

  get dragging(): boolean { return this.drag !== null }

  endDrag(): void {
    if (!this.drag) return
    const { start, label } = this.drag
    this.drag = null
    if (this.unsent) { this.unsent = false; this.conn.submitDocument(this.s.doc) }
    if (this.s.doc !== start) this.pushHistory(label, this.s.doc)
  }

  undo(): void { this.jumpTo(this.s.historyIndex - 1) }
  redo(): void { this.jumpTo(this.s.historyIndex + 1) }
  jumpTo(i: number): void {
    if (!this.guard() || i < 0 || i >= this.s.history.length || i === this.s.historyIndex) return
    const doc = this.s.history[i].doc
    this.applyDoc(doc, i)
    this.set({ meshSel: emptySel() })
    this.conn.submitDocument(doc)
  }

  private applyDoc(doc: MapDocument, historyIndex = this.s.historyIndex): void {
    const hasInst = (id: string) => doc.prefabInstances.some((p) => p.id === id)
    /** `x` as it stands in `doc`: a brush that moved to another entity (convex -> func_detail) is followed there; null when it is gone. */
    const follow = (x: Selection): Selection | null => {
      if (x.instanceId !== undefined) return hasInst(x.instanceId) ? x : null
      let e = findEntity(doc, x.entityId)
      if (x.brushId === undefined) return e ? x : null
      let brush = e?.brushes?.find((b) => b.id === x.brushId)
      if (!brush) {
        e = doc.entities.find((en) => en.brushes?.some((b) => b.id === x.brushId))
        brush = e?.brushes?.find((b) => b.id === x.brushId)
        if (!e || !brush) return null
      }
      if (x.faceId !== undefined && !brush.faces.some((f) => f.id === x.faceId)) return null
      return e!.id === x.entityId ? x : { ...x, entityId: e!.id }
    }
    const sel = this.s.selection
    const keep = sel ? follow(sel) : null
    const multi = this.s.multi.flatMap((x) => { const f = follow(x); return f ? [f] : [] })
    const editMode = this.s.editMode === 'mesh' && !keep?.brushId && !this.drag ? 'object' : this.s.editMode
    this.set({ doc, historyIndex, selection: keep ?? (multi.length ? multi[multi.length - 1] : null), multi, editMode, dirty: doc !== this.savedDoc, ...this.expansion(doc, this.s.library, this.s.path) })
  }

  private pushHistory(label: string, doc: MapDocument): void {
    const history = [...this.s.history.slice(0, this.s.historyIndex + 1), { label, doc }].slice(-MAX_HISTORY)
    this.set({ history, historyIndex: history.length - 1 })
  }

  private expansion(doc: MapDocument, library: Library, path: string): Partial<State> {
    const k = this.expandKey
    if (k && k.instances === doc.prefabInstances && k.library === library && k.path === path) return {}
    this.expandKey = { instances: doc.prefabInstances, library, path }
    const r = expandPrefabs(doc, library, path)
    return { prefabEntities: r.entities, problems: r.problems }
  }

  // ---- live lighting ----------------------------------------------------------------
  /** A viewport (`token`) needs / no longer needs lighting; Core is told when the first one starts and the last one stops. */
  setLightingWanted(token: unknown, wanted: boolean): void {
    const before = this.lightingWanters.size > 0
    if (wanted) this.lightingWanters.add(token); else this.lightingWanters.delete(token)
    const now = this.lightingWanters.size > 0
    if (now === before) return
    this.conn.sendLighting?.(msg.lighting({ enabled: now }))
    if (!now) { this.lightmap.clear(); this.lightvol.clear(); this.set({}) }
  }
  get lightingWanted(): boolean { return this.lightingWanters.size > 0 }
  /**
   * The relief of textures as geometry for the bake (model/reliefBake.ts): the Core lights the flat faces; to give a relief real shadows it has to see the
   * displaced triangles. Called every frame a lighting view is drawn (O(1) when nothing changed); a change is sent once it settles, and only when the
   * result differs from what Core already holds. Texture relief edits arrive as a new texture environment revision.
   */
  syncRelief(): void {
    if (!this.lightingWanted || this.s.status !== 'connected' || !this.conn.sendLighting) return
    const d = this.derived, basis = this.derivedBasis, m = this.reliefSeen
    if (m && m.doc === this.s.doc && m.prefabs === this.s.prefabEntities && m.derived === d && m.basis === basis && m.texRev === texEnv.rev) return
    this.reliefSeen = { doc: this.s.doc, prefabs: this.s.prefabEntities, derived: d, basis, texRev: texEnv.rev }
    if (this.reliefTimer) clearTimeout(this.reliefTimer)
    this.reliefTimer = setTimeout(() => { this.reliefTimer = null; this.sendRelief() }, 150)
  }
  private sendRelief(): void {
    if (!this.lightingWanted || this.s.status !== 'connected' || !this.conn.sendLighting) return
    const w = this.world(), d = this.derived, basis = this.derivedBasis
    const entries = d && basis ? usableEntries(d, basis, w) : []
    // every brush Core carved is `known`: one whose result is out of date is left out (its pieces are not the authored faces any more)
    const known = new Set((d?.carved ?? []).map((e) => carvedKey(e.entityId, e.brushId, e.instance)))
    const faces = reliefFaces(w, entries, known)
    const sig = reliefSignature(faces)
    if (sig === this.reliefSent) return
    this.reliefSent = sig
    this.conn.sendLighting(reliefMessage(faces))
  }

  /** The viewport camera moved: tell Core so it refines what you look at first (throttled: 1 m or 1 s). */
  reportCamera(pos: [number, number, number], now: number = Date.now()): void {
    if (!this.lightingWanted) return
    const l = this.lastCamera
    if (l && Math.hypot(pos[0] - l.pos[0], pos[1] - l.pos[1], pos[2] - l.pos[2]) < 1 && now - l.at < 1000) return
    this.lastCamera = { pos: [...pos], at: now }
    this.conn.sendLighting?.(msg.lighting({ camera: pos }))
  }

  // ---- messages from Core (wired up by the app) -----------------------------------
  setStatus(status: Status): void {
    this.set(status === 'connected' ? { status } : { status, synced: false })
    if (status === 'connected') { this.reliefSent = undefined; this.reliefSeen = null } // a new connection: Core may hold nothing (or something else)
    if (status === 'connected' && this.lightingWanted) this.conn.sendLighting?.(msg.lighting({ enabled: true }))
  }

  onMessage(m: Inbound): void {
    switch (m.type) {
      case 'document_sync':
        if (this.conn.updateInFlight || this.conn.hasPendingUpdate || this.drag) return // ours wins
        this.reliefSent = undefined // another document: Core's relief is not ours any more
        this.lastAcked = m.doc
        this.savedDoc = m.dirty ? null : m.doc
        this.set({
          path: m.path, rev: m.rev, synced: true, history: [{ label: 'Loaded', doc: m.doc }], historyIndex: 0, meshSel: emptySel(),
        })
        this.applyDoc(m.doc, 0)
        if (this.derived?.rev === m.rev) this.derivedBasis = { doc: m.doc, prefabs: this.s.prefabEntities }
        return
      case 'prefab_library':
        this.set({ library: m.prefabs, ...this.expansion(this.s.doc, m.prefabs, this.s.path) })
        return
      case 'lit_mesh': this.lightmap.applyMesh(m); this.set({}); return
      case 'light_styles': this.lightmap.applyStyles(m); this.set({}); return
      case 'lightvol': if (this.lightvol.applyGrid(m)) this.set({}); return
      case 'entity_light': this.lightvol.applyEntities(m); this.set({}); return
      case 'lightmap_tiles': if (this.lightmap.applyTiles(m)) this.set({}); return
      case 'lighting_status': this.lightmap.applyStatus(m); if (!m.enabled) this.lightvol.clear(); this.set({}); return
      case 'export_progress': this.set({ exporting: { stage: m.stage, fraction: m.fraction }, message: progressMessage(m) }); return
      case 'export_result': this.set({ exporting: null, message: resultMessage(m), lastExport: { ok: m.ok, cancelled: m.cancelled, bsp: m.bsp, error: m.error } }); return
      case 'derived_geometry':
        this.derived = { rev: m.rev, carved: m.carved, skipped: m.skipped, decals: m.decals }
        // it usually arrives just before the ack that carries the same revision; onAck fills the basis in then
        this.derivedBasis = m.rev === this.s.rev ? { doc: this.lastAcked, prefabs: this.s.prefabEntities } : null
        if (m.skipped.length) this.set({ message: `${m.skipped.length} subtractive result(s) too complex to carve; showing the authored brushes` })
        else this.set({})
        return
      case 'profile':
        this.set({ profile: m.profile, exportFormat: formatFromProfile(m) })
        return
      case 'entity_defs':
        // near-black markers would vanish against the dark viewport, so lift them to a readable grey
        this.colors = new Map(m.classes.map((c) => [c.name, Math.max(...c.color) < 0.35 ? [0.6, 0.6, 0.6] : c.color] as [string, [number, number, number]]))
        this.boxes = new Map(m.classes.map((c) => [c.name, boxFromQuake(c.box)] as [string, Box]))
        this.set({ entityDefs: m.classes, entityWarnings: m.warnings })
        return
      case 'settings': {
        const { type: _t, ...coreSettings } = m
        const first = this.state.coreSettings === null
        const p = editorPrefs(coreSettings.editor)
        // the editor preferences' defaults apply once, when Core first tells us them (later changes are the person's own toggles)
        this.set({ assetsRoot: m.assetsRoot, coreSettings, ...(first ? { uvLock: p.uvLock, autoDetail: p.autoDetail } : {}) })
        return
      }
      case 'error':
        this.set({ message: m.message })
        return
      default:
        return
    }
  }

  onAck(rev: number, doc: MapDocument | null): void {
    if (doc) this.lastAcked = doc
    if (this.derived?.rev === rev) this.derivedBasis = { doc: doc ?? this.lastAcked, prefabs: this.s.prefabEntities }
    this.set({ rev })
  }

  onRejected(message: string): void {
    this.drag = null
    const i = this.s.history.map((h) => h.doc).lastIndexOf(this.lastAcked)
    const history = i >= 0 ? this.s.history.slice(0, i + 1) : [{ label: 'Loaded', doc: this.lastAcked }]
    this.set({ history, historyIndex: history.length - 1, message: `Core rejected the edit: ${message}` })
    this.applyDoc(this.lastAcked, history.length - 1)
  }

  /** `saved` is the document that was current when the save was requested (edits made since stay dirty). */
  onSaved(path: string, saved: MapDocument = this.s.doc): void {
    this.savedDoc = saved
    this.set({ path, dirty: this.s.doc !== saved, message: `Saved ${path}` })
  }

  private world(): World { return { doc: this.s.doc, prefabs: this.s.prefabEntities } }

  /** Brushes that can be drawn from Core's boolean result right now, by carvedKey (prefab content included). */
  carved(): Map<string, CarvedMesh> {
    const d = this.derived, basis = this.derivedBasis
    if (!d || !basis || d.carved.length === 0) return NO_CARVED
    const w = this.world(), m = this.carvedMemo
    if (m && m.world.doc === w.doc && m.world.prefabs === w.prefabs && m.derived === d && m.basis === basis && m.texRev === texEnv.rev) return m.value
    const value = usableCarved(d, basis, w, this.meshCache)
    this.carvedMemo = { world: w, derived: d, basis, texRev: texEnv.rev, value }
    return value
  }

  private decalEntryMemo: { world: World; derived: DerivedResult; basis: World; value: DecalEntry[] } | null = null
  /** The decals that can be drawn right now, as derived fragments (the Lit view lights them with the face they lie on). */
  decalEntries(): DecalEntry[] {
    const d = this.derived, basis = this.derivedBasis
    if (!d || !basis || d.decals.length === 0) return NO_DECAL_ENTRIES
    const w = this.world(), m = this.decalEntryMemo
    if (m && m.world.doc === w.doc && m.world.prefabs === w.prefabs && m.derived === d && m.basis === basis) return m.value
    const value = usableDecalEntries(d, basis, w)
    this.decalEntryMemo = { world: w, derived: d, basis, value }
    return value
  }

  /** Decal meshes that can be drawn right now. */
  decals(): CarvedMesh[] {
    const d = this.derived, basis = this.derivedBasis
    if (!d || !basis || d.decals.length === 0) return NO_DECALS
    const w = this.world(), m = this.decalMemo
    if (m && m.world.doc === w.doc && m.world.prefabs === w.prefabs && m.derived === d && m.basis === basis) return m.value
    const value = usableDecals(d, basis, w, this.decalCache)
    this.decalMemo = { world: w, derived: d, basis, value }
    return value
  }

  /** Marker colour for a point-entity class from the .ent file, if it defines one. */
  entityColor(classname: string): [number, number, number] | undefined { return this.colors.get(classname) }

  /** Bounding box of a point-entity class in editor space (metres, Y-up, relative to the origin), from the .ent `box` converted from Quake units. Unknown classes and classes without a box get the default. */
  entityBox(classname: string): Box { return this.boxes.get(classname) ?? DEFAULT_BOX }
}
