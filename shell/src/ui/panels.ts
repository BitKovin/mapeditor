import { fieldKind, propertyField, propertyKeys } from './properties'
import type { IContentRenderer } from 'dockview-core'
import { Entity, PrefabInstance, RefValue, classOf, refPaths, instanceNames, isBrushEntity, originOf, showsCustomOrigin } from '../model/document'
import { refKeysOf, refTargets, RefTarget } from '../model/refs'
import { parseScaleVec, setEntityAngle, setEntityScale, setEntityRef, setOverrideRef, shareTargetName, renameInstance, revertOverride, setBrushOp, setCarve, setClassname, setInstanceVec, setKeyvalue, setOrigin, setOverrideField, setOverrideKeyvalue, editShape, convertShape } from '../state/actions'
import { ParamShape, ShapePatch, ShapeSpec, shapeOf } from '../model/parametric'
import { Turn, defaultShapeParams } from '../model/shapes'
import { specOf as terrainSpecOf } from '../model/terrain'
import type { EntityClassDef } from '../net/protocol'
import type { Store } from '../state/store'
import { wholeEntityIfSole } from '../state/store'
import { toggleHidden } from '../state/actions'
import { brushKey, entityKey, hiddenState, instanceKey, ruleMatcher } from '../model/visibility'
import { fmt, h } from './dom'

const EYE_ON = '<svg viewBox="0 0 16 16" width="14" height="14" aria-hidden="true"><path d="M1 8s2.6-5 7-5 7 5 7 5-2.6 5-7 5-7-5-7-5z" fill="none" stroke="currentColor" stroke-width="1.3"/><circle cx="8" cy="8" r="2.2" fill="currentColor"/></svg>'
const EYE_OFF = '<svg viewBox="0 0 16 16" width="14" height="14" aria-hidden="true"><path d="M1 8s2.6-5 7-5 7 5 7 5-2.6 5-7 5-7-5-7-5z" fill="none" stroke="currentColor" stroke-width="1.3"/><path d="M2 14L14 2" stroke="currentColor" stroke-width="1.5"/></svg>'

abstract class Panel implements IContentRenderer {
  readonly element = h('div', { class: 'panel' })
  private unsub: (() => void) | null = null
  constructor(protected store: Store) {}
  protected abstract refresh(): void
  init(): void { this.unsub = this.store.subscribe(() => this.refresh()); this.refresh() }
  dispose(): void { this.unsub?.() }
}

