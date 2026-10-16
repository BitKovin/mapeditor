// Fast Texture Tool window (REQUIREMENTS §6 B5; Scythe's Fast Texture Tool, Shift+Q). A floating, NON modal window (the viewport stays usable: select other faces
// and the island follows). It shows the UV island of the selected faces of one brush over the texture and edits it by direct manipulation:
//   drag the body = move, corner / edge handles = scale (Shift on a corner = keep the aspect), the round handle above = rotate (Shift = 15 degree steps),
//   Mirror U / V, Rotate 90, Align edge (click a UV edge to make it horizontal / vertical), Follow Quads / Ortho / Axial unwrap, snap, wheel zoom, middle drag pan.
// The blue dashed rectangle per face is the shape it would have at the texture's own pixels per meter (the "ideal" shape: matching it = texel density as authored).
// A second mode, Lightmap, shows where the charts of the selected faces sit on the lightmap page (over the baked page) and how dense they are, and edits the
// density (x2, /2, a number); chart placement is packed by the Core and is not hand editable. Maths: model/ftt.ts and model/lightmapView.ts (tested).
import {
  HandleId, IDENTITY, UvBounds, Xform, alignEdgeXform, boundsCentre, dragXform, handlePoint, hitTest, idealSize, islandPolygons, mirrorAbout, nearestEdge, polygonsBounds, rotateAbout,
} from '../model/ftt'
import type { Brush } from '../model/document'
import { Chart, chartsOf, pagePicture } from '../model/lightmapView'
import { lightmapScaleOf, LIGHTMAP_SCALE_MAX, LIGHTMAP_SCALE_MIN } from '../model/uv'
import { texEnv } from '../model/texprops'
import { faceMapKey } from '../render/lightmapStore'
import { FttTarget, editLightmapScale, fttApply, fttBrush, fttDrag, fttTarget, fttUnwrap } from '../state/actions'
import type { Store } from '../state/store'
import type { TextureCache } from '../state/textures'
import { fmt, h } from './dom'

export interface FttEnv { store: Store; textures: TextureCache; doc?: Document }

const W = 600, H = 420, SNAPS: [string, number][] = [['off', 0], ['1 repeat', 1], ['1/2', 0.5], ['1/4', 0.25], ['1/8', 0.125], ['1/16', 0.0625]]
const CURSOR: Record<string, string> = { nw: 'nwse-resize', se: 'nwse-resize', ne: 'nesw-resize', sw: 'nesw-resize', n: 'ns-resize', s: 'ns-resize', e: 'ew-resize', w: 'ew-resize', rotate: 'grab', move: 'move' }

let openWin: { root: HTMLElement; close: () => void } | null = null

/** Shift+Q: opens the tool, or closes it when it is open. */
export function toggleFastTextureTool(env: FttEnv): void {
  if (openWin && openWin.root.isConnected) openWin.close()
  else openFastTextureTool(env)
}

