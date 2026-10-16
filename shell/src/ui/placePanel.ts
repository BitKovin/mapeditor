import type { IContentRenderer } from 'dockview-core'
import type { EntityClassDef } from '../net/protocol'
import { addPointEntity, spawnInstance } from '../state/actions'
import type { DirEntry } from '../net/protocol'
import { ENTITY_DRAG, PREFAB_DRAG, beginDrag, endDrag } from './dragTypes'
import { boxSize, originOnSurface } from '../model/entityBox'
import { isModernLight } from '../model/lights'
import { entityIcon } from '../render/icons'
import type { PrefabIcons } from '../render/icons'
import type { PrefabPreviews } from '../state/prefabPreview'
import type { Store } from '../state/store'
import type { ShapeId, Turn } from '../model/shapes'
import { BRUSH_TOOLS, ENTITIES, LIGHTS, MESH_TOOLS, SHAPES } from './bindings'
import { h } from './dom'
import { tabbed } from './tabs'

const card = (label: string, sub: string, enabled: boolean, onClick: () => void): HTMLElement => {
  const b = h('button', { class: `card${enabled ? '' : ' off'}`, title: enabled ? label : `${label} (coming later)` }, h('span', { class: 'card-title' }, label), h('span', { class: 'card-sub' }, sub))
  if (!enabled) b.setAttribute('disabled', '')
  b.addEventListener('click', onClick)
  return b
}

/** Preview services for the Place tab: icons of prefabs and the loader they draw from. */
export interface PlaceServices { prefabIcons: PrefabIcons; previews: PrefabPreviews }

/** "Place" tab (UE's Place Actors): shapes, entities, lights. */
export class PlacePanel implements IContentRenderer {
  readonly element = h('div', { class: 'panel' })
  private unsub: (() => void) | null = null
  private shownDefs: EntityClassDef[] | null = null
  private shownShape = ''
  private filter = ''
  private prefabFiles: string[] | null = null
  private prefabLoading = false
  private redrawPrefabs: (() => void) | null = null
  private forgetIcons: (() => void)[] = []
  constructor(private store: Store, private targetPoint: () => [number, number, number], private listDir?: (dir: string) => Promise<{ path: string; entries: DirEntry[] }>, private services?: PlaceServices) {}

  init(): void {
    this.build()
    // the definitions arrive after connecting (and change when the assets folder or the .ent file does)
    this.unsub = this.store.subscribe(() => { if (this.store.state.entityDefs !== this.shownDefs || this.store.state.shape !== this.shownShape) this.build() })
  }
  dispose(): void { this.unsub?.() }

  private num(label: string, value: number, min: number, max: number, step: number, on: (v: number) => void): HTMLElement {
    const inp = h('input', { type: 'number', min: String(min), max: String(max), step: String(step), value: String(value) })
    inp.addEventListener('change', () => { const v = Number(inp.value); if (Number.isFinite(v)) on(Math.min(max, Math.max(min, v))) })
    inp.addEventListener('keydown', (e) => e.stopPropagation())
    return h('label', { class: 'field' }, h('span', {}, label), inp)
  }

  /** Parameters of the shape the Box tool currently draws. */
  private shapeParams(): HTMLElement {
    const p = this.store.state.shapeParams, shape = this.store.state.shape
    const rows: HTMLElement[] = []
    if (shape === 'cylinder') rows.push(this.num('Sides', p.sides, 3, 64, 1, (v) => this.store.setShapeParams({ sides: Math.round(v) })))
    if (shape === 'stairs') rows.push(this.num('Steps', p.steps, 2, 64, 1, (v) => this.store.setShapeParams({ steps: Math.round(v) })))
    if (shape === 'arch') rows.push(this.num('Segments', p.archSegments, 3, 64, 1, (v) => this.store.setShapeParams({ archSegments: Math.round(v) })), this.num('Wall thickness (m)', p.archThickness, 0.05, 20, 0.05, (v) => this.store.setShapeParams({ archThickness: v })))
    if (shape !== 'box') {
      const turn = h('select', { title: 'Which way the shape faces in the box you draw (quarter turns about its height); editable later in Details' }, ...[0, 1, 2, 3].map((t) => h('option', { value: String(t) }, `${t * 90}°`)))
      turn.value = String(p.turn)
      turn.addEventListener('change', () => this.store.setShapeParams({ turn: Number(turn.value) as Turn }))
      rows.push(h('label', { class: 'field' }, h('span', {}, 'Orientation'), turn))
    }
    return h('div', { class: 'pad' }, h('div', { class: 'dim' }, `Current shape: ${shape}`), ...rows,
      h('div', { class: 'dim' }, (shape === 'stairs' || shape === 'arch' ? 'Builds several convex brushes, so it can live in worldspawn. ' : '') + (shape !== 'box' ? 'The shape stays editable in Details (parameters, size, orientation).' : '')))
  }