export class OutlinerPanel extends Panel {
  private sig = ''
  /** The search text: rows whose label does not contain every word of it are left out (an entity stays while any of its brushes matches). */
  private query = ''
  /** Entities whose brushes are listed. Everything starts collapsed; a selected brush highlights its collapsed entity instead of opening it. */
  private readonly expanded = new Set<string>()
  private readonly search = h('input', { type: 'search', class: 'outliner-search', placeholder: 'Search outliner…', title: 'Filter by class, target name, id, brush or prefab name (Esc clears)', 'aria-label': 'Search outliner', spellcheck: 'false', autocomplete: 'off' }) as HTMLInputElement
  private readonly bar = h('div', { class: 'outliner-bar' }, this.search)
  init(): void {
    this.search.addEventListener('input', () => { this.query = this.search.value; this.refresh() })
    this.search.addEventListener('keydown', (ev) => {
      ev.stopPropagation() // typing must not trigger viewport / app shortcuts
      if (ev.key === 'Escape' && this.search.value) { ev.preventDefault(); this.search.value = ''; this.query = ''; this.refresh() }
    })
    super.init()
  }
  /** The selection the outliner last scrolled to: only a changed selection moves the list (an unrelated rebuild must not take the scroll position away). */
  private revealed = ''
  protected refresh(): void {
    const st = this.store.state
    // Rebuilding on every drag tick would eat pointer events; only rebuild when the tree or selection changed.
    const sig = JSON.stringify([st.doc.entities.map((e) => [e.id, e.classname, e.keyvalues?.targetname, e.brushes?.map((b) => b.id)]), st.doc.prefabInstances.map((p) => [p.id, p.file, p.name]), st.selection, st.multi.length, st.problems, st.hidden, st.hideRules, this.query, [...this.expanded]])
    if (sig === this.sig) return
    this.sig = sig
    const root = h('div', { class: 'tree' })
    const hid = new Set(st.hidden), ruled = ruleMatcher(st.hideRules)
    const words = this.query.toLowerCase().split(/\s+/).filter(Boolean)
    const filtering = words.length > 0
    const hit = (text: string) => { const t = text.toLowerCase(); return words.every((w) => t.includes(w)) }
    let shown = 0
    for (const e of st.doc.entities) {
      const target = e.keyvalues?.targetname?.trim() ?? ''
      const eText = `${e.classname ?? 'point_entity'} ${e.id} ${target}`
      const eHit = !filtering || hit(eText)
      const spec0 = terrainSpecOf(e)
      const brushHit = (b: NonNullable<Entity['brushes']>[number]) => hit(`${eText} ${b.op === 'subtract' ? 'subtract' : 'add'} brush ${b.id}`)
      if (filtering && !eHit && !(spec0 ? hit(`${eText} terrain`) : (e.brushes ?? []).some(brushHit))) continue
      shown++
      const spec = terrainSpecOf(e)
      const nBrushes = spec ? 0 : e.brushes?.length ?? 0
      // while searching, an entity that is listed only for a matching brush is open so the brush can be seen
      const open = nBrushes > 0 && (this.expanded.has(e.id) || (filtering && !eHit))
      const inSel = st.selection?.entityId === e.id
      const isSel = inSel && (!st.selection!.brushId || (nBrushes > 0 && !open))  // a brush picked inside a collapsed entity lights the entity's row
      const hs = hiddenState(hid, ruled, e)
      const row = h('div', { class: `row${isSel ? ' sel' : ''}${hs !== 'no' ? ' hid' : ''}`, 'data-key': entityKey(e.id) }, this.twisty(e.id, nBrushes, open), this.eye(hs, entityKey(e.id)), `${e.classname ?? 'point_entity'}`, target ? h('span', { class: 'target', title: 'targetname' }, ` “${target}”`) : '', h('span', { class: 'dim' }, ` ${e.id}${nBrushes && !open ? ` · ${nBrushes} brush${nBrushes === 1 ? '' : 'es'}` : ''}`))
      row.addEventListener('click', () => this.pick(hs !== 'no', { entityId: e.id }))
      root.append(row)
      // a terrain is hundreds of tile brushes: one row for all of them (REQUIREMENTS §39)
      if (spec) {
        if (filtering && !eHit && !hit(`${eText} terrain`)) continue
        const tr = h('div', { class: `row child${hs !== 'no' ? ' hid' : ''}`, 'data-key': entityKey(e.id) + '#terrain' }, `terrain ${spec.cols} x ${spec.rows}`, h('span', { class: 'dim' }, ` ${e.brushes?.length ?? 0} tiles`))
        tr.addEventListener('click', () => this.pick(hs !== 'no', { entityId: e.id }))
        root.append(tr)
        continue
      }
      if (!open) continue
      for (const b of e.brushes ?? []) {
        if (filtering && !eHit && !brushHit(b)) continue
        const bsel = st.selection?.entityId === e.id && st.selection.brushId === b.id
        const bs = hiddenState(hid, ruled, e, b.id)
        const br = h('div', { class: `row child${bsel ? ' sel' : ''}${bs !== 'no' ? ' hid' : ''}`, 'data-key': brushKey(e.id, b.id) }, this.eye(bs, brushKey(e.id, b.id)), `${b.op === 'subtract' ? '− ' : '+ '}brush`, h('span', { class: 'dim' }, ` ${b.id}`))
        br.addEventListener('click', () => this.pick(bs !== 'no', wholeEntityIfSole(this.store.state.doc, { entityId: e.id, brushId: b.id })))
        root.append(br)
      }
    }
    const names = instanceNames(st.doc.prefabInstances)
    st.doc.prefabInstances.forEach((p, i) => {
      if (filtering && !hit(`${names[i]} ${p.file} prefab`)) return
      shown++
      const isSel = st.multi.some((m) => m.instanceId === p.id) || st.selection?.instanceId === p.id
      const ph = hid.has(instanceKey(p.id))
      const row = h('div', { class: `row prefab${isSel ? ' sel' : ''}${ph ? ' hid' : ''}`, 'data-key': instanceKey(p.id) }, this.eye(ph ? 'self' : 'no', instanceKey(p.id)), '▣ ', names[i], h('span', { class: 'dim' }, ` ${p.file}`))
      row.addEventListener('click', (ev) => (ph ? this.pick(true, { entityId: '', instanceId: p.id }) : ev.ctrlKey || ev.metaKey ? this.store.toggleSelect({ entityId: '', instanceId: p.id }) : this.store.select({ entityId: '', instanceId: p.id })))
      root.append(row)
    })
    if (!st.doc.entities.length && !st.doc.prefabInstances.length) root.append(h('div', { class: 'dim pad' }, 'Empty map. Add a box from the toolbar.'))
    else if (filtering && !shown) root.append(h('div', { class: 'dim pad' }, `Nothing matches “${this.query.trim()}”.`))
    if (st.problems.length) root.append(h('div', { class: 'problems' }, ...st.problems.map((p) => h('div', {}, `⚠ ${p}`))))
    // the search box is re-attached on every rebuild: keep the caret in it when the user is typing
    const typing = this.element.ownerDocument.activeElement === this.search
    const caret: [number | null, number | null] = [this.search.selectionStart, this.search.selectionEnd]
    const scrollTop = this.element.scrollTop  // a rebuild (an eye clicked, a fold) must not move the list
    this.element.replaceChildren(this.bar, root)
    this.element.scrollTop = scrollTop
    if (typing) { this.search.focus(); try { this.search.setSelectionRange(caret[0], caret[1]) } catch { /* not supported for this input type */ } }
    this.revealSelection(root)
  }

