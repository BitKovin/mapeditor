// A viewport: its own camera, its own settings, its own canvas. Any number can exist
// (DESIGN.md §6.1, up to MAX_VIEWPORTS); each subscribes to the shared Store.
import { styleAnimates } from '../render/lightStyles'
import type { IContentRenderer } from 'dockview-core'
import { Brush, MapDocument, addBrush, classOf, findEntity, isBrushEntity, mapBrush, mapEntity, moveBrushToDetail, newId, originOf, showsCustomOrigin } from '../model/document'
import { angleHandle, anglesToward, handleLines, hasAngleHandle } from '../model/entityHandles'
import { markSmooth, smoothEdgeKeys } from '../model/smooth'
import { SizeLabel, boxSizeLabels, faceSizeLabels, withoutOverlaps } from '../model/sizeLabels'
import { anglesForDirection } from '../model/decal'
import { brushBounds, faceNormal } from '../model/geometry'
import { dragFace, extendFace } from '../model/faceDrag'
import { Vec3, add, dot, length, normalize, scale, snap, sub } from '../model/math'
import { averageNormal, centroid, cleanSel, edgeKey, growSel, invertSel, shrinkSel, allSel, emptySel, MeshSel } from '../model/meshTopology'
import { MAX_BEVEL_SEGMENTS, OpResult, bevelEdges, bridgeLoops, deleteFaces, dissolveEdges, dissolveFaces, dissolveVerts, edgeRing, extrudeFaces, fillHole, flipFaces, insetFaces, isError, loopCut, mergeVerts, mirrorVerts, proportionalVerts, recalcNormals, transformVerts } from '../model/meshOps'
import { buildShape, frameOf, shapePreviewLines } from '../model/shapes'
import { attachShape, shapeMembers } from '../model/parametric'
import { Heightfield, TerrainStroke, circleOnTerrain, rayTerrain, specOf as terrainSpecOf, terrainOf, tileOf } from '../model/terrain'
import { clashes, depthAxis, hitClipPoint, rayPlaneHit, snapPoint } from '../tools/clipperPoints'
import { handles2D, pixelsToMetres, resizeMap, screenAxis, Handle2D, ScreenAxis } from '../model/resize2d'
import { mirrors, rotateAbout, scaleAbout, transformSelections, translateBy } from '../model/transform'
import { instanceOf } from '../model/derived'
import { Camera, FlyCamera, OrbitCamera, OrthoCamera, Ray } from '../render/camera'
import { AXIS_DIR, Axis, axisDistance, dragDelta, gizmoLength, angleDelta, hitAxis, hitDir, hitRing, lineDistance, ringAngle } from '../render/gizmo'
import { Hit, pick } from '../render/picking'
import { chainNames, pathOfHit, refKeysOf } from '../model/refs'
import { Renderer } from '../render/renderer'
import { CameraMode, isOrthoMode, FLY_SPEED_MAX, FLY_SPEED_MIN, WHEEL_SPEED_FACTOR, clampSpeed, GRID_SIZES, ROT_SNAPS, SCALE_SNAPS, rotSnapRad, scaleSnapStep, RENDER_MODES, RenderMode, ViewportSettings, defaultViewportSettings, isImplemented, snapStep } from '../render/viewportSettings'
import { copySelection, cutSelection, pasteClipboard } from '../state/clipboard'
import { TexKey, addDrawnBrush, addDrawnBrushes, copySurface, flowTexture, liftMaterial, duplicateSelection, gizmoPivot, selectedList, groupSelection, ungroupSelection, copyUv, pasteUv, hideSelection, unhideAll } from '../state/actions'
import { addPointEntity, invertSelectionItems, selectAllItems, selectByVolume, clipSelection, nudgeTexture, deleteSelection, makeEntityFromSelection, moveSelectionToWorld, nudgeSelection, flipSelection, hollowSelection, mergeSelection, rotateSelection90, selectedBrushes, snapSelection, spawnInstance } from '../state/actions'
import { Plane, planeFromPoints } from '../model/brushOps'
import { decalBoundsLines, entityAnnotations } from '../model/annotations'
import { entityArrows, placedEntities } from '../model/arrows'
import { isModernLight, lightGizmo, lightGizmoColor, lightParams } from '../model/lights'
import { showMenu, MenuItem } from './contextMenu'
import { DragItem, dragItem, isDragTransfer, itemFromTransfer, onDragItemChange } from './dragTypes'
import type { Box } from '../model/entityBox'
import { entityOrigin, prefabOrigin } from '../model/placement'
import type { Entity } from '../model/document'
import { autoSkyOf, isCubemapPath, SKY_TEXTURE_KEY } from '../model/sky'
import { worldspawnOf } from '../model/levelSettings'
import type { PrefabPreviews } from '../state/prefabPreview'
import type { Selection, Store } from '../state/store'
import { wholeEntityIfSole } from '../state/store'
import type { TextureCache } from '../state/textures'
import { DrawPlane, MIN_SIZE, Rect, buildBrush, fallbackPlane, heightFor, planeThrough, previewLines, rayPlane, rectFromDrag, snapToPlane } from '../tools/boxTool'
import { ModalKind, advanceVirtualCursor, alongLine, describeModal, stepSegments, grabDelta, metresPerPixel, planePoint, rotateAngle, scaleFactor, scaleVector, typeInto, typedValue, widthFromMouse } from '../tools/modal'
import { boxSelect, combineSel, nearestFaceEdge, pickEdge, pickFaceId, pickVertex, Project } from '../tools/meshPick'
import { ActiveElem, MeshPickAt, clickSelect } from '../model/meshSelect'
import { visibleEntities } from '../model/visibility'
import { KnifePoint, connectVerts, knifeCut, knifePos, subdivideEdges } from '../model/meshCut'
import { densityLegend, densityOf } from '../render/lightmapDensity'
import { h } from './dom'

interface Modal {
  kind: ModalKind
  axis: Axis | null
  typed: string
  value: number
  /** Moves the operation to where the mouse is now. */
  move(ray: Ray, mouse: { x: number; y: number }): void
  /** Re-applies with the current `typed` / axis. */
  refresh(): void
  /** Mouse wheel during the modal (bevel: segment count). Returns true when it was used. */
  wheel?(deltaY: number): boolean
  /** Bevel only: the current segment count (shown in the status line). */
  segments?: number
}

export const MAX_VIEWPORTS = 10

type Drawing =
  | { phase: 'rect'; plane: DrawPlane; start: Vec3; current: Vec3 }
  | { phase: 'height'; plane: DrawPlane; rect: Rect; height: number }

export class ViewportPanel implements IContentRenderer {
  readonly element = h('div', { class: 'viewport' })
  /** Both cameras always exist so switching modes is lossless; `camera` is the one in use. */
  private readonly cams: Record<CameraMode, Camera> & { fly: FlyCamera; orbit: OrbitCamera } = { fly: new FlyCamera(), orbit: new OrbitCamera(), top: new OrthoCamera('top'), front: new OrthoCamera('front'), side: new OrthoCamera('side') }
  get camera(): Camera { return this.cams[this.settings.cameraMode] }
  readonly settings: ViewportSettings = defaultViewportSettings()
  private canvas = h('canvas')
  private renderer: Renderer | null = null
  private unsubs: (() => void)[] = []
  private raf = 0
  private hoverAxis: Axis | null = null
  private hoverFace = false
  private faceGood: MapDocument | null = null
  /** Alt-drag extend: the brush the drag has grown so far (selected when the drag ends). */
  private extended: { entityId: string; brushId: string } | null = null
  private drawing: Drawing | null = null
  private lastRay: Ray | null = null
  private shift = false
  /** Fly keys currently held (KeyboardEvent.code), only tracked while RMB-flying. */
  private flyKeys = new Set<string>()
  private flyTimer = 0
  private flyLast = 0
  private speedBox: HTMLElement | null = null
  private speedSlider: HTMLInputElement | null = null
  private speedText: HTMLInputElement | null = null
  private cameraSelect: HTMLSelectElement | null = null
  private drag:
    | { kind: 'orbit' | 'pan' | 'look'; x: number; y: number }
    | { kind: 'gizmo'; axis: Axis; pivot: Vec3; startT: number; mesh: boolean; mode: 'move' | 'rotate' | 'scale'; extrude?: number[]; origin?: string }
    | { kind: 'face'; entityId: string; brushId: string; faceId: number; pivot: Vec3; dir: Vec3; startT: number; extend?: { brushId: string } }
    | { kind: 'resize'; h: -1 | 0 | 1; v: -1 | 0 | 1; min: Vec3; max: Vec3; r: ScreenAxis; u: ScreenAxis; x0: number; y0: number }
    | { kind: 'angle'; entityId: string; base: Vec3; n: Vec3; keepPitch: boolean }
    | { kind: 'terrain' }
    | null = null
  private hoverHandle: string | null = null
  /** While Alt is held over this view (Brush mode): the face an Alt + drag would grab (REQUIREMENTS §33 H8). */
  private altFace: { entityId: string; brushId: string; faceId: number } | null = null
  /** The entity handle under the cursor (REQUIREMENTS §33 H1 / H2). */
  private hoverEntHandle: 'origin' | 'angle' | null = null
  /** The entity whose origin was selected by clicking its origin square: the usual move gizmo sits on the origin and moves only the origin (not the brushes). */
  private originSel: string | null = null
  private pointerInside = false
  private modal: Modal | null = null
  private boxSel: { x0: number; y0: number; x1: number; y1: number; toggle: boolean; base: MeshSel; moved: boolean } | null = null
  private boxEl = h('div', { class: 'box-select', style: 'display:none' })
  private mouse = { x: 0, y: 0 }
  /** While a modal operation holds the pointer lock: the cursor position it would have, advanced by raw movement (canvas pixels, may leave the canvas). */
  private virt: { x: number; y: number } | null = null
  /** Loop cut armed (Ctrl+R): hovering an edge previews the cut. */
  private loopArmed = false
  private loopPreview: number[] | null = null
  /** Clipper (Radiant X): up to three points define the cutting plane; Enter keeps the back side. */
  private clip: { pts: { p: Vec3; dir: Vec3 }[] } | null = null
  /** Index of the clipper point being dragged with LMB, or null. */
  private clipDrag: number | null = null
  private lastGood: MapDocument | null = null
  /** Pixels the pointer travelled during an RMB press: a press that barely moved opens the context menu. */
  private rmbTravel = 0
  /** True while the held RMB pans an orthographic view (it counts travel like look/orbit so a plain right-click still opens the menu). */
  private rmbPanning = false
  /** Where the RMB went down: the pointer is locked while it is held, so pointer-up coordinates cannot be trusted. */
  private rmbDown = { x: 0, y: 0 }
  /** During a mesh drag: the func_detail entity the brush was moved into because it became concave (null = no move). */
  private detailId: string | null = null
  /** The brush a mesh drag edits, as (entity, brush) in the document the drag started from. */
  private dragBrush: { entityId: string; brushId: string } | null = null
  private converted: { entityId: string; brushId: string; meshSel: MeshSel } | null = null
  private startSel: MeshSel = emptySel()
  /** The last reason a modal edit was refused; it stays on the status bar after the modal ends. */
  private lastErr: string | null = null

  constructor(private store: Store, private textures: TextureCache, private onPopout?: () => void, private previews?: PrefabPreviews, private onTurnIntoPrefab?: () => void) {
    this.element.append(this.canvas, this.boxEl, this.buildHeader())
    const c = this.canvas
    c.tabIndex = 0
    c.addEventListener('pointerdown', (e) => { this.bindDocument(); this.onDown(e) })
    c.addEventListener('pointerenter', () => { this.pointerInside = true; this.bindDocument() })
    c.addEventListener('pointerleave', () => { this.pointerInside = false; this.setAltFace(null) })
    c.addEventListener('focus', () => this.bindDocument())
    c.addEventListener('pointermove', (e) => this.onMove(e))
    c.addEventListener('pointerup', (e) => this.onUp(e))
    c.addEventListener('contextmenu', (e) => e.preventDefault())
    c.addEventListener('wheel', (e) => this.onWheel(e), { passive: false })
    c.addEventListener('dblclick', (e) => this.onDblClick(e))
    // prefabs and entities dragged in from the Place tab: a half-transparent preview follows the cursor, the drop puts the real thing exactly there
    c.addEventListener('dragover', (e) => {
      const item = dragItem()
      if (!item && !isDragTransfer(e.dataTransfer)) return
      e.preventDefault()
      if (e.dataTransfer) e.dataTransfer.dropEffect = 'copy'
      if (item) this.showGhost(item, e)
    })
    c.addEventListener('dragleave', () => this.clearGhost())
    c.addEventListener('drop', (e) => {
      const item = dragItem() ?? itemFromTransfer(e.dataTransfer)
      if (!item) return
      e.preventDefault()
      this.clearGhost()
      this.drop(item, e)
    })
    this.unsubs.push(onDragItemChange(() => { if (!dragItem()) this.clearGhost() }))
    if (previews) this.unsubs.push(previews.onChange(() => { if (this.ghost) this.invalidate() }))
    c.addEventListener('keydown', (e) => this.onKey(e))
    c.addEventListener('blur', () => this.stopFly())
    this.cams.fly.setFrom(this.cams.orbit) // same starting view in both modes
    this.cams.fly.speed = this.settings.flySpeed
    new ResizeObserver(() => this.invalidate()).observe(this.element)
  }

  init(): void {
    try { this.renderer = new Renderer(this.canvas, this.textures) } catch (e) {
      this.element.append(h('div', { class: 'gl-error' }, `Viewport unavailable: ${(e as Error).message}`))
      return
    }
    this.unsubs.push(this.store.subscribe(() => { this.checkTool(); this.updateLightStatus(); this.invalidate() }), this.textures.onChange(() => this.invalidate()))
    this.bindDocument()
    this.invalidate()
  }
  /** The document the page-level listeners (fly keys, Shift, Esc, closing the View menu) are bound to. A popout moves this
   *  viewport into another window's document, and events there never reach listeners on the main one, so the binding
   *  follows `element.ownerDocument`: it is re-checked whenever the pointer or focus reaches the viewport. */
  private boundDoc: Document | null = null
  private unbindDoc: (() => void) | null = null
  private bindDocument(): void {
    const doc = this.element.ownerDocument
    if (doc === this.boundDoc) return
    this.unbindDoc?.()
    this.stopFly() // a key held in the old window will never send its keyup here
    const key = (e: KeyboardEvent) => {
      if (e.key === 'Shift') { this.shift = e.type === 'keydown'; this.refreshDrawing() }
      if (e.key === 'Alt') {
        // Alt alone would focus the window menu; here it shows the face an Alt + drag grabs
        if (e.type === 'keydown' && this.pointerInside) e.preventDefault()
        this.setAltFace(e.type === 'keydown' && this.pointerInside && this.lastRay ? this.altFaceAt(this.lastRay) : null)
      }
      if (e.key === 'Escape' && this.store.state.pipette) { const cb = this.store.state.pipette; this.store.setPipette(null); cb(null); this.store.setMessage(null) }
      if (e.key === 'Escape' && this.drawing) { this.drawing = null; this.invalidate() }
    }
    // fly keys are caught at the document in the capture phase: while the pointer is locked some browsers stop delivering
    // key events to the canvas, and the app's own shortcuts (Q = select tool ...) must not see them while flying
    const flyDown = (e: KeyboardEvent) => this.onFlyKey(e, true), flyUp = (e: KeyboardEvent) => this.onFlyKey(e, false)
    const away = (ev: PointerEvent) => { const m = this.viewMenu; if (m && !m.contains(ev.target as Node) && !this.viewBtn?.contains(ev.target as Node)) m.style.display = 'none' }
    doc.addEventListener('keydown', key); doc.addEventListener('keyup', key)
    doc.addEventListener('keydown', flyDown, true); doc.addEventListener('keyup', flyUp, true)
    doc.addEventListener('pointerdown', away)
    this.boundDoc = doc
    this.unbindDoc = () => {
      doc.removeEventListener('keydown', key); doc.removeEventListener('keyup', key)
      doc.removeEventListener('keydown', flyDown, true); doc.removeEventListener('keyup', flyUp, true)
      doc.removeEventListener('pointerdown', away)
      this.boundDoc = null; this.unbindDoc = null
    }
  }
  private viewMenu: HTMLElement | null = null
  private viewBtn: HTMLElement | null = null

  /** Set by dispose(); the app drops disposed viewports so tools and keys never target a dead one. */
  disposed = false
  private rafWin: Window = window
  private flyWin: Window = window
  /** Refreshes the lighting progress label in the header (set up by buildHeader). */
  private updateLightStatus: () => void = () => {}

  dispose(): void { if (this.styleTimer) { (this.element.ownerDocument.defaultView ?? window).clearTimeout(this.styleTimer); this.styleTimer = 0 } this.store.setLightingWanted(this, false); this.disposed = true; this.unsubs.forEach((u) => u()); this.unbindDoc?.(); this.renderer?.dispose(); this.stopFly(); if (this.raf) this.rafWin.cancelAnimationFrame(this.raf) }

