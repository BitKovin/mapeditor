// Pure helpers of the texture group UI (REQUIREMENTS §20 T8): starter templates, authoring checks and the preview's texture transform.
import type { CellMap } from './layoutPlan'
import { isFlatRelief } from './relief'
import { TexGroup } from './texgroup'
import { TexInfo } from './texprops'

/** Complete small groups on the demo textures: what "New group" starts from. Each is a valid group file. */
export const GROUP_TEMPLATES: { id: string; label: string; group: Record<string, unknown> }[] = [
  {
    id: 'facade', label: 'Building facade (floors, bays, top floor)',
    group: {
      format: 1, name: 'Building facade',
      description: 'Ground floor, repeating floors of window bays, a top floor and a cornice. Walls, roofs and floors are told apart by the face orientation.',
      params: { floorHeight: { type: 'number', default: 3, min: 2, max: 6, step: 0.1, label: 'Floor height (m)' }, bayWidth: { type: 'number', default: 3, min: 2, max: 8, step: 0.1, label: 'Bay width (m)' } },
      slots: { wall: 'dev/wall', window: 'dev/metal', door: 'dev/wood', shop: 'dev/concrete', cornice: 'dev/concrete', roof: 'dev/floor' },
      fallback: 'wall',
      rules: { windowBay: { split: 'u', parts: [{ size: '*', tex: 'wall' }, { size: 1.2, tex: 'window', fit: 'stretch' }, { size: '*', tex: 'wall' }] } },
      root: {
        choice: [
          {
            when: { var: 'facing', '==': 'wall' }, split: 'v',
            parts: [
              { size: { param: 'floorHeight', mul: 1.3 }, split: 'u', parts: [{ size: '*', repeat: { size: { param: 'bayWidth' }, as: 'bay' }, choice: [{ when: { all: ['bay.first', { var: 'faceW', '>=': 6 }] }, tex: 'door', fit: 'stretch' }, { tex: 'shop', fit: 'stretch' }] }] },
              { size: '*', repeat: { size: { param: 'floorHeight' }, as: 'floor' }, split: 'u', parts: [{ size: '*', repeat: { size: { param: 'bayWidth' }, as: 'bay' }, ref: 'windowBay' }] },
              { size: 0.5, tex: 'cornice', fit: 'width' },
            ],
          },
          { when: { var: 'facing', '==': 'up' }, tex: 'roof' },
          { tex: 'wall' },
        ],
      },
    },
  },
  {
    id: 'panels', label: 'Panel wall (trim, random panels, vents)',
    group: {
      format: 1, name: 'Panel wall',
      description: 'Bottom and top trim with panels between. Panels vary by seed; every fourth bay has a vent.',
      params: { panelWidth: { type: 'number', default: 1.5, min: 0.5, max: 4, step: 0.1, label: 'Panel width (m)' } },
      slots: { panel: 'dev/metal', panelB: 'dev/concrete', vent: 'dev/grey', trim: 'dev/wall' },
      fallback: 'panel',
      root: {
        split: 'v',
        parts: [
          { size: 0.3, tex: 'trim', fit: 'height' },
          { size: '*', split: 'u', parts: [{ size: '*', repeat: { size: { param: 'panelWidth' }, as: 'p' }, choice: [{ when: { var: 'p.index', mod: [4, 3] }, tex: 'vent', fit: 'stretch' }, { tex: 'panel', fit: 'stretch', weight: 3 }, { tex: 'panelB', fit: 'stretch', weight: 1 }], mode: 'random' }] },
          { size: 0.2, tex: 'trim', fit: 'height' },
        ],
      },
    },
  },
  {
    id: 'floor', label: 'Bordered floor (trim ring, random tiles)',
    group: {
      format: 1, name: 'Bordered floor',
      description: 'A ring of trim around the edge and tiles of three kinds inside. Useful on floors, ceilings and rugs.',
      params: { border: { type: 'number', default: 0.3, min: 0.05, max: 1, step: 0.05, label: 'Border (m)' }, tile: { type: 'number', default: 1, min: 0.25, max: 3, step: 0.25, label: 'Tile (m)' } },
      slots: { tile: 'dev/floor', tileB: 'dev/concrete', tileC: 'dev/grey', border: 'dev/wood' },
      fallback: 'tile',
      rules: {
        tiles: { split: 'v', parts: [{ size: '*', repeat: { size: { param: 'tile' }, as: 'r' }, split: 'u', parts: [{ size: '*', repeat: { size: { param: 'tile' }, as: 'c' }, choice: ['tile', 'tile', 'tileB', 'tileC'], mode: 'random' }] }] },
      },
      root: {
        split: 'v',
        parts: [
          { size: { param: 'border' }, tex: 'border' },
          { size: '*', split: 'u', parts: [{ size: { param: 'border' }, tex: 'border' }, { size: '*', ref: 'tiles' }, { size: { param: 'border' }, tex: 'border' }] },
          { size: { param: 'border' }, tex: 'border' },
        ],
      },
    },
  },
]