  /** The fold arrow of an entity row with brushes (an empty spacer of the same width for the others, so the rows line up). */
  private twisty(entityId: string, brushes: number, open: boolean): HTMLElement {
    if (!brushes) return h('span', { class: 'twisty none' })
    const t = h('button', { class: 'twisty', title: open ? 'Collapse brushes' : 'Show brushes', 'aria-label': open ? 'Collapse brushes' : 'Show brushes', 'aria-expanded': String(open) }, open ? '▾' : '▸')
    t.addEventListener('click', (ev) => {
      ev.stopPropagation()
      if (this.expanded.has(entityId)) this.expanded.delete(entityId); else this.expanded.add(entityId)
      this.refresh()
    })
    t.addEventListener('dblclick', (ev) => ev.stopPropagation())
    return t
  }

  /** The eye of a row: open = shown, struck through = hidden by this row's own key (click shows it again). Hidden through the entity or a class rule: it says so and cannot be switched here. */
  private eye(state: 'no' | 'self' | 'rule' | 'parent', key: string): HTMLElement {
    const hidden = state !== 'no'
    const title = state === 'rule' ? 'Hidden by a Hide rule (viewport > Hide)' : state === 'parent' ? 'Hidden with its entity' : hidden ? 'Hidden: click to show' : 'Click to hide (stays in the map)'
    const b = h('button', { class: `eye${hidden ? ' off' : ''}${state === 'rule' || state === 'parent' ? ' locked' : ''}`, title, 'aria-label': title, 'aria-pressed': String(hidden) })
    b.innerHTML = hidden ? EYE_OFF : EYE_ON
    b.addEventListener('click', (ev) => { ev.stopPropagation(); if (state === 'no' || state === 'self') toggleHidden(this.store, key) })
    b.addEventListener('dblclick', (ev) => ev.stopPropagation())
    return b
  }
  /** A hidden thing cannot be selected (it could be moved or deleted unseen): the click only says how to get it back. */
  private pick(hidden: boolean, sel: Parameters<Store['select']>[0]): void {
    if (hidden) this.store.setMessage('Hidden: click its eye in the Outliner to show it again')
    else this.store.select(sel)
  }

  /** Scrolls the row of the primary selection into view (a brush: its own row; an entity picked as a whole, a point entity, a terrain: the entity's row; a prefab instance: its row). */
  private revealSelection(root: HTMLElement): void {
    const sel = this.store.state.selection
    const key = sel ? `${sel.entityId}|${sel.brushId ?? ''}|${sel.instanceId ?? ''}` : ''
    if (key === this.revealed) return
    this.revealed = key
    if (!sel) return
    const rows = [...root.querySelectorAll<HTMLElement>('[data-key]')]
    const find = (k: string) => rows.find((r) => r.dataset.key === k)
    const row = sel.instanceId !== undefined ? find(`i:${sel.instanceId}`)
      : (sel.brushId !== undefined ? find(brushKey(sel.entityId, sel.brushId)) : undefined) ?? find(entityKey(sel.entityId))
    if (row) revealIn(this.element, row)
  }
}

/** Scrolls `row` to the middle of `scroller` when it is not fully inside it (nothing moves for a row that is already visible, e.g. one just clicked). */
export function revealIn(scroller: HTMLElement, row: HTMLElement): void {
  const a = scroller.getBoundingClientRect(), b = row.getBoundingClientRect()
  if (b.top >= a.top && b.bottom <= a.bottom) return
  row.scrollIntoView?.({ block: 'center' })
}

export class InspectorPanel extends Panel {
  private sig = ''
  private selKey = ''
  // refresh() keeps the panel as it is while an input of it has focus (typing must not lose the caret) and leaves `sig` stale: when the focus leaves
  // (blur, Tab, a click elsewhere) the panel has to catch up, or the controls show the old state (a reset button that stays disabled after an edit)
  init(): void {
    super.init()
    this.element.addEventListener('focusout', () => queueMicrotask(() => setTimeout(() => this.refresh(), 0)))
  }
  protected refresh(): void {
    const st = this.store.state
    const sel = st.selection
    const inst = sel?.instanceId !== undefined ? st.doc.prefabInstances.find((p) => p.id === sel.instanceId) : undefined
    const e = sel && sel.instanceId === undefined ? st.doc.entities.find((x) => x.id === sel.entityId) : undefined
    const contents = inst ? st.prefabEntities.filter((r) => r.instancePath[1] === inst.id).map((r) => [r.instancePath, r.sourceEntityId, r.entity.classname, r.entity.keyvalues, r.entity.refs]) : null
    // a terrain entity holds hundreds of brushes: do not serialize them on every edit of a stroke
    const eSig = e && e.terrain !== undefined ? { ...e, brushes: e.brushes?.length } : e
    const sig = JSON.stringify([inst, contents, this.openPath, eSig, sel?.brushId, this.store.canEdit, st.entityDefs.length, st.entityDefs.find((c) => c.name === e?.classname)?.name])
    if (sig === this.sig) return
    // Don't rebuild while the user is typing in one of our inputs (they'd lose focus mid-edit).
    // Leave `sig` stale so the panel refreshes as soon as focus leaves.
    // A different selection always rebuilds: the focused input then belongs to an entity the user is no longer editing.
    const selKey = JSON.stringify([inst?.id, e?.id, sel?.brushId])
    const selectionChanged = selKey !== this.selKey
    if (!selectionChanged && this.element.contains(this.element.ownerDocument.activeElement)) return
    this.selKey = selKey
    this.sig = sig
    if (inst) return void this.element.replaceChildren(this.buildInstance(inst))
    if (!e) return void this.element.replaceChildren(h('div', { class: 'dim pad' }, 'Nothing selected.'))
    this.element.replaceChildren(this.build(e, sel!.brushId ?? (isBrushEntity(e) && classOf(e) !== 'worldspawn' && e.brushes?.length === 1 ? e.brushes[0].id : undefined)))
  }

