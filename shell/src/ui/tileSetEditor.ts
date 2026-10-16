// The editor of simple texture sets (REQUIREMENTS §20 simple sets): no rules, no text. A wall set is three rows of PIECES (Top / Middle / Bottom); a piece is a
// texture or a block of textures that always stay together (a door of two halves, a two storey window bay). Click a piece to edit it, drag it to another row,
// add textures and they sort themselves by name. A frame (9-slice) is nine slots. The preview on the right is the real layout (model/layout.ts).
import type { Facing } from '../model/layout'
import { groupTextures, parseGroup } from '../model/texgroup'
import { texEnv } from '../model/texprops'
import {
  FrameSlot, Kit, LANES, LaneId, Place, WEIGHTS, WallKit, addPiece, addTextures, duplicatePiece, emptyFrameKit, emptyWallKit, fitPieceCols, kitToJson, movePiece, pieceCols,
  pieceRows, removePiece, setLaneRows, setPieceCell,
} from '../model/tileset'
import type { TextureCache } from '../state/textures'
import { showMenu } from './contextMenu'
import { openDialog } from './dialog'
import { fmt, h } from './dom'
import { ThumbnailLoader } from './textureThumbs'
import { drawGroupPreview } from './groupPreview'
import { pickTexture } from './texturePropsWindow'

export interface TileSetEnv {
  textures: TextureCache
  listTextures: () => Promise<string[]>
  read: (path: string) => Promise<unknown>
  save: (path: string, group: unknown) => Promise<void>
  /** Applies the set to the selected faces / brushes; returns how many faces. */
  apply: (path: string) => number
  saved: () => void
}

const NAME_OK = /^[A-Za-z0-9_\-]+(\/[A-Za-z0-9_\-]+)*$/
const LANE_INFO: Record<LaneId, { title: string; hint: string }> = {
  top: { title: 'Top row', hint: 'Rooftop or cap: always above everything else' },
  middle: { title: 'Middle rows', hint: 'Repeats upward to fill the height of the wall' },
  bottom: { title: 'Bottom row', hint: 'Ground floor: always at the bottom' },
}
const PLACE_LABEL: Record<Place, string> = { any: 'Anywhere', left: 'Left end only', right: 'Right end only', middle: 'Middle only', once: 'Exactly once per row' }
const PLACE_SHORT: Record<Place, string> = { any: '', left: 'left end', right: 'right end', middle: 'middle', once: 'once' }
const FRAME_LABEL: Record<FrameSlot, string> = { tl: 'Top left corner', t: 'Top edge (repeats)', tr: 'Top right corner', l: 'Left edge (repeats)', c: 'Middle (repeats)', r: 'Right edge (repeats)', bl: 'Bottom left corner', b: 'Bottom edge (repeats)', br: 'Bottom right corner' }
const weightLabel = (w: number): string => WEIGHTS.find((x) => Math.abs(x.value - w) < 1e-6)?.label ?? `x${fmt(w)}`

/** A texture chooser that keeps asking until you are done: toggles thumbnails and resolves the chosen paths (empty = cancelled). */
function pickTextures(env: TileSetEnv, title: string): Promise<string[]> {
  return new Promise((resolve) => {
    const chosen = new Set<string>()
    let done = false
    const search = h('input', { placeholder: 'Search textures', class: 'search' })
    const grid = h('div', { class: 'thumbs', style: 'max-height:56vh;min-height:240px' })
    const thumbs = new ThumbnailLoader(env.textures, grid)
    const d = openDialog(title, { width: 680, onClose: () => { thumbs.dispose(); if (!done) resolve([]) } })
    const ok = h('button', { class: 'primary' }, 'Add')
    let all: string[] = []
    const label = () => { ok.textContent = chosen.size ? `Add ${chosen.size}` : 'Add'; ok.toggleAttribute('disabled', chosen.size === 0) }
    const render = () => {
      const q = search.value.toLowerCase()
      thumbs.reset()
      grid.replaceChildren(...all.filter((p) => p.toLowerCase().includes(q)).slice(0, 400).map((p) => {
        const tile = h('div', { class: `tile${chosen.has(p) ? ' sel' : ''}`, title: p }, thumbs.canvas(p), h('div', { class: 'tile-name' }, p.split('/').pop() ?? p))
        tile.addEventListener('click', () => { if (chosen.has(p)) chosen.delete(p); else chosen.add(p); tile.classList.toggle('sel', chosen.has(p)); label() })
        return tile
      }))
    }
    search.addEventListener('input', render); search.addEventListener('keydown', (e) => e.stopPropagation())
    ok.addEventListener('click', () => { done = true; resolve([...chosen]); d.close() })
    d.body.append(search, grid, h('div', { class: 'btns' }, ok, h('span', { class: 'dim' }, 'Click to select several. Names like top / bottom / roof / door / window sort them into rows.')))
    label()
    void env.listTextures().then((l) => { all = l; render() })
  })
}

