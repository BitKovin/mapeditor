// Typed property editors for entity classes whose .ent keys carry a `default` (the modern light entities): every defined key is shown with its value (the
// default when unset), a widget that fits its type, the .ent's min / max / step, and a reset button. Classic Quake 3 classes (no defaults) keep the plain
// key / value list. The pure parts (colour, clamping) are unit tested; the DOM is covered by the browser e2e.
import type { EntityClassDef, EntityKeyDef } from '../net/protocol'
import type { Entity } from '../model/document'
import { h } from './dom'

/** The keys of a class that are edited as typed properties: those with a default. */
export const propertyKeys = (def?: EntityClassDef): EntityKeyDef[] => (def?.keys ?? []).filter((k) => k.default !== undefined)

/** "r g b" (each 0..1) -> numbers, or null when it is not three numbers. */
export function parseColor1(v: string): [number, number, number] | null {
  const p = v.trim().split(/\s+/).map(Number)
  return p.length === 3 && p.every(Number.isFinite) ? [p[0], p[1], p[2]] : null
}
const clamp01 = (x: number): number => Math.min(1, Math.max(0, x))
export function colorToHex(c: readonly [number, number, number]): string {
  return '#' + c.map((x) => Math.round(clamp01(x) * 255).toString(16).padStart(2, '0')).join('')
}
export function hexToColor(hex: string): [number, number, number] | null {
  const m = /^#?([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(hex.trim())
  return m ? [parseInt(m[1], 16) / 255, parseInt(m[2], 16) / 255, parseInt(m[3], 16) / 255] : null
}
/** The stored form: three numbers, at most 4 decimals, no trailing zeros. */
export const formatColor = (c: readonly [number, number, number]): string => c.map((x) => String(+clamp01(x).toFixed(4))).join(' ')

/** A typed number inside the .ent's range; integers are rounded. NaN stays NaN (the caller keeps the old value). */
export function clampValue(k: EntityKeyDef, raw: number): number {
  if (!Number.isFinite(raw)) return NaN
  let v = k.type === 'integer' ? Math.round(raw) : raw
  if (k.min !== undefined) v = Math.max(k.min, v)
  if (k.max !== undefined) v = Math.min(k.max, v)
  return v
}

export const currentValue = (e: Entity, k: EntityKeyDef): string => e.keyvalues?.[k.key] ?? k.default ?? ''
export const isDefault = (e: Entity, k: EntityKeyDef): boolean => e.keyvalues?.[k.key] === undefined || e.keyvalues[k.key] === k.default
export const isChecked = (v: string): boolean => { const n = Number(v); return Number.isFinite(n) ? n !== 0 : v.trim() !== '' && v.trim().toLowerCase() !== 'false' }

/** The editor of one key: [widget, reset button]. `onChange(null)` = back to the default (the key is removed). */
export function propertyField(k: EntityKeyDef, e: Entity, ro: boolean, onChange: (value: string | null) => void): HTMLElement {
  const value = currentValue(e, k)
  let widget: HTMLElement
  if (k.type === 'boolean') {
    const cb = h('input', { type: 'checkbox' })
    cb.checked = isChecked(value)
    cb.addEventListener('change', () => onChange(cb.checked ? '1' : '0'))
    widget = cb
  } else if (k.type === 'color1') {
    const c = parseColor1(value) ?? [1, 1, 1]
    const pick = h('input', { type: 'color', value: colorToHex(c), title: `${k.name}: ${formatColor(c)}` })
    pick.addEventListener('change', () => { const n = hexToColor(pick.value); if (n) onChange(formatColor(n)) })
    widget = pick
  } else if (k.items?.length) {
    const sel = h('select', {}, ...k.items.map((i) => h('option', { value: i.value }, i.name)))
    sel.value = value
    sel.addEventListener('change', () => onChange(sel.value))
    widget = sel
  } else if (k.type === 'real' || k.type === 'integer' || k.type === 'angle') {
    const inp = h('input', { type: 'number', value: String(+Number(value).toFixed(6)), step: String(k.step ?? (k.type === 'integer' ? 1 : 0.1)) })
    if (k.min !== undefined) inp.min = String(k.min)
    if (k.max !== undefined) inp.max = String(k.max)
    inp.addEventListener('change', () => {
      const v = clampValue(k, Number(inp.value))
      if (Number.isNaN(v)) { inp.value = String(+Number(currentValue(e, k)).toFixed(6)); return }
      inp.value = String(v)
      onChange(String(v))
    })
    widget = inp
  } else {
    const inp = h('input', { value })
    inp.addEventListener('change', () => onChange(inp.value))
    widget = inp
  }
  const reset = h('button', { class: 'reset', title: `Back to the default (${k.default})` }, '↺')
  if (isDefault(e, k)) reset.setAttribute('disabled', '')
  reset.addEventListener('click', () => onChange(null))
  if (ro) { widget.setAttribute('disabled', ''); reset.setAttribute('disabled', '') }
  return h('label', { class: `field prop${isDefault(e, k) ? '' : ' changed'}`, title: k.description }, h('span', {}, k.name), widget, reset)
}