  /** Decal tool: size of the projector placed by clicking a surface; the texture is the one chosen in the browser. */
  private decalPanel(): HTMLElement {
    const d = this.store.state.decalParams
    const go = h('button', { class: 'primary' }, 'Decal tool')
    go.addEventListener('click', () => { this.store.setTool('decal'); this.store.setMessage('Decal tool: click a surface. The texture is the one selected in the Content Browser (use one with an alpha channel).') })
    return h('div', { class: 'pad' }, go,
      this.num('Width (m)', d.width, 0.05, 50, 0.05, (v) => this.store.setDecalParams({ width: v })),
      this.num('Height (m)', d.height, 0.05, 50, 0.05, (v) => this.store.setDecalParams({ height: v })),
      this.num('Reach / depth (m)', d.depth, 0.05, 10, 0.05, (v) => this.store.setDecalParams({ depth: v })),
      h('div', { class: 'dim' }, 'A decal is a projector entity: move, rotate or delete it like any entity; the surface it paints is computed by Core, follows carving and prefabs, and is owned by the entity it lands on.'))
  }

  /** Every `.map.json` below the profile's prefab root (listed once, refreshed by the button). */
  private async loadPrefabs(): Promise<void> {
    const root = this.store.state.profile?.prefabRoot
    if (!this.listDir || root === undefined || this.prefabLoading) return
    this.prefabLoading = true
    const found: string[] = []
    const walk = async (dir: string, depth: number): Promise<void> => {
      let r: { path: string; entries: DirEntry[] }
      try { r = await this.listDir!(dir) } catch { return }
      for (const e of r.entries) {
        const path = dir ? `${dir.replace(/\/$/, '')}/${e.name}` : e.name
        if (e.dir) { if (depth < 5) await walk(path, depth + 1) }
        else if (e.name.toLowerCase().endsWith('.map.json')) found.push(path)
      }
    }
    await walk(root, 0)
    this.prefabFiles = found.sort()
    this.prefabLoading = false
    this.redrawPrefabs?.() // only the list: rebuilding the whole panel would jump back to the first tab
  }

  /** Prefab files: click adds an instance at the camera target, dragging one into a viewport drops it under the cursor. */
  private prefabPanel(): HTMLElement {
    if (this.prefabFiles === null) void this.loadPrefabs()
    const rows = h('div', { class: 'toolrows' })
    const input = h('input', { class: 'filter', placeholder: 'Filter prefabs…', value: this.filter })
    const refresh = h('button', { title: 'Rescan the prefab folder' }, '↻')
    refresh.addEventListener('click', () => { this.services?.prefabIcons.clear(); this.services?.previews.clear(); this.prefabFiles = null; void this.loadPrefabs() })
    const root = (this.store.state.profile?.prefabRoot ?? '').replace(/\/$/, '')
    const draw = () => {
      const q = this.filter.trim().toLowerCase()
      const files = (this.prefabFiles ?? []).filter((f) => !q || f.toLowerCase().includes(q))
      for (const f of this.forgetIcons.splice(0)) f()
      rows.replaceChildren(...(files.length ? files.map((f) => {
        const label = root && f.startsWith(root + '/') ? f.slice(root.length + 1) : f
        const img = h('img', { class: 'thumb', width: '40', height: '40', alt: '' })
        this.showPrefabIcon(f, img)
        const b = h('button', { class: 'toolrow withicon', title: `${f}\nClick to add, or drag into a viewport`, draggable: 'true' }, img, h('span', { class: 'rowtext' }, label.replace(/\.map\.json$/i, '')))
        b.addEventListener('click', () => spawnInstance(this.store, f, this.targetPoint()))
        this.makeDraggable(b, img, { kind: 'prefab', file: f })
        return b
      }) : [h('div', { class: 'dim pad' }, this.prefabFiles === null ? 'Looking for prefabs…' : `No *.map.json files under '${root}'.`)]))
    }
    input.addEventListener('input', () => { this.filter = input.value; draw() })
    input.addEventListener('keydown', (e) => e.stopPropagation())
    this.redrawPrefabs = draw
    draw()
    return h('div', {}, h('div', { class: 'row-split' }, input, refresh), h('div', { class: 'pad-top' }), rows)
  }

