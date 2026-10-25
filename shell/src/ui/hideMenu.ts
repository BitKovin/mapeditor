// The viewport's "Hide" button: hides whole classes of entities (trigger_*, editor_playable_area, ...) by rule, plus a checklist of the brush
// entity classes (volumes) found in the map. A view setting: nothing here is saved in the map or undone, and hidden entities stay in the map and the export.
import { setHideRules } from '../state/actions'
import type { Store } from '../state/store'
import { addRule, matchingRule, normalizePattern, volumeClasses } from '../model/visibility'
import type { HideRule } from '../model/visibility'
import { h } from './dom'

export interface HideMenu { wrap: HTMLElement; menu: HTMLElement; button: HTMLElement; refresh(): void }

export function buildHideMenu(store: Store): HideMenu {
  const button = h('button', { class: 'chip', title: 'Hide entities by class: triggers, editor volumes, your own rules. Hidden things are not drawn or picked but stay in the map.' }, 'Hide ▾')
  const menu = h('div', { class: 'vp-menu hide-menu', style: 'display:none' })
  const rulesBox = h('div', { class: 'hide-rules' })
  const classesBox = h('div', { class: 'hide-classes' })

  const input = h('input', { type: 'text', class: 'hide-input', placeholder: 'class or pattern, e.g. func_* ', title: 'A class name; * matches anything (trigger_* = every class starting with trigger_)', spellcheck: 'false' })
  const add = h('button', { class: 'chip', title: 'Add this rule and hide what it matches' }, 'Add')
  const commit = () => {
    if (!normalizePattern(input.value)) return
    setHideRules(store, addRule(store.state.hideRules, input.value))
    input.value = ''
  }
  add.addEventListener('click', commit)
  input.addEventListener('keydown', (e) => { e.stopPropagation(); if (e.key === 'Enter') commit() })
  input.addEventListener('keyup', (e) => e.stopPropagation())
  const clear = h('button', { class: 'chip', title: 'Switch every rule off (hidden brushes and entities from the outliner stay hidden)' }, 'Show all classes')
  clear.addEventListener('click', () => setHideRules(store, store.state.hideRules.map((r) => ({ ...r, enabled: false }))))

  menu.append(
    h('div', { class: 'hide-title dim' }, 'Hide by class'), rulesBox,
    h('div', { class: 'hide-add' }, input, add),
    h('div', { class: 'hide-title dim' }, 'Volumes in this map'), classesBox,
    h('div', { class: 'hide-foot' }, clear),
  )

  const setRule = (i: number, patch: Partial<HideRule>) => setHideRules(store, store.state.hideRules.map((r, j) => (j === i ? { ...r, ...patch } : r)))

  const refresh = () => {
    const st = store.state, rules = st.hideRules
    const active = rules.filter((r) => r.enabled).length
    button.textContent = active ? `Hide (${active}) ▾` : 'Hide ▾'
    button.classList.toggle('on', active > 0)
    if (menu.style.display === 'none') return  // the lists are built when the menu opens
    rulesBox.replaceChildren(...rules.map((r, i) => {
      const cb = h('input', { type: 'checkbox' }); cb.checked = r.enabled
      cb.addEventListener('change', () => setRule(i, { enabled: cb.checked }))
      const row = h('label', { class: 'vp-menu-row' }, cb, h('span', { class: 'hide-pattern' }, r.pattern))
      if (r.custom) {
        const x = h('button', { class: 'hide-x', title: 'Remove this rule' }, '✕')
        x.addEventListener('click', (ev) => { ev.preventDefault(); ev.stopPropagation(); setHideRules(store, rules.filter((_, j) => j !== i)) })
        row.append(x)
      }
      return row
    }))
    const classes = volumeClasses(st.doc.entities)
    classesBox.replaceChildren(...(classes.length ? classes.map(({ classname, count }) => {
      const n = normalizePattern(classname)
      const exact = rules.find((r) => normalizePattern(r.pattern) === n)
      const by = matchingRule(classname, rules)
      const viaOther = !!by && by !== exact && by.enabled
      const cb = h('input', { type: 'checkbox' }); cb.checked = !!by
      if (viaOther) { cb.disabled = true; cb.title = `Hidden by the rule ${by!.pattern}` }
      cb.addEventListener('change', () => {
        if (cb.checked) setHideRules(store, addRule(rules, classname))
        else setHideRules(store, rules.flatMap((r) => (normalizePattern(r.pattern) !== n ? [r] : r.custom ? [] : [{ ...r, enabled: false }])))
      })
      return h('label', { class: `vp-menu-row${viaOther ? ' dim' : ''}`, title: viaOther ? `Hidden by the rule ${by!.pattern}` : '' }, cb, h('span', { class: 'hide-pattern' }, classname), h('span', { class: 'dim' }, `×${count}`))
    }) : [h('div', { class: 'dim hide-empty' }, 'No brush entities yet')]))
  }

  button.addEventListener('click', (ev) => {
    ev.stopPropagation()
    menu.style.display = menu.style.display === 'none' ? '' : 'none'
    refresh()
  })
  const wrap = h('span', { class: 'vp-menu-wrap' }, button, menu)
  refresh()
  return { wrap, menu, button, refresh }
}
