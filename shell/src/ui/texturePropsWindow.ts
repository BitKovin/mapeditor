// The Texture Properties window (REQUIREMENTS §19 A1-A6): a visual editor for one texture's pixels per meter and its repetition behaviour per axis.
// Everything can be set on a picture: the texture view (drag the split line, click the edge to anchor), the wall preview (drag its edges to resize it,
// drag the alternate texture to line the pattern up) and the mode buttons; the numbers are secondary. Changes apply live to every viewport and are
// written to Core (`<texture file>.json`) a moment after the last change. The preview is drawn from the same plan as the viewports (model/uvmesh.ts).
import { chooseHotspot, hotspotAt, hotspotSize, moveRect, rectFromCorners } from '../model/hotspots'
import { HEAT_RANGE, heatColor, heatLabel } from '../model/texelDensity'
import { buildPreview, dragAltOffset, pieceTransform, splitFromPointer } from '../model/texPreview'
import { AxisRepeat, DEFAULT_PPM, PPM_MAX, PPM_MIN, TexInfo, TextureProps, Transparency, TransparencyMode, naturalSize, resolvedTransparency } from '../model/texprops'
import type { TextureCache } from '../state/textures'
import { openDialog } from './dialog'
import { fmt, h } from './dom'

export interface PropsWindowEnv {
  textures: TextureCache
  listTextures: () => Promise<string[]>
  /** Writes the properties to Core. */
  save: (path: string, props: TextureProps) => Promise<void>
  /** The texture currently chosen in the browser (offered as the alternate texture with one click). */
  activeTexture: () => string
}

const clone = (p: TextureProps): TextureProps => JSON.parse(JSON.stringify(p))
const AXES = [{ key: 'u' as const, label: 'Horizontal', start: 'Left', end: 'Right' }, { key: 'v' as const, label: 'Vertical', start: 'Top', end: 'Bottom' }]
const PPM_PRESETS = [16, 32, 64, 128, 256]

/** Searchable thumbnail picker; resolves the chosen path or null. */
export function pickTexture(env: PropsWindowEnv, title: string, exclude?: string): Promise<string | null> {
  return new Promise((resolve) => {
    let chosen: string | null = null
    const d = openDialog(title, { width: 640, onClose: () => resolve(chosen) })
    const search = h('input', { placeholder: 'Search textures', class: 'search' })
    const grid = h('div', { class: 'thumbs', style: 'max-height:60vh;min-height:240px' })
    let all: string[] = []
    const render = () => {
      const q = search.value.toLowerCase()
      const shown = all.filter((p) => p !== exclude && p.toLowerCase().includes(q)).slice(0, 400)
      grid.replaceChildren(...shown.map((p) => {
        const c = h('canvas', { width: '84', height: '84' })
        const g = c.getContext('2d')
        const st = env.textures.get(p)
        if (g) { g.fillStyle = '#2a2a2a'; g.fillRect(0, 0, 84, 84); if (st.status === 'ready') g.drawImage(st.image.source as CanvasImageSource, 0, 0, 84, 84) }
        const tile = h('div', { class: 'tile', title: p }, c, h('div', { class: 'tile-name' }, p.split('/').pop() ?? p))
        tile.addEventListener('click', () => { chosen = p; d.close() })
        return tile
      }))
    }
    search.addEventListener('input', render)
    search.addEventListener('keydown', (e) => e.stopPropagation())
    d.body.append(search, grid)
    void env.listTextures().then((l) => { all = l; render() })
    search.focus()
  })
}