/** Every slot texture whose relief is not flat along the border of its tile: its neighbouring cells would show a step or a crack where they meet. */
export function reliefBorderWarnings(g: TexGroup, info: (path: string) => TexInfo | undefined): string[] {
  const out: string[] = []
  for (const [slot, path] of Object.entries(g.slots)) {
    const r = info(path)?.props.relief
    if (!r || isFlatRelief(r)) continue
    const bad = r.vertices.some(([x, y, z]) => (x < 1e-6 || x > 1 - 1e-6 || y < 1e-6 || y > 1 - 1e-6) && Math.abs(z) > 1e-4)
    if (bad) out.push(`slot '${slot}' (${path}): the relief is not flat at the border of its tile, so neighbouring parts will not join cleanly`)
  }
  return out
}

/**
 * Canvas pattern transform for a cell in the preview: maps image pixels to canvas pixels, so a texture drawn through it sits exactly where the cell's UV map
 * puts it. Canvas x = scale * layout x, canvas y = scale * (faceH - layout y). Returns [a, b, c, d, e, f] of a DOMMatrix.
 */
export function previewPatternMatrix(m: CellMap, imgW: number, imgH: number, scale: number, faceH: number): [number, number, number, number, number, number] {
  const [pu, qu, tu] = m.u, [pv, qv, tv] = m.v
  const det = pu * qv - qu * pv
  // (x, y) from image pixels (iu, iv): u = iu / imgW, v = iv / imgH
  const k11 = qv / imgW / det, k12 = -qu / imgH / det, k13 = (-qv * tu + qu * tv) / det
  const k21 = -pv / imgW / det, k22 = pu / imgH / det, k23 = (pv * tu - pu * tv) / det
  return [scale * k11, -scale * k21, scale * k12, -scale * k22, scale * k13, scale * faceH - scale * k23]
}

/** The cell under a layout point (metres), or -1. Cells on a shared edge belong to the one that starts there (the later one). */
export function cellAt(cells: readonly { x0: number; y0: number; x1: number; y1: number }[], x: number, y: number): number {
  for (let i = cells.length - 1; i >= 0; i--) { const c = cells[i]; if (x >= c.x0 && x <= c.x1 && y >= c.y0 && y <= c.y1) return i }
  return -1
}

/** Canvas pixels per metre of the preview and where the wall's corner sits (shared by the drawing and the pointer hit test). */
export function previewScale(canvasW: number, canvasH: number, w: number, h: number, margin: number): number {
  return Math.min((canvasW - 2 * margin) / w, (canvasH - 2 * margin) / h)
}

/**
 * Changes the texture of one slot in the file text and returns the new text (pretty printed), or null when the text is not a group with that slot.
 * Both slot forms are kept: `"wall": "path"` and `"wall": { "texture": "path" }`.
 */
export function setSlotTexture(text: string, slot: string, path: string): string | null {
  let j: unknown
  try { j = JSON.parse(text) } catch { return null }
  if (!j || typeof j !== 'object' || Array.isArray(j)) return null
  const slots = (j as { slots?: Record<string, unknown> }).slots
  if (!slots || typeof slots !== 'object' || !(slot in slots)) return null
  const cur = slots[slot]
  slots[slot] = cur && typeof cur === 'object' ? { ...(cur as object), texture: path } : path
  return JSON.stringify(j, null, 2)
}
