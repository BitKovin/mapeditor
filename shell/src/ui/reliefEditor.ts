// The Relief editor: a popup with its own 3D viewport in which one repeat of a texture is a vertical plane (the surface the texture lies on) and its relief
// mesh is edited with the same modeling tools as the Mesh editor (select vertices / edges / faces, G R S, extrude, inset, bevel, loop cut, merge, dissolve...).
// Depth is the Z axis: negative sinks into the surface, positive stands out of it. The mesh is stored with the texture (`relief` in `<texture file>.json`).
// Editing logic is in model/reliefSession.ts (pure, tested); this file is the DOM, input and view code.
import { mat4, vec4 } from 'gl-matrix'
import type { Vec3 } from '../model/math'
import { dot, length, normalize, scale, sub } from '../model/math'
import { averageNormal, parseEdge } from '../model/meshTopology'
import { MAX_BEVEL_SEGMENTS, bevelEdges, edgeRing, extrudeFaces, insetFaces, loopCut, transformVerts } from '../model/meshOps'
import { PRESETS, PresetKind, buildPreset, isFlatRelief, reliefEdgeLines, reliefReport, reliefTriangles } from '../model/relief'
import { ReliefSession } from '../model/reliefSession'
import { rotateAbout, scaleAbout, translateBy } from '../model/transform'
import { TextureProps, naturalSize } from '../model/texprops'
import { triangulateFace } from '../model/geometry'
import { OrbitCamera } from '../render/camera'
import { AXIS_DIR, Axis } from '../render/gizmo'
import { ReliefView, ReliefScene } from '../render/reliefView'
import type { TextureCache } from '../state/textures'
import { ModalKind, describeModal, grabDelta, metresPerPixel, planePoint, rotateAngle, scaleFactor, scaleVector, stepSegments, typeInto, typedValue } from '../tools/modal'
import { boxSelect, combineSel, pickEdge, pickFaceId, pickVertex } from '../tools/meshPick'
import { confirmDialog } from './dialog'
import { fmt, h } from './dom'

export interface ReliefEnv {
  textures: TextureCache
  /** Writes the properties to Core. */
  save: (path: string, props: TextureProps) => Promise<void>
}

interface Modal { kind: ModalKind; axis: Axis | null; typed: string; value: number; segments?: number; /** mouse wheel (bevel segments); true = used */ wheel?: (dy: number) => boolean; move: (mouse: { x: number; y: number }) => void; refresh: () => void }

const KEYS_HELP = 'LMB select (Shift adds, drag = box) · RMB orbit · MMB pan · wheel zoom · 1 2 3 vertex / edge / face · G R S move / rotate / scale (X Y Z axis, type a number) · E extrude · I inset · Ctrl+B bevel · Ctrl+R loop cut · M merge · X dissolve · Del delete faces · F fill · Alt+N flip · A all · Alt+A none · Ctrl+I invert · + / - grow / shrink · Ctrl+Z / Ctrl+Y undo / redo'