  /** A reference: one picker per target, plus an empty one to add another (entities that share a name are triggered together). `set(null)` clears / reverts. */
  private refRow(key: string, current: RefValue | undefined, exclude: string | null, ro: boolean, set: (target: RefValue | null) => void, note = '', label = key): HTMLElement {
    const st = this.store.state
    const targets = refTargets(st.doc, st.prefabEntities, st.library)
    const paths = refPaths(current)
    const apply = (list: string[]) => set(list.length ? (list.length === 1 ? list[0] : list) : null)
    const picker = (value: string, noneLabel: string, change: (v: string) => void): HTMLSelectElement => {
      const sel = h('select', {}, h('option', { value: '' }, noneLabel))
      const groups = new Map<string, RefTarget[]>()
      for (const t of targets) if (t.path !== exclude) groups.set(t.group, [...(groups.get(t.group) ?? []), t])
      for (const [g, list] of groups) sel.append(h('optgroup', { label: g || 'This map' }, ...list.map((t) => h('option', { value: t.path }, t.label))))
      if (value && !targets.some((t) => t.path === value)) sel.append(h('option', { value }, `⚠ ${value} (missing)`))
      sel.value = value
      if (ro) sel.setAttribute('disabled', '')
      sel.addEventListener('change', () => change(sel.value))
      return sel
    }
    const rows: HTMLElement[] = paths.map((p, i) => {
      const clr = h('button', { title: paths.length > 1 ? 'Remove this target' : 'Clear / revert' }, '×')
      if (ro) clr.setAttribute('disabled', '')
      clr.addEventListener('click', () => apply(paths.filter((_, j) => j !== i)))
      return h('div', { class: 'refrow' }, picker(p, '— none —', (v) => apply(v ? paths.map((x, j) => (j === i ? v : x)) : paths.filter((_, j) => j !== i))), clr)
    })
    const pip = h('button', { title: paths.length ? 'Pick another target in the viewport' : 'Pick an entity in the viewport' }, '⌖')
    if (ro) pip.setAttribute('disabled', '')
    pip.addEventListener('click', () => this.store.setPipette((p) => { if (p) apply([...paths, p]) }))
    rows.push(h('div', { class: 'refrow' }, picker('', paths.length ? '+ add a target' : '— none —', (v) => { if (v) apply([...paths, v]) }), pip))
    // one key holds one name: targets with different names cannot all be reached (Core writes the first one's name)
    const names = [...new Set(paths.map((p) => targets.find((t) => t.path === p)?.resolvedName ?? '').filter(Boolean))]
    if (paths.length > 1 && names.length > 1) {
      const share = h('button', { title: 'Give every target of this document the name of the first' }, 'Share first name')
      if (ro || paths.some((p) => p.includes('/'))) share.setAttribute('disabled', '')
      share.addEventListener('click', () => shareTargetName(this.store, paths))
      rows.push(h('div', { class: 'refwarn' }, `The targets have different names (${names.join(', ')}): only the first is written. `, share))
    }
    return h('label', { class: `field kv ref${rows.length > 1 ? ' multi' : ''}`, title: [label !== key ? key : '', note].filter(Boolean).join('\n') }, h('span', {}, label), h('div', { class: 'refmulti' }, ...rows))
  }

  /** Entity (inside the open instance) whose keyvalues are being edited; a path relative to the instance. */
  private openPath: string | null = null