export function openTextureProperties(env: PropsWindowEnv, path: string): { close(): void } {
  const textures = env.textures
  const original = clone(textures.propsOf(path))
  let draft = clone(original)
  let wallW = 4, wallH = 5, mul = 1
  let heat = false, cuts = false
  /** hotspot editing: drawing / moving / resizing rectangles on the texture view */
  let hotMode = false, hotSel = -1
  let saveTimer: ReturnType<typeof setTimeout> | null = null
  let dirty = false

  const dlg = openDialog(`Texture properties: ${path}`, { width: 1040, onClose: () => { if (saveTimer) { clearTimeout(saveTimer); void flush() } } })
  dlg.root.querySelector('.modal')?.classList.add('texprops')
  const status = h('span', { class: 'dim' })

  const tex = (): TexInfo => {
    const base = textures.info(path)
    return { path, width: base?.width ?? 64, height: base?.height ?? 64, ppm: draft.pixelsPerMeter ?? textures.profilePpm, props: draft }
  }
  const lookup = (p: string): TexInfo | undefined => (p === path ? tex() : textures.info(p))

  const flush = async (): Promise<void> => {
    saveTimer = null
    if (!dirty) return
    dirty = false
    status.textContent = 'Saving...'
    try { await env.save(path, draft); status.textContent = 'Saved next to the texture' } catch (e) { status.textContent = `Could not save: ${(e as Error).message}` }
  }
  const commit = (): void => {
    textures.setProps(path, draft)
    dirty = true
    status.textContent = 'Unsaved...'
    if (saveTimer) clearTimeout(saveTimer)
    saveTimer = setTimeout(() => void flush(), 350)
  }

  // ---- canvases
  const TV = 300
  const texView = h('canvas', { width: String(TV), height: String(TV), class: 'tp-canvas' })
  const PV = 420
  const preview = h('canvas', { width: String(PV), height: String(PV), class: 'tp-canvas' })
  const info = h('div', { class: 'dim' }), warn = h('div', { class: 'tp-warn' })
  const legend = h('canvas', { width: '200', height: '12', class: 'tp-legend' })

  const texRect = (): { ox: number; oy: number; tw: number; th: number } => {
    const t = tex(), m = 22
    const s = Math.min((TV - 2 * m) / t.width, (TV - 2 * m) / t.height)
    const tw = t.width * s, th = t.height * s
    return { ox: (TV - tw) / 2, oy: (TV - th) / 2, tw, th }
  }

  const drawImage = (g: CanvasRenderingContext2D, p: string, x: number, y: number, w: number, hh: number): void => {
    const st = textures.get(p)
    g.fillStyle = '#2a2a2a'; g.fillRect(x, y, w, hh)
    if (st.status === 'ready') g.drawImage(st.image.source as CanvasImageSource, x, y, w, hh)
  }

  const drawTextureView = (): void => {
    const g = texView.getContext('2d')
    if (!g) return
    g.fillStyle = '#161616'; g.fillRect(0, 0, TV, TV)
    const { ox, oy, tw, th } = texRect()
    drawImage(g, path, ox, oy, tw, th)
    g.font = '11px sans-serif'
    for (const ax of AXES) {
      const a = draft.repeat[ax.key], horiz = ax.key === 'v'
      if (a.mode === 'repeat' || a.mode === 'mirror') continue
      // anchored edge: a bright bar with a pin
      g.fillStyle = '#0a84e8'
      if (horiz) g.fillRect(ox - 6, a.anchor === 'start' ? oy - 4 : oy + th, tw + 12, 4); else g.fillRect(a.anchor === 'start' ? ox - 4 : ox + tw, oy - 6, 4, th + 12)
      if (!a.alt) {
        const frac = a.split, open = a.anchor === 'end' ? 0 : 1 // the repeating band lies between the split and the open edge
        const lo = Math.min(frac, open), hi = Math.max(frac, open)
        g.fillStyle = 'rgba(240,160,48,0.30)'
        if (horiz) g.fillRect(ox, oy + lo * th, tw, (hi - lo) * th); else g.fillRect(ox + lo * tw, oy, (hi - lo) * tw, th)
        g.fillStyle = 'rgba(78,196,106,0.14)'
        const flo = Math.min(frac, 1 - open), fhi = Math.max(frac, 1 - open)
        if (horiz) g.fillRect(ox, oy + flo * th, tw, (fhi - flo) * th); else g.fillRect(ox + flo * tw, oy, (fhi - flo) * tw, th)
        // the split line with grips
        g.strokeStyle = '#f0a030'; g.lineWidth = 2; g.beginPath()
        if (horiz) { g.moveTo(ox - 10, oy + frac * th); g.lineTo(ox + tw + 10, oy + frac * th) } else { g.moveTo(ox + frac * tw, oy - 10); g.lineTo(ox + frac * tw, oy + th + 10) }
        g.stroke()
        g.fillStyle = '#f0a030'
        if (horiz) { g.fillRect(ox - 14, oy + frac * th - 5, 8, 10); g.fillRect(ox + tw + 6, oy + frac * th - 5, 8, 10) } else { g.fillRect(ox + frac * tw - 5, oy - 14, 10, 8); g.fillRect(ox + frac * tw - 5, oy + th + 6, 10, 8) }
        g.fillStyle = '#fff'
        const mid = (a2: number, b2: number) => (a2 + b2) / 2
        if (horiz) { g.fillText('repeats', ox + 4, oy + mid(lo, hi) * th + 4); g.fillText('fixed', ox + 4, oy + mid(flo, fhi) * th + 4) } else { g.fillText('repeats', ox + mid(lo, hi) * tw - 14, oy + 14); g.fillText('fixed', ox + mid(flo, fhi) * tw - 10, oy + 14) }
      } else {
        g.fillStyle = 'rgba(78,196,106,0.14)'; g.fillRect(ox, oy, tw, th)
        g.fillStyle = '#fff'; g.fillText(`fixed; continues with ${a.alt.texture.split('/').pop()}`, ox + 4, oy + th - 6)
      }
    }
    draft.hotspots.forEach((r, i) => {
      g.strokeStyle = i === hotSel ? '#ffd24a' : '#3ad0e0'; g.lineWidth = i === hotSel ? 2 : 1
      g.fillStyle = i === hotSel ? 'rgba(255,210,74,0.18)' : 'rgba(58,208,224,0.12)'
      g.fillRect(ox + r.x * tw, oy + r.y * th, r.w * tw, r.h * th); g.strokeRect(ox + r.x * tw, oy + r.y * th, r.w * tw, r.h * th)
      g.fillStyle = '#fff'; g.font = '11px sans-serif'; g.fillText(String(i + 1), ox + r.x * tw + 4, oy + r.y * th + 13)
      if (i === hotSel) { g.fillStyle = '#ffd24a'; g.fillRect(ox + (r.x + r.w) * tw - 5, oy + (r.y + r.h) * th - 5, 10, 10) }
    })
    g.strokeStyle = '#555'; g.lineWidth = 1; g.strokeRect(ox - 0.5, oy - 0.5, tw + 1, th + 1)
  }

  const wallBox = (): { x0: number; y0: number; s: number; w: number; hgt: number } => {
    const m = 26
    const s = Math.min((PV - 2 * m) / wallW, (PV - 2 * m) / wallH)
    const w = wallW * s, hgt = wallH * s
    return { x0: m, y0: PV - m - hgt, s, w, hgt }
  }

  const drawPreview = (): void => {
    const g = preview.getContext('2d')
    if (!g) return
    g.fillStyle = '#161616'; g.fillRect(0, 0, PV, PV)
    const t = tex()
    const pv = buildPreview(t, lookup, wallW, wallH, mul)
    const { x0, y0, s, w, hgt } = wallBox()
    const toCanvas = (p: [number, number]): [number, number] => [x0 + p[0] * s, y0 + hgt - p[1] * s]
    g.save(); g.beginPath(); g.rect(x0, y0, w, hgt); g.clip()
    for (const piece of pv.pieces) {
      const st = textures.get(piece.texture)
      const poly = piece.poly.map(toCanvas)
      g.beginPath(); poly.forEach((q, i) => (i ? g.lineTo(q[0], q[1]) : g.moveTo(q[0], q[1]))); g.closePath()
      const m = st.status === 'ready' ? pieceTransform(piece, toCanvas, st.image.width, st.image.height) : null
      if (st.status === 'ready' && m && typeof g.createPattern === 'function') {
        const pat = g.createPattern(st.image.source as CanvasImageSource, 'repeat')
        if (pat) {
          pat.setTransform(new DOMMatrix(m))
          g.fillStyle = pat
        } else g.fillStyle = '#333'
      } else g.fillStyle = '#333'
      g.fill()
    }
    if (heat) {
      // density of every region: pixels per metre it shows against the pixels per meter it was authored for
      const e = -Math.log2(mul)
      const c = heatColor(e)
      g.fillStyle = `rgba(${Math.round(c[0] * 255)},${Math.round(c[1] * 255)},${Math.round(c[2] * 255)},0.55)`
      g.fillRect(x0, y0, w, hgt)
    }
    if (cuts) {
      // the generated invisible vertices: the cut lines and their end points
      g.strokeStyle = 'rgba(255,255,255,0.55)'; g.lineWidth = 1
      g.fillStyle = '#ffd24a'
      for (const piece of pv.pieces) {
        const poly = piece.poly.map(toCanvas)
        g.beginPath(); poly.forEach((q, i) => (i ? g.lineTo(q[0], q[1]) : g.moveTo(q[0], q[1]))); g.closePath(); g.stroke()
        for (const q of poly) g.fillRect(q[0] - 2, q[1] - 2, 4, 4)
      }
    }
    g.restore()
    // joint line between the first texture and the continuation, ruler, frame
    const nat = naturalSize(t)
    const v = draft.repeat.v, u = draft.repeat.u
    g.setLineDash([5, 4]); g.strokeStyle = '#0a84e8'; g.lineWidth = 1
    if (v.mode === 'anchor') { const jy = v.anchor === 'end' ? y0 + hgt - nat[1] * mul * s : y0 + nat[1] * mul * s; if (jy > y0 && jy < y0 + hgt) { g.beginPath(); g.moveTo(x0, jy); g.lineTo(x0 + w, jy); g.stroke() } }
    if (u.mode === 'anchor') { const jx = u.anchor === 'start' ? x0 + nat[0] * mul * s : x0 + w - nat[0] * mul * s; if (jx > x0 && jx < x0 + w) { g.beginPath(); g.moveTo(jx, y0); g.lineTo(jx, y0 + hgt); g.stroke() } }
    g.setLineDash([])
    g.strokeStyle = '#888'; g.strokeRect(x0 - 0.5, y0 - 0.5, w + 1, hgt + 1)
    g.fillStyle = '#9a9a9a'; g.font = '10px sans-serif'
    for (let m = 0; m <= wallW; m++) { g.fillRect(x0 + m * s, y0 + hgt, 1, m % 5 === 0 ? 7 : 4); if (m % 2 === 0) g.fillText(`${m}`, x0 + m * s - 3, y0 + hgt + 17) }
    for (let m = 0; m <= wallH; m++) { g.fillRect(x0 - (m % 5 === 0 ? 7 : 4), y0 + hgt - m * s, m % 5 === 0 ? 7 : 4, 1); if (m % 2 === 0) g.fillText(`${m}`, 3, y0 + hgt - m * s + 3) }
    g.fillStyle = '#0a84e8'; g.fillRect(x0 + w, y0, 5, hgt); g.fillRect(x0, y0 - 5, w, 5) // resize grips
    warn.textContent = !pv.cut && (u.mode !== 'repeat' || v.mode !== 'repeat') ? 'Too many pieces for this wall size: drawn as a plain repeat.' : ''
    const nsz = naturalSize(t)
    const hi = chooseHotspot(draft.hotspots, t, [wallW, wallH])
    info.textContent = (hi >= 0 ? `Hotspot for this wall: ${hi + 1}. ` : '') + `${t.width} x ${t.height} px at ${fmt(t.ppm)} px/m = ${fmt(nsz[0])} x ${fmt(nsz[1])} m per repeat. ${heat ? `Heat map: ${heatLabel(-Math.log2(mul))}.` : ''} ${pv.pieces.length} pieces.`
    // legend
    const lg = legend.getContext('2d')
    if (lg) for (let x = 0; x < 200; x++) { const c = heatColor(((x / 199) * 2 - 1) * -HEAT_RANGE); lg.fillStyle = `rgb(${c[0] * 255 | 0},${c[1] * 255 | 0},${c[2] * 255 | 0})`; lg.fillRect(x, 0, 1, 12) }
    legend.style.display = heat ? 'block' : 'none'
  }

  const redraw = (): void => { drawTextureView(); drawPreview() }

  // ---- texture view interaction
  type Drag = { kind: 'split'; axis: 'u' | 'v' } | null
  let drag: Drag = null
  const texPos = (e: PointerEvent): [number, number] => { const r = texView.getBoundingClientRect(); return [((e.clientX - r.left) / r.width) * TV, ((e.clientY - r.top) / r.height) * TV] }
  const hitTexView = (x: number, y: number): { kind: 'split'; axis: 'u' | 'v' } | { kind: 'edge'; axis: 'u' | 'v'; side: 'start' | 'end' } | null => {
    const { ox, oy, tw, th } = texRect()
    for (const ax of AXES) {
      const a = draft.repeat[ax.key]
      if (a.mode !== 'anchor') continue
      const horiz = ax.key === 'v'
      if (!a.alt) {
        const line = horiz ? oy + a.split * th : ox + a.split * tw
        if (horiz ? Math.abs(y - line) <= 7 && x > ox - 18 && x < ox + tw + 18 : Math.abs(x - line) <= 7 && y > oy - 18 && y < oy + th + 18) return { kind: 'split', axis: ax.key }
      }
      const frac = horiz ? (y - oy) / th : (x - ox) / tw, cross = horiz ? (x - ox) / tw : (y - oy) / th
      if (cross >= -0.05 && cross <= 1.05) { if (Math.abs(frac) <= 0.06) return { kind: 'edge', axis: ax.key, side: 'start' }; if (Math.abs(frac - 1) <= 0.06) return { kind: 'edge', axis: ax.key, side: 'end' } }
    }
    return null
  }
  type HDrag = { kind: 'new'; ax: number; ay: number } | { kind: 'move'; i: number; lx: number; ly: number } | { kind: 'resize'; i: number } | null
  let hdrag: HDrag = null
  const frac = (e: PointerEvent): [number, number] => { const [x, y] = texPos(e); const { ox, oy, tw, th } = texRect(); return [(x - ox) / tw, (y - oy) / th] }
  texView.addEventListener('pointermove', (e) => {
    const [x, y] = texPos(e)
    if (hotMode) {
      const [fx, fy] = frac(e)
      if (hdrag?.kind === 'new') { draft.hotspots[hotSel] = rectFromCorners(hdrag.ax, hdrag.ay, fx, fy); commit(); redraw() }
      else if (hdrag?.kind === 'move') { draft.hotspots[hdrag.i] = moveRect(draft.hotspots[hdrag.i], fx - hdrag.lx, fy - hdrag.ly); hdrag.lx = fx; hdrag.ly = fy; commit(); redraw() }
      else if (hdrag?.kind === 'resize') { const r = draft.hotspots[hdrag.i]; draft.hotspots[hdrag.i] = rectFromCorners(r.x, r.y, fx, fy); commit(); redraw() }
      else texView.style.cursor = 'crosshair'
      return
    }
    if (drag) {
      const { ox, oy, tw, th } = texRect()
      const a = draft.repeat[drag.axis]
      a.split = splitFromPointer(drag.axis === 'v' ? (y - oy) / th : (x - ox) / tw)
      commit(); redraw()
      return
    }
    const hit = hitTexView(x, y)
    texView.style.cursor = !hit ? 'default' : hit.kind === 'edge' ? 'pointer' : hit.axis === 'v' ? 'ns-resize' : 'ew-resize'
  })
  texView.addEventListener('pointerdown', (e) => {
    if (hotMode) {
      const [fx, fy] = frac(e), r = draft.hotspots[hotSel]
      const { tw, th } = texRect()
      if (r && Math.abs((r.x + r.w - fx) * tw) < 9 && Math.abs((r.y + r.h - fy) * th) < 9) hdrag = { kind: 'resize', i: hotSel }
      else { const i = hotspotAt(draft.hotspots, fx, fy); if (i >= 0) { hotSel = i; hdrag = { kind: 'move', i, lx: fx, ly: fy } } else if (draft.hotspots.length < 64) { draft.hotspots.push(rectFromCorners(fx, fy, fx, fy)); hotSel = draft.hotspots.length - 1; hdrag = { kind: 'new', ax: fx, ay: fy } } }
      texView.setPointerCapture?.(e.pointerId); hotList(); redraw()
      return
    }
    const [x, y] = texPos(e), hit = hitTexView(x, y)
    if (!hit) return
    if (hit.kind === 'edge') { draft.repeat[hit.axis].anchor = hit.side; commit(); rebuildControls(); redraw(); return }
    drag = hit; texView.setPointerCapture?.(e.pointerId)
  })
  const endDrag = () => { drag = null; if (hdrag) { hdrag = null; hotList() } }
  texView.addEventListener('pointerup', endDrag); texView.addEventListener('pointercancel', endDrag)

  // ---- preview interaction: drag the wall's edges to resize it, drag the alternate texture to move its pattern
  type PDrag = { kind: 'w' } | { kind: 'h' } | { kind: 'alt'; axis: 'u' | 'v'; lastX: number; lastY: number } | null
  let pdrag: PDrag = null
  const pvPos = (e: PointerEvent): [number, number] => { const r = preview.getBoundingClientRect(); return [((e.clientX - r.left) / r.width) * PV, ((e.clientY - r.top) / r.height) * PV] }
  const altAt = (x: number, y: number): 'u' | 'v' | null => {
    const { x0, y0, s, hgt } = wallBox()
    const wx = (x - x0) / s, wy = (y0 + hgt - y) / s
    const pv = buildPreview(tex(), lookup, wallW, wallH, mul)
    for (const piece of pv.pieces) {
      if (piece.texture === path) continue
      let inside = true
      for (let i = 0; i < piece.poly.length; i++) { const a = piece.poly[i], b = piece.poly[(i + 1) % piece.poly.length]; if ((b[0] - a[0]) * (wy - a[1]) - (b[1] - a[1]) * (wx - a[0]) < -1e-9) { inside = false; break } }
      if (inside) return draft.repeat.v.alt?.texture === piece.texture ? 'v' : draft.repeat.u.alt?.texture === piece.texture ? 'u' : null
    }
    return null
  }
  preview.addEventListener('pointerdown', (e) => {
    const [x, y] = pvPos(e), { x0, y0, w, hgt } = wallBox()
    if (Math.abs(x - (x0 + w)) <= 8 && y >= y0 - 8 && y <= y0 + hgt) pdrag = { kind: 'w' }
    else if (Math.abs(y - y0) <= 8 && x >= x0 && x <= x0 + w + 8) pdrag = { kind: 'h' }
    else { const a = altAt(x, y); if (a) pdrag = { kind: 'alt', axis: a, lastX: x, lastY: y } }
    if (pdrag) preview.setPointerCapture?.(e.pointerId)
  })
  preview.addEventListener('pointermove', (e) => {
    const [x, y] = pvPos(e), { x0, y0, s, w, hgt } = wallBox()
    if (!pdrag) {
      preview.style.cursor = Math.abs(x - (x0 + w)) <= 8 ? 'ew-resize' : Math.abs(y - y0) <= 8 ? 'ns-resize' : altAt(x, y) ? 'move' : 'default'
      return
    }
    const d = pdrag
    if (d.kind === 'w') { wallW = Math.min(16, Math.max(0.5, Math.round(((x - x0) / s) * 20) / 20)); sizeInputs() }
    else if (d.kind === 'h') { wallH = Math.min(16, Math.max(0.5, Math.round(((y0 + hgt - y) / s) * 20) / 20)); sizeInputs() }
    else {
      const a = draft.repeat[d.axis].alt, B = a ? textures.info(a.texture) : undefined
      if (a && B) { a.offset = dragAltOffset(a.offset, (x - d.lastX) / s, -(y - d.lastY) / s, B, mul); commit() }
      d.lastX = x; d.lastY = y
    }
    redraw()
  })
  const endP = () => { pdrag = null }
  preview.addEventListener('pointerup', endP); preview.addEventListener('pointercancel', endP)

  // ---- controls
  const seg = <T extends string>(opts: { value: T; label: string; title: string }[], cur: T, on: (v: T) => void): HTMLElement => {
    const row = h('div', { class: 'seg' })
    for (const o of opts) {
      const b = h('button', { title: o.title, class: o.value === cur ? 'on' : '' }, o.label)
      b.addEventListener('click', () => on(o.value))
      row.append(b)
    }
    return row
  }
  const numInput = (value: number, step: string, on: (v: number) => void, min?: string, max?: string): HTMLInputElement => {
    const i = h('input', { type: 'number', step, value: fmt(value), ...(min ? { min } : {}), ...(max ? { max } : {}) })
    i.addEventListener('change', () => { const v = Number(i.value); if (Number.isFinite(v)) on(v) })
    i.addEventListener('keydown', (e) => e.stopPropagation())
    return i
  }

  const controls = h('div', { class: 'tp-controls' })
  const axisBlock = (ax: (typeof AXES)[number]): HTMLElement => {
    const a: AxisRepeat = draft.repeat[ax.key]
    const set = (patch: Partial<AxisRepeat>) => { draft.repeat[ax.key] = { ...a, ...patch }; commit(); rebuildControls(); redraw() }
    const box = h('div', { class: 'tp-axis' }, h('div', { class: 'section' }, `${ax.label} repetition`))
    box.append(seg([
      { value: 'repeat' as const, label: 'Repeat', title: 'Plain tiling' },
      { value: 'mirror' as const, label: 'Mirror', title: 'Every second tile is flipped' },
      { value: 'anchor' as const, label: 'Anchor', title: `Keep one edge of the texture glued to the same edge of the face; the rest repeats` },
    ], a.mode, (mode) => set({ mode })))
    const seam = textures.info(path)?.seamless?.[ax.key === 'u' ? 0 : 1]
    box.append(h('div', { class: 'tp-row' }, h('span', {}, 'Edges'), seg([
      { value: 'auto' as const, label: 'Auto', title: 'Decided from the pixels: do the opposite edges of the texture continue each other?' },
      { value: 'wrap' as const, label: 'Wrap', title: 'The opposite edges continue each other (a seamless tile): the filter may blend across them' },
      { value: 'clamp' as const, label: 'Clamp', title: 'The edges are borders: the filter never blends the opposite edge in (no line at tile boundaries). Faces are cut at tile boundaries.' },
    ], a.edge, (edge) => set({ edge })), a.edge === 'auto' ? h('span', { class: 'dim', title: 'Result of the pixel analysis' }, seam === undefined ? '(not analysed)' : seam ? '(seamless)' : '(edges differ: clamped)') : h('span')))
    if (a.mode === 'anchor') {
      box.append(
        h('div', { class: 'tp-row' }, h('span', {}, 'Keep fixed'), seg([
          { value: 'start' as const, label: ax.start, title: `${ax.start} edge of the texture stays at the ${ax.start.toLowerCase()} edge of the face` },
          { value: 'end' as const, label: ax.end, title: `${ax.end} edge of the texture stays at the ${ax.end.toLowerCase()} edge of the face` },
        ], a.anchor, (anchor) => set({ anchor }))),
        h('div', { class: 'tp-row' }, h('span', {}, 'Fill with'), seg([
          { value: 'mirror' as const, label: 'Mirrored', title: 'The repeating part is mirrored at every tile (no visible seam)' },
          { value: 'repeat' as const, label: 'Repeated', title: 'The repeating part is tiled as it is' },
        ], a.fill, (fill) => set({ fill }))),
      )
      const altRow = h('div', { class: 'tp-row' }, h('span', {}, 'Repeating part'))
      const choose = h('button', { title: 'Take the repeating part from another texture (a seamless tile that continues this one)' }, a.alt ? a.alt.texture : 'Own band (no other texture)')
      choose.addEventListener('click', () => {
        void pickTexture(env, 'Texture for the repeating part', path).then((p) => { if (p) set({ alt: { texture: p, offset: a.alt?.offset ?? [0, 0] } }) })
      })
      altRow.append(choose)
      if (a.alt) {
        const clear = h('button', { title: 'Back to repeating the texture\'s own band' }, 'Clear'); clear.addEventListener('click', () => set({ alt: null }))
        altRow.append(clear)
      } else {
        const use = h('button', { title: 'Use the texture selected in the Content Browser' }, 'Use browser selection')
        use.addEventListener('click', () => { const p = env.activeTexture(); if (p && p !== path) set({ alt: { texture: p, offset: [0, 0] } }); else status.textContent = 'Select another texture in the Content Browser first' })
        altRow.append(use)
      }
      box.append(altRow)
      if (a.alt) {
        const o = a.alt.offset
        const off = (i: 0 | 1) => numInput(o[i], '0.01', (v) => { const n: [number, number] = [o[0], o[1]]; n[i] = v; set({ alt: { texture: a.alt!.texture, offset: n } }) })
        box.append(h('div', { class: 'tp-row' }, h('span', {}, 'Phase (repeats)'), off(0), off(1)), h('div', { class: 'dim' }, 'Drag the continuation in the preview to line the pattern up. Its own pixels per meter sets its size.'))
        if (!textures.info(a.alt.texture)) box.append(h('div', { class: 'tp-warn' }, 'This texture is not loaded (missing?). The texture\'s own band is used meanwhile.'))
      }
    }
    return box
  }

  const hotBox = h('div', { class: 'tp-hot' })
  const hotList = (): void => {
    const mode = h('input', { type: 'checkbox' }); mode.checked = hotMode
    mode.addEventListener('change', () => { hotMode = mode.checked; hotSel = hotMode ? hotSel : -1; hotList(); redraw() })
    const del = h('button', { title: 'Delete the selected hotspot' }, 'Delete')
    if (hotSel < 0 || !draft.hotspots[hotSel]) del.setAttribute('disabled', '')
    del.addEventListener('click', () => { draft.hotspots.splice(hotSel, 1); hotSel = -1; commit(); hotList(); redraw() })
    const t = tex()
    const rows = draft.hotspots.map((r, i) => { const [w, hh] = hotspotSize(r, t); const row = h('div', { class: i === hotSel ? 'tp-hrow on' : 'tp-hrow' }, `${i + 1}: ${Math.round(r.w * t.width)} x ${Math.round(r.h * t.height)} px (${fmt(w)} x ${fmt(hh)} m)`); row.addEventListener('click', () => { hotSel = i; hotList(); redraw() }); return row })
    hotBox.replaceChildren(
      h('div', { class: 'section' }, 'Hotspots'),
      h('label', { class: 'tp-row' }, mode, ' Edit: drag on the texture to draw, drag to move, drag the corner to resize'),
      ...rows, del,
      h('div', { class: 'dim' }, 'A face picks the hotspot that fits its size best and shows exactly that rectangle (Surface Inspector > Hotspot).'))
  }
  hotList()

  // Transparency (REQUIREMENTS §23): how light and the views pass this texture
  const transpBox = h('div', { class: 'tp-transp' })
  const buildTransp = (): void => {
    const t = draft.transparency, info = textures.info(path)
    const set = (patch: Partial<Transparency>) => { draft.transparency = { ...draft.transparency, ...patch }; commit(); buildTransp(); redraw() }
    const unit = (label: string, v: number, key: 'cutoff' | 'opacity' | 'tint', title: string) =>
      h('label', { class: 'tp-row', title }, h('span', {}, label), numInput(v, '0.05', (x) => set({ [key]: Math.min(1, Math.max(0, x)) } as Partial<Transparency>), '0', '1'))
    const alpha = info?.alpha ?? 'none', now = resolvedTransparency(t, alpha), autoGlass = t.mode === 'auto' && now === 'translucent'
    transpBox.replaceChildren(
      h('div', { class: 'section' }, 'Transparency'),
      seg<TransparencyMode>([
        { value: 'auto', label: 'Auto', title: 'By the image: half transparent alpha = glass, on / off alpha = cutout, no alpha = opaque' },
        { value: 'opaque', label: 'Opaque', title: 'Alpha is ignored: light never passes' },
        { value: 'cutout', label: 'Cutout', title: 'Texels below the cutoff are holes (fences, grates, leaves): light passes there' },
        { value: 'translucent', label: 'Translucent', title: 'Light passes, dimmed by the opacity and tinted by the texture colour (glass, cloth)' },
      ], t.mode, (mode) => set({ mode })),
      ...(now === 'cutout' ? [unit('Cutoff', t.cutoff, 'cutoff', 'Alpha below this is a hole')] : []),
      ...(now === 'translucent' ? [...(autoGlass ? [] : [unit('Opacity', t.opacity, 'opacity', '0 = clear, 1 = blocks all light (times the texel alpha)')]), unit('Tint', t.tint, 'tint', '0 = white light, 1 = light takes the colour of the glass')] : []),
      h('div', { class: 'dim' }, now === 'opaque' ? (t.mode === 'cutout' ? 'This image has no transparent texels: nothing to cut out, it stays opaque.' : 'Opaque: casts full shadows.')
        : now === 'cutout' ? 'Cutout: light and bounce rays pass through the holes; the views do not draw them.'
        : autoGlass ? 'Glass (the image has half transparent alpha): each texel lets through 1 - alpha of the light, in its own colour. Coloured glass casts coloured light; the views draw it see-through.'
        : 'Translucent: light passes dimmed by the opacity (times the texel alpha) and coloured by the texture; the views draw the face see-through. A pane counts once (where light enters).'),
    )
  }
  buildTransp()

  const ppmRow = h('div', { class: 'tp-ppm' })
  const buildPpm = (): void => {
    const t = tex()
    const input = numInput(t.ppm, '1', (v) => { draft.pixelsPerMeter = Math.min(PPM_MAX, Math.max(PPM_MIN, v)); commit(); buildPpm(); redraw() }, String(PPM_MIN), String(PPM_MAX))
    const slider = h('input', { type: 'range', min: '0', max: '1000', value: String(Math.round((Math.log(t.ppm / PPM_MIN) / Math.log(PPM_MAX / PPM_MIN)) * 1000)) })
    slider.addEventListener('input', () => { draft.pixelsPerMeter = Math.round(PPM_MIN * Math.pow(PPM_MAX / PPM_MIN, Number(slider.value) / 1000)); commit(); input.value = fmt(draft.pixelsPerMeter); redraw() })
    slider.addEventListener('change', () => buildPpm())
    const presets = h('div', { class: 'btns' }, ...PPM_PRESETS.map((v) => { const b = h('button', { class: t.ppm === v ? 'on' : '' }, String(v)); b.addEventListener('click', () => { draft.pixelsPerMeter = v; commit(); buildPpm(); redraw() }); return b }))
    const def = h('button', { title: 'Use the game profile\'s value' }, `Profile default (${fmt(textures.profilePpm || DEFAULT_PPM)})`)
    def.addEventListener('click', () => { draft.pixelsPerMeter = null; commit(); buildPpm(); redraw() })
    const nat = naturalSize(t)
    ppmRow.replaceChildren(
      h('div', { class: 'section' }, 'Pixels per meter'),
      h('div', { class: 'tp-row' }, input, slider),
      presets,
      def,
      h('div', { class: 'dim' }, `One repeat is ${fmt(nat[0])} x ${fmt(nat[1])} m. Faces without their own scale show the texture at this size; the heat map compares against it.`),
    )
  }

  const sizeW = numInput(wallW, '0.5', (v) => { wallW = Math.min(16, Math.max(0.5, v)); redraw() }), sizeH = numInput(wallH, '0.5', (v) => { wallH = Math.min(16, Math.max(0.5, v)); redraw() })
  const sizeInputs = () => { sizeW.value = fmt(wallW); sizeH.value = fmt(wallH) }
  const mulInput = numInput(mul, '0.25', (v) => { mul = Math.min(8, Math.max(0.125, v)); redraw() })
  const heatBox = h('input', { type: 'checkbox' }), cutBox = h('input', { type: 'checkbox' })
  heatBox.addEventListener('change', () => { heat = heatBox.checked; redraw() })
  cutBox.addEventListener('change', () => { cuts = cutBox.checked; redraw() })

  const rebuildControls = (): void => { controls.replaceChildren(axisBlock(AXES[0]), axisBlock(AXES[1])) }

  const revert = h('button', { title: 'Back to the values this window opened with' }, 'Revert')
  revert.addEventListener('click', () => { draft = clone(original); hotSel = -1; commit(); rebuildControls(); buildPpm(); hotList(); buildTransp(); redraw() })
  const done = h('button', { class: 'primary' }, 'Close')
  done.addEventListener('click', () => dlg.close())

  rebuildControls(); buildPpm()
  dlg.body.append(
    h('div', { class: 'tp-grid' },
      h('div', { class: 'tp-left' },
        h('div', { class: 'section' }, 'Texture'), texView,
        h('div', { class: 'dim' }, 'Anchor: drag the orange line to choose which part repeats; click an edge to choose which edge stays fixed.'),
        ppmRow, transpBox, hotBox),
      h('div', { class: 'tp-mid' }, controls),
      h('div', { class: 'tp-right' },
        h('div', { class: 'section' }, 'Wall preview'), preview, warn, info,
        h('div', { class: 'tp-row' }, h('span', {}, 'Wall (m)'), sizeW, sizeH, h('span', {}, 'Scale x'), mulInput),
        h('div', { class: 'tp-row' }, h('label', {}, heatBox, ' Heat map'), h('label', {}, cutBox, ' Show cuts (invisible vertices)')), legend,
        h('div', { class: 'dim' }, 'Drag the blue grips to resize the wall. The picture is built from the same UVs the viewports use.'))),
    h('div', { class: 'modal-buttons' }, status, revert, done))
  redraw()
  textures.get(path)
  const unsub = textures.onChange(() => { if (dlg.root.isConnected) redraw(); else unsub() })
  return { close: () => dlg.close() }
}
