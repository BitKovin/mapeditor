// The "Layout" section of the Surface Inspector: what a texture group laid out on the selected face(s) lets the author change (REQUIREMENTS §20 T8).
import { groupEnv, ParamDef, ParamValue } from '../model/texgroup'
import { clearFaceLayout, rerollLayoutSeed, selectedLayout, setFaceLayout } from '../state/actions'
import type { Store } from '../state/store'
import { fmt, h } from './dom'

export function layoutSection(store: Store, ro: boolean, openGroup: (path: string) => void): HTMLElement[] {
  const sel = selectedLayout(store)
  if (!sel) return []
  const { layout, count, total } = sel
  const group = groupEnv.get(layout.group)
  const stop = (e: Event) => e.stopPropagation()
  const dis = <T extends HTMLElement>(el: T): T => { if (ro) el.setAttribute('disabled', ''); return el }
  const row = (label: string, ...cells: HTMLElement[]) => h('div', { class: 'si-row' }, h('span', { class: 'si-label' }, label), ...cells)

  const param = (name: string, d: ParamDef): HTMLElement => {
    const cur = (layout.params[name] ?? d.default) as ParamValue
    // a value equal to the default is not stored (null), so a face only keeps what it changed
    const set = (v: ParamValue) => setFaceLayout(store, { params: { [name]: v === d.default ? null : v } })
    if (d.type === 'bool') {
      const c = dis(h('input', { type: 'checkbox' })); c.checked = cur === true
      c.addEventListener('change', () => set(c.checked)); return row(d.label, c)
    }
    if (d.type === 'choice') {
      const s = dis(h('select', {}, ...d.options.map((o) => { const op = h('option', { value: o }, o); if (o === cur) op.setAttribute('selected', ''); return op })))
      s.addEventListener('change', () => set(s.value)); return row(d.label, s)
    }
    const i = dis(h('input', { type: 'number', step: String(d.step), min: String(d.min), max: String(d.max), value: fmt(cur as number), title: `${fmt(d.min)} to ${fmt(d.max)}` }))
    i.addEventListener('change', () => { const v = Number(i.value); if (Number.isFinite(v)) set(v) })
    i.addEventListener('keydown', stop)
    return row(d.label, i)
  }

  const seed = dis(h('input', { type: 'number', step: '1', placeholder: 'automatic', value: layout.seed === null ? '' : String(layout.seed), title: 'Empty = derived from the face (stable). A number fixes the random choices; the same number gives the same result on every face.' }))
  seed.addEventListener('change', () => { const v = seed.value.trim(); setFaceLayout(store, { seed: v === '' ? null : Math.trunc(Number(v)) >>> 0 }) })
  seed.addEventListener('keydown', stop)
  const reroll = dis(h('button', { title: 'Pick a new random seed for every selected face' }, 'Re-roll'))
  reroll.addEventListener('click', () => rerollLayoutSeed(store))
  const rot = dis(h('select', { title: 'Turns the layout on the face' }, ...[0, 90, 180, 270].map((d) => { const o = h('option', { value: String(d) }, `${d} deg`); if (d === layout.rotate) o.setAttribute('selected', ''); return o })))
  rot.addEventListener('change', () => setFaceLayout(store, { rotate: Number(rot.value) as 0 | 90 | 180 | 270 }))
  const flip = dis(h('input', { type: 'checkbox', title: 'Mirror the layout left to right' })); flip.checked = layout.flipU
  flip.addEventListener('change', () => setFaceLayout(store, { flipU: flip.checked }))
  const edit = h('button', { title: 'Open the group in the editor' }, 'Edit group...'); edit.addEventListener('click', () => openGroup(layout.group))
  const clear = dis(h('button', { title: 'Back to one plain texture (the face keeps its texture)' }, 'Clear layout'))
  clear.addEventListener('click', () => clearFaceLayout(store))

  const state = group ? '' : 'The group is not loaded (missing file, or its rules have an error): the face is drawn plainly.'
  return [
    h('div', { class: 'section' }, 'Texture group layout'),
    h('div', { class: 'si-row' }, h('span', { class: 'si-label' }, 'Group'), h('span', { class: 'si-name', title: layout.group }, `${layout.group}${count < total ? ` (${count} of ${total} faces)` : ''}`)),
    ...(state ? [h('div', { class: 'problems' }, state)] : []),
    ...(group ? Object.entries(group.params).map(([n, d]) => param(n, d)) : []),
    row('Seed', seed, reroll),
    row('Rotate / flip', rot, h('label', { class: 'numfield' }, h('span', {}, 'flip'), flip)),
    h('div', { class: 'btns' }, edit, clear),
  ]
}