  private buildInstance(inst: PrefabInstance): HTMLElement {
    const st = this.store.state
    const ro = !this.store.canEdit
    const root = h('div', { class: 'form' })
    const field = (label: string, input: HTMLElement) => root.append(h('label', { class: 'field' }, h('span', {}, label), input))
    const disable = (...els: HTMLElement[]) => { if (ro) for (const x of els) x.setAttribute('disabled', '') }

    const nameIn = h('input', { value: instanceNames(st.doc.prefabInstances)[st.doc.prefabInstances.findIndex((p) => p.id === inst.id)] ?? '' })
    disable(nameIn)
    nameIn.addEventListener('change', () => { if (!renameInstance(this.store, inst.id, nameIn.value)) nameIn.value = inst.name ?? '' })
    field('name', nameIn)
    field('file', h('input', { value: inst.file, disabled: '' }))
    field('id', h('input', { value: inst.id, disabled: '' }))
    const vec = (label: string, key: 'origin' | 'angles' | 'scale', dflt: number) => {
      const cur = inst[key] ?? [dflt, dflt, dflt]
      const row = h('div', { class: 'vec' })
      ;([0, 1, 2] as const).forEach((i) => {
        const inp = h('input', { type: 'number', step: key === 'angles' ? '5' : '0.05', value: fmt(cur[i]) })
        disable(inp)
        inp.addEventListener('change', () => { if (!setInstanceVec(this.store, inst.id, key, i, Number(inp.value))) inp.value = fmt(cur[i]) })
        row.append(inp)
      })
      field(label, row)
    }
    vec('origin', 'origin', 0); vec('angles (°)', 'angles', 0); vec('scale', 'scale', 1)

    // ---- contents: every entity of the prefab (children included); click one to override its values for THIS instance
    const mine = st.prefabEntities.filter((r) => r.instancePath[1] === inst.id)
    root.append(h('div', { class: 'section' }, `Contents (${mine.length})`))
    if (!mine.length) root.append(h('div', { class: 'dim' }, 'The prefab file has no entities (or is not loaded).'))
    for (const r of mine) {
      const path = [...r.instancePath.slice(2), r.sourceEntityId].join('/')
      const ov = inst.overrides?.[path]
      const label = r.entity.keyvalues?.targetname || r.entity.classname || r.sourceEntityId
      const open = this.openPath === path
      const row = h('div', { class: `row child${open ? ' sel' : ''}` }, `${open ? '▾' : '▸'} ${label}`, h('span', { class: 'dim' }, ` ${path}${ov ? ' ●' : ''}`))
      row.addEventListener('click', () => { this.openPath = open ? null : path; this.sig = ''; this.refresh() })
      root.append(row)
      if (open) root.append(this.overrideEditor(inst, path, r.entity, ov, ro))
    }

    // ---- overrides summary (also lists deleted entities, which no longer appear in Contents)
    const all = Object.entries(inst.overrides ?? {})
    if (all.length) {
      root.append(h('div', { class: 'section' }, `Overrides (${all.length})`))
      for (const [path, o] of all) {
        const parts = [o.deleted ? 'deleted' : '', ...Object.keys(o.keyvalues ?? {}), ...Object.keys(o.refs ?? {}).map((k) => `ref ${k}`), ...(['origin', 'angles', 'scale'] as const).filter((k) => o[k])].filter(Boolean)
        const undo = h('button', { title: o.deleted ? 'Restore' : 'Revert all overrides of this entity' }, o.deleted ? 'Restore' : 'Revert')
        disable(undo)
        undo.addEventListener('click', () => revertOverride(this.store, inst.id, path))
        root.append(h('div', { class: 'field kv' }, h('span', { title: path }, path), h('span', { class: 'dim' }, parts.join(', ')), undo))
      }
    }
    return root
  }

  private overrideEditor(inst: PrefabInstance, path: string, eff: Entity, ov: PrefabInstance['overrides'] extends infer T ? (T extends Record<string, infer O> ? O | undefined : never) : never, ro: boolean): HTMLElement {
    const box = h('div', { class: 'form' })
    for (const [k, v] of Object.entries(eff.keyvalues ?? {})) {
      const inp = h('input', { value: v })
      const overridden = ov?.keyvalues?.[k] !== undefined
      const rev = h('button', { title: 'Revert to the prefab value' }, '↺')
      if (!overridden) rev.setAttribute('disabled', '')
      if (ro) { inp.setAttribute('disabled', ''); rev.setAttribute('disabled', '') }
      inp.addEventListener('change', () => setOverrideKeyvalue(this.store, inst.id, path, k, inp.value))
      rev.addEventListener('click', () => setOverrideKeyvalue(this.store, inst.id, path, k, null))
      box.append(h('label', { class: `field kv${overridden ? ' overridden' : ''}` }, h('span', {}, k), inp, rev))
    }
    const defs = this.store.state.entityDefs
    const refs = refKeysOf(defs.find((c) => c.name === eff.classname), [...Object.keys(eff.refs ?? {}), ...Object.keys(ov?.refs ?? {})])
    const chain = path.split('/').slice(0, -1) // instance ids between the open instance and the entity
    const own = [inst.id, ...chain].join('/')
    for (const k of refs) {
      const over = ov?.refs?.[k]
      // the prefab file's own ref is relative to the entity's own scope; show it in root terms so it matches the list
      const fileRef = eff.refs?.[k] !== undefined && over === undefined ? refPaths(eff.refs[k]).map((r) => `${own}/${r}`) : undefined
      box.append(this.refRow(k, over ?? fileRef, null, ro, (p) => setOverrideRef(this.store, inst.id, path, k, p), over !== undefined ? 'Overridden in this instance' : 'From the prefab file'))
    }
    const nk = h('input', { placeholder: 'new key' }), nv = h('input', { placeholder: 'value' }), add = h('button', {}, 'Add')
    const del = h('button', { title: 'Remove this entity from this instance only' }, 'Delete in instance')
    if (ro) for (const x of [nk, nv, add, del]) x.setAttribute('disabled', '')
    add.addEventListener('click', () => { if (nk.value.trim()) setOverrideKeyvalue(this.store, inst.id, path, nk.value.trim(), nv.value) })
    del.addEventListener('click', () => { this.openPath = null; setOverrideField(this.store, inst.id, path, 'deleted', true) })
    box.append(h('div', { class: 'field kv' }, nk, nv, add), del)
    return box
  }