  /** A click adds an entity at the camera target, resting on the ground like a dropped one. */
  private at(classname: string): [number, number, number] { return originOnSurface(this.targetPoint(), [0, 1, 0], this.store.entityBox(classname)) }

  /** Puts the prefab's picture into `img` as soon as it exists (it is drawn from the prefab's real content). */
  private showPrefabIcon(file: string, img: HTMLImageElement): void {
    const icons = this.services?.prefabIcons
    if (!icons) return
    const cb = (u: string | null) => { if (u) img.src = u; else img.classList.add('none') }
    const now = icons.get(file, cb)
    if (now) img.src = now
    this.forgetIcons.push(() => icons.forget(file, cb))
  }

  /** The same drag for prefabs and entities: the viewport shows a half-transparent preview while it is over it, and places it where it was dropped. */
  private makeDraggable(el: HTMLElement, img: HTMLImageElement | null, item: Parameters<typeof beginDrag>[0]): void {
    el.addEventListener('dragstart', (e) => {
      const dt = e.dataTransfer
      if (dt) {
        dt.setData(item.kind === 'prefab' ? PREFAB_DRAG : ENTITY_DRAG, item.kind === 'prefab' ? item.file : item.classname)
        dt.effectAllowed = 'copy'
        if (img?.src && img.complete) dt.setDragImage(img, 20, 20)
      }
      beginDrag(item)
    })
    el.addEventListener('dragend', () => endDrag())
  }

  /** One placeable point entity: a preview icon, its colour and name, its size in metres; click to add, drag into a viewport to place. */
  private entityRow(classname: string, label: string, title: string, keyvalues?: Record<string, string>): HTMLElement {
    const box = this.store.entityBox(classname), color = this.store.entityColor(classname)
    const img = h('img', { class: 'thumb', width: '40', height: '40', alt: '' })
    const url = entityIcon(classname, box, color)
    if (url) img.src = url; else img.classList.add('none')
    const sw = h('span', { class: 'swatch' })
    sw.style.background = `rgb(${(color ?? [1, 0.85, 0.25]).map((v) => Math.round(v * 255)).join(',')})`
    const [w, ht, d] = boxSize(box)
    const b = h('button', { class: 'toolrow withicon', title: `${title}\nBox ${+w.toFixed(3)} x ${+d.toFixed(3)} x ${+ht.toFixed(3)} m (width x depth x height)\nClick to add, or drag into a viewport`, draggable: 'true' },
      img, h('span', { class: 'rowtext' }, h('span', {}, sw, label), h('span', { class: 'dim sub' }, `${+w.toFixed(2)} x ${+d.toFixed(2)} x ${+ht.toFixed(2)} m`)))
    b.addEventListener('click', () => addPointEntity(this.store, classname, this.at(classname), keyvalues))
    this.makeDraggable(b, img, { kind: 'entity', classname, ...(keyvalues ? { keyvalues } : {}) })
    return b
  }