export function openReliefEditor(env: ReliefEnv, path: string): { close(): void } {
  const textures = env.textures
  const dims = (): [number, number] => { const i = textures.info(path); return i ? naturalSize(i) : [1, 1] }
  const [w0, h0] = dims()
  const session = new ReliefSession(textures.propsOf(path).relief, w0, h0)
  const startRelief = session.toRelief()
  let dirty = false
  let modal: Modal | null = null
  let status = ''
  let snapOn = true, snapStep = 0.05, showTexture = true, showGhosts = true
  let loopArmed = false, loopHover: string | null = null
  let box: { x0: number; y0: number; x1: number; y1: number; shift: boolean } | null = null
  const mouse = { x: 0, y: 0 }
  let unsub: () => void = () => {}
  let ro: ResizeObserver | null = null

  // ---- DOM ------------------------------------------------------------------------------------------------------------------------------------------
  const canvas = h('canvas', { class: 'rl-canvas', tabindex: '0' })
  const boxEl = h('div', { class: 'rl-box' })
  const hud = h('div', { class: 'rl-hud' })
  const help = h('div', { class: 'rl-help' }, KEYS_HELP)
  const stage = h('div', { class: 'rl-stage' }, canvas, boxEl, hud)
  const side = h('div', { class: 'rl-side' })
  const foot = h('div', { class: 'modal-buttons' })
  const win = h('div', { class: 'modal reliefwin', role: 'dialog', 'aria-label': `Relief: ${path}` },
    h('div', { class: 'modal-title' }, `Relief: ${path}`),
    h('div', { class: 'rl-body' }, stage, side), help, foot)
  const backdrop = h('div', { class: 'modal-backdrop' }, win)
  document.body.append(backdrop)

  let view: ReliefView
  try { view = new ReliefView(canvas) } catch (e) {
    side.append(h('div', { class: 'tp-warn' }, `The relief view needs WebGL2: ${(e as Error).message}`))
    const close = h('button', {}, 'Close'); close.addEventListener('click', () => closeNow())
    foot.append(close)
    return { close: closeNow }
  }

  const cam = new OrbitCamera()
  const frame = (): void => {
    cam.target = [session.width / 2, session.height / 2, 0]
    const aspect = canvas.clientWidth / Math.max(1, canvas.clientHeight)
    cam.distance = (Math.max(session.height / 2, session.width / 2 / Math.max(0.3, aspect)) / Math.tan(cam.fovY / 2)) * 1.35
  }
  const setView = (yaw: number, pitch: number): void => { cam.yaw = yaw; cam.pitch = pitch; frame(); redraw() }
  cam.yaw = 0.55; cam.pitch = 0.3; frame()

  // ---- projection -----------------------------------------------------------------------------------------------------------------------------------
  const aspect = (): number => canvas.clientWidth / Math.max(1, canvas.clientHeight)
  const project = (p: Vec3): { x: number; y: number; depth: number } | null => {
    const vp = mat4.clone(cam.viewProj(aspect()))
    const c = vec4.transformMat4(vec4.create(), [p[0], p[1], p[2], 1], vp)
    if (c[3] <= 1e-6) return null
    return { x: ((c[0] / c[3]) * 0.5 + 0.5) * canvas.clientWidth, y: (1 - ((c[1] / c[3]) * 0.5 + 0.5)) * canvas.clientHeight, depth: c[2] / c[3] }
  }
  const rayAt = (x: number, y: number) => cam.rayFromNdc((x / canvas.clientWidth) * 2 - 1, 1 - (y / canvas.clientHeight) * 2, aspect())
  const forward = (): Vec3 => normalize(sub(cam.target, cam.eye))
  const localMouse = (e: { clientX: number; clientY: number }): { x: number; y: number } => { const r = canvas.getBoundingClientRect(); return { x: e.clientX - r.left, y: e.clientY - r.top } }
  const step = (): number => (snapOn ? snapStep : 0)

  // ---- drawing --------------------------------------------------------------------------------------------------------------------------------------
  let raf = 0
  const redraw = (): void => { if (!raf) raf = requestAnimationFrame(() => { raf = 0; draw() }) }

  const scene = (): ReliefScene => {
    const b = session.brush, s = session.sel
    const w = session.width, hh = session.height
    const selTris: number[] = []
    for (const f of b.faces) if (s.faces.includes(f.id)) for (const [i, j, k] of triangulateFace(b, f)) for (const v of [i, j, k]) selTris.push(...b.vertices[v])
    const selEdges: number[] = []
    for (const k of s.edges) { const [x, y] = parseEdge(k); selEdges.push(...b.vertices[x], ...b.vertices[y]) }
    const points = b.vertices.flatMap((p) => p), selPoints = s.verts.flatMap((i) => b.vertices[i])
    // grid on the surface plane: the snap step, or ten divisions when snapping is off
    const grid: number[] = []
    const gs = snapOn && snapStep > 0 ? snapStep : Math.max(w, hh) / 10
    if (Math.max(w, hh) / gs <= 120) {
      for (let x = gs; x < w - 1e-9; x += gs) grid.push(x, 0, 0, x, hh, 0)
      for (let y = gs; y < hh - 1e-9; y += gs) grid.push(0, y, 0, w, y, 0)
    }
    const outline = [0, 0, 0, w, 0, 0, w, 0, 0, w, hh, 0, w, hh, 0, 0, hh, 0, 0, hh, 0, 0, 0, 0]
    const overlay: number[] = []
    if (loopArmed && loopHover) {
      const ring = edgeRing(b, loopHover)
      if (ring) for (const e of ring.ring) { const [p, q] = e.split('>').map(Number); overlay.push(...b.vertices[p], ...b.vertices[q]) }
    }
    return { tris: reliefTriangles(b, w, hh), selTris, edges: reliefEdgeLines(b), selEdges, points, selPoints, outline, grid, tile: { w, h: hh }, textured: showTexture, ghosts: showGhosts, axes: Math.min(w, hh) * 0.25, overlay }
  }

  const syncTexture = (): void => {
    const st = textures.get(path)
    view.setTexture(st.status === 'ready' ? (st.image.source as TexImageSource) : null)
    const [w, hh] = dims()
    if (st.status === 'ready' && (w !== session.width || hh !== session.height)) { session.resize(w, hh); frame() }
  }

  const draw = (): void => {
    syncTexture()
    view.draw(mat4.clone(cam.viewProj(aspect())) as unknown as Float32Array, scene())
    if (box) {
      const x = Math.min(box.x0, box.x1), y = Math.min(box.y0, box.y1)
      boxEl.style.cssText = `display:block;left:${x}px;top:${y}px;width:${Math.abs(box.x1 - box.x0)}px;height:${Math.abs(box.y1 - box.y0)}px`
    } else boxEl.style.display = 'none'
    renderHud()
  }

  const rep = () => reliefReport(session.brush, session.width, session.height)
  const renderHud = (): void => {
    const r = rep(), s = session.sel
    const sel = session.mode === 'vertex' ? `${s.verts.length} vertices` : session.mode === 'edge' ? `${s.edges.length} edges` : `${s.faces.length} faces`
    const msg = modal ? status : loopArmed ? 'Loop cut: hover an edge, click to place the cut, move to slide, click to confirm (Esc cancels)' : session.error ?? status
    hud.textContent = `${msg ? msg + '   |   ' : ''}Selected ${sel}   |   ${r.vertices} vertices, ${r.faces} faces, depth ${fmt(r.minZ)} .. ${fmt(r.maxZ)} m`
  }

  // ---- side panel -----------------------------------------------------------------------------------------------------------------------------------
  const num = (value: number, stepv: number, apply: (v: number) => void, title: string): HTMLInputElement => {
    const i = h('input', { type: 'number', step: String(stepv), value: fmt(value), title })
    i.addEventListener('change', () => { const v = Number(i.value); if (Number.isFinite(v)) apply(v) })
    i.addEventListener('keydown', (e) => { e.stopPropagation(); if (e.key === 'Enter') i.blur() })
    return i
  }
  const btn = (label: string, title: string, fn: () => void, cls = ''): HTMLButtonElement => {
    const b = h('button', { title, class: cls }, label)
    b.addEventListener('click', () => { fn(); canvas.focus() })
    return b
  }
  const section = (t: string): HTMLElement => h('div', { class: 'section' }, t)

  let panelSig = ''
  const buildPanel = (): void => {
    const r = rep(), s = session.sel
    const avgZ = s.verts.length ? s.verts.reduce((a, i) => a + session.brush.vertices[i][2], 0) / s.verts.length : 0
    const sig = JSON.stringify([session.mode, session.canUndo, session.canRedo, snapOn, snapStep, showTexture, showGhosts, !!modal, s.verts.length, fmt(avgZ), r.warnings, dirty, session.width, session.height, session.brush.faces.length])
    if (sig === panelSig) return
    if (side.contains(document.activeElement) && (document.activeElement as HTMLElement).tagName === 'INPUT') return
    panelSig = sig
    const modeBtn = (m: 'vertex' | 'edge' | 'face', label: string, key: string) => btn(label, `${label} mode (${key})`, () => { session.setMode(m); redraw() }, session.mode === m ? 'on' : '')
    const tool = (label: string, keys: string, title: string, fn: () => void) => btn(`${label}  ${keys}`, title, fn)
    const info = textures.info(path)
    side.replaceChildren(
      h('div', { class: 'dim', title: 'One repeat of the texture: its size in the world follows the texture\'s pixels per meter' }, `One repeat: ${fmt(session.width)} x ${fmt(session.height)} m${info ? ` (${info.width} x ${info.height} px at ${fmt(info.ppm)} px/m)` : ' (texture loading)'}`),
      section('Select'),
      h('div', { class: 'seg' }, modeBtn('vertex', 'Vertex', '1'), modeBtn('edge', 'Edge', '2'), modeBtn('face', 'Face', '3')),
      h('div', { class: 'btns' }, btn('All', 'Select everything (A)', () => { session.selectAll(); redraw() }), btn('None', 'Clear the selection (Alt+A)', () => { session.selectNone(); redraw() }),
        btn('Invert', 'Invert the selection (Ctrl+I)', () => { session.invert(); redraw() }), btn('+', 'Grow the selection (+)', () => { session.grow(); redraw() }), btn('-', 'Shrink the selection (-)', () => { session.shrink(); redraw() })),
      section('Tools'),
      h('div', { class: 'btns' },
        tool('Move', 'G', 'Move the selection; X / Y / Z constrain (Z = depth), type a number', () => startModal('grab')),
        tool('Rotate', 'R', 'Rotate around the view or an axis', () => startModal('rotate')),
        tool('Scale', 'S', 'Scale from the selection centre', () => startModal('scale')),
        tool('Extrude', 'E', 'Push the selected faces out of or into the surface', () => startModal('extrude')),
        tool('Inset', 'I', 'Inset the selected faces', () => startModal('inset')),
        tool('Bevel', 'Ctrl+B', 'Chamfer the selected edges (mouse wheel = segments)', () => startModal('bevel')),
        tool('Loop cut', 'Ctrl+R', 'Hover an edge, click, slide, click', () => { loopArmed = true; modal = null; redraw() }),
        tool('Merge', 'M', 'Weld the selected vertices at their centre', () => run(() => session.merge())),
        tool('Dissolve', 'X', 'Remove the selection, merging the faces around it', () => run(() => session.dissolve())),
        tool('Fill', 'F', 'Close a hole from the selected vertices', () => run(() => session.fill())),
        tool('Delete faces', 'Del', 'Delete the selected faces', () => run(() => session.deleteSelectedFaces())),
        tool('Flip', 'Alt+N', 'Flip the selected faces (red faces look into the surface)', () => run(() => session.flip()))),
      section('Depth of the selected vertices'),
      h('div', { class: 'row2' }, h('label', { class: 'numfield' }, h('span', {}, 'z'), num(avgZ, 0.01, (v) => run(() => session.setDepth(v)), 'Metres: negative sinks in, positive stands out')),
        btn('Flatten', 'Put the selected vertices on the surface (depth 0)', () => run(() => session.setDepth(0)))),
      section('Start from'),
      h('div', { class: 'btns' }, ...PRESETS.map((p) => btn(p.label, `${p.hint} (replaces the mesh; undoable)`, () => { session.load(buildPreset(p.id as PresetKind, session.width, session.height)); changed() }))),
      section('View'),
      h('div', { class: 'btns' },
        btn('Front', 'Look straight at the surface', () => setView(0, 0)), btn('Side', 'Look along the surface: shows the depth profile', () => setView(Math.PI / 2, 0)),
        btn('Top', 'Look down onto the top edge', () => setView(0, 1.5)), btn('3D', 'Oblique view', () => setView(0.55, 0.3)), btn('Frame', 'Fit the tile in the view (F)', () => { frame(); redraw() })),
      h('div', { class: 'rl-toggles' },
        toggle('Texture', showTexture, (v) => { showTexture = v }), toggle('Neighbours', showGhosts, (v) => { showGhosts = v }), toggle('Snap', snapOn, (v) => { snapOn = v }),
        h('label', { class: 'numfield', title: 'Snap step in metres (moves, extrude, inset, grid)' }, h('span', {}, 'step'), num(snapStep, 0.01, (v) => { if (v > 0) { snapStep = v; panelSig = ''; buildPanel(); redraw() } }, 'Metres'))),
      ...(r.warnings.length ? [section('Check'), ...r.warnings.map((t) => h('div', { class: 'tp-warn' }, t))] : []))

    const undo = btn('Undo', 'Ctrl+Z', () => { if (session.undo()) changed() }), redo = btn('Redo', 'Ctrl+Y', () => { if (session.redo()) changed() })
    if (!session.canUndo) undo.setAttribute('disabled', ''); if (!session.canRedo) redo.setAttribute('disabled', '')
    const flat = btn('Remove relief', 'Back to a flat surface (saved as no relief)', () => { session.reset(); changed() })
    const cancel = btn('Cancel', 'Close without saving (Esc)', () => void requestClose())
    const apply = btn('Apply', 'Save the relief with the texture', () => void applyAndClose(), 'primary')
    foot.replaceChildren(h('span', { class: 'dim rl-state' }, dirty ? 'Unsaved changes' : ''), undo, redo, flat, cancel, apply)
  }
  const toggle = (label: string, on: boolean, set: (v: boolean) => void): HTMLElement => {
    const cb = h('input', { type: 'checkbox' }); cb.checked = on
    cb.addEventListener('change', () => { set(cb.checked); panelSig = ''; refresh() })
    return h('label', { class: 'rl-toggle' }, cb, h('span', {}, label))
  }

  const refresh = (): void => { buildPanel(); redraw() }
  /** After any edit of the mesh. */
  const changed = (): void => {
    dirty = JSON.stringify(session.toRelief()) !== JSON.stringify(startRelief)
    refresh()
  }
  const run = (fn: () => string | null): void => { const err = fn(); status = err ?? ''; changed() }

  // ---- modal tools ----------------------------------------------------------------------------------------------------------------------------------
  function startModal(kind: ModalKind, edge?: string): boolean {
    if (modal) return false
    const b = session.brush, sel = session.sel
    if (['grab', 'rotate', 'scale'].includes(kind) && sel.verts.length === 0) { status = 'Select something first'; redraw(); return false }
    if ((kind === 'extrude' || kind === 'inset') && session.mode !== 'face') { status = `${kind === 'extrude' ? 'Extrude' : 'Inset'} works on faces: press 3, select a face first`; redraw(); return false }
    if ((kind === 'extrude' || kind === 'inset') && sel.faces.length === 0) { status = 'Select at least one face first'; redraw(); return false }
    if (kind === 'bevel' && (session.mode === 'vertex' || sel.edges.length === 0)) { status = 'Bevel works on edges: press 2, select edges first'; redraw(); return false }
    const pivot = session.pivot()
    const fwd = forward(), ray0 = rayAt(mouse.x, mouse.y)
    // a move without an axis slides on the surface plane (depth stays) unless the view looks along it
    const slidePlane: Vec3 = Math.abs(fwd[2]) > 0.25 ? [0, 0, 1] : scale(fwd, -1)
    const start = planePoint(ray0, pivot, slidePlane) ?? pivot
    const mouse0 = { ...mouse }
    const sp = project(pivot) ?? { x: 0, y: 0, depth: 1 }
    const mpp = metresPerPixel(cam.distanceTo(pivot), cam.fovY, canvas.clientHeight)
    const toward: Vec3 = scale(fwd, -1)
    /** metres along `dir` for the mouse movement since the start: through the screen projection of the direction, so it also works when `dir` points at the camera (then vertical mouse movement is used) */
    const along = (dir: Vec3, m: { x: number; y: number }): number => {
      const a = project(pivot), c = project([pivot[0] + dir[0], pivot[1] + dir[1], pivot[2] + dir[2]])
      const dx = m.x - mouse0.x, dy = m.y - mouse0.y
      if (a && c) {
        const sx = c.x - a.x, sy = c.y - a.y, l2 = sx * sx + sy * sy
        if (l2 > 64) return (dx * sx + dy * sy) / l2
      }
      return -dy * mpp * Math.sign(dir[2] || dir[1] || 1)
    }
    session.beginModal()
    const m: Modal = { kind, axis: null, typed: '', value: kind === 'scale' ? 1 : 0, move: () => {}, refresh: () => {} }
    const typed = (): number | null => typedValue(m.typed)
    const say = (): void => { status = describeModal(kind, m.value, m.axis, m.typed, kind === 'bevel' ? m.segments : undefined); if (session.error) status += `  [${session.error}]` }
    const moveVerts = (map: (p: Vec3) => Vec3): void => { session.modalUpdate((bb, ss) => transformVerts(bb, ss.verts, map)) }

    if (kind === 'grab') {
      let delta: Vec3 = [0, 0, 0]
      m.refresh = () => {
        const v = typed()
        if (v !== null) delta = scale(AXIS_DIR[m.axis ?? 0], v)
        m.value = length(delta)
        moveVerts(translateBy(delta)); say()
      }
      m.move = (mp) => {
        if (m.typed !== '') return
        if (m.axis === null) delta = grabDelta(rayAt(mp.x, mp.y), start, pivot, slidePlane, null, step())
        else { const t = along(AXIS_DIR[m.axis], mp), d = step() > 0 ? Math.round(t / step()) * step() : t; delta = scale(AXIS_DIR[m.axis], d) }
        m.refresh()
      }
    } else if (kind === 'rotate') {
      let angle = 0
      m.refresh = () => {
        const v = typed()
        if (v !== null) angle = (v * Math.PI) / 180
        m.value = angle
        moveVerts(rotateAbout(pivot, m.axis === null ? toward : AXIS_DIR[m.axis], angle)); say()
      }
      m.move = (mp) => {
        if (m.typed !== '') return
        const axisVec = m.axis === null ? toward : AXIS_DIR[m.axis]
        angle = rotateAngle(sp, mouse0, mp, dot(axisVec, toward) >= 0, snapOn ? Math.PI / 12 : 0)
        m.refresh()
      }
    } else if (kind === 'scale') {
      let f = 1
      m.refresh = () => {
        const v = typed()
        if (v !== null) f = v
        m.value = f
        moveVerts(scaleAbout(pivot, scaleVector(f === 0 ? 1e-6 : f, m.axis))); say()
      }
      m.move = (mp) => { if (m.typed === '') { f = scaleFactor(sp, mouse0, mp, snapOn ? 0.05 : 0); m.refresh() } }
    } else if (kind === 'extrude' || kind === 'inset' || kind === 'bevel') {
      const faces = sel.faces, edges = sel.edges
      const dir = kind === 'extrude' ? averageNormal(b, b.faces.filter((fc) => faces.includes(fc.id))) : ([0, 0, 0] as Vec3)
      let dist = 0, segments = 1
      m.refresh = () => {
        const v = typed()
        if (v !== null) dist = v
        m.value = dist
        if (kind === 'extrude') session.modalUpdate((bb) => extrudeFaces(bb, faces, dist))
        else if (kind === 'inset') session.modalUpdate((bb) => insetFaces(bb, faces, dist))
        else session.modalUpdate((bb, ss) => bevelEdges(bb, ss.edges.length ? ss.edges : edges, dist, segments))
        say()
      }
      if (kind === 'bevel') { m.segments = 1; m.wheel = (dy) => { segments = stepSegments(segments, dy, MAX_BEVEL_SEGMENTS); m.segments = segments; m.refresh(); return true } }
      m.move = (mp) => {
        if (m.typed !== '') return
        if (kind === 'extrude') { const t = along(dir, mp); dist = step() > 0 ? Math.round(t / step()) * step() : t }
        else { const d = Math.hypot(mp.x - mouse0.x, mp.y - mouse0.y) * mpp * Math.sign(mp.x - mouse0.x + 1e-9); dist = Math.max(0, step() > 0 ? Math.round(d / (step() / 2)) * (step() / 2) : d) }
        m.refresh()
      }
    } else if (kind === 'loopcut' && edge) {
      const info = edgeRing(b, edge)
      if (!info) { session.endModal(false); return false }
      const [p, q] = info.ring[0].split('>').map(Number)
      const a = project(b.vertices[p]), c = project(b.vertices[q])
      let frac = 0.5
      m.refresh = () => {
        const v = typed()
        if (v !== null) frac = v
        m.value = frac
        session.modalUpdate((bb) => loopCut(bb, edge, Math.min(0.999, Math.max(0.001, frac)))); say()
      }
      m.move = (mp) => {
        if (m.typed !== '' || !a || !c) return
        const dx = c.x - a.x, dy = c.y - a.y, l2 = dx * dx + dy * dy
        if (l2 > 1e-6) { frac = ((mp.x - a.x) * dx + (mp.y - a.y) * dy) / l2; m.refresh() }
      }
    } else { session.endModal(false); return false }
    modal = m
    m.refresh()
    redraw()
    return true
  }

  const endModal = (confirm: boolean): void => {
    if (!modal) return
    modal = null
    session.endModal(confirm)
    status = ''
    changed()
  }
  // ---- selection / camera input ---------------------------------------------------------------------------------------------------------------------
  const pickAt = (mp: { x: number; y: number }): (number | string)[] => {
    const b = session.brush
    if (session.mode === 'vertex') { const v = pickVertex(b, project, mp.x, mp.y, 10); return v === null ? [] : [v] }
    if (session.mode === 'edge') { const e = pickEdge(b, project, mp.x, mp.y, 8); return e === null ? [] : [e] }
    const f = pickFaceId(b, rayAt(mp.x, mp.y)); return f === null ? [] : [f]
  }

  type Drag = { kind: 'orbit' | 'pan' | 'select'; x: number; y: number; moved: boolean; shift: boolean } | null
  let drag: Drag = null

  canvas.addEventListener('contextmenu', (e) => e.preventDefault())
  canvas.addEventListener('pointerdown', (e) => {
    canvas.focus()
    const mp = localMouse(e)
    mouse.x = mp.x; mouse.y = mp.y
    if (modal || loopArmed) {
      if (loopArmed && e.button === 0) {
        const edgeKeyHit = pickEdge(session.brush, project, mp.x, mp.y, 12)
        loopArmed = false; loopHover = null
        if (edgeKeyHit) startModal('loopcut', edgeKeyHit)
        redraw(); return
      }
      if (e.button === 0) endModal(true)
      else if (e.button === 2) { if (loopArmed) { loopArmed = false; redraw() } else endModal(false) }
      return
    }
    canvas.setPointerCapture(e.pointerId)
    if (e.button === 2) drag = { kind: 'orbit', x: e.clientX, y: e.clientY, moved: false, shift: e.shiftKey }
    else if (e.button === 1) { e.preventDefault(); drag = { kind: 'pan', x: e.clientX, y: e.clientY, moved: false, shift: e.shiftKey } }
    else if (e.button === 0) { drag = { kind: 'select', x: mp.x, y: mp.y, moved: false, shift: e.shiftKey } }
  })
  canvas.addEventListener('pointermove', (e) => {
    const mp = localMouse(e)
    mouse.x = mp.x; mouse.y = mp.y
    if (modal) { modal.move(mp); redraw(); return }
    if (loopArmed) { loopHover = pickEdge(session.brush, project, mp.x, mp.y, 12); redraw(); return }
    if (!drag) return
    if (drag.kind === 'orbit') { cam.orbit(e.clientX - drag.x, e.clientY - drag.y); drag.x = e.clientX; drag.y = e.clientY; drag.moved = true; redraw() }
    else if (drag.kind === 'pan') { cam.pan(e.clientX - drag.x, e.clientY - drag.y, canvas.clientHeight); drag.x = e.clientX; drag.y = e.clientY; drag.moved = true; redraw() }
    else {
      if (!drag.moved && Math.hypot(mp.x - drag.x, mp.y - drag.y) > 4) drag.moved = true
      if (drag.moved) { box = { x0: drag.x, y0: drag.y, x1: mp.x, y1: mp.y, shift: drag.shift }; redraw() }
    }
  })
  canvas.addEventListener('pointerup', (e) => {
    if (!drag) return
    const d = drag; drag = null
    try { canvas.releasePointerCapture(e.pointerId) } catch { /* not captured */ }
    if (d.kind !== 'select') return
    const mp = localMouse(e)
    let add
    if (d.moved && box) add = boxSelect(session.brush, project, session.mode, box.x0, box.y0, box.x1, box.y1)
    else add = session.pick(pickAt(mp))
    box = null
    if (!d.moved && !d.shift && add.verts.length === 0) session.selectNone()
    else session.setSel(d.shift ? combineSel(session.brush, session.sel, add, session.mode, true) : d.moved ? combineSel(session.brush, { verts: [], edges: [], faces: [] }, add, session.mode, false) : add)
    refresh()
  })
  canvas.addEventListener('wheel', (e) => {
    e.preventDefault()
    if (modal?.wheel?.(e.deltaY)) { redraw(); return }
    cam.zoom(e.deltaY); redraw()
  }, { passive: false })

  // ---- keyboard -------------------------------------------------------------------------------------------------------------------------------------
  const onKey = (e: KeyboardEvent): void => {
    if (!backdrop.isConnected) return
    const inInput = (e.target as HTMLElement | null)?.tagName === 'INPUT'
    if (e.key === 'Escape') {
      e.preventDefault(); e.stopPropagation()
      if (inInput) { (e.target as HTMLElement).blur(); return }
      if (modal) endModal(false)
      else if (loopArmed) { loopArmed = false; redraw() }
      else void requestClose()
      return
    }
    if (inInput) { e.stopPropagation(); return }
    e.stopPropagation()
    const k = e.key.toLowerCase(), mod = e.ctrlKey || e.metaKey
    const consume = (): void => e.preventDefault()
    if (modal) {
      if (e.key === 'Enter') { consume(); endModal(true); return }
      if ((k === 'x' || k === 'y' || k === 'z') && ['grab', 'rotate', 'scale'].includes(modal.kind)) {
        consume()
        const a = (k === 'x' ? 0 : k === 'y' ? 1 : 2) as Axis
        modal.axis = modal.axis === a ? null : a
        modal.refresh(); modal.move(mouse); redraw(); return
      }
      const next = typeInto(modal.typed, e.key)
      if (next !== null) { consume(); modal.typed = next; modal.refresh(); redraw() }
      return
    }
    if (mod && k === 'z' && !e.shiftKey) { consume(); if (session.undo()) changed(); return }
    if (mod && (k === 'y' || (k === 'z' && e.shiftKey))) { consume(); if (session.redo()) changed(); return }
    if (mod && k === 'i') { consume(); session.invert(); refresh(); return }
    if (mod && k === 'b') { consume(); startModal('bevel'); return }
    if (mod && k === 'r') { consume(); loopArmed = true; loopHover = pickEdge(session.brush, project, mouse.x, mouse.y, 12); redraw(); return }
    if (mod) return
    if (e.altKey) {
      if (k === 'a') { consume(); session.selectNone(); refresh() } else if (k === 'n') { consume(); run(() => session.flip()) }
      return
    }
    switch (k) {
      case '1': session.setMode('vertex'); refresh(); break
      case '2': session.setMode('edge'); refresh(); break
      case '3': session.setMode('face'); refresh(); break
      case 'a': session.selectAll(); refresh(); break
      case 'g': startModal('grab'); break
      case 'r': startModal('rotate'); break
      case 's': startModal('scale'); break
      case 'e': startModal('extrude'); break
      case 'i': startModal('inset'); break
      case 'm': run(() => session.merge()); break
      case 'x': run(() => session.dissolve()); break
      case 'f': if (session.sel.verts.length) run(() => session.fill()); else { frame(); redraw() } break
      case 'delete': case 'backspace': run(() => session.deleteSelectedFaces()); break
      case '+': case '=': session.grow(); refresh(); break
      case '-': session.shrink(); refresh(); break
      default: return
    }
    consume()
  }
  document.addEventListener('keydown', onKey, true)

  // ---- closing / saving -----------------------------------------------------------------------------------------------------------------------------
  let closed = false
  function closeNow(): void {
    if (closed) return
    closed = true
    cancelAnimationFrame(raf)
    document.removeEventListener('keydown', onKey, true)
    unsub()
    ro?.disconnect()
    try { view?.dispose() } catch { /* context already gone */ }
    backdrop.remove()
  }
  async function requestClose(): Promise<void> {
    if (modal) endModal(false)
    if (dirty && !(await confirmDialog('Discard changes?', 'The relief has unsaved changes. Close without saving them?', 'Discard'))) return
    closeNow()
  }
  async function applyAndClose(): Promise<void> {
    if (modal) endModal(true)
    const relief = session.toRelief()
    const props: TextureProps = { ...textures.propsOf(path), relief: isFlatRelief(relief) && relief.faces.length === 1 ? null : relief }
    status = 'Saving...'; redraw()
    try {
      await env.save(path, props)
      textures.setProps(path, props)
      closeNow()
    } catch (e) { status = `Could not save: ${(e as Error).message}`; redraw() }
  }

  unsub = textures.onChange(() => { panelSig = ''; refresh() })
  ro = typeof ResizeObserver !== 'undefined' ? new ResizeObserver(() => redraw()) : null
  ro?.observe(canvas)
  backdrop.addEventListener('mousedown', (e) => { if (e.target === backdrop) void requestClose() })
  buildPanel(); redraw()
  canvas.focus()
  return { close: closeNow }
}