  private build(e: Entity, brushId?: string): HTMLElement {
    const ro = !this.store.canEdit
    const root = h('div', { class: 'form' })
    const field = (label: string, input: HTMLElement) => root.append(h('label', { class: 'field' }, h('span', {}, label), input))

    const defs = this.store.state.entityDefs
    const def = defs.find((c) => c.name === e.classname)
    const cls = h('input', { value: classOf(e), list: 'classnames' })
    root.append(h('datalist', { id: 'classnames' }, ...defs.map((c) => h('option', { value: c.name }, `${c.kind === 'group' ? 'brush' : 'point'}${c.description ? ' - ' + c.description.split('\n')[0].slice(0, 60) : ''}`))))
    if (ro) cls.setAttribute('disabled', '')
    cls.addEventListener('change', () => { setClassname(this.store, e.id, cls.value.trim()); if (this.store.state.doc.entities.find((x) => x.id === e.id)?.classname !== cls.value.trim()) cls.value = classOf(e) })
    field('classname', cls)
    field('id', h('input', { value: e.id, disabled: '' }))
    {  // the entity's name comes before every other key (REQUIREMENTS §17 #5): other entities' targets point at it
      const tn = h('input', { value: e.keyvalues?.targetname ?? '', placeholder: 'no name', title: 'targetname: the name other entities use to target this one' })
      if (ro) tn.setAttribute('disabled', '')
      tn.addEventListener('keydown', (ev) => ev.stopPropagation())
      tn.addEventListener('change', () => setKeyvalue(this.store, e.id, 'targetname', tn.value.trim() ? tn.value.trim() : null))
      field('targetname', tn)
    }
    if (def?.description) root.append(h('div', { class: 'dim ent-desc' }, def.description.length > 400 ? def.description.slice(0, 400) + '…' : def.description))

    if (!isBrushEntity(e) || showsCustomOrigin(e)) {
      const o = originOf(e)
      const row = h('div', { class: 'vec' })
      ;([0, 1, 2] as const).forEach((i) => {
        const inp = h('input', { type: 'number', step: '0.05', value: fmt(o[i]) })
        if (ro) inp.setAttribute('disabled', '')
        inp.addEventListener('change', () => setOrigin(this.store, e.id, i, Number(inp.value)))
        row.append(inp)
      })
      field(showsCustomOrigin(e) ? 'origin (pivot)' : 'origin', row)
    }

    if (brushId) {
      const b = e.brushes?.find((x) => x.id === brushId)
      if (b) {
        root.append(h('div', { class: 'section' }, `Brush ${b.id}`))
        const op = h('select', {}, h('option', { value: 'add' }, 'Additive'), h('option', { value: 'subtract' }, 'Subtractive'))
        op.value = b.op ?? 'add'
        if (ro) op.setAttribute('disabled', '')
        op.addEventListener('change', () => setBrushOp(this.store, e.id, b.id, op.value as 'add' | 'subtract'))
        field('operation', op)
        if (b.op === 'subtract') {
          const c = b.carve ?? {}
          const box = (label: string, key: 'self' | 'structural' | 'nonStructural', def: boolean) => {
            const cb = h('input', { type: 'checkbox' })
            cb.checked = c[key] ?? def
            if (ro) cb.setAttribute('disabled', '')
            cb.addEventListener('change', () => setCarve(this.store, e.id, b.id, key, cb.checked))
            return h('label', { class: 'check' }, cb, label)
          }
          root.append(h('div', { class: 'checks' }, h('div', { class: 'dim' }, 'Carves brushes of:'),
            box('its own entity', 'self', true), box('structural entities', 'structural', true), box('other non-structural entities', 'nonStructural', false)))
        }
        root.append(h('div', { class: 'dim' }, `${b.vertices.length} vertices · ${b.faces.length} faces`))
        const sh = shapeOf(b)
        if (sh) this.shapeSection(root, e.id, sh, ro)
      }
    }

    if (isBrushEntity(e) && classOf(e) !== 'worldspawn') {  // `_vertex_lit` (Core, bsp_compile): the entity keeps no lightmap texels in the export; its light is only in the vertex colour
      const vl = h('input', { type: 'checkbox', title: 'Export without lightmaps: the light is read at the vertices and stored in the vertex colour (smaller files, coarser light; only light style 0). Good for small detail geometry.' }) as HTMLInputElement
      const kv = e.keyvalues?._vertex_lit
      vl.checked = kv !== undefined && kv !== '' && kv !== '0' && kv !== 'false'
      if (ro) vl.setAttribute('disabled', '')
      vl.addEventListener('change', () => setKeyvalue(this.store, e.id, '_vertex_lit', vl.checked ? '1' : null))
      root.append(h('div', { class: 'checks' }, h('label', { class: 'check', title: vl.title }, vl, 'Vertex lit only (no lightmap)')))
    }

    if (def?.flags.length) this.flagBoxes(root, e, def, ro)

    const vec3 = (label: string, cur: [number, number, number], step: string, set: (i: 0 | 1 | 2, v: number) => boolean) => {
      const row = h('div', { class: 'vec' })
      ;([0, 1, 2] as const).forEach((i) => {
        const inp = h('input', { type: 'number', step, value: fmt(cur[i]) })
        if (ro) inp.setAttribute('disabled', '')
        inp.addEventListener('change', () => { if (!set(i, Number(inp.value))) inp.value = fmt(cur[i]) })
        row.append(inp)
      })
      field(label, row)
    }
    vec3('angles (°)', e.angles ?? [0, 0, 0], '5', (i, v) => setEntityAngle(this.store, e.id, i, v))
    if (!isBrushEntity(e) && e.classname !== 'decal')
      vec3('scale', parseScaleVec(e.keyvalues?.modelscale_vec), '0.05', (i, v) => setEntityScale(this.store, e.id, i, v))

    // every key the class defines, in .ent order, with a widget chosen by its type (properties.ts); a target key is the entity picker with its pipette.
    // Keys the form edits elsewhere are left out: targetname (top), angle / angles (the angles row), modelscale_vec (the scale row), spawnflags (the checkboxes)
    const hidden = new Set(['classname', 'targetname', 'origin', 'angle', 'angles', '_vertex_lit'])
    if (!isBrushEntity(e) && e.classname !== 'decal') hidden.add('modelscale_vec')
    if (def?.flags.length) hidden.add('spawnflags')
    const typed = propertyKeys(def, hidden)
    if (typed.length) {
      root.append(h('div', { class: 'section' }, def!.name.startsWith('light_') ? 'Light' : 'Properties'))
      for (const k of typed) {
        if (fieldKind(k) === 'ref') root.append(this.refRow(k.key, e.refs?.[k.key], e.id, ro, (p) => setEntityRef(this.store, e.id, k.key, p), k.description, k.name))
        else root.append(propertyField(k, e, ro, (v) => setKeyvalue(this.store, e.id, k.key, v)))
      }
    }
    const shown = new Set([...hidden, ...typed.map((k) => k.key)])
    // keys the class doesn't define: references set on the entity (or just added) keep their picker, the rest are text.
    // A plain-text value of a target key (an imported name the picker can't show) stays visible here too.
    const pending = this.pendingRefs.get(e.id) ?? new Set<string>()
    for (const k of Object.keys(e.refs ?? {})) pending.delete(k)
    const otherRefs = [...new Set([...Object.keys(e.refs ?? {}), ...pending])].filter((k) => !shown.has(k))
    const otherKv = Object.entries(e.keyvalues ?? {}).filter(([k]) => !shown.has(k) || typed.some((t) => t.key === k && fieldKind(t) === 'ref'))
    root.append(h('div', { class: 'section' }, typed.length ? 'Other keyvalues' : 'Keyvalues'))
    for (const k of otherRefs)
      root.append(this.refRow(k, e.refs?.[k], e.id, ro, (p) => { pending.delete(k); setEntityRef(this.store, e.id, k, p) }))
    for (const [k, v] of otherKv) {
      const inp = h('input', { value: v })
      const del = h('button', { title: 'Remove' }, '×')
      if (ro) { inp.setAttribute('disabled', ''); del.setAttribute('disabled', '') }
      inp.addEventListener('keydown', (ev) => ev.stopPropagation())
      inp.addEventListener('change', () => setKeyvalue(this.store, e.id, k, inp.value))
      del.addEventListener('click', () => setKeyvalue(this.store, e.id, k, null))
      root.append(h('label', { class: 'field kv' }, h('span', {}, k), inp, del))
    }
    // adding a reference key (target, killtarget, ...) adds an empty picker row instead of a text value
    const refKeys = refKeysOf(def).filter((k) => !shown.has(k) && !otherRefs.includes(k))
    const nk = h('input', { placeholder: 'new key', list: 'keynames' }), nv = h('input', { placeholder: 'value' })
    root.append(h('datalist', { id: 'keynames' }, ...refKeys.map((k) => h('option', { value: k }, 'reference'))))
    const addBtn = h('button', {}, 'Add')
    if (ro) for (const x of [nk, nv, addBtn]) x.setAttribute('disabled', '')
    nk.addEventListener('keydown', (ev) => ev.stopPropagation())
    nv.addEventListener('keydown', (ev) => ev.stopPropagation())
    addBtn.addEventListener('click', () => {
      const k = nk.value.trim()
      if (!k) return
      if (refKeysOf(def).includes(k) && !nv.value) {
        this.pendingRefs.set(e.id, pending.add(k))
        this.sig = ''
        addBtn.blur() // the panel does not rebuild while one of its controls has focus
        this.refresh()
      } else setKeyvalue(this.store, e.id, k, nv.value)
    })
    root.append(h('div', { class: 'field kv' }, nk, nv, addBtn))
    if (pending.size) this.pendingRefs.set(e.id, pending)
    else this.pendingRefs.delete(e.id)
    return root
  }