  private build(): void {
    const defs = this.store.state.entityDefs
    this.shownDefs = defs
    this.shownShape = this.store.state.shape
    const grid = (...cards: HTMLElement[]) => h('div', { class: 'cards' }, ...cards)
    const isLight = (c: EntityClassDef) => c.name.toLowerCase().startsWith('light')
    const points = defs.filter((c) => c.kind === 'point')
    this.element.replaceChildren(tabbed([
      { label: 'Shapes', content: () => h('div', {}, grid(...SHAPES.map((s) => card(s.label, s.keys ? `Key ${s.keys}` : s.implemented ? '' : 'soon', s.implemented, () => {
        this.store.setShape(s.id as ShapeId)
        this.store.setMessage(`${s.label}: drag on a surface (the start snaps to the grid), release, move for height, click. Shift = equal sides, Esc = cancel`)
      }))), this.shapeParams()) },
      { label: 'Prefabs', content: () => this.prefabPanel() },
      { label: 'Decals', content: () => this.decalPanel() },
      // With definitions: every point class from the .ent file. Without: the built-in starter list.
      { label: 'Entities', content: () => points.length ? this.classList(points.filter((c) => !isLight(c))) : h('div', { class: 'toolrows pad-top' }, ...ENTITIES.map((e) => this.entityRow(e.classname, e.label, e.classname, e.keyvalues))) },
      { label: 'Lights', content: () => h('div', {}, h('div', { class: 'toolrows pad-top' }, ...LIGHTS.map((l) => this.entityRow(l.classname, l.label, `${l.classname}: ${l.sub}`))),
        ...(points.some((c) => isLight(c) && c.name !== 'light' && !isModernLight(c.name)) ? [h('div', { class: 'section' }, 'Classic Quake 3 lights'), this.classList(points.filter((c) => isLight(c) && !isModernLight(c.name)))] : [])) },
    ], { vertical: true }))
  }

  /** A filterable list of point classes: preview icon, .ent colour, size from the .ent box. */
  private classList(classes: EntityClassDef[]): HTMLElement {
    const rows = h('div', { class: 'toolrows' })
    const input = h('input', { class: 'filter', placeholder: `Filter ${classes.length} entities…`, value: this.filter })
    const draw = () => {
      const q = this.filter.trim().toLowerCase()
      rows.replaceChildren(...classes.filter((c) => !q || c.name.toLowerCase().includes(q)).slice(0, 300).map((c) => this.entityRow(c.name, c.name, c.description || c.name)))
    }
    input.addEventListener('input', () => { this.filter = input.value; draw() })
    draw()
    return h('div', {}, input, h('div', { class: 'pad-top' }), rows)
  }
}

/** "Mesh Edit" tab: Blender-style modeling tools. Tab toggles Brush mode <-> Mesh mode; buttons and keys run the same tools. */
export class MeshEditPanel implements IContentRenderer {
  readonly element = h('div', { class: 'panel' })
  private unsub: (() => void) | null = null
  private sig = ''
  private hollow = '0.25'
  private signature(): string { const s = this.store.state; return `${s.editMode}|${s.meshMode}|${s.meshSel.verts.length}|${s.meshSel.edges.length}|${s.meshSel.faces.length}|${s.proportional}|${s.propRadius}` }
  private cuts = '1'
  constructor(private store: Store, private run: (id: string) => boolean) {}

  init(): void {
    this.draw()
    this.unsub = this.store.subscribe(() => { if (this.signature() !== this.sig) this.draw() })
  }
  dispose(): void { this.unsub?.() }