export function openFastTextureTool(env: FttEnv): void {
  if (openWin && openWin.root.isConnected) return
  const { store, textures } = env
  const doc = env.doc ?? document
  const canvas = h('canvas', { width: String(W), height: String(H), style: 'display:block;background:#1b1b1b;border:1px solid #333;touch-action:none' })
  const info = h('div', { class: 'dim', style: 'min-height:15px;user-select:text' })
  const status = h('div', { class: 'dim', style: 'min-height:15px' })

  let target: FttTarget | null = null, brush: Brush | null = null
  let polys: { id: number; poly: [number, number][] }[] = []
  let bounds: UvBounds | null = null
  const view = { s: 120, ox: 40, oy: 40 }
  let mode: 'uv' | 'lightmap' = 'uv'
  let backdrop: { page: number; at: number; canvas: HTMLCanvasElement } | null = null
  let snap = 0, alignOn = false, hoverEdge: [[number, number], [number, number]] | null = null
  let drag: { kind: 'move' | HandleId; start: [number, number]; bounds: UvBounds; target: FttTarget } | null = null
  let pan: { x: number; y: number; ox: number; oy: number } | null = null

  const toUv = (e: { clientX: number; clientY: number }): [number, number] => {
    const r = canvas.getBoundingClientRect(), k = W / (r.width || 1)
    return [((e.clientX - r.left) * k - view.ox) / view.s, ((e.clientY - r.top) * k - view.oy) / view.s]
  }
  const px = (u: number, v: number): [number, number] => [view.ox + u * view.s, view.oy + v * view.s]

  function fit(): void {
    if (!bounds) return
    const w = Math.max(bounds.max[0] - bounds.min[0], 0.25), hh = Math.max(bounds.max[1] - bounds.min[1], 0.25)
    view.s = Math.max(8, Math.min(4000, Math.min((W - 110) / w, (H - 110) / hh)))
    const c = boundsCentre(bounds)
    view.ox = W / 2 - c[0] * view.s; view.oy = H / 2 - c[1] * view.s
  }

  function refresh(first = false): void {
    target = fttTarget(store)
    brush = target ? fttBrush(store.state.doc, target) : null
    polys = brush && target ? islandPolygons(brush, target.faceIds) : []
    bounds = polygonsBounds(polys.map((p) => p.poly))
    if (first || (!drag && bounds && !onScreen(bounds))) fit()
    if (mode === 'lightmap' && brush && target && document.activeElement !== scaleIn) { const f0 = brush.faces.find((x) => x.id === target!.faceIds[0]); if (f0) scaleIn.value = fmt(lightmapScaleOf(f0)) }
    describe(); draw()
  }
  const onScreen = (b: UvBounds): boolean => { const [x0, y0] = px(b.min[0], b.min[1]), [x1, y1] = px(b.max[0], b.max[1]); return x1 > 0 && y1 > 0 && x0 < W && y0 < H }

  function describe(): void {
    if (mode === 'lightmap') { info.textContent = lightmapInfo(); return }
    if (!brush || !target || !bounds) { info.textContent = 'Select faces (or a brush) in the viewport: their UV island appears here.'; return }
    const f = brush.faces.find((x) => x.id === target!.faceIds[0])!, ti = texEnv.info(f.texture ?? 'dev/grey')
    const bw = bounds.max[0] - bounds.min[0], bh = bounds.max[1] - bounds.min[1]
    const ideal = idealSize(brush, f)
    info.textContent = `${polys.length} face${polys.length === 1 ? '' : 's'} of ${target.brushId} · island ${fmt(bw)} x ${fmt(bh)} repeats${ti ? ` (${Math.round(bw * ti.width)} x ${Math.round(bh * ti.height)} px)` : ''} · first face ideal ${fmt(ideal[0])} x ${fmt(ideal[1])} repeats (dashed)`
  }

  function draw(): void {
    const g = canvas.getContext('2d')
    if (!g) return
    g.setTransform(1, 0, 0, 1, 0, 0)
    g.fillStyle = '#1b1b1b'; g.fillRect(0, 0, W, H)
    if (mode === 'lightmap') { drawLightmap(g); return }
    // the texture, repeated (one repeat = view.s pixels)
    const path = brush && target ? brush.faces.find((x) => x.id === target!.faceIds[0])?.texture ?? 'dev/grey' : null
    const st = path ? textures.get(path) : null
    const ti = path ? texEnv.info(path) : null
    if (st && st.status === 'ready' && ti) {
      try {
        const pat = g.createPattern(st.image.source as CanvasImageSource, 'repeat')
        if (pat) { pat.setTransform(new DOMMatrix([view.s / ti.width, 0, 0, view.s / ti.height, view.ox, view.oy])); g.globalAlpha = 0.55; g.fillStyle = pat; g.fillRect(0, 0, W, H); g.globalAlpha = 1 }
      } catch { /* keep the dark background */ }
    }
    // repeat grid
    g.lineWidth = 1; g.strokeStyle = 'rgba(255,255,255,.18)'
    const u0 = Math.floor(-view.ox / view.s), u1 = Math.ceil((W - view.ox) / view.s), v0 = Math.floor(-view.oy / view.s), v1 = Math.ceil((H - view.oy) / view.s)
    if (u1 - u0 < 200 && v1 - v0 < 200) {
      g.beginPath()
      for (let u = u0; u <= u1; u++) { const x = Math.round(view.ox + u * view.s) + 0.5; g.moveTo(x, 0); g.lineTo(x, H) }
      for (let v = v0; v <= v1; v++) { const y = Math.round(view.oy + v * view.s) + 0.5; g.moveTo(0, y); g.lineTo(W, y) }
      g.stroke()
    }
    if (!bounds || !brush) { g.fillStyle = '#888'; g.font = '13px sans-serif'; g.textAlign = 'center'; g.fillText('Nothing selected', W / 2, H / 2); g.textAlign = 'start'; return }
    // faces + ideal shapes
    for (const { id, poly } of polys) {
      g.beginPath(); poly.forEach(([u, v], i) => { const [x, y] = px(u, v); i ? g.lineTo(x, y) : g.moveTo(x, y) }); g.closePath()
      g.fillStyle = 'rgba(10,132,232,.22)'; g.fill(); g.strokeStyle = '#6cb6ff'; g.lineWidth = 1.5; g.stroke()
      const f = brush.faces.find((x) => x.id === id)!, b = polygonsBounds([poly])!, ideal = idealSize(brush, f)
      const [x, y] = px(b.min[0], b.min[1])
      g.setLineDash([5, 4]); g.strokeStyle = 'rgba(80,170,255,.9)'; g.lineWidth = 1; g.strokeRect(x + 0.5, y + 0.5, ideal[0] * view.s, ideal[1] * view.s); g.setLineDash([])
    }
    if (alignOn && hoverEdge) { const [a, b] = hoverEdge, [x0, y0] = px(a[0], a[1]), [x1, y1] = px(b[0], b[1]); g.strokeStyle = '#ffd34d'; g.lineWidth = 3; g.beginPath(); g.moveTo(x0, y0); g.lineTo(x1, y1); g.stroke() }
    // island box and handles
    const [bx0, by0] = px(bounds.min[0], bounds.min[1]), [bx1, by1] = px(bounds.max[0], bounds.max[1])
    g.strokeStyle = '#ffd34d'; g.lineWidth = 1; g.strokeRect(bx0 + 0.5, by0 + 0.5, bx1 - bx0, by1 - by0)
    const gap = 24 / view.s
    const rp = px(...handlePoint(bounds, 'rotate', gap)), np = px(...handlePoint(bounds, 'n'))
    g.beginPath(); g.moveTo(np[0], np[1]); g.lineTo(rp[0], rp[1]); g.stroke()
    g.fillStyle = '#ffd34d'; g.beginPath(); g.arc(rp[0], rp[1], 5, 0, Math.PI * 2); g.fill()
    g.fillStyle = '#fff'
    for (const hd of ['nw', 'n', 'ne', 'e', 'se', 's', 'sw', 'w'] as HandleId[]) { const [x, y] = px(...handlePoint(bounds, hd)); g.fillRect(x - 4, y - 4, 8, 8); g.strokeRect(x - 3.5, y - 3.5, 7, 7) }
  }

  // ---- lightmap mode
  const lm = store.lightmap
  const lmCharts = (): Chart[] => (target && brush ? chartsOf(brush, target.faceIds, (id) => lm.faceMaps.get(faceMapKey('', target!.entityId, target!.brushId, id)), lm.atlas) : [])
  const pageOf = (cs: Chart[]): number => (cs.length ? cs[0].page : -1)

  function lightmapInfo(): string {
    if (!target || !brush) return 'Select faces (or a brush) in the viewport: their lightmap charts appear here.'
    if (!lm.ready) return 'No lightmap yet: switch lighting on in the viewport (Lighting only / Lit) and let it bake.'
    const cs = lmCharts()
    if (!cs.length) return 'The selected faces have no light data (not lightmapped, or not baked yet).'
    return cs.slice(0, 4).map((c) => `face ${c.faceId}: page ${c.page}, ${Math.round(c.max[0] - c.min[0])} x ${Math.round(c.max[1] - c.min[1])} texels at (${Math.round(c.min[0])}, ${Math.round(c.min[1])}), ${fmt(c.density[0])} x ${fmt(c.density[1])} texels/m (density x${fmt(c.scale)})`).join(' | ') + (cs.length > 4 ? ` | +${cs.length - 4} more` : '')
  }

  /** The page as a small picture, rebuilt at most once a second while tiles arrive. */
  function pageBackdrop(page: number): HTMLCanvasElement | null {
    const px = lm.pagesOf(0)?.[page]
    if (!px || !lm.atlas) return null
    const now = Date.now()
    if (backdrop && backdrop.page === page && now - backdrop.at < 1000) return backdrop.canvas
    const c = backdrop?.canvas ?? doc.createElement('canvas'), size = 256
    c.width = size; c.height = size
    const g = c.getContext('2d')
    if (!g) return null
    const data = pagePicture(px, lm.atlas, size)
    g.putImageData(new ImageData(data, size, size), 0, 0)
    backdrop = { page, at: now, canvas: c }
    return c
  }

  function drawLightmap(g: CanvasRenderingContext2D): void {
    g.fillStyle = '#888'; g.font = '13px sans-serif'; g.textAlign = 'center'
    const cs = lm.ready ? lmCharts() : []
    if (!cs.length) { g.fillText(!lm.ready ? 'No lightmap yet' : target ? 'No light data for the selection' : 'Nothing selected', W / 2, H / 2); g.textAlign = 'start'; return }
    g.textAlign = 'start'
    const page = pageOf(cs), side = Math.min(W, H) - 20, k = side / lm.atlas, ox = (W - side) / 2, oy = (H - side) / 2
    g.fillStyle = '#0d0d0d'; g.fillRect(ox, oy, side, side)
    const img = pageBackdrop(page)
    if (img) { g.imageSmoothingEnabled = false; g.drawImage(img, ox, oy, side, side); g.imageSmoothingEnabled = true }
    g.strokeStyle = 'rgba(255,255,255,.12)'; g.lineWidth = 1; g.beginPath()
    for (let t = 0; t <= lm.atlas; t += lm.atlas / 16) { const o = Math.round(t * k) + 0.5; g.moveTo(ox + o, oy); g.lineTo(ox + o, oy + side); g.moveTo(ox, oy + o); g.lineTo(ox + side, oy + o) }
    g.stroke()
    const poly = (c: Chart, fill: string, stroke: string, lw: number): void => {
      g.beginPath(); c.polygon.forEach(([x, y], i) => { i ? g.lineTo(ox + x * k, oy + y * k) : g.moveTo(ox + x * k, oy + y * k) }); g.closePath()
      g.fillStyle = fill; g.fill(); g.strokeStyle = stroke; g.lineWidth = lw; g.stroke()
    }
    // the other lightmapped faces of the brush on this page, for context
    if (brush && target) {
      const rest = chartsOf(brush, brush.faces.map((f) => f.id).filter((id) => !target!.faceIds.includes(id)), (id) => lm.faceMaps.get(faceMapKey('', target!.entityId, target!.brushId, id)), lm.atlas)
      for (const c of rest) if (c.page === page) poly(c, 'rgba(255,255,255,.06)', 'rgba(255,255,255,.35)', 1)
    }
    for (const c of cs) if (c.page === page) poly(c, 'rgba(10,132,232,.25)', '#6cb6ff', 1.5)
    g.fillStyle = '#ccc'; g.font = '11px sans-serif'; g.fillText(`page ${page} of ${lm.pageCount} · ${lm.atlas} x ${lm.atlas} texels`, ox + 4, oy + side - 6)
    if (cs.some((c) => c.page !== page)) g.fillText('(selection spans several pages: first one shown)', ox + 4, oy + 14)
  }

  // ---- pointer
  canvas.addEventListener('pointerdown', (e) => {
    if (mode === 'lightmap') return
    if (e.button === 1) { e.preventDefault(); pan = { x: e.clientX, y: e.clientY, ox: view.ox, oy: view.oy }; canvas.setPointerCapture?.(e.pointerId); return }
    if (e.button !== 0 || !bounds || !target || !store.canEdit) return
    const uv = toUv(e)
    if (alignOn) {
      const edge = nearestEdge(polys.map((p) => p.poly), uv, 10 / view.s)
      const t = edge ? alignEdgeXform(edge[0], edge[1]) : null
      if (t && t !== IDENTITY) { fttApply(store, target, 'Align UV edge', t); status.textContent = 'Edge aligned' } else status.textContent = edge ? 'That edge is already straight' : 'Click near a UV edge'
      return
    }
    const hit = hitTest(bounds, polys.map((p) => p.poly), uv, 8 / view.s, 24 / view.s)
    if (!hit || !store.beginDrag('Fast texture')) return
    drag = { kind: hit, start: uv, bounds, target }
    canvas.setPointerCapture?.(e.pointerId)
  })
  canvas.addEventListener('pointermove', (e) => {
    if (mode === 'lightmap') { canvas.style.cursor = 'default'; return }
    if (pan) { const k = W / (canvas.getBoundingClientRect().width || 1); view.ox = pan.ox + (e.clientX - pan.x) * k; view.oy = pan.oy + (e.clientY - pan.y) * k; draw(); return }
    const uv = toUv(e)
    if (drag) { fttDrag(store, drag.target, dragXform(drag.kind, drag.start, uv, drag.bounds, { snap, shift: e.shiftKey })); return }
    if (alignOn) { hoverEdge = nearestEdge(polys.map((p) => p.poly), uv, 10 / view.s); canvas.style.cursor = hoverEdge ? 'pointer' : 'default'; draw(); return }
    const hit = bounds ? hitTest(bounds, polys.map((p) => p.poly), uv, 8 / view.s, 24 / view.s) : null
    canvas.style.cursor = hit ? CURSOR[hit] : 'default'
  })
  const release = (): void => { if (drag) { store.endDrag(); drag = null } pan = null }
  canvas.addEventListener('pointerup', release); canvas.addEventListener('pointercancel', release)
  canvas.addEventListener('wheel', (e) => {
    e.preventDefault()
    if (mode === 'lightmap') return
    const uv = toUv(e), k = Math.exp(-e.deltaY * 0.0015), s = Math.max(8, Math.min(4000, view.s * k))
    const r = canvas.getBoundingClientRect(), kk = W / (r.width || 1)
    view.s = s; view.ox = (e.clientX - r.left) * kk - uv[0] * s; view.oy = (e.clientY - r.top) * kk - uv[1] * s
    draw()
  }, { passive: false })

  // ---- buttons
  const act = (label: string, title: string, run: (t: FttTarget, b: UvBounds) => void): HTMLButtonElement => {
    const b = h('button', { title }, label)
    b.addEventListener('click', () => { if (!target || !bounds || !store.canEdit) { status.textContent = 'Select faces first'; return } status.textContent = ''; run(target, bounds) })
    return b
  }
  const xf = (label: string, title: string, make: (c: [number, number]) => Xform, undoLabel: string) => act(label, title, (t, b) => { fttApply(store, t, undoLabel, make(boundsCentre(b))) })
  const unwrap = (label: string, title: string, mode: 'follow' | 'ortho' | 'axial') => act(label, title, (t) => {
    const n = fttUnwrap(store, t, mode)
    status.textContent = n ? '' : mode === 'follow' ? 'Follow Quads needs two or more faces (the first one is the source)' : 'Nothing changed'
  })
  const alignBtn = h('button', { title: 'Then click an edge of the island: it is turned until horizontal or vertical' }, 'Align edge')
  alignBtn.addEventListener('click', () => { alignOn = !alignOn; alignBtn.classList.toggle('active', alignOn); hoverEdge = null; status.textContent = alignOn ? 'Click an edge of the island' : ''; draw() })
  const snapSel = h('select', { title: 'Snap of move and scale, in texture repeats' }, ...SNAPS.map(([l, v]) => h('option', { value: String(v) }, `Snap: ${l}`)))
  snapSel.addEventListener('change', () => { snap = Number(snapSel.value) })
  const frame = h('button', { title: 'Zoom to the island' }, 'Frame'); frame.addEventListener('click', () => { fit(); draw() })

  const toolbar = h('div', { class: 'fft-bar' },
    xf('Mirror U', 'Flip the island left-right about its centre', (c) => mirrorAbout('u', c[0]), 'Mirror UV left-right'),
    xf('Mirror V', 'Flip the island top-bottom about its centre', (c) => mirrorAbout('v', c[1]), 'Mirror UV top-bottom'),
    xf('Rot -90', 'Rotate the island 90 degrees counter-clockwise on screen', (c) => rotateAbout(-90, c), 'Rotate UV -90'),
    xf('Rot +90', 'Rotate the island 90 degrees clockwise on screen', (c) => rotateAbout(90, c), 'Rotate UV 90'),
    alignBtn,
    unwrap('Follow Quads', 'Continue the first selected face\'s UVs over the others along their shared edges (Flow)', 'follow'),
    unwrap('Ortho', 'Project every face on its own, readable from the front', 'ortho'),
    unwrap('Axial', 'Project every face along the world axis it faces', 'axial'),
    snapSel, frame)

  // ---- mode switch and the lightmap density controls
  const lmBar = h('div', { class: 'fft-bar', style: 'display:none' })
  const scaleIn = h('input', { type: 'number', step: '0.25', min: String(LIGHTMAP_SCALE_MIN), max: String(LIGHTMAP_SCALE_MAX), style: 'width:70px', title: 'Lightmap density of the selected faces: 1 = the level\'s texels per metre' })
  scaleIn.addEventListener('keydown', (e) => e.stopPropagation())
  scaleIn.addEventListener('change', () => { const v = Number(scaleIn.value); if (v > 0) editLightmapScale(store, 'Lightmap density', () => v) })
  const dens = (label: string, title: string, f: (v: number) => number): HTMLButtonElement => { const b = h('button', { title }, label); b.addEventListener('click', () => { if (store.canEdit) editLightmapScale(store, 'Lightmap density', f) }); return b }
  lmBar.append(h('span', { class: 'dim' }, 'Density'), dens('x2', 'Twice the lightmap texels per metre (four times the texels)', (v) => v * 2), dens('/2', 'Half the lightmap texels per metre', (v) => v / 2), scaleIn,
    h('span', { class: 'dim' }, 'Charts are packed by the Core; the density is what you control. Changing it re-bakes the faces.'))
  const modeUv = h('button', { class: 'active', title: 'Texture UV of the selected faces' }, 'UV'), modeLm = h('button', { title: 'Where the selected faces sit on the lightmap page' }, 'Lightmap')
  const setMode = (m: 'uv' | 'lightmap'): void => {
    mode = m; modeUv.classList.toggle('active', m === 'uv'); modeLm.classList.toggle('active', m === 'lightmap')
    toolbar.style.display = m === 'uv' ? '' : 'none'; lmBar.style.display = m === 'lightmap' ? '' : 'none'
    if (m === 'lightmap') { alignOn = false; alignBtn.classList.remove('active') }
    status.textContent = ''; refresh()
  }
  modeUv.addEventListener('click', () => setMode('uv')); modeLm.addEventListener('click', () => setMode('lightmap'))
  const modes = h('div', { class: 'fft-bar' }, modeUv, modeLm)

  // ---- window
  const closeBtn = h('button', { class: 'fft-x', title: 'Close (Shift+Q)' }, '×')
  const head = h('div', { class: 'fft-head' }, h('span', {}, 'Fast Texture Tool'), closeBtn)
  const root = h('div', { class: 'fft-win' }, head, h('div', { class: 'fft-body' }, modes, toolbar, lmBar, canvas, info, status,
    h('div', { class: 'dim' }, 'Drag: move · handles: scale (Shift on a corner = aspect, round handle = rotate, Shift = 15°) · wheel: zoom · middle drag: pan')))
  doc.body.append(root)
  let dx = 0, dy = 0, moving = false
  head.addEventListener('pointerdown', (e) => { if ((e.target as HTMLElement).closest('button')) return; const r = root.getBoundingClientRect(); dx = e.clientX - r.left; dy = e.clientY - r.top; moving = true; head.setPointerCapture?.(e.pointerId) })
  head.addEventListener('pointermove', (e) => { if (moving) { root.style.left = `${Math.max(0, e.clientX - dx)}px`; root.style.top = `${Math.max(0, e.clientY - dy)}px`; root.style.right = 'auto' } })
  head.addEventListener('pointerup', () => { moving = false })

  const unsub = store.subscribe(() => refresh())
  const onKey = (e: KeyboardEvent): void => { if (e.key === 'Escape' && drag) { e.preventDefault(); store.cancelDrag(); drag = null } }
  doc.addEventListener('keydown', onKey, true)
  const win = {
    root,
    close: () => { if (drag) { store.cancelDrag(); drag = null } unsub(); doc.removeEventListener('keydown', onKey, true); root.remove(); if (openWin === win) openWin = null },
  }
  closeBtn.addEventListener('click', win.close)
  openWin = win
  const off = textures.onChange(() => { if (root.isConnected) draw(); else off() })
  refresh(true)
}