  /** Reference keys added with "Add" that have no target yet (they only exist as an empty picker row until one is chosen). */
  private pendingRefs = new Map<string, Set<string>>()

  /** Details > Shape: the generator of a placed cylinder / wedge / arch / stairs, editable at any time (REQUIREMENTS §22 P1). */
  private shapeSection(root: HTMLElement, entityId: string, s: ShapeSpec, ro: boolean): void {
    const names: Record<ParamShape, string> = { cylinder: 'Cylinder', wedge: 'Wedge', arch: 'Arch', stairs: 'Stairs', sphere: 'Sphere' }
    root.append(h('div', { class: 'section' }, `Shape: ${names[s.kind]}${s.parts > 1 ? ` (part ${s.part + 1} of ${s.parts})` : ''}`))
    const p = { ...defaultShapeParams(), ...s.params }
    const num = (label: string, value: number, min: number, max: number, step: number, apply: (v: number) => ShapePatch, int = false) => {
      const inp = h('input', { type: 'number', min: String(min), max: String(max), step: String(step), value: fmt(value) })
      if (ro) inp.setAttribute('disabled', '')
      inp.addEventListener('keydown', (ev) => ev.stopPropagation())
      inp.addEventListener('change', () => {
        let v = Number(inp.value)
        if (!Number.isFinite(v)) { inp.value = fmt(value); return }
        v = Math.min(max, Math.max(min, int ? Math.round(v) : v))
        if (!editShape(this.store, entityId, s.group, apply(v))) inp.value = fmt(value)
      })
      root.append(h('label', { class: 'field' }, h('span', {}, label), inp))
    }
    if (s.kind === 'cylinder') num('sides', p.sides, 3, 64, 1, (v) => ({ params: { sides: v } }), true)
    if (s.kind === 'stairs') num('steps', p.steps, 2, 64, 1, (v) => ({ params: { steps: v } }), true)
    if (s.kind === 'sphere') {
      num('segments', p.segments, 4, 32, 1, (v) => ({ params: { segments: v } }), true)
      num('rings', p.rings, 2, 16, 1, (v) => ({ params: { rings: v } }), true)
    }
    if (s.kind === 'cylinder' || s.kind === 'sphere') {
      const sm = h('input', { type: 'checkbox', title: 'The round edges are marked smooth (light bake and export shade it round)' }) as HTMLInputElement
      sm.checked = p.smooth !== false
      if (ro) sm.setAttribute('disabled', '')
      sm.addEventListener('change', () => { if (!editShape(this.store, entityId, s.group, { params: { smooth: sm.checked } })) sm.checked = !sm.checked })
      root.append(h('label', { class: 'field' }, h('span', {}, 'smooth'), sm))
    }
    if (s.kind === 'arch') {
      num('segments', p.archSegments, 3, 64, 1, (v) => ({ params: { archSegments: v } }), true)
      num('wall thickness (m)', p.archThickness, 0.01, 100, 0.05, (v) => ({ params: { archThickness: v } }))
    }
    num('width (m)', s.frame.du, 0.01, 10000, 0.25, (v) => ({ size: { du: v } }))
    num('depth (m)', s.frame.dv, 0.01, 10000, 0.25, (v) => ({ size: { dv: v } }))
    num('height (m)', s.frame.h, 0.01, 10000, 0.25, (v) => ({ size: { h: v } }))
    const turn = h('select', { title: 'Which way the shape faces in its box (quarter turns about its height)' }, ...[0, 1, 2, 3].map((t) => h('option', { value: String(t) }, `${t * 90}°`)))
    turn.value = String(s.turn)
    if (ro) turn.setAttribute('disabled', '')
    turn.addEventListener('change', () => editShape(this.store, entityId, s.group, { turn: Number(turn.value) as Turn }))
    root.append(h('label', { class: 'field' }, h('span', {}, 'orientation'), turn))
    const conv = h('button', { title: 'Keep the brushes as they are and forget the generator (a mesh edit does this too)' }, 'Convert to brushes')
    if (ro) conv.setAttribute('disabled', '')
    conv.addEventListener('click', () => convertShape(this.store, entityId, s.group))
    root.append(h('div', { class: 'pad' }, conv), h('div', { class: 'dim' }, 'Editing a part in Mesh mode, clipping it or dragging its faces turns the shape into ordinary brushes.'))
  }