/** `path` null = a new set of `kind`. */
export async function openTileSetEditor(env: TileSetEnv, path: string | null, kind: 'wall' | 'frame' = 'wall'): Promise<void> {
  let kit: Kit = kind === 'frame' ? emptyFrameKit() : emptyWallKit()
  let name = '', description = ''
  if (path !== null) {
    try {
      const raw = (await env.read(path)) as Record<string, unknown>
      const g = parseGroup(raw)
      if (!g.group?.kit) { alert(`'${path}' is not a simple set. Open it in the Advanced tab.`); return }
      kit = g.group.kit; name = g.group.name; description = g.group.description
    } catch (e) { alert(`Could not read '${path}': ${(e as Error).message}`); return }
  }

  let sel: { lane: LaneId; index: number } | null = null
  const view = { w: 9, h: 10, seed: 1, outlines: true }
  const dlg = openDialog(path === null ? (kit.kind === 'frame' ? 'New 9-slice frame' : 'New wall set') : `Texture set: ${path}`, { width: 1180, onClose: () => { unsub(); window.clearTimeout(timer) } })
  let timer = 0
  const nameIn = h('input', { type: 'text', class: 'search', style: 'width:240px', placeholder: 'folder/name', value: path ?? '' })
  if (path !== null) nameIn.setAttribute('readonly', '')
  const titleIn = h('input', { type: 'text', class: 'search', style: 'width:200px', placeholder: 'Title (optional)', value: name })
  titleIn.addEventListener('keydown', (e) => e.stopPropagation()); nameIn.addEventListener('keydown', (e) => e.stopPropagation())
  titleIn.addEventListener('input', () => { name = titleIn.value })
  const status = h('span', { class: 'dim' })
  const saveBtn = h('button', { class: 'primary' }, 'Save'), applyBtn = h('button', { title: 'Save, then apply to the selected faces / brushes' }, 'Save and apply to selection')
  const board = h('div', { class: 'ts-board' }), inspector = h('div', { class: 'ts-inspector' }), options = h('div', { class: 'ts-options' })
  const canvas = h('canvas', { width: '460', height: '400', style: 'background:#161616;border:1px solid #333;max-width:100%' })
  const problems = h('div', { class: 'gp-problems' })

  // ---- thumbnails
  const thumb = (p: string | null, size: number): HTMLCanvasElement => {
    const c = h('canvas', { width: String(size), height: String(size), class: 'ts-thumb' })
    const g = c.getContext('2d')
    if (g) {
      g.fillStyle = '#2a2a2a'; g.fillRect(0, 0, size, size)
      const st = p ? env.textures.get(p) : null
      if (st && st.status === 'ready') g.drawImage(st.image.source as CanvasImageSource, 0, 0, size, size)
      else { g.strokeStyle = '#777'; g.lineWidth = 1; g.strokeRect(2.5, 2.5, size - 5, size - 5); g.fillStyle = '#999'; g.font = `${Math.round(size / 2)}px sans-serif`; g.textAlign = 'center'; if (!p) g.fillText('+', size / 2, size * 0.68) }
    }
    return c
  }
  const choose = async (title: string): Promise<string | null> => pickTexture({ textures: env.textures, listTextures: env.listTextures, save: async () => {}, activeTexture: () => '' }, title)

  // ---- wall board
  const wall = (): WallKit => kit as WallKit
  const selected = () => (sel && kit.kind === 'wall' ? wall()[sel.lane].pieces[sel.index] ?? null : null)

  function pieceCard(lane: LaneId, index: number): HTMLElement {
    const p = wall()[lane].pieces[index]
    const cols = pieceCols(p)
    const grid = h('div', { class: 'ts-grid', style: `grid-template-columns:repeat(${cols},34px)` })
    for (const row of p.cells) for (const t of row) grid.append(thumb(t, 34))
    const tags = [weightLabel(p.weight), PLACE_SHORT[p.place], p.match ? `= ${p.match}` : ''].filter(Boolean).join(' · ')
    const card = h('div', { class: `ts-card${sel && sel.lane === lane && sel.index === index ? ' sel' : ''}`, draggable: 'true', title: 'Click to edit. Drag to another row. Right-click for more.' }, grid, h('div', { class: 'ts-tags' }, tags))
    card.addEventListener('click', () => { sel = { lane, index }; render() })
    card.addEventListener('dragstart', (e) => { e.dataTransfer?.setData('text/plain', JSON.stringify({ lane, index })); card.classList.add('drag') })
    card.addEventListener('dragend', () => card.classList.remove('drag'))
    card.addEventListener('contextmenu', (e) => {
      e.preventDefault(); sel = { lane, index }
      showMenu(e.clientX, e.clientY, [
        { label: 'Duplicate', run: () => { duplicatePiece(wall(), lane, index); render() } },
        { label: 'Delete', run: () => { removePiece(wall(), lane, index); sel = null; render() } },
      ], dlg.root.ownerDocument)
    })
    return card
  }

  function laneEl(lane: LaneId): HTMLElement {
    const k = wall(), l = k[lane], info = LANE_INFO[lane]
    const rowsMinus = h('button', { title: 'One tile row less per repeat' }, '-'), rowsPlus = h('button', { title: 'One more tile row: pieces can be two or more storeys tall' }, '+')
    rowsMinus.addEventListener('click', () => { setLaneRows(k, lane, l.rows - 1); render() }); rowsPlus.addEventListener('click', () => { setLaneRows(k, lane, l.rows + 1); render() })
    const add = h('button', { class: 'ts-add', title: `Add a texture to the ${info.title.toLowerCase()}` }, '+ Add')
    add.addEventListener('click', () => void choose(`Texture for the ${info.title.toLowerCase()}`).then((p) => { if (!p) return; const piece = addPiece(k, lane, p, 'any', lane === 'middle' ? 3 : 1); if (piece) sel = { lane, index: l.pieces.length - 1 }; render() }))
    const body = h('div', { class: 'ts-lane-body' }, ...l.pieces.map((_, i) => pieceCard(lane, i)), add)
    const el = h('div', { class: 'ts-lane' }, h('div', { class: 'ts-lane-head' }, h('b', {}, info.title), h('span', { class: 'dim' }, info.hint), h('span', { class: 'ts-spacer' }), h('span', { class: 'dim' }, `${l.rows} tile row${l.rows === 1 ? '' : 's'}`), rowsMinus, rowsPlus), body)
    el.addEventListener('dragover', (e) => { e.preventDefault(); el.classList.add('drop') })
    el.addEventListener('dragleave', () => el.classList.remove('drop'))
    el.addEventListener('drop', (e) => {
      e.preventDefault(); el.classList.remove('drop')
      try {
        const from = JSON.parse(e.dataTransfer?.getData('text/plain') ?? '') as { lane: LaneId; index: number }
        if (!LANES.includes(from.lane)) return
        const at = movePiece(k, from.lane, from.index, lane)
        if (at >= 0) sel = { lane, index: at }
        render()
      } catch { /* not ours */ }
    })
    return el
  }

  function inspectWall(): HTMLElement[] {
    const k = wall(), p = selected()
    if (!p || !sel) return [h('div', { class: 'dim' }, 'Click a piece to edit it: its textures, how often it is used and where. Drag pieces between rows. A piece with several textures always stays together.')]
    const { lane, index } = sel
    const step = (label: string, value: number, dec: () => void, inc: () => void) => { const m = h('button', {}, '-'), pl = h('button', {}, '+'); m.addEventListener('click', dec); pl.addEventListener('click', inc); return h('span', { class: 'ts-step' }, h('span', { class: 'dim' }, label), m, h('b', {}, String(value)), pl) }
    const cells = h('div', { class: 'ts-grid big', style: `grid-template-columns:repeat(${pieceCols(p)},64px)` })
    p.cells.forEach((row, r) => row.forEach((t, c) => { const th = thumb(t, 64); th.addEventListener('click', () => void choose('Texture for this tile').then((x) => { if (x) { setPieceCell(p, r, c, x); render() } })); cells.append(th) }))
    const freq = h('div', { class: 'ts-seg' }, ...WEIGHTS.map((w) => { const b = h('button', { class: Math.abs(p.weight - w.value) < 1e-6 ? 'active' : '' }, w.label); b.addEventListener('click', () => { p.weight = w.value; render() }); return b }))
    const where = h('select', {}, ...(Object.keys(PLACE_LABEL) as Place[]).map((pl) => { const o = h('option', { value: pl }, PLACE_LABEL[pl]); if (pl === p.place) o.setAttribute('selected', ''); return o }))
    where.addEventListener('change', () => { p.place = where.value as Place; render() })
    const rowSel = h('select', { title: 'Move to another row' }, ...LANES.map((l) => { const o = h('option', { value: l }, LANE_INFO[l].title); if (l === lane) o.setAttribute('selected', ''); return o }))
    rowSel.addEventListener('change', () => { const at = movePiece(k, lane, index, rowSel.value as LaneId); if (at >= 0) sel = { lane: rowSel.value as LaneId, index: at }; render() })
    const names = [...new Set(LANES.flatMap((l) => k[l].pieces.map((x) => x.match)).filter((x): x is string => !!x))]
    const matchList = h('datalist', { id: 'ts-matches' }, ...names.map((n) => h('option', { value: n })))
    const matchIn = h('input', { type: 'text', list: 'ts-matches', placeholder: 'any', maxlength: '32', style: 'width:140px', value: p.match ?? '' })
    matchIn.addEventListener('keydown', (e) => e.stopPropagation())
    matchIn.addEventListener('change', () => { const v = matchIn.value.trim(); p.match = /^[A-Za-z0-9 _\-]{1,32}$/.test(v) ? v : null; render() })
    const dup = h('button', {}, 'Duplicate'), del = h('button', {}, 'Delete')
    dup.addEventListener('click', () => { duplicatePiece(k, lane, index); render() }); del.addEventListener('click', () => { removePiece(k, lane, index); sel = null; render() })
    return [
      h('div', { class: 'ts-ins-row' }, h('b', {}, 'Selected piece'), step('wide', pieceCols(p), () => { fitPieceCols(p, pieceCols(p) - 1); render() }, () => { fitPieceCols(p, pieceCols(p) + 1); render() }),
        step('tall', pieceRows(p), () => { setLaneRows(k, lane, pieceRows(p) - 1); render() }, () => { setLaneRows(k, lane, pieceRows(p) + 1); render() }), h('span', { class: 'dim' }, 'Tall changes the whole row')),
      h('div', { class: 'dim' }, 'Click a square to choose its texture. Wide / tall make a block that is always placed as one.'), cells,
      h('div', { class: 'ts-ins-row' }, h('span', { class: 'dim' }, 'How often'), freq, h('span', { class: 'dim' }, 'Where'), where),
      h('div', { class: 'ts-ins-row' }, h('span', { class: 'dim', title: 'Pieces with different names never sit directly on each other (a framed window never on a plain window). Pieces without a name sit on anything.' }, 'Lines up with'), matchIn, matchList, h('span', { class: 'dim' }, 'same name = may stack; empty = any')),
      h('div', { class: 'ts-ins-row' }, h('span', { class: 'dim' }, 'Row'), rowSel, dup, del),
    ]
  }

  function frameBoard(): HTMLElement {
    const k = kit as Extract<Kit, { kind: 'frame' }>
    const grid = h('div', { class: 'ts-frame' })
    for (const s of ['tl', 't', 'tr', 'l', 'c', 'r', 'bl', 'b', 'br'] as FrameSlot[]) {
      const cell = h('div', { class: 'ts-slot', title: FRAME_LABEL[s] }, thumb(k.cells[s], 84), h('div', { class: 'ts-tags' }, FRAME_LABEL[s]))
      cell.addEventListener('click', () => void choose(FRAME_LABEL[s]).then((p) => { if (p) { k.cells[s] = p; render() } }))
      cell.addEventListener('contextmenu', (e) => { e.preventDefault(); showMenu(e.clientX, e.clientY, [{ label: 'Clear', run: () => { k.cells[s] = null; render() } }], dlg.root.ownerDocument) })
      grid.append(cell)
    }
    return h('div', {}, h('div', { class: 'dim' }, 'Corners keep their size. Edges repeat along their side, the middle repeats both ways: nothing is stretched except the tiny scaling that fits a whole number of tiles. Click a slot to choose its texture.'), grid)
  }

  function optionsRow(): HTMLElement[] {
    const auto = h('input', { type: 'checkbox' }); auto.checked = kit.tile === null
    const tw = h('input', { type: 'number', min: '0.1', step: '0.1', style: 'width:64px', value: fmt(kit.tile?.[0] ?? 1) }), th = h('input', { type: 'number', min: '0.1', step: '0.1', style: 'width:64px', value: fmt(kit.tile?.[1] ?? 1) })
    const setTile = () => { kit.tile = auto.checked ? null : [Math.max(0.1, Number(tw.value) || 1), Math.max(0.1, Number(th.value) || 1)]; render(false) }
    auto.addEventListener('change', () => { if (!auto.checked && !kit.tile) { const t = texSize(); tw.value = fmt(t[0]); th.value = fmt(t[1]) } setTile(); tw.toggleAttribute('disabled', auto.checked); th.toggleAttribute('disabled', auto.checked) })
    tw.addEventListener('change', setTile); th.addEventListener('change', setTile); tw.addEventListener('keydown', (e) => e.stopPropagation()); th.addEventListener('keydown', (e) => e.stopPropagation())
    tw.toggleAttribute('disabled', auto.checked); th.toggleAttribute('disabled', auto.checked)
    const ms = h('input', { type: 'number', min: '0', max: '500', step: '5', style: 'width:60px', value: String(Math.round(kit.maxStretch * 100)), title: 'How much a tile may be scaled to make a whole number fit. Beyond it tiles keep their size and the last one is cut off (or a row is left out). 0 = no limit.' })
    ms.addEventListener('change', () => { const v = Number(ms.value); if (Number.isFinite(v) && v >= 0) { kit.maxStretch = Math.min(v, 500) / 100; render(false) } }); ms.addEventListener('keydown', (e) => e.stopPropagation())
    const out: HTMLElement[] = [h('label', { class: 'numfield', title: 'How big one tile is on the wall' }, auto, h('span', {}, 'Tile size from the first texture')), h('label', { class: 'numfield' }, h('span', {}, 'or'), tw, h('span', {}, 'x'), th, h('span', {}, 'm'))]
    out.push(h('label', { class: 'numfield', title: ms.title }, h('span', {}, 'Max stretch'), ms, h('span', {}, '% (0 = no limit)')))
    if (kit.kind === 'wall') {
      const avoid = h('input', { type: 'checkbox', title: 'Do not put the same piece twice in a row when there is a choice' }); avoid.checked = kit.avoidRepeat
      avoid.addEventListener('change', () => { (kit as WallKit).avoidRepeat = avoid.checked; render(false) })
      out.push(h('label', { class: 'numfield' }, avoid, h('span', {}, 'Do not repeat the same piece twice in a row')))
    }
    return out
  }
  const texSize = (): [number, number] => {
    const first = [...(kit.kind === 'wall' ? LANES.flatMap((l) => (kit as WallKit)[l].pieces.flatMap((p) => p.cells.flat())) : Object.values(kit.cells))].find((t): t is string => !!t)
    const ti = first ? texEnv.info(first) : undefined
    return ti ? [Math.round((ti.width / ti.ppm) * 100) / 100, Math.round((ti.height / ti.ppm) * 100) / 100] : [1, 1]
  }

  // ---- preview
  function redrawPreview(): void {
    const parsed = parseGroup({ format: 1, name, kit: kitToJson(kit) })
    const g = parsed.group
    if (!g) { problems.replaceChildren(...[...parsed.errors, ...parsed.warnings].map((m) => h('div', { class: 'gp-err' }, m))); return }
    for (const t of groupTextures(g)) texEnv.info(t)
    const r = drawGroupPreview(canvas, g, { w: view.w, h: view.h, facing: 'wall' as Facing, seed: view.seed }, env.textures, { outlines: view.outlines, labels: false })
    const missing = groupTextures(g).filter((t) => env.textures.get(t).status === 'missing').map((t) => `texture '${t}' was not found in the assets folder`)
    const msgs = [...parsed.warnings, ...r.warnings, ...missing]
    problems.replaceChildren(...(msgs.length ? msgs.map((m) => h('div', { class: 'gp-warn' }, m)) : [h('div', { class: 'dim' }, 'No problems found.')]))
  }

  function render(boardToo = true): void {
    if (boardToo) {
      board.replaceChildren(...(kit.kind === 'wall' ? LANES.map(laneEl) : [frameBoard()]))
      inspector.replaceChildren(...(kit.kind === 'wall' ? inspectWall() : []))
    }
    options.replaceChildren(...optionsRow())
    redrawPreview()
  }

  // ---- toolbar and window
  const addMany = h('button', { title: 'Choose several textures: they sort themselves into rows by name (top, bottom, roof, door, window)' }, 'Add textures...')
  addMany.addEventListener('click', () => void pickTextures(env, 'Add textures to the set').then((paths) => { if (!paths.length) return; const n = addTextures(wall(), paths); status.textContent = `${n} piece${n === 1 ? '' : 's'} added: check the rows, drag to correct`; render() }))
  const num = (label: string, get: () => number, set: (v: number) => void, min: number, step = 0.5) => {
    const i = h('input', { type: 'number', min: String(min), step: String(step), value: fmt(get()), style: 'width:70px' })
    i.addEventListener('change', () => { const v = Number(i.value); if (v >= min) { set(v); redrawPreview() } }); i.addEventListener('keydown', (e) => e.stopPropagation())
    return h('label', { class: 'numfield' }, h('span', {}, label), i)
  }
  const seedIn = num('seed', () => view.seed, (v) => { view.seed = Math.trunc(v) }, 0, 1)
  const reroll = h('button', {}, 'Re-roll'); reroll.addEventListener('click', () => { view.seed = Math.floor(Math.random() * 1e6); (seedIn.querySelector('input') as HTMLInputElement).value = String(view.seed); redrawPreview() })
  const outl = h('input', { type: 'checkbox' }); outl.checked = true; outl.addEventListener('change', () => { view.outlines = outl.checked; redrawPreview() })
  const controls = h('div', { class: 'gp-controls' }, num('width m', () => view.w, (v) => { view.w = v }, 0.5), num('height m', () => view.h, (v) => { view.h = v }, 0.5), seedIn, reroll, h('label', { class: 'numfield' }, outl, h('span', {}, 'outlines')))

  const doSave = async (): Promise<string | null> => {
    const n = nameIn.value.trim()
    if (!NAME_OK.test(n) || n.length > 120) { status.textContent = 'Name: letters, digits, _ and - with / for folders'; return null }
    const g = parseGroup({ format: 1, kit: kitToJson(kit) })
    if (!g.group) { status.textContent = 'The set is not usable yet'; return null }
    try { await env.save(n, { format: 1, name: name || undefined, ...(description ? { description } : {}), kit: kitToJson(kit), root: {} /* placeholder: a Core built before simple sets refuses a group without "root"; this editor reads `kit` first and ignores it */ }); status.textContent = `Saved ${n}`; env.saved(); return n } catch (e) { status.textContent = `Not saved: ${(e as Error).message}`; return null }
  }
  saveBtn.addEventListener('click', () => void doSave())
  applyBtn.addEventListener('click', async () => { const n = await doSave(); if (n) { const c = env.apply(n); status.textContent = c ? `Saved and applied to ${c} face(s)` : 'Saved. Select a brush or a face to apply it' } })
  const unsub = env.textures.onChange(() => { if (dlg.root.isConnected) { render() } })

  dlg.body.append(
    h('div', { class: 'gp-bar' }, nameIn, titleIn, saveBtn, applyBtn, status),
    h('div', { class: 'gp-body' },
      h('div', { class: 'gp-left', style: 'flex:1 1 56%' }, kit.kind === 'wall' ? h('div', { class: 'gp-bar' }, addMany) : h('span'), board, inspector, options),
      h('div', { class: 'gp-right', style: 'flex:1 1 44%' }, controls, canvas, problems)))
  render()
}
