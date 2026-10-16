// The texture group editor window (REQUIREMENTS §20 T8): the file as text on the left, a live preview of a wall of a chosen size on the right, the group's
// parameters, the seed, and every problem the reader / solver / relief check found. Not a node-by-node visual tree editor (listed as not built in §20).
import { GROUP_TEMPLATES, cellAt, previewScale, reliefBorderWarnings, setSlotTexture } from '../model/groupTools'
import type { LayoutResult } from '../model/layout'
import { Facing } from '../model/layout'
import { GroupParse, ParamDef, ParamValue, TexGroup, parseGroupText, resolveParams } from '../model/texgroup'
import { texEnv } from '../model/texprops'
import type { TextureCache } from '../state/textures'
import { openDialog } from './dialog'
import { fmt, h } from './dom'
import { drawGroupPreview } from './groupPreview'
import { pickTexture } from './texturePropsWindow'

export interface GroupEditorEnv {
  textures: TextureCache
  /** Every texture path (for the slot picker). */
  listTextures: () => Promise<string[]>
  /** The raw file of a group (Core). */
  read: (path: string) => Promise<unknown>
  /** Writes a group to Core and updates every viewport. */
  save: (path: string, group: unknown) => Promise<void>
  /** Applies the group to the selected faces / brushes; returns how many faces. */
  apply: (path: string) => number
  /** Called after a successful save (the panel rescans). */
  saved: () => void
}

const NAME_OK = /^[A-Za-z0-9_\-]+(\/[A-Za-z0-9_\-]+)*$/
export const validGroupName = (s: string): boolean => NAME_OK.test(s) && s.length <= 120