  /** The class's spawnflags as checkboxes; they edit the `spawnflags` keyvalue. */
  private flagBoxes(root: HTMLElement, e: Entity, def: EntityClassDef, ro: boolean): void {
    const current = Number.parseInt(e.keyvalues?.spawnflags ?? '0', 10) || 0
    root.append(h('div', { class: 'section' }, 'Spawnflags'))
    const checks = h('div', { class: 'checks' })
    for (const f of [...def.flags].sort((a, b) => a.bit - b.bit)) {
      const cb = h('input', { type: 'checkbox' })
      cb.checked = (current & (1 << f.bit)) !== 0
      if (ro) cb.setAttribute('disabled', '')
      cb.addEventListener('change', () => {
        const now = Number.parseInt(this.store.state.doc.entities.find((x) => x.id === e.id)?.keyvalues?.spawnflags ?? '0', 10) || 0
        const next = cb.checked ? now | (1 << f.bit) : now & ~(1 << f.bit)
        setKeyvalue(this.store, e.id, 'spawnflags', next ? String(next) : null)
      })
      checks.append(h('label', { class: 'check', title: f.description || f.name }, cb, f.name))
    }
    root.append(checks)
  }
}

export class HistoryPanel extends Panel {
  protected refresh(): void {
    const { history, historyIndex } = this.store.state
    const list = h('div', { class: 'tree' })
    history.forEach((entry, i) => {
      const row = h('div', { class: `row${i === historyIndex ? ' sel' : ''}${i > historyIndex ? ' dim' : ''}` }, entry.label)
      row.addEventListener('click', () => this.store.jumpTo(i))
      list.append(row)
    })
    this.element.replaceChildren(list)
  }
}