  private draw(): void {
    const st = this.store.state
    this.sig = this.signature()
    const mesh = st.editMode === 'mesh'
    const mode = h('div', { class: 'seg' })
    const seg = (label: string, on: boolean, click: () => void) => { const b = h('button', { class: on ? 'on' : '' }, label); b.addEventListener('click', click); mode.append(b) }
    seg('Brush mode', !mesh, () => this.store.setEditMode('object'))
    seg('Mesh mode', mesh, () => this.store.setEditMode('mesh'))
    const el = h('div', { class: 'seg' })
    for (const [id, label, key] of [['vertex', 'Vertex', '1'], ['edge', 'Edge', '2'], ['face', 'Face', '3']] as const) {
      const b = h('button', { class: mesh && st.meshMode === id ? 'on' : '', title: `Key ${key}`, ...(mesh ? {} : { disabled: '' }) }, `${label} (${key})`)
      b.addEventListener('click', () => this.run(`mode:${key}`))
      el.append(b)
    }
    const list = h('div', { class: 'toolrows' })
    for (const t of MESH_TOOLS) {
      const b = h('button', { class: 'toolrow', title: t.hint || t.label, ...(t.implemented && mesh ? {} : { disabled: '' }) }, h('span', {}, t.implemented ? t.label : `${t.label} (later)`), h('kbd', {}, t.keys))
      b.addEventListener('click', () => this.run(t.id === 'subdivide' ? `subdivide:${Number(this.cuts) || 1}` : t.id))
      list.append(b)
      if (t.id === 'subdivide') {
        const inp = h('input', { type: 'number', min: '1', max: '64', step: '1', value: this.cuts, title: 'New vertices per edge', ...(mesh ? {} : { disabled: '' }) })
        inp.addEventListener('input', () => { this.cuts = inp.value })
        inp.addEventListener('keydown', (e) => e.stopPropagation())
        list.append(h('div', { class: 'row-split pad' }, h('span', { class: 'dim' }, 'Cuts'), inp))
      }
    }
    const brushList = h('div', { class: 'toolrows' })
    for (const t of BRUSH_TOOLS) {
      const b = h('button', { class: 'toolrow', title: t.hint, ...(mesh ? { disabled: '' } : {}) }, h('span', {}, t.label), h('span', { class: 'key' }, t.keys))
      b.addEventListener('click', () => this.run(t.id === 'hollow' ? `brush:hollow:${Number(this.hollow) || 0.25}` : `brush:${t.id}`))
      brushList.append(b)
      if (t.id === 'hollow') {
        const inp = h('input', { type: 'number', min: '0.01', step: '0.05', value: this.hollow, title: 'Wall thickness', ...(mesh ? { disabled: '' } : {}) })
        inp.addEventListener('input', () => { this.hollow = inp.value })
        inp.addEventListener('keydown', (e) => e.stopPropagation())
        brushList.append(h('div', { class: 'row-split pad' }, h('span', { class: 'dim' }, 'Wall thickness'), inp))
      }
    }
    const auto = h('input', { type: 'checkbox', ...(st.autoDetail ? { checked: '' } : {}) })
    auto.addEventListener('change', () => this.store.setAutoDetail(auto.checked))
    const autoRow = h('label', { class: 'row-split pad', title: 'When an edit makes a world brush concave, move that brush into a func_detail entity instead of refusing the edit' }, auto, h('span', {}, 'Concave → func_detail'))
    const prop = h('input', { type: 'checkbox', ...(st.proportional ? { checked: '' } : {}) })
    prop.addEventListener('change', () => this.store.setProportional(prop.checked))
    const rad = h('input', { type: 'number', min: '0.01', step: '0.1', value: String(+st.propRadius.toFixed(3)), title: 'Radius in metres (mouse wheel during G / R / S changes it)' })
    rad.addEventListener('change', () => { const v = Number(rad.value); if (v > 0) this.store.setPropRadius(v) })
    rad.addEventListener('keydown', (e) => e.stopPropagation())
    const propRow = h('div', { class: 'row-split pad', title: 'Unselected vertices near the selection follow G / R / S and the gizmo with a smooth falloff' }, h('label', {}, prop, h('span', {}, ' Proportional editing (O)')), rad)
    const n = st.meshSel
    this.element.replaceChildren(
      h('div', { class: 'section' }, 'Mode  (Tab)'), mode,
      h('div', { class: 'section' }, 'Select'), el,
      h('div', { class: 'dim pad' }, mesh ? `Selected: ${n.verts.length} vertices · ${n.edges.length} edges · ${n.faces.length} faces` : 'Select a brush, then press Tab to edit its vertices, edges and faces.'),
      autoRow, ...(mesh ? [propRow] : []),
      ...(mesh ? [] : [h('div', { class: 'section' }, 'Brush tools'), brushList]),
      h('div', { class: 'section' }, mesh ? 'Tools' : 'Mesh tools (Tab)'), list,
      h('div', { class: 'dim pad' }, 'worldspawn and func_group brushes always stay convex and closed: a tool that would break that is refused with a message. Faces that stop being planar are split into triangles.'))
  }
}