  invalidate(): void {
    if (this.raf) {
      if (!this.rafWin.closed) return
      this.raf = 0 // the window that owned the pending frame is gone: it will never fire
    }
    const win = this.element.ownerDocument.defaultView ?? window // popouts live in another window
    this.rafWin = win
    this.raf = win.requestAnimationFrame(() => {
      this.raf = 0
      this.stepFly(win.performance.now()) // flying is integrated per rendered frame with the real frame time: no fixed tick, no jitter
      this.render()
      if (this.flyKeys.size) this.invalidate()
    })
  }

  /** Moves the fly camera by the time since the last step. Shared by the frame loop and the fallback timer. */
  private stepFly(now: number): void {
    if (!this.flyKeys.size) { this.flyLast = 0; return }
    const dt = this.flyLast ? Math.min(0.1, Math.max(0, (now - this.flyLast) / 1000)) : 0
    this.flyLast = now
    this.moveFly(dt)
  }

  /** Screen position (page coordinates) of a world point. Used by the e2e test to aim at things. */
  worldToScreen(p: Vec3): { x: number; y: number } {
    const r = this.canvas.getBoundingClientRect()
    const m = this.camera.viewProj(r.width / r.height)
    const w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15]
    const nx = (m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12]) / w
    const ny = (m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13]) / w
    return { x: r.left + ((nx + 1) / 2) * r.width, y: r.top + ((1 - ny) / 2) * r.height }
  }

  /** Editor preferences (Settings > Editor) that arrived after this viewport was made replace its starting settings. */
  applyDefaults(p: Partial<ViewportSettings>): void {
    Object.assign(this.settings, p)
    this.cams.fly.speed = this.settings.flySpeed
    this.element.querySelector(':scope > .vp-header')?.replaceWith(this.buildHeader())
    this.invalidate()
  }

  frameAll(): void {
    const min: Vec3 = [Infinity, Infinity, Infinity], max: Vec3 = [-Infinity, -Infinity, -Infinity]
    let any = false
    for (const e of [...this.store.state.doc.entities, ...this.store.state.prefabEntities.map((p) => p.entity)])
      for (const b of e.brushes ?? []) {
        const bb = brushBounds(b)
        if (!bb) continue
        any = true
        for (let i = 0; i < 3; i++) { min[i] = Math.min(min[i], bb.min[i]); max[i] = Math.max(max[i], bb.max[i]) }
      }
    if (any) this.camera.frame(min, max)
    this.invalidate()
  }

  private checkTool(): void {
    this.canvas.style.cursor = this.store.state.tool === 'select' ? 'default' : 'crosshair'
    if (this.store.state.tool !== 'box' && this.drawing) this.drawing = null
    if (this.store.state.editMode !== 'mesh') { this.loopArmed = false; this.loopPreview = null; this.knife = null }
    if (this.clip && !selectedBrushes(this.store).length) this.stopClipper()
  }

  /** World -> canvas pixels (y down) with depth; null behind the camera. */
  private project: Project = (p) => {
    const w = this.canvas.clientWidth, hgt = this.canvas.clientHeight
    const m = this.camera.viewProj(w / hgt)
    const cw = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15]
    if (cw <= 1e-6) return null
    const nx = (m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12]) / cw, ny = (m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13]) / cw
    return { x: ((nx + 1) / 2) * w, y: ((1 - ny) / 2) * hgt, depth: cw }
  }

  private pickOpts(): { carved: ReturnType<Store['carved']>; instances: Map<import('../model/document').Entity, string>; includeSubtractive: boolean; entityBox: (classname: string) => Box } {
    return { carved: this.store.carved(), instances: new Map(this.store.state.prefabEntities.map((p) => [p.entity, instanceOf(p)])), includeSubtractive: this.settings.showSubtractive, entityBox: (c) => this.store.entityBox(c) }
  }

  /** A single selected brush face (Shift+click) gets one handle along its normal: dragging it moves the face plane (Radiant style). */
  private faceGizmo(): { entityId: string; brushId: string; faceId: number; pivot: Vec3; dir: Vec3 } | null {
    const st = this.store.state, sel = st.selection
    if (st.tool !== 'select' || this.modal || st.editMode !== 'object' || !sel || sel.instanceId !== undefined) return null
    if (sel.brushId === undefined || sel.faceId === undefined || st.multi.length > 1) return null
    const b = findEntity(st.doc, sel.entityId)?.brushes?.find((x) => x.id === sel.brushId)
    const f = b?.faces.find((x) => x.id === sel.faceId)
    if (!b || !f) return null
    return { entityId: sel.entityId, brushId: sel.brushId, faceId: sel.faceId, pivot: centroid(b, f.loop), dir: faceNormal(b, f) }
  }
  /** Test hook: screen positions of the face handle (null when no face is selected). */
  debugFaceHandle(): { from: { x: number; y: number }; to: { x: number; y: number } } | null {
    const g = this.faceGizmo()
    if (!g) return null
    const tip = add(g.pivot, scale(g.dir, gizmoLength(this.camera.distanceTo(g.pivot)) * 0.6))
    return { from: this.worldToScreen(g.pivot), to: this.worldToScreen(tip) }
  }
  private faceHit(e: PointerEvent): ReturnType<ViewportPanel['faceGizmo']> {
    const g = this.store.canEdit ? this.faceGizmo() : null
    if (!g) return null
    const len = gizmoLength(this.camera.distanceTo(g.pivot))
    return hitDir(this.ray(e), g.pivot, g.dir, len * 1.15, len * 0.07) ? g : null
  }

  /** The document face under `ray` that Alt + drag would grab (Brush mode, Select tool), or null. */
  private altFaceAt(ray: Ray): { entityId: string; brushId: string; faceId: number } | null {
    const st = this.store.state
    if (st.tool !== 'select' || st.editMode !== 'object' || this.modal || this.clip || st.pipette || !this.store.canEdit || this.drawing) return null
    const { hit, instanceId } = this.pickAny(ray)
    if (!hit || instanceId !== undefined || hit.brushId === undefined || hit.faceId === undefined) return null
    return findEntity(st.doc, hit.entityId)?.brushes?.some((b) => b.id === hit.brushId) ? { entityId: hit.entityId, brushId: hit.brushId, faceId: hit.faceId } : null
  }
  private setAltFace(f: ViewportPanel['altFace']): void {
    const a = this.altFace
    if (a === f || (a && f && a.entityId === f.entityId && a.brushId === f.brushId && a.faceId === f.faceId)) return
    this.altFace = f
    this.invalidate()
  }
  /** Alt + LMB on any face: select it and start dragging it (resizes its brush, REQUIREMENTS §33 H8). */
  private startAltFaceDrag(e: PointerEvent): boolean {
    const f = this.altFaceAt(this.ray(e))
    if (!f) return false
    const b = findEntity(this.store.state.doc, f.entityId)?.brushes?.find((x) => x.id === f.brushId)
    const face = b?.faces.find((x) => x.id === f.faceId)
    if (!b || !face) return false
    this.store.select({ entityId: f.entityId, brushId: f.brushId, faceId: f.faceId })
    if (!this.store.beginDrag('Resize brush')) return false
    const pivot = centroid(b, face.loop), dir = faceNormal(b, face)
    this.faceGood = null; this.extended = null
    this.drag = { kind: 'face', entityId: f.entityId, brushId: f.brushId, faceId: f.faceId, pivot, dir, startT: lineDistance(this.ray(e), pivot, dir) ?? 0 }
    this.setAltFace(null)
    return true
  }

  /** Size of the selected brushes (one label per axis) or of the selected face (REQUIREMENTS §33 H7). */
  private sizeLabels(): SizeLabel[] {
    const st = this.store.state
    if (!this.settings.showSizes || st.editMode !== 'object' || this.drawing) return []
    const mpp = (p: Vec3) => this.mppAt(p)
    const fg = this.faceGizmo()
    if (fg) {
      const b = findEntity(st.doc, fg.entityId)?.brushes?.find((x) => x.id === fg.brushId), f = b?.faces.find((x) => x.id === fg.faceId)
      return b && f ? withoutOverlaps(faceSizeLabels(b, f, mpp), (p) => this.project(p)) : []
    }
    const brushes = selectedBrushes(this.store)
    if (!brushes.length) return []
    const min: Vec3 = [Infinity, Infinity, Infinity], max: Vec3 = [-Infinity, -Infinity, -Infinity]
    for (const sb of brushes) { const bb = brushBounds(sb.brush); if (bb) for (let i = 0; i < 3; i++) { min[i] = Math.min(min[i], bb.min[i]); max[i] = Math.max(max[i], bb.max[i]) } }
    if (!min.every(Number.isFinite)) return []
    const cam = this.camera
    const skip = cam instanceof OrthoCamera ? cam.forward.findIndex((x) => Math.abs(x) > 0.9) : -1
    return boxSizeLabels(min, max, (p) => this.project(p), mpp, skip)
  }

  /** Mesh mode: the edited brush's smooth edges in green (the other edges are light blue; REQUIREMENTS §33 H6). */
  private smoothEdgeOverlay(b: Brush | undefined): { color: [number, number, number]; lines: number[] }[] {
    if (!b) return []
    const keys = smoothEdgeKeys(b)
    if (!keys.size) return []
    const lines: number[] = []
    for (const k of keys) { const [x, y] = k.split('-').map(Number); lines.push(...b.vertices[x], ...b.vertices[y]) }
    return [{ color: [0.3, 1, 0.35], lines }]
  }

  // ---- entity handles (REQUIREMENTS §33 H1 / H2) ----------------------------------------------------------------------------
  /** The one selected entity of the document that shows the origin handle (and, pointing somewhere, the angle handle). */
  private entityHandles(): { entity: Entity; origin: Vec3; angle: { base: Vec3; tip: Vec3 } | null } | null {
    const st = this.store.state, sel = st.selection
    if (st.tool !== 'select' || st.editMode !== 'object' || this.modal || this.clip || st.pipette || this.drawing || !this.store.canEdit) return null
    if (!sel || sel.instanceId !== undefined || sel.brushId !== undefined || st.multi.length > 1) return null
    const entity = findEntity(st.doc, sel.entityId)
    if (!entity || classOf(entity) === 'worldspawn' || (isBrushEntity(entity) && !showsCustomOrigin(entity))) return null
    return { entity, origin: originOf(entity), angle: hasAngleHandle(entity) ? angleHandle(entity, (c) => this.store.entityBox(c)) : null }
  }
  private entityHandleHit(e: { clientX: number; clientY: number }): 'origin' | 'angle' | null {
    const hs = this.entityHandles()
    if (!hs) return null
    const m = this.canvasPoint(e)
    const near = (p: Vec3): number => { const q = this.project(p); return q ? Math.hypot(q.x - m.x, q.y - m.y) : Infinity }
    if (hs.angle && near(hs.angle.tip) <= 9) return 'angle'
    return near(hs.origin) <= 8 ? 'origin' : null
  }
  /** Metres per screen pixel at `p`. */
  private mppAt(p: Vec3): number {
    const cam = this.camera, hgt = this.canvas.clientHeight
    return cam instanceof OrthoCamera ? cam.metresPerPixel(hgt) : metresPerPixel(cam.distanceTo(p), cam.fovY, hgt)
  }
  private entityHandleOverlay(): { color: [number, number, number]; lines: number[] }[] {
    const hs = this.entityHandles()
    if (!hs) return []
    const { right, up } = this.camera.basis
    const active = this.originSelected() ? 'origin' : this.drag?.kind === 'angle' ? 'angle' : this.hoverEntHandle
    const out: { color: [number, number, number]; lines: number[] }[] = [
      { color: active === 'origin' ? [1, 1, 1] : [1, 0.85, 0.2], lines: handleLines(hs.origin, 5 * this.mppAt(hs.origin), right, up, 'origin') },
    ]
    if (hs.angle) out.push({ color: active === 'angle' ? [1, 1, 1] : [1, 0.45, 0.15], lines: [...handleLines(hs.angle.tip, 5 * this.mppAt(hs.angle.tip), right, up, 'angle'), ...(active === 'angle' ? [...hs.angle.base, ...hs.angle.tip] : [])] })
    return out
  }
  /** Whether the selected entity's origin is the thing being edited (set by clicking the origin square; ends with any selection change, a click elsewhere or Esc). */
  private originSelected(): boolean {
    if (this.originSel === null) return false
    if (this.entityHandles()?.entity.id === this.originSel) return true
    this.originSel = null
    return false
  }
  /** The gizmo in use: with the origin selected it is always the move gizmo, whatever mode the toolbar is in. */
  private gizmoMode(): 'move' | 'rotate' | 'scale' { return this.originSelected() ? 'move' : this.settings.gizmo }
  /** The plane the angle handle's drag runs on: the view plane (orthographic, or Shift in perspective), else the horizontal plane. */
  private handlePlane(shift: boolean): Vec3 {
    const cam = this.camera
    if (cam instanceof OrthoCamera) return cam.forward
    return shift ? cam.basis.forward : [0, 1, 0]
  }
  private startEntityHandleDrag(e: PointerEvent): boolean {
    const which = this.entityHandleHit(e), hs = this.entityHandles()
    if (!which || !hs) return false
    if (which === 'origin') {
      // the square does not drag on a plane: clicking it selects the origin, and the usual move gizmo (axis arrows) moves it
      this.originSel = hs.entity.id
      this.hoverAxis = null
      this.store.setMessage('Origin selected: drag an arrow to move it (Esc or a click elsewhere to finish)')
      this.invalidate()
      return true
    }
    const n = this.handlePlane(e.shiftKey)
    if (!hs.angle || !this.store.beginDrag('Turn entity')) return false
    this.drag = { kind: 'angle', entityId: hs.entity.id, base: hs.angle.base, n, keepPitch: Math.abs(n[1]) > 0.9 }
    return true
  }
  private entityHandleMove(e: PointerEvent, d: Extract<NonNullable<ViewportPanel['drag']>, { kind: 'angle' }>): void {
    const ray = this.ray(e)
    const at = rayPlaneHit(ray, d.base, d.n)
    if (!at || dot(sub(at, ray.origin), ray.dir) < 0) return
    const snapDeg = (rotSnapRad(this.settings) * 180) / Math.PI
    const cur = findEntity(this.store.state.doc, d.entityId)
    const a = cur ? anglesToward(cur.angles, sub(at, d.base), d.keepPitch, snapDeg) : null
    if (!a) return
    this.store.dragUpdate((start) => mapEntity(start, d.entityId, (x) => ({ ...x, angles: [d.keepPitch ? (x.angles?.[0] ?? 0) : a[0], a[1], x.angles?.[2] ?? 0] })))
    this.store.setMessage(`Angle: yaw ${+a[1].toFixed(2)}°, pitch ${+a[0].toFixed(2)}°`)
  }

  /** Where the gizmo sits now: the selection's pivot (object mode) or the selected elements' centre (mesh mode). */
  private pivotNow(): Vec3 | null {
    const st = this.store.state
    if (st.tool !== 'select' || this.modal) return null
    if (st.editMode === 'object') return this.faceGizmo() ? null : gizmoPivot(st.doc, selectedList(this.store))
    if (st.editMode === 'mesh') {
      const t = this.store.meshTarget()
      return t && st.meshSel.verts.length ? centroid(t.brush, st.meshSel.verts) : null
    }
    return null
  }

  private render(): void {
    const st = this.store.state
    const sel = st.selection
    const pivot = this.pivotNow()
    const selBrush = sel?.brushId !== undefined && sel.faceId !== undefined
      ? st.doc.entities.find((e) => e.id === sel.entityId)?.brushes?.find((b) => b.id === sel.brushId) : undefined
    const mt = st.editMode === 'mesh' ? this.store.meshTarget() : null
    const preview = this.previewLines()
    if (this.needsLighting()) { this.store.reportCamera([this.camera.eye[0], this.camera.eye[1], this.camera.eye[2]]); this.store.syncRelief() }
    this.renderer?.draw({
      camera: this.camera, settings: this.settings, lighting: this.store.lightmap, lightvol: this.store.lightvol, world: { doc: st.doc, prefabs: st.prefabEntities }, selection: sel, selections: st.editMode === 'object' ? st.multi : undefined,
      entityColor: (c) => this.store.entityColor(c), entityBox: (c) => this.store.entityBox(c), ghost: this.ghost, carved: this.store.carved(), decals: this.store.decals(), decalEntries: this.store.decalEntries(),
      entities: [
        ...visibleEntities(st.doc.entities, st.hidden).map((entity) => ({ entity, readonly: false })),
        ...st.prefabEntities.map((p) => ({ entity: p.entity, readonly: true, instanceId: p.instancePath[1], instance: instanceOf(p) })),
      ],
      gizmo: this.faceGizmo() && this.store.canEdit ? { pivot: this.faceGizmo()!.pivot, dir: this.faceGizmo()!.dir, length: gizmoLength(this.camera.distanceTo(this.faceGizmo()!.pivot)), active: this.drag?.kind === 'face' || this.hoverFace ? 0 : null }
        : pivot && this.store.canEdit ? { pivot, length: gizmoLength(this.camera.distanceTo(pivot)), active: this.drag?.kind === 'gizmo' ? this.drag.axis : this.hoverAxis, mode: this.gizmoMode() } : null,
      annotations: this.settings.showOrigins ? entityAnnotations([...visibleEntities(st.doc.entities, st.hidden), ...st.prefabEntities.map((p) => p.entity)]) : undefined,
      overlays: [...this.arrowOverlays(), ...this.lightOverlays(), ...this.decalSelectionOverlay()],
      handles: [...this.entityHandleOverlay(), ...this.smoothEdgeOverlay(mt?.brush)],
      sizeLabels: this.sizeLabels(),
      preview: [...(preview ?? []), ...(this.loopPreview ?? []), ...this.knifeOverlay(), ...this.clipOverlay(), ...this.resizeOverlay(), ...this.terrainCursor()],
      highlight: selBrush && sel!.faceId !== undefined ? { brush: selBrush, faceId: sel!.faceId } : null,
      hoverFace: this.altFace && !this.drag ? (() => { const a = this.altFace!, b = findEntity(st.doc, a.entityId)?.brushes?.find((x) => x.id === a.brushId); return b ? { brush: b, faceId: a.faceId } : null })() : null,
      sky: viewportSky(st.doc),
      mesh: mt ? { brush: mt.brush, mode: st.meshMode, sel: st.meshSel } : null,
      time: this.styleTimeOverride ?? (this.element.ownerDocument.defaultView ?? window).performance.now() / 1000,
    })
    // light styles play at 10 Hz: while an animated layer is shown in a lit view, redraw at 20 Hz (nothing else would trigger a frame)
    const lm = this.store.lightmap
    if (this.styleTimeOverride === null && this.needsLighting() && lm.ready && lm.styles.some((style) => styleAnimates(lm.patterns, style))) this.scheduleStyleFrame()
  }

  private styleTimeOverride: number | null = null
  /** Freezes light style playback at `seconds` (inspect one frame of an animated style, tests); null = play in real time. */
  setStyleTime(seconds: number | null): void { this.styleTimeOverride = seconds; this.invalidate() }

  private styleTimer = 0
  private scheduleStyleFrame(): void {
    if (this.styleTimer || this.disposed) return
    const win = this.element.ownerDocument.defaultView ?? window
    this.styleTimer = win.setTimeout(() => { this.styleTimer = 0; this.invalidate() }, 50)
  }

  // ---- input ------------------------------------------------------------------------
  private rayAtCanvas(p: { x: number; y: number }): Ray {
    const r = this.canvas.getBoundingClientRect()
    return this.camera.rayFromNdc((p.x / r.width) * 2 - 1, 1 - (p.y / r.height) * 2, r.width / r.height)
  }
  private ray(e: PointerEvent): Ray {
    const r = this.canvas.getBoundingClientRect()
    return this.camera.rayFromNdc(((e.clientX - r.left) / r.width) * 2 - 1, 1 - ((e.clientY - r.top) / r.height) * 2, r.width / r.height)
  }
  private gizmoHit(e: PointerEvent): { axis: Axis; pivot: Vec3 } | null {
    if (!this.store.canEdit) return null
    const pivot = this.pivotNow()
    if (!pivot) return null
    const len = gizmoLength(this.camera.distanceTo(pivot))
    const axis = this.gizmoMode() === 'rotate' ? hitRing(this.ray(e), pivot, len, len * 0.06) : hitAxis(this.ray(e), pivot, len * 1.15, len * 0.07)
    return axis === null ? null : { axis, pivot }
  }

  /**
   * Where something dragged in from the Place tab would land: the surface under the cursor, else the ground, else the plane through
   * the camera target facing the camera (orthographic views). The same function drives the live preview and the drop, so they agree.
   */
  private dropOrigin(item: DragItem, e: { clientX: number; clientY: number }): Vec3 {
    const ray = this.ray(e as PointerEvent)
    const { hit } = this.pickAny(ray)
    let p: Vec3 | null = hit ? hit.point : null
    let n: Vec3 = hit ? hit.normal : [0, 1, 0]
    if (!p && Math.abs(ray.dir[1]) > 1e-6 && -ray.origin[1] / ray.dir[1] > 0) p = add(ray.origin, scale(ray.dir, -ray.origin[1] / ray.dir[1]))
    if (!p) {
      const f = this.camera.basis.forward, den = dot(ray.dir, f)
      if (Math.abs(den) > 1e-6) { const t = dot(sub(this.camera.target, ray.origin), f) / den; if (t > 0) { p = add(ray.origin, scale(ray.dir, t)); n = [-f[0], -f[1], -f[2]] } }
    }
    const step = snapStep(this.settings)
    if (item.kind === 'prefab') return p ? prefabOrigin(p, step) : [0, 0, 0]
    return entityOrigin(p ?? [0, 0, 0], p ? n : [0, 1, 0], this.store.entityBox(item.classname), step)
  }

  private drop(item: DragItem, e: { clientX: number; clientY: number }): void {
    const at = this.dropOrigin(item, e)
    if (item.kind === 'prefab') spawnInstance(this.store, item.file, at)
    else addPointEntity(this.store, item.classname, at, item.keyvalues)
  }

  /** The preview of what is being dragged in: its content at its own origin (cached objects, so GPU buffers are reused) plus where it would land. */
  private ghost: { entities: Entity[]; offset: Vec3 } | null = null
  private ghostEntities = new Map<string, Entity>()
  private showGhost(item: DragItem, e: { clientX: number; clientY: number }): void {
    let entities: Entity[]
    if (item.kind === 'entity') {
      let g = this.ghostEntities.get(item.classname)
      if (!g) this.ghostEntities.set(item.classname, (g = { id: 'ghost', classname: item.classname, origin: [0, 0, 0] }))
      entities = [g]
    } else {
      // while the prefab is still being read from Core, a plain box stands in for it
      entities = this.previews?.entities(item.file) ?? [this.placeholder()]
    }
    this.ghost = { entities, offset: this.dropOrigin(item, e) }
    this.invalidate()
  }
  private placeholderEntity: Entity | null = null
  /** last element clicked in Mesh mode: where a Ctrl+click path starts */
  private meshActive: ActiveElem = null
  /** Ctrl+M was pressed in Mesh mode: the next key picks the mirror axis */
  private mirrorArmed = false
  /** Knife (K) in progress: the points clicked so far on `brush` (the brush the cut was started on) and the point under the cursor */
  private knife: { brush: Brush; points: KnifePoint[]; hover: KnifePoint | null } | null = null
  private placeholder(): Entity { return (this.placeholderEntity ??= { id: 'ghost', classname: '__prefab', origin: [0, 0, 0] }) }
  private clearGhost(): void { if (this.ghost) { this.ghost = null; this.invalidate() } }

  private onDown(e: PointerEvent): void {
    this.canvas.focus()
    if (this.modal) { // a modal operation owns the mouse: LMB confirms, RMB cancels
      if (e.button === 0) this.endModal(true); else if (e.button === 2) this.endModal(false)
      return
    }
    if (e.button === 2 && this.knife) { this.finishKnife(); return }
    if (e.altKey) e.preventDefault() // Alt+LMB belongs to the editor (resize by a face, loop select), not to the browser / window menu
    this.canvas.setPointerCapture(e.pointerId)
    this.shift = e.shiftKey
    this.rmbPanning = e.button === 2 && isOrthoMode(this.settings.cameraMode)
    if (e.button === 2) { this.rmbTravel = 0; this.rmbDown = { x: e.clientX, y: e.clientY } }
    if (e.button === 2) { this.drag = { kind: this.settings.cameraMode === 'fly' ? 'look' : isOrthoMode(this.settings.cameraMode) ? 'pan' : 'orbit', x: e.clientX, y: e.clientY } } // the pointer is locked only once it really drags: a plain right-click must keep a usable cursor for the context menu
    else if (e.button === 1) { this.drag = { kind: 'pan', x: e.clientX, y: e.clientY }; this.lockPointer() }
    else if (e.button === 0) {
      if (this.clip) this.clipClick(e) // an armed clipper owns the LMB whatever tool is active
      else if (this.store.state.tool === 'box') this.boxDown(e)
      else if (this.store.state.tool === 'decal') this.decalDown(e)
      else if (this.store.state.tool === 'terrain') this.terrainDown(e)
      else this.selectDown(e)
    }
    this.invalidate()
  }

  // ---- fly camera --------------------------------------------------------------------
  /** Wheel: dolly (fly) / zoom (orbit). With RMB held in fly mode it changes the fly speed instead, as in UE. */
  private onWheel(e: WheelEvent): void {
    e.preventDefault()
    if (this.modal) { if (this.modal.wheel?.(e.deltaY)) this.invalidate(); return } // a modal owns the wheel (bevel segments); it never zooms meanwhile
    if (this.settings.cameraMode === 'fly' && this.drag?.kind === 'look') {
      this.setFlySpeed(this.settings.flySpeed * (e.deltaY < 0 ? WHEEL_SPEED_FACTOR : 1 / WHEEL_SPEED_FACTOR)) // one notch = x1.2 / ÷1.2
    } else this.camera.zoom(e.deltaY)
    this.invalidate()
  }

  /** Pointer lock is best effort: if the browser refuses, dragging falls back to ordinary cursor motion. */
  private lockPointer(): void {
    try { const r = this.canvas.requestPointerLock() as unknown; if (r && typeof (r as Promise<void>).catch === 'function') (r as Promise<void>).catch(() => {}) } catch { /* not allowed here */ }
  }
  private unlockPointer(): void {
    const d = this.canvas.ownerDocument
    if (d.pointerLockElement === this.canvas) d.exitPointerLock()
  }

  setFlySpeed(raw: number): void {
    const v = clampSpeed(raw)
    this.settings.flySpeed = v
    this.cams.fly.speed = v
    const shown = v >= 10 ? String(Math.round(v * 10) / 10) : String(Math.round(v * 100) / 100)
    if (this.speedText && this.speedText.ownerDocument.activeElement !== this.speedText) this.speedText.value = shown
    if (this.speedSlider) this.speedSlider.value = String(Math.log10(v))
    this.store.setMessage(`Fly speed ${shown} m/s`)
  }

  setCameraMode(mode: CameraMode): void {
    if (mode === this.settings.cameraMode) return
    const from = this.camera
    this.stopFly()
    this.settings.cameraMode = mode
    this.camera.setFrom(from)
    if (this.cameraSelect) this.cameraSelect.value = mode
    if (this.speedBox) this.speedBox.style.display = mode === 'fly' ? '' : 'none'
    this.invalidate()
  }

  /** Fly direction for a key, as [ahead, right, up]. W/S forward/back, A/D strafe, E/Q up/down (UE convention). */
  private static readonly FLY_KEYS: Record<string, [number, number, number]> = {
    KeyW: [1, 0, 0], KeyS: [-1, 0, 0], KeyD: [0, 1, 0], KeyA: [0, -1, 0], KeyE: [0, 0, 1], KeyQ: [0, 0, -1],
  }

  /** While RMB-flying these keys belong to the camera, not to the app's tool shortcuts (Q = select tool etc.). */
  private onFlyKey(e: KeyboardEvent, down: boolean): void {
    if (this.settings.cameraMode !== 'fly' || !(e.code in ViewportPanel.FLY_KEYS)) return
    if (down) {
      if (this.drag?.kind !== 'look' || e.ctrlKey || e.metaKey || e.altKey) return
      e.preventDefault(); e.stopPropagation()
      this.flyKeys.add(e.code)
      this.startFly()
    } else if (this.flyKeys.delete(e.code)) e.stopPropagation()
  }

  /** Flight is driven by a timer, not requestAnimationFrame: some browsers throttle animation frames while the pointer is locked. */
  private startFly(): void {
    if (this.flyTimer || !this.flyKeys.size) return
    const win = this.element.ownerDocument.defaultView ?? window
    this.flyWin = win
    this.flyLast = win.performance.now()
    this.invalidate()
    // fallback only: some browsers stop delivering animation frames while the pointer is locked. It steps only when the
    // frame loop has been silent for a while and uses the same timestamp, so the two never double-move or stutter.
    this.flyTimer = win.setInterval(() => {
      if (!this.flyKeys.size) return this.stopFly()
      const now = win.performance.now()
      if (now - this.flyLast > 50) { this.stepFly(now); this.invalidate() }
    }, 25)
  }

  private stopFly(): void {
    this.flyKeys.clear()
    if (this.flyTimer) this.flyWin.clearInterval(this.flyTimer)
    this.flyTimer = 0
  }

  /** Move the fly camera for `dt` seconds according to the held keys (Shift = 3x). Public so tests can step it. */
  advance(dt: number): void { if (this.moveFly(dt)) this.invalidate() }

  private moveFly(dt: number): boolean {
    let a = 0, r = 0, u = 0
    for (const k of this.flyKeys) { const d = ViewportPanel.FLY_KEYS[k]; a += d[0]; r += d[1]; u += d[2] }
    if (!a && !r && !u) return false
    const dist = this.settings.flySpeed * (this.shift ? 3 : 1) * dt
    this.cams.fly.move(a * dist, r * dist, u * dist)
    return true
  }

  /** Exposed for tests. */
  setHeldKeys(codes: string[]): void { this.flyKeys = new Set(codes) }

  // ---- 2D resize handles (orthographic views) ----------------------------------------------------------------
  /** Bounds of the selected brushes plus the eight handles, when this is an ortho view in object mode with a brush selected. */
  private resizeInfo(): { min: Vec3; max: Vec3; r: ScreenAxis; u: ScreenAxis; handles: Handle2D[] } | null {
    const cam = this.camera, st = this.store.state
    if (!(cam instanceof OrthoCamera) || st.tool !== 'select' || st.editMode !== 'object' || this.modal || this.clip || st.pipette || !this.store.canEdit) return null
    const brushes = selectedBrushes(this.store)
    if (!brushes.length || this.faceGizmo()) return null
    const min: Vec3 = [Infinity, Infinity, Infinity], max: Vec3 = [-Infinity, -Infinity, -Infinity]
    for (const sb of brushes) {
      const bb = brushBounds(sb.brush)
      if (!bb) continue
      for (let i = 0; i < 3; i++) { min[i] = Math.min(min[i], bb.min[i]); max[i] = Math.max(max[i], bb.max[i]) }
    }
    if (!min.every(Number.isFinite)) return null
    const b = cam.basis, r = screenAxis(b.right), u = screenAxis(b.up)
    return { min, max, r, u, handles: handles2D(min, max, r, u) }
  }
  private handleHit(e: PointerEvent): Handle2D | null {
    const info = this.resizeInfo()
    if (!info) return null
    const m = this.canvasPoint(e)
    let best: Handle2D | null = null, bd = 10
    for (const hd of info.handles) {
      const s = this.project(hd.pos)
      if (!s) continue
      const d = Math.hypot(s.x - m.x, s.y - m.y)
      if (d <= bd) { bd = d; best = hd }
    }
    return best
  }
  /** Small squares on the handles (drawn through the line preview). */
  private resizeOverlay(): number[] {
    const info = this.resizeInfo(), cam = this.camera
    if (!info || !(cam instanceof OrthoCamera)) return []
    const half = 4 * cam.metresPerPixel(this.canvas.clientHeight)
    const { right, up } = cam.basis
    const out: number[] = []
    const at = (c: Vec3, a: number, b: number): Vec3 => [c[0] + right[0] * a + up[0] * b, c[1] + right[1] * a + up[1] * b, c[2] + right[2] * a + up[2] * b]
    for (const hd of info.handles) {
      const q = [at(hd.pos, -half, -half), at(hd.pos, half, -half), at(hd.pos, half, half), at(hd.pos, -half, half)]
      for (let i = 0; i < 4; i++) out.push(...q[i], ...q[(i + 1) % 4])
    }
    const c = (a: number, b: number): Vec3 => { const p: Vec3 = [(info.min[0] + info.max[0]) / 2, (info.min[1] + info.max[1]) / 2, (info.min[2] + info.max[2]) / 2]; p[info.r.axis] = a; p[info.u.axis] = b; return p }
    const [x0, x1, y0, y1] = [info.min[info.r.axis], info.max[info.r.axis], info.min[info.u.axis], info.max[info.u.axis]]
    const corners = [c(x0, y0), c(x1, y0), c(x1, y1), c(x0, y1)]
    for (let i = 0; i < 4; i++) out.push(...corners[i], ...corners[(i + 1) % 4])
    return out
  }

  private selectDown(e: PointerEvent): void {
    if (this.store.state.pipette) return this.pipetteClick(e)
    if (this.clip) return this.clipClick(e)
    if (this.knife) return this.meshDown(e) // a knife in progress owns the clicks, even over the gizmo
    const mesh = this.store.state.editMode === 'mesh'
    const rh = e.shiftKey || e.ctrlKey || e.metaKey ? null : this.handleHit(e)
    const ri = rh ? this.resizeInfo() : null
    if (rh && ri && this.store.beginDrag('Resize')) {
      this.originSel = null
      const m = this.canvasPoint(e)
      this.drag = { kind: 'resize', h: rh.h, v: rh.v, min: ri.min, max: ri.max, r: ri.r, u: ri.u, x0: m.x, y0: m.y }
      return
    }
    // Shift / Ctrl clicks are selection clicks: they never grab the gizmo that may be sitting under the cursor
    const plain = !(e.shiftKey || e.ctrlKey || e.metaKey || mesh)
    const fg = plain ? this.faceHit(e) : null
    if (!fg && plain && e.altKey && this.startAltFaceDrag(e)) return
    if (!fg && !(e.ctrlKey || e.metaKey || e.altKey || mesh) && this.startEntityHandleDrag(e)) return
    if (fg) {
      // Alt + drag (on the handle or on the face itself) resizes the brush in place, outward and inward (REQUIREMENTS §17 #4, §24 C1); it no longer
      // grows a new brush (extendFace stays in the model for the day a separate "extrude brush" key wants it)
      const extend = false
      if (this.store.beginDrag(e.altKey ? 'Resize brush' : 'Drag face')) {
        this.faceGood = null; this.extended = null
        this.drag = { kind: 'face', entityId: fg.entityId, brushId: fg.brushId, faceId: fg.faceId, pivot: fg.pivot, dir: fg.dir, startT: lineDistance(this.ray(e), fg.pivot, fg.dir) ?? 0, ...(extend ? { extend: { brushId: newId('b') } } : {}) }
        return
      }
    }
    const gm = this.gizmoMode()
    // Shift + drag the move gizmo's arrow with faces selected in Mesh mode = extrude along it (Scythe, REQUIREMENTS §21 E6)
    const st0 = this.store.state
    const extrude = mesh && e.shiftKey && !(e.ctrlKey || e.metaKey) && gm === 'move' && st0.meshMode === 'face' && st0.meshSel.faces.length ? [...st0.meshSel.faces] : null
    const g = (e.shiftKey && !extrude) || e.ctrlKey || e.metaKey ? null : this.gizmoHit(e)
    const originId = !mesh && this.originSelected() ? this.originSel : null // the gizmo then moves the origin alone
    if (g && this.store.beginDrag(extrude ? 'Extrude' : originId ? 'Move origin' : `${gm === 'move' ? 'Move' : gm === 'rotate' ? 'Rotate' : 'Scale'}${mesh ? ' elements' : ''}`)) {
      this.startSel = this.store.state.meshSel; this.lastGood = null; this.detailId = null; this.converted = null; this.dragBrush = this.currentBrush()
      const startT = gm === 'rotate' ? ringAngle(this.ray(e), g.pivot, g.axis) ?? 0 : axisDistance(this.ray(e), g.pivot, g.axis) ?? 0
      this.drag = { kind: 'gizmo', axis: g.axis, pivot: g.pivot, startT, mesh, mode: gm, ...(extrude ? { extrude } : {}), ...(originId ? { origin: originId } : {}) }
      return
    }
    if (mesh) return this.meshDown(e)
    this.originSel = null // a click that grabs neither the origin nor its gizmo ends the origin editing
    if (this.loopArmed) this.loopArmed = false
    // carved brushes are hit where they really are; prefab content selects its INSTANCE (the content itself is read-only)
    const { hit, instanceId } = this.pickAny(this.ray(e))
    // Radiant style: LMB = brush, Shift+LMB = face, Ctrl+LMB = add to / remove from the multi-selection (Ctrl+Shift = faces)
    const face = e.shiftKey && hit?.brushId !== undefined && instanceId === undefined
    const sel: Selection | null = instanceId !== undefined ? { entityId: '', instanceId } : hit ? wholeEntityIfSole(this.store.state.doc, { entityId: hit.entityId, brushId: hit.brushId, ...(face ? { faceId: hit.faceId } : {}) }) : null
    if (sel && (e.ctrlKey || e.metaKey)) this.store.toggleSelect(sel)
    else if (sel?.brushId !== undefined && sel.faceId === undefined && instanceId === undefined) {
      // a click on any part of a placed shape selects the whole shape (REQUIREMENTS §22 P1)
      const owner = this.store.state.doc.entities.find((x) => x.id === sel.entityId)
      const parts = shapeMembers(owner, sel.brushId)
      // a click on a tile of a terrain selects the whole terrain (REQUIREMENTS §39), so moving or deleting it takes every tile
      const tile = owner?.brushes?.find((b) => b.id === sel.brushId)
      if (owner && tile && tileOf(tile) && terrainSpecOf(owner)) this.store.select({ entityId: sel.entityId })
      else if (parts.length > 1) this.store.selectMany(parts.map((brushId) => ({ entityId: sel.entityId, brushId })))
      else this.store.select(sel)
    } else this.store.select(sel)
  }

  /** Nearest thing under the ray among the document's own entities and prefab content; prefab hits name their top-level instance. */
  private pickAny(ray: Ray): { hit: Hit | null; instanceId?: string } {
    const st = this.store.state
    const own = pick(ray, visibleEntities(st.doc.entities, st.hidden), this.pickOpts())  // hidden things are not picked
    const pre = st.prefabEntities.length ? pick(ray, st.prefabEntities.map((p) => p.entity), this.pickOpts()) : null
    if (pre && (!own || pre.t < own.t)) {
      const r = st.prefabEntities.find((p) => p.entity === pre.entity)
      if (r) return { hit: pre, instanceId: r.instancePath[1] }
    }
    return { hit: own }
  }

  /** Double-click selects every brush of the entity under the cursor. */
  /** Loop, ring, path or plain selection of the element under the cursor (REQUIREMENTS §21 E1); null when nothing is there. */
  private meshClick(e: MouseEvent, loop: boolean): ReturnType<typeof clickSelect> {
    const t = this.store.meshTarget()
    if (!t) return null
    const st = this.store.state, p = this.canvasPoint(e), b = t.brush, mode = st.meshMode
    const face = mode === 'face' ? pickFaceId(b, this.ray(e as PointerEvent)) : null
    const at: MeshPickAt = {
      vertex: mode === 'vertex' ? pickVertex(b, this.project, p.x, p.y) : null,
      edge: mode === 'face' ? null : pickEdge(b, this.project, p.x, p.y, loop && mode === 'vertex' ? 24 : 8),
      face, faceEdge: face !== null && loop ? nearestFaceEdge(b, face, this.project, p.x, p.y) : null,
    }
    // the active element only counts while it is still selected (another tool may have replaced the selection)
    const active = this.meshActive && (st.meshSel[this.meshActive.mode === 'vertex' ? 'verts' : this.meshActive.mode === 'edge' ? 'edges' : 'faces'] as (number | string)[]).includes(this.meshActive.id) ? this.meshActive : null
    return clickSelect(b, mode, at, { shift: e.shiftKey, ctrl: e.ctrlKey || e.metaKey, alt: e.altKey }, st.meshSel, active, loop)
  }

  private onDblClick(e: MouseEvent): void {
    const st = this.store.state
    if (st.tool === 'select' && st.editMode === 'mesh' && !this.modal) { // double-click = loop (Scythe / Blender's Alt+click)
      const r = this.meshClick(e, true)
      if (r) { this.meshActive = r.active; this.store.setMeshSel(r.sel) }
      return
    }
    if (st.tool !== 'select' || st.editMode !== 'object') return
    const { hit, instanceId } = this.pickAny(this.ray(e as PointerEvent))
    if (!hit || instanceId !== undefined) return // an instance is already selected as a whole by one click
    const ent = st.doc.entities.find((x) => x.id === hit.entityId)
    if (!ent) return
    this.store.selectMany(ent.brushes?.length ? (ent.brushes.length === 1 ? [wholeEntityIfSole(st.doc, { entityId: ent.id, brushId: ent.brushes[0].id })] : ent.brushes.map((b) => ({ entityId: ent.id, brushId: b.id }))) : [{ entityId: ent.id }])
  }

  // ---- mesh edit mode: element selection -----------------------------------------------------
  private canvasPoint(e: { clientX: number; clientY: number }): { x: number; y: number } {
    const r = this.canvas.getBoundingClientRect()
    return { x: e.clientX - r.left, y: e.clientY - r.top }
  }

  private meshDown(e: PointerEvent): void {
    const t = this.store.meshTarget()
    if (!t) return
    const st = this.store.state, p = this.canvasPoint(e), b = t.brush
    if (this.knife) { const k = this.knifePointAt(e); if (k) { this.knife.points.push(k); this.store.setMessage(`Knife: ${this.knife.points.length} point(s); Enter or right-click cuts, Esc cancels`); this.invalidate() } return }
    if (this.loopArmed) {
      const edge = pickEdge(b, this.project, p.x, p.y)
      if (edge) { this.loopArmed = false; this.loopPreview = null; this.startModal('loopcut', edge) }
      return
    }
    const r = this.meshClick(e, e.altKey)
    if (r) { this.meshActive = r.active; this.store.setMeshSel(r.sel); return }
    this.boxSel = { x0: p.x, y0: p.y, x1: p.x, y1: p.y, toggle: e.shiftKey, base: st.meshSel, moved: false }
  }

  private updateBoxSel(p: { x: number; y: number }): void {
    const bs = this.boxSel, t = this.store.meshTarget()
    if (!bs || !t) return
    bs.x1 = p.x; bs.y1 = p.y
    if (Math.hypot(bs.x1 - bs.x0, bs.y1 - bs.y0) > 4) bs.moved = true
    if (!bs.moved) return
    const el = this.boxEl
    el.style.display = ''
    el.style.left = `${Math.min(bs.x0, bs.x1)}px`; el.style.top = `${Math.min(bs.y0, bs.y1)}px`
    el.style.width = `${Math.abs(bs.x1 - bs.x0)}px`; el.style.height = `${Math.abs(bs.y1 - bs.y0)}px`
    const inBox = boxSelect(t.brush, this.project, this.store.state.meshMode, bs.x0, bs.y0, bs.x1, bs.y1)
    this.store.setMeshSel(bs.toggle ? combineSel(t.brush, bs.base, inBox, this.store.state.meshMode, true) : inBox)
  }

  private finishBoxSel(): void {
    const bs = this.boxSel
    this.boxSel = null
    this.boxEl.style.display = 'none'
    if (bs && !bs.moved && !bs.toggle) this.store.setMeshSel(emptySel()) // a click on nothing clears the selection
  }

  // ---- modal operators (G R S E I Ctrl+B Ctrl+R) ----------------------------------------------
  /** Runs `fn` on the brush being edited, inside the document a drag started from; invalid results keep the last good state. */
  private currentBrush(): { entityId: string; brushId: string } | null {
    const sel = this.store.state.selection
    return sel?.brushId !== undefined ? { entityId: sel.entityId, brushId: sel.brushId } : null
  }

  private meshDoc(start: MapDocument, fn: (b: Brush, sel: MeshSel) => OpResult): MapDocument {
    // the brush is looked up in `start` by the id it had when the drag began: once it has moved into a func_detail the
    // live selection points at the new entity, which `start` does not contain
    const target = this.dragBrush
    const entity = target ? start.entities.find((e) => e.id === target.entityId) : undefined
    const brush = entity?.brushes?.find((b) => b.id === target!.brushId)
    if (!target || !entity || !brush) return start
    const r = fn(brush, this.startSel)
    if (isError(r)) { this.lastErr = r.error; this.store.setMessage(r.error); return this.lastGood ?? start }
    const problem = this.store.brushProblem(entity, r.brush)
    if (problem && this.store.needsDetail(entity, r.brush)) {
      this.detailId ??= newId('e')
      this.converted = { entityId: this.detailId, brushId: target.brushId, meshSel: cleanSel(r.brush, r.sel) }
      this.lastErr = null
      this.store.setMessage('The brush is concave: it moves into a func_detail entity')
      const doc = moveBrushToDetail(start, target.entityId, target.brushId, r.brush, this.detailId)
      this.lastGood = doc
      this.store.setMeshSel(cleanSel(r.brush, r.sel))
      return doc
    }
    if (problem) { this.lastErr = problem; this.store.setMessage(problem); return this.lastGood ?? start }
    this.lastErr = null
    this.converted = null
    const doc = mapBrush(start, target.entityId, target.brushId, () => r.brush)
    this.lastGood = doc
    this.store.setMeshSel(cleanSel(r.brush, r.sel))
    return doc
  }

  /** After a confirmed drag that moved the brush into a func_detail entity: select it there, keeping the mesh selection. */
  private followConverted(): void {
    const c = this.converted
    if (!c || !findEntity(this.store.state.doc, c.entityId)) return
    this.store.select({ entityId: c.entityId, brushId: c.brushId })
    this.store.setMeshSel(c.meshSel) // the store clears the mesh selection while the brush is between entities
  }

  private applyMesh(fn: (b: Brush, sel: MeshSel) => OpResult): void { this.store.dragUpdate((start) => this.meshDoc(start, fn)) }

  startModal(kind: ModalKind, edge?: string): boolean {
    const st = this.store.state
    if (this.modal || !this.store.canEdit) return false
    const mesh = st.editMode === 'mesh'
    const t = mesh ? this.store.meshTarget() : null
    if (mesh && !t) return false
    const multi = selectedList(this.store)
    if (!mesh && (st.editMode !== 'object' || multi.length === 0 || !['grab', 'rotate', 'scale'].includes(kind))) return false
    if (mesh && ['grab', 'rotate', 'scale'].includes(kind) && st.meshSel.verts.length === 0) { this.store.setMessage('Select something first'); return false }
    if (kind === 'extrude' && (st.meshMode !== 'face' || st.meshSel.faces.length === 0)) { this.store.setMessage('Extrude works on selected faces: press 3, select a face, then E'); return false }
    if (kind === 'inset' && (st.meshMode !== 'face' || st.meshSel.faces.length === 0)) { this.store.setMessage('Inset works on selected faces: press 3, select a face, then I'); return false }
    if (kind === 'bevel' && (st.meshMode === 'vertex' || st.meshSel.edges.length === 0)) { this.store.setMessage('Bevel works on edges (or faces): press 2 or 3, select, then Ctrl+B'); return false }
    const pivot = mesh ? centroid(t!.brush, st.meshSel.verts.length ? st.meshSel.verts : t!.brush.vertices.map((_, i) => i)) : gizmoPivot(st.doc, multi)
    if (!pivot) return false
    const cam = this.camera, forward = normalize(sub(cam.target, cam.eye)), ray0 = this.lastRay ?? cam.rayFromNdc(0, 0, 1)
    const start = planePoint(ray0, pivot, forward) ?? pivot
    const mouse0 = { ...this.mouse }
    const step = snapStep(this.settings)
    const screenPivot = this.project(pivot) ?? { x: 0, y: 0, depth: 1 }
    const mpp = metresPerPixel(cam.distanceTo(pivot), cam.fovY, this.canvas.clientHeight)
    const label = { grab: 'Move', rotate: 'Rotate', scale: 'Scale', extrude: 'Extrude', inset: 'Inset', bevel: 'Bevel', loopcut: 'Loop cut' }[kind]
    if (!this.store.beginDrag(label)) return false
    this.startSel = st.meshSel; this.lastGood = null; this.detailId = null; this.converted = null; this.dragBrush = this.currentBrush()
    let lastRay = ray0, lastMouse = mouse0

    const m: Modal = { kind, axis: null, typed: '', value: kind === 'scale' ? 1 : 0, move: () => {}, refresh: () => {} }
    const typed = (): number | null => typedValue(m.typed)
    const say = () => this.store.setMessage(describeModal(kind, m.value, m.axis, m.typed, kind === 'bevel' ? m.segments : undefined))
    const moveObject = (map: (p: Vec3) => Vec3, flip: boolean, yaw: number) => this.store.dragUpdate((d) => transformSelections(d, multi, map, flip, yaw, this.store.state.uvLock))
    const moveMesh = (map: (p: Vec3) => Vec3) => this.applyMesh((b, sel) => this.moveVerts(b, sel.verts, map))
    const toward: Vec3 = scale(forward, -1) // pointing at the viewer

    if (kind === 'grab') {
      let delta: Vec3 = [0, 0, 0]
      m.refresh = () => {
        const v = typed()
        if (v !== null) delta = scale(AXIS_DIR[m.axis ?? 0], v)
        m.value = length(delta)
        const map = translateBy(delta)
        mesh ? moveMesh(map) : moveObject(map, false, 0)
        say()
      }
      m.move = (ray) => { lastRay = ray; if (m.typed === '') { delta = grabDelta(ray, start, pivot, forward, m.axis, step); m.refresh() } }
      if (mesh) m.wheel = (dy) => this.propWheel(dy, m)
    } else if (kind === 'rotate') {
      let angle = 0
      m.refresh = () => {
        const v = typed()
        if (v !== null) angle = (v * Math.PI) / 180
        m.value = angle
        const axisVec = m.axis === null ? toward : AXIS_DIR[m.axis]
        const map = rotateAbout(pivot, axisVec, angle)
        mesh ? moveMesh(map) : moveObject(map, false, m.axis === 1 ? (angle * 180) / Math.PI : 0)
        say()
      }
      if (mesh) m.wheel = (dy) => this.propWheel(dy, m)
      m.move = (_r, mouse) => {
        lastMouse = mouse
        if (m.typed !== '') return
        const axisVec = m.axis === null ? toward : AXIS_DIR[m.axis]
        angle = rotateAngle(screenPivot, mouse0, mouse, dot(axisVec, toward) >= 0, rotSnapRad(this.settings))
        m.refresh()
      }
    } else if (kind === 'scale') {
      let f = 1
      m.refresh = () => {
        const v = typed()
        if (v !== null) f = v
        m.value = f
        const vec = scaleVector(f === 0 ? 1e-6 : f, m.axis)
        const map = scaleAbout(pivot, vec)
        mesh ? moveMesh(map) : moveObject(map, mirrors(vec), 0)
        say()
      }
      if (mesh) m.wheel = (dy) => this.propWheel(dy, m)
      m.move = (_r, mouse) => { lastMouse = mouse; if (m.typed === '') { f = scaleFactor(screenPivot, mouse0, mouse, scaleSnapStep(this.settings)); m.refresh() } }
    } else if (kind === 'extrude' || kind === 'inset' || kind === 'bevel') {
      const faces = st.meshSel.faces, edges = st.meshSel.edges
      const b0 = t!.brush
      const dir = kind === 'extrude' ? averageNormal(b0, b0.faces.filter((fc) => faces.includes(fc.id))) : ([0, 0, 0] as Vec3)
      const t0 = kind === 'extrude' ? alongLine(ray0, pivot, dir) ?? 0 : 0
      let dist = 0, segments = 1
      m.refresh = () => {
        const v = typed()
        if (v !== null) dist = v
        m.value = dist
        if (kind === 'extrude') this.applyMesh((b) => extrudeFaces(b, faces, dist))
        else if (kind === 'inset') this.applyMesh((b) => insetFaces(b, faces, dist))
        else this.applyMesh((b, sel) => bevelEdges(b, sel.edges.length ? sel.edges : edges, dist, segments))
        say()
      }
      if (kind === 'bevel') m.wheel = (dy) => { segments = stepSegments(segments, dy, MAX_BEVEL_SEGMENTS); m.segments = segments; m.refresh(); return true }
      if (kind === 'bevel') m.segments = 1
      m.move = (ray, mouse) => {
        lastRay = ray; lastMouse = mouse
        if (m.typed !== '') return
        if (kind === 'extrude') { const t1 = alongLine(ray, pivot, dir); if (t1 !== null) dist = step > 0 ? Math.round((t1 - t0) / step) * step : t1 - t0 }
        else dist = widthFromMouse(mouse0, mouse, mpp, step > 0 ? step / 2 : 0)
        m.refresh()
      }
    } else if (kind === 'loopcut' && edge) {
      const info = edgeRing(t!.brush, edge)
      if (!info) { this.store.cancelDrag(); return false }
      const [p, q] = info.ring[0].split('>').map(Number)
      const a = t!.brush.vertices[p], c = t!.brush.vertices[q], rail = sub(c, a), len = length(rail), dirr = normalize(rail)
      let frac = 0.5
      m.refresh = () => {
        const v = typed()
        if (v !== null) frac = v
        m.value = frac
        this.applyMesh((b) => loopCut(b, edge, Math.min(0.999, Math.max(0.001, frac))))
        say()
      }
      m.move = (ray, mouse) => {
        lastRay = ray; lastMouse = mouse
        if (m.typed !== '') return
        const tt = alongLine(ray, a, dirr)
        if (tt !== null && len > 1e-9) { frac = tt / len; m.refresh() }
      }
    } else { this.store.cancelDrag(); return false }
    void lastRay; void lastMouse
    this.modal = m
    this.virt = { ...this.mouse }
    this.lockPointer() // G/R/S/E/I/bevel/loop cut keep working when the mouse leaves the viewport (best effort: falls back to the real cursor)
    m.refresh()
    this.invalidate()
    return true
  }

  private endModal(confirm: boolean): void {
    if (!this.modal) return
    this.modal = null
    this.virt = null
    this.unlockPointer()
    if (confirm) { this.store.endDrag(); this.followConverted() }
    else { this.store.cancelDrag(); this.store.setMeshSel(this.startSel) }
    this.store.setMessage(confirm ? this.lastErr ?? (this.converted ? 'The brush was concave and moved into a new func_detail entity' : null) : null)
    this.lastErr = null
    this.invalidate()
  }

  /** Keys that belong to an active modal (axis, numbers, Enter, Esc). Returns true when consumed. */
  private modalKey(e: KeyboardEvent): boolean {
    const m = this.modal
    if (!m) return false
    if (e.key === 'Escape') { this.endModal(false); return true }
    if (e.key === 'Enter') { this.endModal(true); return true }
    const k = e.key.toLowerCase()
    if ((k === 'x' || k === 'y' || k === 'z') && ['grab', 'rotate', 'scale'].includes(m.kind)) {
      const a = (k === 'x' ? 0 : k === 'y' ? 1 : 2) as Axis
      m.axis = m.axis === a ? null : a
      m.refresh(); if (this.lastRay) m.move(this.lastRay, this.mouse)
      return true
    }
    const next = typeInto(m.typed, e.key)
    if (next !== null) { m.typed = next; m.refresh(); return true }
    return false
  }

  // ---- keyboard: modeling shortcuts ----------------------------------------------------------
  private onKey(e: KeyboardEvent): void {
    if (this.modal) { if (this.modalKey(e)) { e.preventDefault(); e.stopPropagation() } else if (!e.ctrlKey) e.stopPropagation(); return }
    if (e.altKey && !['a', 'n', 'arrowleft', 'arrowright', 'arrowup', 'arrowdown', 'pageup', 'pagedown'].includes(e.key.toLowerCase())) return
    const st = this.store.state, k = e.key.toLowerCase(), mod = e.ctrlKey || e.metaKey
    const consume = () => { e.preventDefault(); e.stopPropagation() }
    if (this.clipKey(e)) return void consume()
    if (st.tool === 'terrain' && !mod) {
      if (k === 'escape' && this.terrainRun) { consume(); this.terrainCancel(); return }
      if (k === '[' || k === ']') { // brush size, as in most sculpting tools
        consume()
        const b = st.terrain.brush
        this.store.setTerrain({ brush: { radius: Math.min(200, Math.max(0.25, +(b.radius * (k === ']' ? 1.25 : 0.8)).toFixed(2))) } })
        return
      }
    }
    if (st.editMode === 'object' && st.tool === 'select') {
      // selection: Ctrl+A all, Ctrl+I invert, Alt+A or Esc none (Esc first leaves the origin editing, keeping the entity selected)
      if (k === 'escape' && !mod && !e.shiftKey && this.originSel !== null) { consume(); this.originSel = null; this.store.setMessage(null); this.invalidate(); return }
      if (mod && !e.shiftKey && k === 'a') { consume(); selectAllItems(this.store); return }
      if (mod && !e.shiftKey && k === 'i') { consume(); invertSelectionItems(this.store); return }
      if ((k === 'escape' && !mod && !e.shiftKey && !st.pipette) || (e.altKey && k === 'a')) { if (selectedList(this.store).length) { consume(); this.store.select(null) } return }
      // Radiant texture keys (on the selected faces; a selected brush = all its faces): Shift+arrows shift, Ctrl+Shift+arrows scale,
      // Shift+PageUp/PageDown rotate; Alt makes the step 8x finer. They take the keys away from the selection nudge.
      if (e.shiftKey && this.store.canEdit) {
        const texKey = ({ arrowleft: 'left', arrowright: 'right', arrowup: 'up', arrowdown: 'down', pageup: 'rotccw', pagedown: 'rotcw' } as Record<string, TexKey>)[k]
        if (texKey) {
          const mode = texKey === 'rotcw' || texKey === 'rotccw' ? 'rotate' : e.ctrlKey || e.metaKey ? 'scale' : 'shift'
          if (nudgeTexture(this.store, mode, texKey, e.altKey)) return void consume()
        }
      }
      // Radiant nudge: arrows move along the screen's dominant ground axis, PageUp/PageDown move up/down, by one grid step
      const arrow = ['arrowleft', 'arrowright', 'arrowup', 'arrowdown', 'pageup', 'pagedown'].indexOf(k)
      if (arrow >= 0 && !mod && !e.altKey && !e.shiftKey && selectedList(this.store).length && this.store.canEdit) {
        const step = snapStep(this.settings) || 0.25
        const { right, forward } = this.camera.basis
        const axisOf = (v: Vec3): Vec3 => (Math.abs(v[0]) >= Math.abs(v[2]) ? [Math.sign(v[0]) || 1, 0, 0] : [0, 0, Math.sign(v[2]) || 1])
        const f = axisOf(forward), r = axisOf(right)
        const dir: Vec3 = arrow === 0 ? scale(r, -1) : arrow === 1 ? r : arrow === 2 ? f : arrow === 3 ? scale(f, -1) : arrow === 4 ? [0, 1, 0] : [0, -1, 0]
        consume(); nudgeSelection(this.store, scale(dir, step)); return
      }
      if (!mod && k === 'x' && !e.shiftKey) { consume(); this.runBrushTool('clipper'); return }
      if (mod && k === 'h') { consume(); this.runBrushTool('hollow'); return }
      if (mod && k === 'm') { consume(); this.runBrushTool('merge'); return }
      if (mod && k === 'g') { consume(); this.runBrushTool('snap'); return }
      if (!mod && (k === 'g' || k === 'r' || k === 's')) { if (this.startModal(k === 'g' ? 'grab' : k === 'r' ? 'rotate' : 'scale')) consume() }
      else if (mod && k === 'd') { consume(); if (duplicateSelection(this.store)) this.startModal('grab') }
      else if (mod && !e.shiftKey && k === 'c') { consume(); void copySelection(this.store) }
      else if (mod && !e.shiftKey && k === 'x') { consume(); void cutSelection(this.store) }
      else if (mod && !e.shiftKey && k === 'v') { consume(); void pasteClipboard(this.store).then((ok) => { if (ok) this.startModal('grab') }) }
      return
    }
    if (st.editMode !== 'mesh') return
    const t = this.store.meshTarget()
    if (!t) return
    const mode = st.meshMode
    const id = !mod && (k === '1' || k === '2' || k === '3') ? `mode:${k}`
      : !mod && (k === 'g' || k === 'r' || k === 's') ? (k === 'g' ? 'grab' : k === 'r' ? 'rotate' : 'scale')
      : !mod && k === 'e' ? 'extrude' : !mod && k === 'i' ? 'inset' : mod && k === 'b' ? 'bevel' : mod && k === 'r' ? 'loopcut'
      : !mod && k === 'k' ? 'knife' : !mod && k === 'o' ? 'proportional' : !mod && k === 'j' ? 'connect' : !mod && k === 'm' ? 'merge' : !mod && k === 'f' ? 'fill' : !mod && k === 'x' ? 'dissolve'
      : k === 'delete' || k === 'backspace' ? 'delete' : !mod && e.altKey && k === 'n' ? 'flip' : !mod && e.shiftKey && k === 'n' ? 'recalc'
      : !e.altKey && !mod && k === 'a' ? 'selectall' : e.altKey && k === 'a' ? 'selectnone' : mod && k === 'i' ? 'invert'
      : k === '+' || k === '=' ? 'grow' : k === '-' || k === '_' ? 'shrink' : null
    if (this.knife && (k === 'enter' || k === 'escape')) { consume(); if (k === 'enter') this.finishKnife(); else { this.knife = null; this.store.setMessage('Knife cancelled'); this.invalidate() } return }
    if (this.mirrorArmed) { // Ctrl+M, then X / Y / Z (Esc or anything else cancels)
      this.mirrorArmed = false; consume()
      const axis = 'xyz'.indexOf(k)
      if (axis < 0 || mod) { this.store.setMessage('Mirror cancelled'); return }
      const uvLock = this.store.state.uvLock
      this.store.applyMeshOp(`Mirror ${k.toUpperCase()}`, (b, s) => mirrorVerts(b, s.verts, axis as 0 | 1 | 2, uvLock))
      return
    }
    if (mod && k === 'm') { consume(); this.mirrorArmed = true; this.store.setMessage('Mirror: press X, Y or Z (Esc cancels)'); return }
    if (k === 'escape' && this.loopArmed) { this.loopArmed = false; this.loopPreview = null; this.invalidate(); return }
    if (id) { consume(); this.runTool(id) }
    void mode; void t
  }

  /** The knife point under the cursor: a vertex near it, else the point of the nearest edge under the cursor (Ctrl = the edge's middle). */
  private knifePointAt(e: MouseEvent): KnifePoint | null {
    const t = this.store.meshTarget()
    if (!t) return null
    const p = this.canvasPoint(e), b = t.brush
    const v = pickVertex(b, this.project, p.x, p.y)
    if (v !== null) return { v }
    const edge = pickEdge(b, this.project, p.x, p.y, 10)
    if (!edge) return null
    if (e.ctrlKey || e.metaKey) return { edge, t: 0.5 }
    const [a, c] = edge.split('-').map(Number), d = sub(b.vertices[c], b.vertices[a]), len = length(d)
    const along = len > 1e-9 ? alongLine(this.ray(e as PointerEvent), b.vertices[a], scale(d, 1 / len)) : null
    return { edge, t: along === null ? 0.5 : Math.min(0.98, Math.max(0.02, along / len)) }
  }

  private updateKnifeHover(e: MouseEvent): void {
    if (!this.knife) return
    this.knife.hover = this.knifePointAt(e)
    this.invalidate()
  }

  /** Lines between the knife points (and on to the point under the cursor). */
  private knifeOverlay(): number[] {
    const k = this.knife, t = this.store.meshTarget()
    if (!k || !t || t.brush !== k.brush) return []
    const pts = [...k.points, ...(k.hover ? [k.hover] : [])].map((q) => knifePos(k.brush, q))
    const out: number[] = []
    for (let i = 1; i < pts.length; i++) out.push(...pts[i - 1], ...pts[i])
    for (const q of pts) out.push(q[0] - 0.03, q[1], q[2], q[0] + 0.03, q[1], q[2], q[0], q[1] - 0.03, q[2], q[0], q[1] + 0.03, q[2]) // a small cross per point
    return out
  }

  private finishKnife(): void {
    const k = this.knife
    this.knife = null
    this.invalidate()
    if (!k) return
    if (this.store.meshTarget()?.brush !== k.brush) { this.store.setMessage('Knife cancelled: the brush changed while cutting'); return }
    if (k.points.length < 2) { this.store.setMessage('Knife cancelled: click at least two points'); return }
    this.store.applyMeshOp('Knife', (b) => knifeCut(b, k.points))
  }

  /** Moves mesh vertices for G / R / S and the gizmo: plain, or with proportional editing when it is on (REQUIREMENTS §21 E4). */
  private moveVerts(b: Brush, verts: number[], map: (p: Vec3) => Vec3): OpResult {
    const st = this.store.state
    return st.proportional ? proportionalVerts(b, verts, map, st.propRadius, st.uvLock) : transformVerts(b, verts, map, st.uvLock)
  }

  /** Mouse wheel during G / R / S in Mesh mode: the proportional radius (x1.2 per notch), only while proportional editing is on. */
  private propWheel(dy: number, m: Modal): boolean {
    const st = this.store.state
    if (!st.proportional || dy === 0) return false
    this.store.setPropRadius(st.propRadius * (dy < 0 ? 1.2 : 1 / 1.2))
    m.refresh()
    this.store.setMessage(`Proportional radius ${+this.store.state.propRadius.toFixed(3)} m`)
    return true
  }

  /** Runs a mesh tool by id (keys and the Mesh Edit panel both land here). Returns false if it could not start. */
  runTool(id: string): boolean {
    if (id.startsWith('subdivide')) {
      if (this.store.state.editMode !== 'mesh' || this.store.state.meshMode !== 'edge') { this.store.setMessage('Subdivide works on edges: press Tab, then 2, select edges'); return false }
      const cuts = Number(id.split(':')[1]) || 1
      return this.store.applyMeshOp('Subdivide', (b, s) => subdivideEdges(b, s.edges, cuts))
    }
    if (id.startsWith('brush:')) { const [, tool, arg] = id.split(':'); return this.runBrushTool(tool, arg ? Number(arg) : undefined) }
    const st = this.store.state
    if (st.editMode !== 'mesh') { this.store.setMessage('Press Tab to enter mesh mode first'); return false }
    const t = this.store.meshTarget()
    if (!t) return false
    const mode = st.meshMode
    const op = (label: string, fn: (b: Brush, s: MeshSel) => OpResult) => this.store.applyMeshOp(label, fn)
    switch (id) {
      case 'mode:1': case 'mode:2': case 'mode:3': this.store.setMeshMode(id === 'mode:1' ? 'vertex' : id === 'mode:2' ? 'edge' : 'face'); return true
      case 'grab': case 'rotate': case 'scale': case 'extrude': case 'inset': case 'bevel': return this.startModal(id)
      case 'loopcut': this.loopArmed = true; this.store.setMessage('Loop cut: hover an edge and click, then slide and click again (Esc cancels)'); return true
      case 'mirror': this.mirrorArmed = true; this.store.setMessage('Mirror: press X, Y or Z (Esc cancels)'); return true
      case 'knife': this.knife = { brush: t.brush, points: [], hover: null }; this.store.setMessage('Knife: click points on edges or vertices (Ctrl = edge middle); Enter or right-click cuts, Esc cancels'); return true
      case 'proportional': this.store.setProportional(!st.proportional); return true
      case 'connect': return op('Connect vertices', (b, s) => connectVerts(b, s.verts))
      case 'merge': return op('Merge', (b, s) => mergeVerts(b, s.verts))
      case 'fill': return op('Fill', (b, s) => fillHole(b, s.verts))
      case 'dissolve': return op('Dissolve', (b, s) => (mode === 'vertex' ? dissolveVerts(b, s.verts) : mode === 'edge' ? dissolveEdges(b, s.edges) : dissolveFaces(b, s.faces)))
      case 'delete': return op('Delete faces', (b, s) => {
        const ids = mode === 'face' ? s.faces : b.faces.filter((f) => (mode === 'vertex' ? f.loop.some((v) => s.verts.includes(v)) : f.loop.some((v, i) => s.edges.includes(edgeKey(v, f.loop[(i + 1) % f.loop.length]))))).map((f) => f.id)
        return deleteFaces(b, ids)
      })
      case 'flip': return op('Flip normals', (b, s) => flipFaces(b, s.faces))
      case 'recalc': return op('Recalculate normals', (b) => recalcNormals(b, true))
      case 'bridge': return op('Bridge edge loops', (b, s) => bridgeLoops(b, s, mode))
      case 'smooth': case 'sharp': {
        // edge / vertex mode: the selected edges; face mode: the edges between two selected faces (Blender's Shade Smooth), all edges of a lone face
        const s = st.meshSel, b = t.brush
        const faceEdges = b.faces.filter((f) => s.faces.includes(f.id)).flatMap((f) => f.loop.map((v, i) => edgeKey(v, f.loop[(i + 1) % f.loop.length])))
        const inner = faceEdges.filter((k, i) => faceEdges.indexOf(k) !== i)
        const keys = mode === 'face' ? (inner.length ? inner : faceEdges) : s.edges
        if (!keys.length) { this.store.setMessage('Select edges (2) or faces (3) first'); return false }
        const on = id === 'smooth'
        const done = op(on ? 'Mark smooth' : 'Clear smooth', (br, sel) => ({ brush: markSmooth(br, keys, on), sel }))
        if (!done) this.store.setMessage(on ? 'Those edges are already smooth' : 'Those edges are not smooth')
        return done
      }
      case 'selectall': this.store.setMeshSel(allSel(t.brush)); return true
      case 'selectnone': this.store.setMeshSel(emptySel()); return true
      case 'invert': this.store.setMeshSel(invertSel(t.brush, st.meshSel, mode)); return true
      case 'grow': this.store.setMeshSel(growSel(t.brush, st.meshSel, mode)); return true
      case 'shrink': this.store.setMeshSel(shrinkSel(t.brush, st.meshSel, mode)); return true
    }
    return false
  }

  /** Decal tool: click a surface to lay a projector on it (size from the tool settings), pointing into the surface. */
  private decalDown(e: PointerEvent): void {
    if (!this.store.canEdit) return this.store.setMessage('Not connected to Core - editing is disabled')
    const hit = pick(this.ray(e), [...this.store.state.doc.entities, ...this.store.state.prefabEntities.map((p) => p.entity)], this.pickOpts())
    if (!hit?.brushId) return this.store.setMessage('Click on a surface to place a decal')
    const { width, height, depth } = this.store.state.decalParams
    const angles = anglesForDirection(scale(hit.normal, -1))
    addPointEntity(this.store, 'decal', hit.point, { texture: this.store.state.activeTexture, width: String(width), height: String(height), depth: String(depth) }, angles)
  }

  // ---- Terrain tool (REQUIREMENTS §39) ------------------------------------------------------------------------------
  /** Where the brush is: the terrain under the cursor and the point on its surface. */
  private terrainAim: { entityId: string; hf: Heightfield; point: Vec3 } | null = null
  private terrainRun: { stroke: TerrainStroke; timer: number; sent: number; moved: number; gap: number } | null = null

  private terrainUnder(ray: Ray): { entity: Entity; hf: Heightfield; point: Vec3 } | null {
    const st = this.store.state
    let best: { entity: Entity; hf: Heightfield; point: Vec3; t: number } | null = null
    for (const en of visibleEntities(st.doc.entities, st.hidden)) {
      if (en.terrain === undefined) continue
      const hf = terrainOf(en)
      const r = hf ? rayTerrain(hf, ray.origin, ray.dir) : null
      if (hf && r && (!best || r.t < best.t)) best = { entity: en, hf, point: r.point, t: r.t }
    }
    return best
  }
  private updateTerrainAim(): void {
    const u = this.lastRay ? this.terrainUnder(this.lastRay) : null
    this.terrainAim = u ? { entityId: u.entity.id, hf: u.hf, point: u.point } : null
    this.invalidate()
  }
  /** The brush circle (outer radius, and the inner one where the strength starts to fall off) laid on the ground. */
  private terrainCursor(): number[] {
    if (this.store.state.tool !== 'terrain') return []
    const aim = this.terrainAim, run = this.terrainRun
    if (!aim) return []
    const hf = run ? run.stroke.heights : aim.hf, b = this.store.state.terrain.brush
    const lines = circleOnTerrain(hf, aim.point[0], aim.point[2], b.radius)
    if (b.falloff > 0.05) lines.push(...circleOnTerrain(hf, aim.point[0], aim.point[2], Math.max(b.radius * (1 - b.falloff), 0.01), 48))
    return lines
  }
  private terrainDown(e: PointerEvent): void {
    if (!this.store.canEdit) return this.store.setMessage('Not connected to Core - editing is disabled')
    const under = this.terrainUnder(this.ray(e))
    if (!under) return this.store.setMessage('Terrain tool: click on a terrain (make one in the Terrain panel)')
    const t = this.store.state.terrain
    const stroke = new TerrainStroke(under.entity, under.hf, t.tool, t.brush, { invert: e.shiftKey, flattenHeight: t.flattenHeight, texture: this.store.state.activeTexture })
    // sculpting does not need the selection, and a selected terrain makes every panel list its hundred thousand faces on each update
    if (this.store.state.multi.some((x) => x.entityId === under.entity.id) || this.store.state.selection?.entityId === under.entity.id) this.store.select(null)
    if (!this.store.beginDrag(`Terrain: ${t.tool}`)) return
    this.drag = { kind: 'terrain' }
    const win = this.element.ownerDocument.defaultView ?? window
    // holding still keeps working (an airbrush), a little slower than the mouse
    this.terrainRun = { stroke, timer: win.setInterval(() => this.terrainTick(), 120), sent: 0, moved: win.performance.now(), gap: 250 }
    this.terrainAim = { entityId: under.entity.id, hf: under.hf, point: under.point }
    this.terrainApply(() => stroke.moveTo(under.point[0], under.point[2]))
  }
  private terrainMove(e: PointerEvent): void {
    const run = this.terrainRun
    if (!run) return
    const ray = this.ray(e), r = rayTerrain(run.stroke.heights, ray.origin, ray.dir)
    if (!r) return
    run.stroke.invert = e.shiftKey
    this.terrainAim = { entityId: run.stroke.entityId, hf: run.stroke.heights, point: r.point }
    run.moved = (this.element.ownerDocument.defaultView ?? window).performance.now()
    this.terrainApply(() => run.stroke.moveTo(r.point[0], r.point[2]))
  }
  private terrainTick(): void {
    const run = this.terrainRun, now = (this.element.ownerDocument.defaultView ?? window).performance.now()
    if (!run || now - run.moved < 120) return
    this.terrainApply(() => run.stroke.repeat())
  }
  /** Runs a stamp and shows the result at once; Core gets it at most every 250 ms during the stroke (more on a big terrain: 6 x what sending took), and the final one when the button is released. */
  private terrainApply(act: () => void): void {
    const run = this.terrainRun
    if (!run) return
    act()
    this.store.dragUpdate((start) => run.stroke.document(start), false)
    const now = (this.element.ownerDocument.defaultView ?? window).performance.now()
    if (now - run.sent > run.gap) {
      run.sent = now
      this.store.submitCurrent()
      run.gap = Math.max(250, 6 * ((this.element.ownerDocument.defaultView ?? window).performance.now() - now))
    }
    this.invalidate()
  }
  private terrainEnd(): void {
    const run = this.terrainRun
    if (run) (this.element.ownerDocument.defaultView ?? window).clearInterval(run.timer)
    this.terrainRun = null
    this.store.endDrag()
  }
  private terrainCancel(): void {
    const run = this.terrainRun
    if (!run) return
    ;(this.element.ownerDocument.defaultView ?? window).clearInterval(run.timer)
    this.terrainRun = null
    this.drag = null
    this.store.cancelDrag()
    this.invalidate()
  }

  private snap(p: Vec3, plane: DrawPlane): Vec3 { return snapToPlane(p, plane, snapStep(this.settings)) }

  private boxDown(e: PointerEvent): void {
    if (this.drawing?.phase === 'height') return this.commitDrawing() // second click finishes the shape
    if (!this.store.canEdit) return this.store.setMessage('Not connected to Core - editing is disabled')
    const ray = this.ray(e)
    const hit = pick(ray, [...this.store.state.doc.entities, ...this.store.state.prefabEntities.map((p) => p.entity)], this.pickOpts())
    const plane = hit ? planeThrough(hit.point, hit.normal) : fallbackPlane(ray)
    // the start point is snapped to the world grid (measured along the surface), never left where the click landed
    const p0 = this.snap(hit ? hit.point : rayPlane(ray, plane) ?? plane.origin, plane)
    this.drawing = { phase: 'rect', plane, start: p0, current: p0 }
  }

  private onMove(e: PointerEvent): void {
    this.shift = e.shiftKey
    if (this.modal && this.virt && this.canvas.ownerDocument.pointerLockElement === this.canvas) {
      // pointer locked for G/R/S/E/I/bevel: the cursor is virtual, so the operation continues past the viewport's edge
      this.virt = advanceVirtualCursor(this.virt, e.movementX, e.movementY)
      this.mouse = { ...this.virt }
      this.lastRay = this.rayAtCanvas(this.virt)
      this.modal.move(this.lastRay, this.mouse)
      return
    }
    this.lastRay = this.ray(e)
    this.mouse = this.canvasPoint(e)
    if (this.modal) { this.modal.move(this.lastRay, this.mouse); return }
    if (this.boxSel) { this.updateBoxSel(this.mouse); return }
    if (this.clipDrag !== null) return this.clipDragMove(e)
    const d = this.drag
    if (!d) {
      if (this.knife) return this.updateKnifeHover(e)
      if (this.loopArmed) return this.updateLoopPreview()
      if (this.drawing) return this.refreshDrawing()
      if (this.store.state.tool === 'terrain') return this.updateTerrainAim()
      this.setAltFace(e.altKey ? this.altFaceAt(this.ray(e)) : null)
      const eh = this.entityHandleHit(e)
      if (eh !== this.hoverEntHandle) { this.hoverEntHandle = eh; this.canvas.style.cursor = eh ? 'move' : this.store.state.tool === 'select' ? 'default' : 'crosshair'; this.invalidate() }
      const a = this.gizmoHit(e)?.axis ?? null
      const hf = !!this.faceHit(e)
      const hh = this.handleHit(e)?.id ?? null
      if (hh !== this.hoverHandle) { this.hoverHandle = hh; this.canvas.style.cursor = hh ? 'move' : this.store.state.tool === 'select' ? 'default' : 'crosshair'; this.invalidate() }
      if (a !== this.hoverAxis || hf !== this.hoverFace) { this.hoverAxis = a; this.hoverFace = hf; this.invalidate() }
      return
    }
    if (d.kind === 'terrain') return this.terrainMove(e)
    if (d.kind === 'face') return this.faceDragMove(e, d)
    if (d.kind === 'angle') return this.entityHandleMove(e, d)
    if (d.kind === 'resize') {
      const cam = this.camera
      if (!(cam instanceof OrthoCamera)) return
      const m = this.canvasPoint(e)
      const [dr, du] = pixelsToMetres(m.x - d.x0, m.y - d.y0, cam.metresPerPixel(this.canvas.clientHeight))
      const map = resizeMap(d.min, d.max, d.r, d.u, d.h, d.v, dr, du, snapStep(this.settings))
      const multi = selectedList(this.store)
      this.store.dragUpdate((start) => transformSelections(start, multi, map, false, 0, this.store.state.uvLock))
      return
    }
    if (d.kind === 'gizmo') {
      let map: (p: Vec3) => Vec3, flip = false, yawDeg = 0
      if (d.mode === 'rotate') {
        const a = ringAngle(this.ray(e), d.pivot, d.axis)
        if (a === null) return
        let delta = angleDelta(d.startT, a)
        const snapRad = rotSnapRad(this.settings)
        if (snapRad > 0) delta = Math.round(delta / snapRad) * snapRad
        map = rotateAbout(d.pivot, AXIS_DIR[d.axis], delta)
        yawDeg = d.axis === 1 ? (delta * 180) / Math.PI : 0
        this.store.setMessage(`Rotate ${'XYZ'[d.axis]}: ${((delta * 180) / Math.PI).toFixed(1)}°`)
      } else if (d.mode === 'scale') {
        const t = axisDistance(this.ray(e), d.pivot, d.axis)
        if (t === null || Math.abs(d.startT) < 1e-6) return
        let f = t / d.startT
        const sn = scaleSnapStep(this.settings)
        if (sn > 0) f = Math.round(f / sn) * sn
        if (Math.abs(f) < 0.01) return
        const vec: Vec3 = [1, 1, 1]
        vec[d.axis] = f
        map = scaleAbout(d.pivot, vec)
        flip = f < 0
        this.store.setMessage(`Scale ${'XYZ'[d.axis]}: ×${f.toFixed(2)}`)
      } else {
        const delta = dragDelta(this.ray(e), d.pivot, d.axis, d.startT, snapStep(this.settings))
        if (d.extrude) { const faces = d.extrude; this.applyMesh((b) => extrudeFaces(b, faces, 0, delta)); return }
        if (d.origin) {
          // the origin alone moves (a brush entity's brushes stay where they are)
          const id = d.origin, o = add(d.pivot, delta)
          this.store.dragUpdate((start) => mapEntity(start, id, (x) => ({ ...x, origin: o })))
          this.store.setMessage(`Origin: ${o.map((v) => +v.toFixed(3)).join(' ')}`)
          return
        }
        map = translateBy(delta)
      }
      if (d.mesh) this.applyMesh((b, sel) => this.moveVerts(b, sel.verts, map))
      else { const multi = selectedList(this.store); this.store.dragUpdate((start) => transformSelections(start, multi, map, flip, yawDeg, this.store.state.uvLock)) }
      return
    }
    // with the pointer locked the cursor stays put and the browser reports relative motion, so it can never hit the screen edge
    const locked = this.canvas.ownerDocument.pointerLockElement === this.canvas
    // (synthetic/automated input has no movement values, so fall back to the cursor delta when the browser reports none)
    const useMove = locked && (e.movementX !== 0 || e.movementY !== 0)
    const dx = useMove ? e.movementX : e.clientX - d.x, dy = useMove ? e.movementY : e.clientY - d.y
    if (d.kind === 'look' || d.kind === 'orbit' || (d.kind === 'pan' && this.rmbPanning)) this.rmbTravel += Math.abs(e.movementX) + Math.abs(e.movementY) + (useMove ? 0 : Math.abs(dx) + Math.abs(dy))
    if ((d.kind === 'look' || d.kind === 'orbit' || (d.kind === 'pan' && this.rmbPanning)) && !locked && this.rmbTravel >= 3) this.lockPointer()
    d.x = e.clientX; d.y = e.clientY
    if (d.kind === 'look') this.cams.fly.look(dx, dy)
    else if (d.kind === 'orbit') this.cams.orbit.orbit(dx, dy)
    else this.camera.pan(dx, dy, this.canvas.clientHeight)
    this.invalidate()
  }

  // ---- Clipper ----------------------------------------------------------------------------------------------
  startClipper(): boolean {
    if (!this.store.canEdit) { this.store.setMessage('Not connected to Core - editing is disabled'); return false }
    if (!selectedBrushes(this.store).length) { this.store.setMessage('Clipper: select a brush first'); return false }
    this.clip = { pts: [] }
    this.store.setMessage('Clipper: click 2-3 points (2 = plane along the view). Enter keeps the back side, Shift+Enter splits, Ctrl+Enter keeps the front, Esc cancels')
    this.invalidate()
    return true
  }
  /** Test hook: arms the clipper with these world points (no clicking). */
  debugClipPoints(pts: Vec3[]): boolean {
    if (!this.startClipper()) return false
    this.clip = { pts: pts.map((p) => ({ p, dir: [0, 0, -1] as Vec3 })) }
    this.invalidate()
    return true
  }
  /** Test hook: the current clipper points (null when the clipper is off). */
  debugClipState(): Vec3[] | null { return this.clip ? this.clip.pts.map((x) => [...x.p] as Vec3) : null }
  private stopClipper(): void { if (this.clip) { this.clip = null; this.invalidate() } }

  private clipPlane(): Plane | null {
    const pts = this.clip?.pts ?? []
    if (pts.length < 2) return null
    const a = pts[0].p, b = pts[1].p
    const c = pts.length > 2 ? pts[2].p : add(a, pts[1].dir) // two points: the plane contains the view direction
    return planeFromPoints(a, b, c)
  }

  /** Bounds centre of the selected brushes: the depth at which ortho clicks place clip points. */
  private selectionCentre(): Vec3 {
    const min: Vec3 = [Infinity, Infinity, Infinity], max: Vec3 = [-Infinity, -Infinity, -Infinity]
    for (const { brush } of selectedBrushes(this.store)) for (const v of brush.vertices) for (let k = 0; k < 3; k++) { min[k] = Math.min(min[k], v[k]); max[k] = Math.max(max[k], v[k]) }
    return min.every(Number.isFinite) ? scale(add(min, max), 0.5) : [0, 0, 0]
  }
  private orthoForward(): Vec3 | null { return this.camera instanceof OrthoCamera ? this.camera.forward : null }

  private clipClick(e: PointerEvent): void {
    const ray = this.ray(e)
    const m = this.canvasPoint(e)
    // grabbing an existing point drags it (Radiant): it moves in the view plane, snapped to the grid
    const grab = hitClipPoint(this.clip!.pts, this.project, m.x, m.y)
    if (grab !== null) { this.clipDrag = grab; return }
    const step = snapStep(this.settings)
    const fwd = this.orthoForward()
    let p: Vec3 | null
    if (fwd) p = rayPlaneHit(ray, this.selectionCentre(), fwd) // orthographic: the point lies in the plane through the selection
    else {
      const { hit } = this.pickAny(ray)
      p = hit ? hit.point : null
      if (!p && Math.abs(ray.dir[1]) > 1e-6 && -ray.origin[1] / ray.dir[1] > 0) p = add(ray.origin, scale(ray.dir, -ray.origin[1] / ray.dir[1]))
    }
    if (!p) return void this.store.setMessage('Clipper: click on a surface or the ground')
    const q = snapPoint(p, step)
    const pts = this.clip!.pts.length >= 3 ? [] : [...this.clip!.pts]
    if (clashes(pts, -1, q)) return void this.store.setMessage('Clipper: that point is already used')
    pts.push({ p: q, dir: ray.dir })
    this.clip = { pts }
    this.invalidate()
  }

  /** LMB drag of a clip point: it stays on the plane through its old position facing the camera. */
  private clipDragMove(e: PointerEvent): void {
    const clip = this.clip, i = this.clipDrag
    if (!clip || i === null || !clip.pts[i]) return
    const ray = this.ray(e)
    const fwd = this.orthoForward() ?? this.camera.basis.forward
    const hit = rayPlaneHit(ray, clip.pts[i].p, fwd)
    if (!hit) return
    const q = snapPoint(hit, snapStep(this.settings), depthAxis(this.orthoForward()))
    if (clashes(clip.pts, i, q)) return
    const pts = clip.pts.map((x, k) => (k === i ? { p: q, dir: x.dir } : x))
    this.clip = { pts }
    this.invalidate()
  }

  private clipOverlay(): number[] {
    if (!this.clip?.pts.length) return []
    const out: number[] = []
    const seg = (a: Vec3, b: Vec3) => out.push(...a, ...b)
    for (const { p } of this.clip.pts) {
      const r = this.camera.distanceTo(p) * 0.012
      seg(add(p, [r, 0, 0]), add(p, [-r, 0, 0])); seg(add(p, [0, r, 0]), add(p, [0, -r, 0])); seg(add(p, [0, 0, r]), add(p, [0, 0, -r]))
    }
    const pl = this.clipPlane()
    const pts = this.clip.pts.map((x) => x.p)
    for (let i = 0; i + 1 < pts.length; i++) seg(pts[i], pts[i + 1])
    if (pts.length === 3) seg(pts[2], pts[0])
    if (pl) {
      const bs = selectedBrushes(this.store)
      let min: Vec3 = [Infinity, Infinity, Infinity], max: Vec3 = [-Infinity, -Infinity, -Infinity]
      for (const { brush } of bs) for (const v of brush.vertices) for (let k = 0; k < 3; k++) { min[k] = Math.min(min[k], v[k]); max[k] = Math.max(max[k], v[k]) }
      const c0: Vec3 = scale(add(min, max), 0.5), half = Math.max(0.5, length(sub(max, min)) * 0.6)
      const c = sub(c0, scale(pl.n, dot(pl.n, c0) - pl.d))
      const ref: Vec3 = Math.abs(pl.n[1]) < 0.9 ? [0, 1, 0] : [1, 0, 0]
      const u = normalize([pl.n[1] * ref[2] - pl.n[2] * ref[1], pl.n[2] * ref[0] - pl.n[0] * ref[2], pl.n[0] * ref[1] - pl.n[1] * ref[0]])
      const v: Vec3 = [pl.n[1] * u[2] - pl.n[2] * u[1], pl.n[2] * u[0] - pl.n[0] * u[2], pl.n[0] * u[1] - pl.n[1] * u[0]]
      const corner = (a: number, b: number): Vec3 => add(c, add(scale(u, a * half), scale(v, b * half)))
      const q = [corner(-1, -1), corner(1, -1), corner(1, 1), corner(-1, 1)]
      for (let i = 0; i < 4; i++) seg(q[i], q[(i + 1) % 4])
      seg(c, add(c, scale(pl.n, half * 0.35))) // points to the side that is CUT AWAY with Enter
    }
    return out
  }

  /** Enter / Shift+Enter / Ctrl+Enter while the clipper is armed. */
  private clipKey(e: KeyboardEvent): boolean {
    if (!this.clip) return false
    if (e.key === 'Escape') { this.stopClipper(); this.store.setMessage(null); return true }
    if (e.key !== 'Enter') return false
    const pl = this.clipPlane()
    if (!pl) { this.store.setMessage('Clipper: place at least two points first'); return true }
    const flip = e.ctrlKey || e.metaKey
    const keep = e.shiftKey ? 'both' : flip ? 'front' : 'back'
    if (clipSelection(this.store, pl, keep)) this.stopClipper()
    return true
  }

  /** Brush-mode (Radiant) tools; the Brush tab and the keys land here. `arg` carries a number for hollow. */
  private runBrushTool(id: string, arg?: number): boolean {
    const step = snapStep(this.settings)
    switch (id) {
      case 'clipper': return this.clip ? (this.stopClipper(), true) : this.startClipper()
      case 'hollow': return hollowSelection(this.store, arg && arg > 0 ? arg : 0.25)
      case 'merge': return mergeSelection(this.store)
      case 'selall': return selectAllItems(this.store)
      case 'selinv': return invertSelectionItems(this.store)
      case 'selinside': return selectByVolume(this.store, 'inside')
      case 'seltouch': return selectByVolume(this.store, 'touching')
      case 'flipx': return flipSelection(this.store, 0)
      case 'flipy': return flipSelection(this.store, 1)
      case 'flipz': return flipSelection(this.store, 2)
      case 'rotx': return rotateSelection90(this.store, 0)
      case 'roty': return rotateSelection90(this.store, 1)
      case 'rotz': return rotateSelection90(this.store, 2)
      case 'snap': return snapSelection(this.store, step > 0 ? step : 0.25)
    }
    return false
  }

  /** The reference pipette: whatever entity is under the cursor (document or prefab content) is handed to the armed field. */
  private pipetteClick(e: PointerEvent): void {
    const cb = this.store.state.pipette
    if (!cb) return
    const { hit } = this.pickAny(this.ray(e))
    const path = hit ? pathOfHit(hit.entity, this.store.state.doc, this.store.state.prefabEntities) : null
    if (!path) return void this.store.setMessage('Pipette: that is not an entity - click one, or press Esc')
    this.store.setPipette(null)
    cb(path)
  }

  private faceDragMove(e: PointerEvent, d: Extract<NonNullable<ViewportPanel['drag']>, { kind: 'face' }>): void {
    const t = lineDistance(this.ray(e), d.pivot, d.dir)
    if (t === null) return
    const dist = snap(t - d.startT, snapStep(this.settings))
    this.store.dragUpdate((start) => {
      const ent = findEntity(start, d.entityId), br = ent?.brushes?.find((x) => x.id === d.brushId)
      if (!ent || !br) return start
      if (d.extend) {
        if (dist < 1e-6) { this.extended = null; this.store.setMessage(dist < -1e-6 ? 'Drag outward to extend the face' : null); return start }
        const r = extendFace(br, d.faceId, dist, d.extend.brushId)
        if ('error' in r) { this.store.setMessage(r.error); return this.faceGood ?? start }
        const problem = this.store.brushProblem(ent, r.brush)
        if (problem) { this.store.setMessage(`Extend face: ${problem}`); return this.faceGood ?? start }
        this.store.setMessage(null)
        this.extended = { entityId: d.entityId, brushId: d.extend.brushId }
        const doc = addBrush(start, d.entityId, r.brush)
        this.faceGood = doc
        return doc
      }
      const r = dragFace(br, d.faceId, dist)
      if ('error' in r) { this.store.setMessage(r.error); return this.faceGood ?? start }
      const doc = mapBrush(start, d.entityId, d.brushId, () => r.brush)
      this.faceGood = doc
      return doc
    })
  }

  private updateLoopPreview(): void {
    const t = this.store.meshTarget()
    const edge = t ? pickEdge(t.brush, this.project, this.mouse.x, this.mouse.y) : null
    const info = t && edge ? edgeRing(t.brush, edge) : null
    if (!t || !info) { if (this.loopPreview) { this.loopPreview = null; this.invalidate() } return }
    const mids = info.ring.map((pq) => { const [p, q] = pq.split('>').map(Number); const a = t.brush.vertices[p], c = t.brush.vertices[q]; return [(a[0] + c[0]) / 2, (a[1] + c[1]) / 2, (a[2] + c[2]) / 2] as Vec3 })
    const lines: number[] = []
    for (let i = 0; i + 1 < mids.length; i++) lines.push(...mids[i], ...mids[i + 1])
    if (info.closed && mids.length > 2) lines.push(...mids[mids.length - 1], ...mids[0])
    this.loopPreview = lines
    this.invalidate()
  }

  /** Recompute the in-progress box from the last mouse ray and the current Shift state. */
  private refreshDrawing(): void {
    const d = this.drawing, ray = this.lastRay
    if (!d || !ray) return
    if (d.phase === 'rect') {
      const p = rayPlane(ray, d.plane)
      if (p) d.current = this.snap(p, d.plane)
    } else {
      const rect = d.rect
      const centre = add(add(add(rect.origin, scale(d.plane.u, rect.du / 2)), scale(d.plane.v, rect.dv / 2)), [0, 0, 0])
      const raw = lineDistance(ray, centre, d.plane.normal)
      if (raw !== null) d.height = heightFor(raw, rect, this.shift, snapStep(this.settings))
    }
    this.invalidate()
  }

  private currentRect(d: Extract<Drawing, { phase: 'rect' }>): Rect { return rectFromDrag(d.plane, d.start, d.current, this.shift) }

  private previewLines(): number[] | null {
    const d = this.drawing
    if (!d) return null
    if (d.phase === 'rect') return previewLines(d.plane, this.currentRect(d), null)
    if (Math.abs(d.height) < MIN_SIZE) return previewLines(d.plane, d.rect, null)
    // the shape itself with the current parameters and orientation (REQUIREMENTS §33 H4); the box when it cannot be built
    const st = this.store.state
    const shape = st.shape !== 'box' ? shapePreviewLines(st.shape, d.plane, d.rect, d.height, st.shapeParams, st.shapeParams.turn) : []
    return shape.length ? shape : previewLines(d.plane, d.rect, d.height)
  }

  private commitDrawing(): void {
    const d = this.drawing
    if (d?.phase !== 'height') return
    const st = this.store.state
    this.drawing = null
    if (st.shape === 'box') {
      const brush = buildBrush(newId('b'), d.plane, d.rect, d.height, st.activeTexture)
      if (brush) addDrawnBrush(this.store, brush)
    } else {
      const r = buildShape(st.shape, d.plane, d.rect, d.height, st.activeTexture, st.shapeParams, () => newId('b'), st.shapeParams.turn)
      if ('error' in r) this.store.setMessage(r.error)
      else addDrawnBrushes(this.store, attachShape(r.brushes, st.shape, frameOf(d.plane, d.rect, d.height), st.shapeParams, st.shapeParams.turn, newId('s')), `Draw ${st.shape}`)
    }
    this.invalidate()
  }

  private onUp(e: PointerEvent): void {
    const rmbClick = e.button === 2 && (this.drag?.kind === 'look' || this.drag?.kind === 'orbit' || (this.drag?.kind === 'pan' && this.rmbPanning)) && this.rmbTravel < 4
    this.clipDrag = null
    if (this.drag?.kind === 'resize' || this.drag?.kind === 'angle') this.store.endDrag()
    if (this.drag?.kind === 'terrain') this.terrainEnd()
    if (this.drag?.kind === 'gizmo' || this.drag?.kind === 'face') { this.store.endDrag(); if (this.drag.kind === 'gizmo' && this.drag.mesh) this.followConverted() }
    if (this.drag?.kind === 'face' && this.drag.extend) {
      // the new brush's end cap becomes the selected face: its handle carries on pushing the extension
      const x = this.extended
      this.extended = null
      if (x && findEntity(this.store.state.doc, x.entityId)?.brushes?.some((b) => b.id === x.brushId)) this.store.select({ entityId: x.entityId, brushId: x.brushId, faceId: 0 })
    }
    if (this.boxSel) this.finishBoxSel()
    if (this.drag?.kind === 'look') this.stopFly()
    if (this.drag && !['gizmo', 'face', 'resize', 'angle', 'terrain'].includes(this.drag.kind)) this.unlockPointer()
    this.drag = null
    if (e.button === 0 && this.drawing?.phase === 'rect') {
      const rect = this.currentRect(this.drawing)
      // a click without a drag (or a sliver) cancels; otherwise move on to choosing the height
      this.drawing = Math.abs(rect.du) < MIN_SIZE || Math.abs(rect.dv) < MIN_SIZE ? null : { phase: 'height', plane: this.drawing.plane, rect, height: 0 }
    }
    if (this.canvas.hasPointerCapture(e.pointerId)) this.canvas.releasePointerCapture(e.pointerId)
    if (rmbClick) this.openContextMenu(e)
    this.invalidate()
  }

  /** Right-click without dragging: select what is under the cursor (unless it is already selected) and offer the brush / entity actions. */
  private openContextMenu(e: PointerEvent): void {
    const st = this.store.state
    if (this.modal || this.clip || st.pipette) return
    if (st.editMode === 'mesh') return this.openMeshMenu()
    if (st.editMode !== 'object') return
    const at = this.rmbDown
    const { hit, instanceId } = this.pickAny(this.ray({ clientX: at.x, clientY: at.y } as PointerEvent))
    // Shift + right-click: lift the material of the face under the cursor (Scythe's Lift Material): it becomes the browser's active texture
    if (e.shiftKey) {
      const tex = hit?.brushId && hit.faceId !== undefined ? liftMaterial(st.doc, hit.entityId, hit.brushId, hit.faceId) : null
      if (tex) { this.store.setActiveTexture(tex); this.store.setMessage(`Lifted ${tex}; click a face and a texture tile (or Apply) to use it`) }
      return
    }
    // a face is selected and another face of the document is right-clicked: offer to copy the texture + UV onto it (the selection stays, so it can be pasted again)
    const only = selectedList(this.store).length === 1 ? selectedList(this.store)[0] : null
    if (only?.brushId && only.faceId !== undefined && hit?.brushId && hit.faceId !== undefined && instanceId === undefined && !(hit.entityId === only.entityId && hit.brushId === only.brushId && hit.faceId === only.faceId)) {
      const from = { entityId: only.entityId, brushId: only.brushId, faceId: only.faceId }, to = { entityId: hit.entityId, brushId: hit.brushId, faceId: hit.faceId }
      showMenu(at.x, at.y, [
        { label: 'Copy UV and texture from the selected face', disabled: !this.store.canEdit, run: () => { copySurface(this.store, from, to) } },
        { label: 'Flow texture from the selected face (follow the shape)', disabled: !this.store.canEdit, run: () => { const m = flowTexture(this.store, from, to); if (m) this.store.setMessage(m) } },
      ], this.element.ownerDocument)
      return
    }
    if (instanceId !== undefined) { if (!selectedList(this.store).some((x) => x.instanceId === instanceId)) this.store.select({ entityId: '', instanceId }) }
    else if (hit && !selectedList(this.store).some((x) => x.entityId === hit.entityId && (!x.brushId || x.brushId === hit.brushId))) this.store.select(wholeEntityIfSole(st.doc, { entityId: hit.entityId, brushId: hit.brushId }))
    const sel = selectedList(this.store)
    const brushes = selectedBrushes(this.store)
    const can = this.store.canEdit
    const classes = st.entityDefs.filter((c) => c.kind === 'group').map((c) => c.name).sort()
    const fallback = ['func_group', 'func_detail', 'func_door', 'func_button']
    const items: MenuItem[] = [
      { label: 'Make entity', disabled: !can || !brushes.length, sub: (classes.length ? classes : fallback).map((c) => ({ label: c, run: () => { makeEntityFromSelection(this.store, c) } })) },
      { label: 'Move to world', disabled: !can || !brushes.length, run: () => { moveSelectionToWorld(this.store) } },
      { sep: true, label: '' },
      { label: 'Clipper', disabled: !can || !brushes.length, run: () => { this.runBrushTool('clipper') } },
      { label: 'Make hollow', disabled: !can || !brushes.length, run: () => { this.runBrushTool('hollow') } },
      { label: 'Merge brushes', disabled: !can || brushes.length < 2, run: () => { this.runBrushTool('merge') } },
      { label: 'Select inside', disabled: !brushes.length, run: () => { this.runBrushTool('selinside') } },
      { label: 'Select touching', disabled: !brushes.length, run: () => { this.runBrushTool('seltouch') } },
      { sep: true, label: '' },
      { label: 'Group (func_group)', disabled: !can || !brushes.length, run: () => { groupSelection(this.store) } },
      { label: 'Turn into prefab…', disabled: !can || !sel.length || !this.onTurnIntoPrefab, run: () => { this.onTurnIntoPrefab?.() } },
      { label: 'Ungroup', disabled: !can || !sel.some((s) => classOf(st.doc.entities.find((x) => x.id === s.entityId) ?? { id: '' }) === 'func_group'), run: () => { ungroupSelection(this.store) } },
      { sep: true, label: '' },
      { label: 'Copy UV', disabled: !(sel.length === 1 && sel[0].faceId !== undefined), run: () => { copyUv(this.store) } },
      { label: 'Paste UV', disabled: !can || !st.uvClipboard || !sel.length, run: () => { pasteUv(this.store) } },
      { sep: true, label: '' },
      { label: 'Hide', disabled: !sel.length, run: () => { hideSelection(this.store) } },
      { label: `Unhide all${st.hidden.length ? ` (${st.hidden.length})` : ''}`, disabled: !st.hidden.length, run: () => { unhideAll(this.store) } },
      { label: 'Properties', disabled: !sel.length, run: () => { this.store.showPanel('inspector') } },
      { sep: true, label: '' },
      { label: 'Cut', disabled: !can || !sel.length, run: () => { void cutSelection(this.store) } },
      { label: 'Copy', disabled: !sel.length, run: () => { void copySelection(this.store) } },
      { label: 'Paste', disabled: !can, run: () => { void pasteClipboard(this.store) } },
      { label: 'Duplicate', disabled: !can || !sel.length, run: () => { duplicateSelection(this.store) } },
      { label: 'Delete', disabled: !can || !sel.length, run: () => { deleteSelection(this.store) } },
    ]
    showMenu(at.x, at.y, items, this.element.ownerDocument)
  }

  /** Mesh mode right-click: the tools that work on the selection as it is (REQUIREMENTS §33 H6). */
  private openMeshMenu(): void {
    const st = this.store.state, at = this.rmbDown, can = this.store.canEdit && !!this.store.meshTarget()
    const any = st.meshSel.verts.length > 0 || st.meshSel.faces.length > 0
    const run = (id: string) => () => { this.runTool(id) }
    showMenu(at.x, at.y, [
      { label: 'Mark smooth', disabled: !can || !any, run: run('smooth') },
      { label: 'Clear smooth', disabled: !can || !any, run: run('sharp') },
      { sep: true, label: '' },
      { label: 'Bridge edge loops', disabled: !can || !any, run: run('bridge') },
      { label: 'Merge (M)', disabled: !can || !any, run: run('merge') },
      { label: 'Dissolve (X)', disabled: !can || !any, run: run('dissolve') },
      { label: 'Flip normals (Alt+N)', disabled: !can || !st.meshSel.faces.length, run: run('flip') },
      { label: 'Recalculate normals (Shift+N)', disabled: !can, run: run('recalc') },
    ], this.element.ownerDocument)
  }

  /** Exposed for tests: the rectangle currently being drawn (after Shift's square rule). */
  get drawingRect(): Rect | null {
    const d = this.drawing
    return !d ? null : d.phase === 'rect' ? this.currentRect(d) : d.rect
  }

  /** Exposed for tests: what is being drawn right now. */
  get drawingState(): Drawing | null { return this.drawing }

  /** Direction arrows of point entities and link arrows between entities (model/arrows.ts). */
  private arrowOverlays(): ReturnType<typeof entityArrows> {
    const st = this.store.state
    const list = st.editMode === 'object' ? st.multi : st.selection ? [st.selection] : []
    const instances = new Set(list.flatMap((x) => (x.instanceId !== undefined ? [x.instanceId] : [])))
    const keys = new Map<string, readonly string[]>()
    const refKeys = (c: string): readonly string[] => {
      let k = keys.get(c)
      if (!k) keys.set(c, (k = refKeysOf(st.entityDefs.find((d) => d.name === c))))
      return k
    }
    const items = placedEntities(st.doc.entities, st.prefabEntities)
    const names = new Map<string, string>()
    return entityArrows(items, {
      boxOf: (c) => this.store.entityBox(c), colorOf: (c) => this.store.entityColor(c), refKeys,
      isSelected: (p) => (p.ids.length ? instances.has(p.ids[0]) : list.some((x) => x.instanceId === undefined && x.entityId === p.entity.id)),
      resolvedName: (p) => {
        const own = p.entity.keyvalues?.targetname ?? ''
        if (!own || !p.ids.length) return own
        const g = p.ids.join('/')
        let n = names.get(g)
        if (n === undefined) names.set(g, (n = chainNames(st.doc, st.library, p.ids).join('|')))
        return `${n}|${own}`
      },
      showDirections: this.settings.showDirections, showAllLinks: this.settings.showLinks,
    })
  }

  /** A selected decal has no box any more: its projector bounds (what picks it) are outlined in the selection colour. */
  private decalSelectionOverlay(): { color: [number, number, number]; lines: number[] }[] {
    const st = this.store.state
    const list = st.editMode === 'object' ? st.multi : st.selection ? [st.selection] : []
    const instances = new Set(list.flatMap((x) => (x.instanceId !== undefined ? [x.instanceId] : [])))
    const lines: number[] = []
    for (const p of placedEntities(st.doc.entities, st.prefabEntities)) {
      if (p.entity.classname !== 'decal' || p.entity.brushes?.length) continue
      const selected = p.ids.length ? instances.has(p.ids[0]) : list.some((x) => x.instanceId === undefined && x.entityId === p.entity.id)
      if (selected) lines.push(...decalBoundsLines(p.entity))
    }
    return lines.length ? [{ color: [1, 0.62, 0.15], lines }] : []
  }

  /** Preview gizmos of the modern light entities (model/lights.ts): every light shows its compact icon, a selected one also its reach. */
  private lightOverlays(): { color: [number, number, number]; lines: number[] }[] {
    if (!this.settings.showLightGizmos) return []
    const st = this.store.state
    const list = st.editMode === 'object' ? st.multi : st.selection ? [st.selection] : []
    const instances = new Set(list.flatMap((x) => (x.instanceId !== undefined ? [x.instanceId] : [])))
    const out: { color: [number, number, number]; lines: number[] }[] = []
    for (const p of placedEntities(st.doc.entities, st.prefabEntities)) {
      if (!isModernLight(p.entity.classname)) continue
      const params = lightParams(p.entity, st.entityDefs.find((d) => d.name === p.entity.classname))
      if (!params) continue
      const selected = p.ids.length ? instances.has(p.ids[0]) : list.some((x) => x.instanceId === undefined && x.entityId === p.entity.id)
      out.push({ color: lightGizmoColor(params), lines: lightGizmo(p.entity, params, selected) })  // readable for black lights, violet + crossed for dark ones
    }
    return out
  }

  /** The modes that show the Core's live lighting: asking for one starts the lighting on the Core. */
  private needsLighting(): boolean { return this.settings.renderMode === 'lightingOnly' || this.settings.renderMode === 'lit' || this.settings.showLightvol || this.settings.showLightmapDensity }

  // ---- header overlay (UE-style) ----------------------------------------------------
  private buildHeader(): HTMLElement {
    const mode = h('select', { title: 'Render mode (per viewport)' })
    for (const m of RENDER_MODES) mode.append(h('option', { value: m.id, ...(m.implemented ? {} : { disabled: '' }) }, m.implemented ? m.label : `${m.label} (later)`))
    // 'Lighting only' shows the Core's live lightmaps: asking for that mode starts the lighting on the Core, leaving it stops it when no other viewport needs it
    const lightStatus = h('span', { class: 'vp-label', title: 'Live lighting progress (Core)' })
    this.updateLightStatus = () => {
      const on = this.needsLighting()
      const lm = this.store.lightmap
      lightStatus.style.display = on ? '' : 'none'
      if (!on) return
      const progress = !lm.status ? 'Lighting: starting…' : lm.status.converged ? 'Lighting: done' : `Lighting: ${Math.round(lm.progress() * 100)}%`
      const dens = this.settings.showLightmapDensity ? densityOf(lm) : null
      lightStatus.textContent = this.settings.showLightmapDensity ? `${progress} | ${dens ? densityLegend(dens.min, dens.max) : densityLegend(0, 0)}` : progress
    }
    mode.addEventListener('change', () => {
      if (isImplemented(mode.value as RenderMode)) this.settings.renderMode = mode.value as RenderMode
      this.store.setLightingWanted(this, this.needsLighting())
      this.updateLightStatus()
      this.invalidate()
    })
    this.updateLightStatus()

    // one "View" menu for every optional overlay (more arrive with lighting: lightvol samples, lightmap density)
    type Opt = { label: string; key?: 'showSizes' | 'showGrid' | 'showOrigins' | 'showDirections' | 'showLinks' | 'showLightGizmos' | 'showLightvol' | 'showLightmapDensity' | 'showTexelHeat' | 'showOutlines' | 'showSubtractive' | 'backfaceCulling'; later?: boolean }
    const OPTIONS: Opt[] = [
      { label: 'Grid', key: 'showGrid' }, { label: 'Selection size', key: 'showSizes' }, { label: 'Entity origins', key: 'showOrigins' }, { label: 'Entity direction arrows', key: 'showDirections' }, { label: 'Entity link arrows (all; the selection\'s always show)', key: 'showLinks' }, { label: 'Light gizmos (icons; the selection\'s reach)', key: 'showLightGizmos' }, { label: 'Face outlines (black edges)', key: 'showOutlines' },
      { label: 'Subtractive brush shapes', key: 'showSubtractive' }, { label: 'Backface culling', key: 'backfaceCulling' }, { label: 'Lightvol samples (baked light probes)', key: 'showLightvol' }, { label: 'Lightmap density (texel checker)', key: 'showLightmapDensity' }, { label: 'Texel density heat map (texture pixels per meter)', key: 'showTexelHeat' },
    ]
    const menu = h('div', { class: 'vp-menu', style: 'display:none' })
    const viewBtn = h('button', { class: 'chip', title: 'View options' }, 'View ▾')
    const drawMenu = () => {
      menu.replaceChildren(...OPTIONS.map((o) => {
        const cb = h('input', { type: 'checkbox', ...(o.later ? { disabled: '' } : {}) })
        if (o.key) cb.checked = this.settings[o.key]
        cb.addEventListener('change', () => { if (o.key) { this.settings[o.key] = cb.checked; if (o.key === 'showLightvol' || o.key === 'showLightmapDensity') { this.store.setLightingWanted(this, this.needsLighting()); this.updateLightStatus() } this.invalidate() } })
        return h('label', { class: `vp-menu-row${o.later ? ' dim' : ''}` }, cb, o.later ? `${o.label} (later)` : o.label)
      }))
    }
    viewBtn.addEventListener('click', (ev) => { ev.stopPropagation(); drawMenu(); menu.style.display = menu.style.display === 'none' ? '' : 'none' })
    this.viewMenu = menu; this.viewBtn = viewBtn
    const viewWrap = h('span', { class: 'vp-menu-wrap' }, viewBtn, menu)

    const snapBtn = h('button', { class: 'chip on', title: 'Grid snapping (on/off)' }, '⊞ Snap')
    const size = h('select', { title: 'Grid size (metres)' })
    for (const g of GRID_SIZES) size.append(h('option', { value: String(g), ...(g === this.settings.gridSize ? { selected: '' } : {}) }, `${g} m`))
    snapBtn.addEventListener('click', () => { this.settings.snapEnabled = !this.settings.snapEnabled; snapBtn.classList.toggle('on', this.settings.snapEnabled) })
    size.addEventListener('change', () => { this.settings.gridSize = Number(size.value) })
    const gizmoBtns = (['move', 'rotate', 'scale'] as const).map((m) => {
      const b = h('button', { class: `chip${this.settings.gizmo === m ? ' on' : ''}`, title: `${m[0].toUpperCase()}${m.slice(1)} gizmo` }, m === 'move' ? '✥' : m === 'rotate' ? '⟳' : '⤢')
      b.addEventListener('click', () => { this.settings.gizmo = m; gizmoBtns.forEach((x, i) => x.classList.toggle('on', (['move', 'rotate', 'scale'] as const)[i] === m)); this.invalidate() })
      return b
    })
    const rot = h('select', { title: 'Rotation snap (degrees) for R and the Details fields' })
    for (const g of ROT_SNAPS) rot.append(h('option', { value: String(g), ...(g === this.settings.rotSnap ? { selected: '' } : {}) }, `${g}°`))
    rot.addEventListener('change', () => { this.settings.rotSnap = Number(rot.value) })
    const scl = h('select', { title: 'Scale snap (factor step) for S' })
    for (const g of SCALE_SNAPS) scl.append(h('option', { value: String(g), ...(g === this.settings.scaleSnap ? { selected: '' } : {}) }, `×${g}`))
    scl.addEventListener('change', () => { this.settings.scaleSnap = Number(scl.value) })

    const cam = h('select', { title: 'Camera: Fly = RMB look + W A S D Q E; Orbit = RMB orbits the target; Top / Front / Side = orthographic, RMB or MMB pans, wheel zooms' })
    for (const [id, label] of [['fly', 'Fly camera'], ['orbit', 'Orbit camera'], ['top', 'Top (XZ)'], ['front', 'Front (XY)'], ['side', 'Side (ZY)']] as const)
      cam.append(h('option', { value: id, ...(id === this.settings.cameraMode ? { selected: '' } : {}) }, label))
    cam.addEventListener('change', () => this.setCameraMode(cam.value as CameraMode))
    this.cameraSelect = cam
    // fly speed: a logarithmic slider plus exact text entry (m/s); RMB + wheel also changes it, x1.2 per notch
    const slider = h('input', { type: 'range', class: 'speed-slider', min: String(Math.log10(FLY_SPEED_MIN)), max: String(Math.log10(FLY_SPEED_MAX)), step: '0.01', value: String(Math.log10(this.settings.flySpeed)), title: 'Fly speed (RMB + wheel: x1.2 per notch, Shift = 3x)' })
    const text = h('input', { type: 'text', class: 'speed-text', value: String(this.settings.flySpeed), title: 'Fly speed in metres per second', inputmode: 'decimal' })
    slider.addEventListener('input', () => this.setFlySpeed(10 ** Number(slider.value)))
    const commit = () => { const v = Number(text.value.replace(',', '.')); if (Number.isFinite(v) && v > 0) this.setFlySpeed(v); else text.value = String(this.settings.flySpeed) }
    text.addEventListener('change', commit)
    text.addEventListener('keydown', (e) => { e.stopPropagation(); if (e.key === 'Enter') { commit(); text.blur() } })
    const speed = h('span', { class: 'speed-box' }, slider, text, h('span', { class: 'dim' }, 'm/s'))
    this.speedSlider = slider; this.speedText = text; this.speedBox = speed

    const pop = h('button', { class: 'chip', title: 'Open this viewport in its own window (drag it to another monitor)' }, '⧉')
    pop.addEventListener('click', () => this.onPopout?.())
    const frame = h('button', { class: 'chip', title: 'Frame everything (F)' }, 'Frame')
    frame.addEventListener('click', () => this.frameAll())
    return h('div', { class: 'vp-header' },
      h('div', { class: 'vp-group' }, h('span', { class: 'vp-label' }, 'Perspective'), cam, speed, mode, lightStatus, viewWrap, frame, ...(this.onPopout ? [pop] : [])),
      h('div', { class: 'vp-group' }, ...gizmoBtns, snapBtn, size, rot, scl))
  }
}

/** The level's sky behind the geometry (REQUIREMENTS §28 K10): with Auto sky on and a skybox chosen in the Level settings. */
export function viewportSky(doc: MapDocument): string | undefined {
  const w = worldspawnOf(doc), t = w?.keyvalues?.[SKY_TEXTURE_KEY]?.trim()
  return autoSkyOf(w) !== 'off' && t && isCubemapPath(t) ? t : undefined
}