/** `path` null = a new group (starts from a template). */
export async function openTexGroupEditor(env: GroupEditorEnv, path: string | null, initial?: unknown): Promise<void> {
  let text = ''
  if (path !== null && initial === undefined) {
    try { text = JSON.stringify(await env.read(path), null, 2) } catch (e) { text = ''; alert(`Could not read '${path}': ${(e as Error).message}`) }
  } else text = JSON.stringify(initial ?? GROUP_TEMPLATES[0].group, null, 2)

  const dlg = openDialog(path === null ? 'New texture group' : `Texture group: ${path}`, { width: 1120, onClose: () => { unsub(); window.clearTimeout(timer) } })
  const nameIn = h('input', { type: 'text', placeholder: 'folder/name (no extension)', value: path ?? '', class: 'search', style: 'width:260px' })
  if (path !== null) nameIn.setAttribute('readonly', '')
  const status = h('span', { class: 'dim' })
  const saveBtn = h('button', { class: 'primary' }, 'Save'), applyBtn = h('button', { title: 'Apply this group to the selected faces / brushes' }, 'Apply to selection')
  const tpl = h('select', { title: 'Replace the text with a starter group' }, h('option', { value: '' }, 'Template...'), ...GROUP_TEMPLATES.map((t) => h('option', { value: t.id }, t.label)))
  const ta = h('textarea', { spellcheck: 'false', style: 'width:100%;height:520px;font:12px/1.35 Consolas,monospace;background:#101010;color:#ddd;border:1px solid #333;resize:none;white-space:pre;overflow:auto;box-sizing:border-box' })
  ta.value = text
  const canvas = h('canvas', { width: '500', height: '420', style: 'background:#161616;border:1px solid #333;max-width:100%' })
  const problems = h('div', { class: 'gp-problems' })
  const controls = h('div', { class: 'gp-controls' })
  const paramBox = h('div', { class: 'gp-params' })
  const slotBox = h('div', { class: 'gp-slots' })
  const cellInfo = h('div', { class: 'dim', style: 'min-height:15px;user-select:text' }, 'Hover the preview to see which rule made a part; click to mark it.')

  // preview settings
  const view = { w: 9, h: 12, facing: 'wall' as Facing, seed: 1, outlines: true, labels: true }
  const mkNum = (label: string, get: () => number, set: (v: number) => void, min: number, step = 0.5) => {
    const i = h('input', { type: 'number', min: String(min), step: String(step), value: fmt(get()), style: 'width:70px' })
    i.addEventListener('change', () => { const v = Number(i.value); if (v >= min) { set(v); redraw() } }); i.addEventListener('keydown', (e) => e.stopPropagation())
    return h('label', { class: 'numfield' }, h('span', {}, label), i)
  }
  const facing = h('select', {}, ...(['wall', 'up', 'down'] as Facing[]).map((f) => h('option', { value: f }, f === 'wall' ? 'wall' : f === 'up' ? 'floor / roof (up)' : 'ceiling (down)')))
  facing.addEventListener('change', () => { view.facing = facing.value as Facing; redraw() })
  const seedIn = mkNum('seed', () => view.seed, (v) => { view.seed = Math.trunc(v) }, 0, 1)
  const reroll = h('button', {}, 'Re-roll'); reroll.addEventListener('click', () => { view.seed = Math.floor(Math.random() * 1e6); (seedIn.querySelector('input') as HTMLInputElement).value = String(view.seed); redraw() })
  const chk = (label: string, get: () => boolean, set: (v: boolean) => void) => { const c = h('input', { type: 'checkbox' }); c.checked = get(); c.addEventListener('change', () => { set(c.checked); redraw() }); return h('label', { class: 'numfield' }, c, h('span', {}, label)) }
  controls.append(mkNum('width m', () => view.w, (v) => { view.w = v }, 0.1), mkNum('height m', () => view.h, (v) => { view.h = v }, 0.1), facing, seedIn, reroll, chk('outlines', () => view.outlines, (v) => { view.outlines = v }), chk('labels', () => view.labels, (v) => { view.labels = v }))

  let parse: GroupParse = { group: null, errors: [], warnings: [] }
  let last: TexGroup | null = null
  const params: Record<string, ParamValue> = {}
  let timer = 0
  let lastResult: LayoutResult | null = null
  let hl = -1

  const buildParams = (g: TexGroup | null): void => {
    paramBox.replaceChildren()
    if (!g) return
    for (const [name, d] of Object.entries(g.params)) paramBox.append(paramControl(name, d))
  }
  const paramControl = (name: string, d: ParamDef): HTMLElement => {
    const cur = resolveParams(last!, params)[name]
    let el: HTMLElement
    if (d.type === 'bool') { const c = h('input', { type: 'checkbox' }); c.checked = cur === true; c.addEventListener('change', () => { params[name] = c.checked; redraw() }); el = c }
    else if (d.type === 'choice') { const s = h('select', {}, ...d.options.map((o) => { const op = h('option', { value: o }, o); if (o === cur) op.setAttribute('selected', ''); return op })); s.addEventListener('change', () => { params[name] = s.value; redraw() }); el = s }
    else { const i = h('input', { type: 'number', min: String(d.min), max: String(d.max), step: String(d.step), value: fmt(cur as number), style: 'width:70px' }); i.addEventListener('change', () => { const v = Number(i.value); if (Number.isFinite(v)) { params[name] = v; redraw() } }); i.addEventListener('keydown', (e) => e.stopPropagation()); el = i }
    return h('label', { class: 'numfield' }, h('span', {}, d.label), el)
  }

  const showProblems = (extra: string[]): void => {
    const items: HTMLElement[] = []
    for (const e of parse.errors) items.push(h('div', { class: 'gp-err' }, e))
    for (const w of [...parse.warnings, ...extra]) items.push(h('div', { class: 'gp-warn' }, w))
    if (items.length === 0) items.push(h('div', { class: 'dim' }, 'No problems found.'))
    problems.replaceChildren(...items)
  }

  function redraw(): void {
    const g = parse.group ?? last
    if (!g) { canvas.getContext('2d')?.clearRect(0, 0, canvas.width, canvas.height); showProblems([]); return }
    const r = drawGroupPreview(canvas, g, { w: view.w, h: view.h, facing: view.facing, params, seed: view.seed }, env.textures, { outlines: view.outlines, labels: view.labels, highlight: hl })
    lastResult = r
    const slotMissing = Object.values(g.slots).filter((p) => env.textures.get(p).status === 'missing').map((p) => `texture '${p}' was not found in the assets folder`)
    showProblems([...r.warnings, ...slotMissing, ...reliefBorderWarnings(g, (p) => texEnv.info(p))])
    status.textContent = `${r.cells.length} cells`
  }

  const buildSlots = (): void => {
    slotBox.replaceChildren()
    if (!last) return
    for (const [slot, texPath] of Object.entries(last.slots)) {
      const c = h('canvas', { width: '28', height: '28', style: 'width:28px;height:28px;background:#232323;flex:none' })
      const g = c.getContext('2d'), st = env.textures.get(texPath)
      if (g && st.status === 'ready') g.drawImage(st.image.source as CanvasImageSource, 0, 0, 28, 28)
      const change = h('button', { title: 'Pick another texture for this slot (the text is reformatted)' }, 'Change...')
      change.addEventListener('click', () => void pickTexture({ textures: env.textures, listTextures: env.listTextures, save: async () => {}, activeTexture: () => '' }, `Texture for slot '${slot}'`).then((p) => {
        if (!p) return
        const next = setSlotTexture(ta.value, slot, p)
        if (next === null) { status.textContent = 'The text must be valid JSON to change a slot'; return }
        ta.value = next; reparse()
      }))
      slotBox.append(h('div', { class: 'gp-slot' }, c, h('b', {}, slot), h('span', { class: 'dim', style: 'flex:1;overflow:hidden;text-overflow:ellipsis' }, texPath), change))
    }
  }
  const pointerCell = (e: MouseEvent): number => {
    if (!lastResult) return -1
    const r = canvas.getBoundingClientRect(), k = canvas.width / (r.width || 1)
    const s = previewScale(canvas.width, canvas.height, view.w, view.h, 8)
    return cellAt(lastResult.cells, ((e.clientX - r.left) * k - 8) / s, view.h - ((e.clientY - r.top) * k - 8) / s)
  }
  const describe = (i: number): string => {
    const c = lastResult?.cells[i]
    return c ? `${c.slot || 'fallback'}: ${fmt(c.x1 - c.x0)} x ${fmt(c.y1 - c.y0)} m at (${fmt(c.x0)}, ${fmt(c.y0)}), fit ${c.fit}, rule path ${c.path}, ${c.texture}` : ''
  }
  canvas.addEventListener('mousemove', (e) => { const i = pointerCell(e); canvas.title = describe(i); if (hl < 0) cellInfo.textContent = describe(i) || 'Hover the preview to see which rule made a part; click to mark it.' })
  canvas.addEventListener('click', (e) => { const i = pointerCell(e); hl = i === hl ? -1 : i; cellInfo.textContent = describe(hl) || 'Hover the preview to see which rule made a part; click to mark it.'; redraw() })

  const reparse = (): void => {
    parse = parseGroupText(ta.value)
    if (parse.group) { const fresh = !last || JSON.stringify(Object.keys(last.params)) !== JSON.stringify(Object.keys(parse.group.params)); last = parse.group; if (fresh) { for (const k of Object.keys(params)) delete params[k]; buildParams(last) } else buildParams(last) }
    hl = -1; buildSlots(); redraw()
  }
  ta.addEventListener('input', () => { window.clearTimeout(timer); timer = window.setTimeout(reparse, 200) })
  ta.addEventListener('keydown', (e) => { e.stopPropagation(); if (e.key === 'Tab') { e.preventDefault(); const s = ta.selectionStart; ta.setRangeText('  ', s, ta.selectionEnd, 'end') } })
  tpl.addEventListener('change', () => {
    const t = GROUP_TEMPLATES.find((x) => x.id === tpl.value); tpl.value = ''
    if (t) { ta.value = JSON.stringify(t.group, null, 2); reparse() }
  })
  const unsub = env.textures.onChange(() => { if (dlg.root.isConnected) { buildSlots(); redraw() } })

  const doSave = async (): Promise<string | null> => {
    const name = nameIn.value.trim()
    if (!validGroupName(name)) { status.textContent = 'Name: letters, digits, _ and - with / for folders'; return null }
    reparse()
    if (!parse.group) { status.textContent = 'Fix the errors first'; return null }
    try { await env.save(name, JSON.parse(ta.value)); status.textContent = `Saved ${name}`; env.saved(); return name } catch (e) { status.textContent = `Not saved: ${(e as Error).message}`; return null }
  }
  saveBtn.addEventListener('click', () => void doSave())
  applyBtn.addEventListener('click', async () => {
    const name = await doSave()
    if (name) { const n = env.apply(name); status.textContent = n ? `Saved and applied to ${n} face(s)` : 'Saved. Select a brush or face to apply it' }
  })

  dlg.body.append(
    h('div', { class: 'gp-bar' }, nameIn, tpl, saveBtn, applyBtn, status),
    h('div', { class: 'gp-body' }, h('div', { class: 'gp-left' }, ta), h('div', { class: 'gp-right' }, controls, paramBox, canvas, cellInfo, slotBox, problems)))
  reparse()
}
