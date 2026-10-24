// The Terrain panel (REQUIREMENTS §39): make a terrain of a given size and resolution, pick the sculpting brush and its radius / strength / falloff.
// Sculpting itself is the Terrain tool in the viewport (T): drag on a terrain.
import type { IContentRenderer } from 'dockview-core'
import { addTerrain } from '../state/actions'
import type { Store } from '../state/store'
import { TERRAIN_TOOLS, TerrainTool, TerrainSpec, describeSpec, gridFor, specOf } from '../model/terrain'
import { h } from './dom'

const TOOL_INFO: Record<TerrainTool, { label: string; hint: string }> = {
  raise: { label: 'Raise', hint: 'Lifts the ground under the brush; Shift lowers' },
  lower: { label: 'Lower', hint: 'Digs the ground under the brush; Shift raises' },
  smooth: { label: 'Smooth', hint: 'Blends the ground toward the average of its neighbours' },
  flatten: { label: 'Flatten', hint: 'Pulls the ground toward one height: the height under the first click of the stroke, or the fixed height below' },
  paint: { label: 'Paint', hint: 'Paints the texture selected in the Content Browser onto the cells under the brush' },
}

export class TerrainPanel implements IContentRenderer {
  readonly element = h('div', { class: 'panel' })
  private unsub: (() => void) | null = null
  private sig = ''
  constructor(private store: Store, private targetPoint: () => [number, number, number]) {}

  init(): void {
    this.draw()
    this.unsub = this.store.subscribe(() => { if (this.signature() !== this.sig) this.draw() })
  }
  dispose(): void { this.unsub?.() }

  /** The terrain the panel talks about: the selected one, else the first in the document. */
  private current(): TerrainSpec | null {
    const st = this.store.state, sel = st.selection
    const own = sel ? st.doc.entities.find((e) => e.id === sel.entityId && e.terrain !== undefined) : undefined
    const e = own ?? st.doc.entities.find((x) => x.terrain !== undefined)
    return e ? specOf(e) : null
  }
  private signature(): string {
    const t = this.store.state.terrain, c = t.create, spec = this.current()
    return [t.tool, this.store.state.tool === 'terrain', t.brush.radius, t.brush.strength, t.brush.falloff, t.flattenHeight, c.sizeX, c.sizeZ, c.cell, c.tile, c.thickness, spec ? describeSpec(spec) : '-'].join('|')
  }

  private num(label: string, value: number, min: number, max: number, step: number, on: (v: number) => void, title = ''): HTMLElement {
    const inp = h('input', { type: 'number', min: String(min), max: String(max), step: String(step), value: String(+value.toFixed(3)), ...(title ? { title } : {}) })
    inp.addEventListener('change', () => { const v = Number(inp.value); if (Number.isFinite(v)) on(Math.min(max, Math.max(min, v))) })
    inp.addEventListener('keydown', (e) => e.stopPropagation())
    return h('label', { class: 'field', ...(title ? { title } : {}) }, h('span', {}, label), inp)
  }

  private draw(): void {
    this.sig = this.signature()
    const st = this.store.state, t = st.terrain, c = t.create
    const grid = gridFor({ ...c, origin: [0, 0, 0], texture: '' })
    const info = 'error' in grid ? grid.error : `${grid.cols} x ${grid.rows} cells in ${Math.ceil(grid.cols / grid.tile) * Math.ceil(grid.rows / grid.tile)} tiles (brushes), about ${(grid.cols * grid.rows * 2).toLocaleString()} triangles`
    const create = h('button', { class: 'primary', title: 'Makes a flat terrain (a func_detail entity of slab brushes) centred on the viewport target, textured with the Content Browser selection', ...('error' in grid ? { disabled: '' } : {}) }, 'Create terrain')
    create.addEventListener('click', () => { addTerrain(this.store, this.targetPoint()) })

    const tools = h('div', { class: 'seg' })
    for (const id of TERRAIN_TOOLS) {
      const b = h('button', { class: st.tool === 'terrain' && t.tool === id ? 'on' : '', title: TOOL_INFO[id].hint }, TOOL_INFO[id].label)
      b.addEventListener('click', () => this.store.setTerrain({ tool: id }))
      tools.append(b)
    }
    const flatten = h('input', { type: 'number', step: '0.1', placeholder: 'under the first click', value: t.flattenHeight === null ? '' : String(t.flattenHeight), title: 'Flatten aims at this height in metres; empty = the height under the first point of the stroke' })
    flatten.addEventListener('change', () => { const v = flatten.value.trim() === '' ? null : Number(flatten.value); this.store.setTerrain({ flattenHeight: v !== null && Number.isFinite(v) ? v : null }) })
    flatten.addEventListener('keydown', (e) => e.stopPropagation())

    const spec = this.current()
    this.element.replaceChildren(
      h('div', { class: 'section' }, 'Sculpt  (T)'), tools,
      h('div', { class: 'pad' },
        this.num('Radius (m)', t.brush.radius, 0.25, 200, 0.25, (v) => this.store.setTerrain({ brush: { radius: v } }), 'Size of the brush; [ and ] in the viewport change it'),
        this.num('Strength', t.brush.strength, 0.01, 1, 0.05, (v) => this.store.setTerrain({ brush: { strength: v } }), 'Raise / Lower move the ground by strength / 2 metres per stamp; Smooth / Flatten blend by this fraction per stamp'),
        this.num('Falloff', t.brush.falloff, 0, 1, 0.05, (v) => this.store.setTerrain({ brush: { falloff: v } }), '0 = hard edge, 1 = full strength only at the centre, dropping smoothly to nothing at the radius'),
        h('label', { class: 'field', title: 'Flatten aims at this height in metres' }, h('span', {}, 'Flatten height'), flatten),
        h('div', { class: 'dim' }, 'Drag on a terrain to sculpt. Shift swaps Raise and Lower. [ ] change the radius. One drag is one undo step.')),
      h('div', { class: 'section' }, 'Terrain'),
      h('div', { class: 'pad dim' }, spec ? describeSpec(spec) : 'No terrain in the map yet.'),
      h('div', { class: 'section' }, 'New terrain'),
      h('div', { class: 'pad' },
        this.num('Width X (m)', c.sizeX, 1, 2000, 1, (v) => this.store.setTerrain({ create: { sizeX: v } })),
        this.num('Depth Z (m)', c.sizeZ, 1, 2000, 1, (v) => this.store.setTerrain({ create: { sizeZ: v } })),
        this.num('Cell (m)', c.cell, 0.05, 64, 0.25, (v) => this.store.setTerrain({ create: { cell: v } }), 'Resolution: the edge of one grid cell (two triangles). Smaller = finer sculpting, more geometry'),
        this.num('Tile (cells)', c.tile, 2, 64, 1, (v) => this.store.setTerrain({ create: { tile: Math.round(v) } }), 'Cells per brush side. Sculpting rebuilds only the tiles it touches; smaller tiles = cheaper strokes, but more brushes'),
        this.num('Thickness (m)', c.thickness, 1, 1000, 1, (v) => this.store.setTerrain({ create: { thickness: v } }), 'Depth of the slab below the flat ground; the ground cannot be dug below its bottom'),
        h('div', { class: 'dim' }, info), create,
        h('div', { class: 'dim' }, 'A terrain is a func_detail of closed slab brushes whose top triangles are smooth-shaded. Click it with the Select tool to move or delete the whole terrain; editing a tile in Mesh mode turns the terrain into ordinary brushes.')),
    )
  }
}
