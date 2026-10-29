// Typed property editors: every key a class defines in the .ent is shown with its value (the default when unset) and a widget chosen by the key's type
// (the .ent element name): a checkbox for boolean, a number field for real / integer / angle, three for real3 / vector, a colour picker for color1 / color255,
// a drop-down for list, the entity picker for target. Unknown types are text. The pure parts (kind, colour, clamping) are unit tested; the DOM by the e2e.
import type { EntityClassDef, EntityKeyDef } from '../net/protocol'
import type { Entity } from '../model/document'
import { h } from './dom'

/** How a key is edited. 'ref' is the entity picker; the caller builds it (it needs the document), see InspectorPanel.refRow. */
export type FieldKind = 'ref' | 'asset' | 'boolean' | 'list' | 'color01' | 'color255' | 'integer' | 'real' | 'vec3' | 'text'

const REF_TYPES = new Set(['target', 'target_destination', 'killtarget'])
const REAL_TYPES = new Set(['real', 'float', 'angle', 'direction'])
const INT_TYPES = new Set(['integer', 'int'])
const VEC_TYPES = new Set(['real3', 'vector', 'vec3', 'angles', 'origin'])
const COLOR01_TYPES = new Set(['color1', 'color'])
// a file of the game folder (any format): the text of its path and a button that opens the file picker. `model` is the classic .ent name for a model file.
const ASSET_TYPES = new Set(['asset', 'model'])

/** Opens the file picker for an asset key; resolves to the game-folder relative path, or null when cancelled. Installed by the app (it owns the connection). */
export type AssetPicker = (key: Pick<EntityKeyDef, 'key' | 'name' | 'type'>, current: string) => Promise<string | null>
let assetPicker: AssetPicker | null = null
export const setAssetPicker = (fn: AssetPicker | null): void => { assetPicker = fn }

export const isRefType = (type: string): boolean => REF_TYPES.has(type)

export function fieldKind(k: Pick<EntityKeyDef, 'type' | 'items'>): FieldKind {
  if (isRefType(k.type)) return 'ref'
  if (ASSET_TYPES.has(k.type)) return 'asset'
  if (k.items?.length) return 'list'
  if (k.type === 'boolean' || k.type === 'bool') return 'boolean'
  if (COLOR01_TYPES.has(k.type)) return 'color01'
  if (k.type === 'color255') return 'color255'
  if (INT_TYPES.has(k.type)) return 'integer'
  if (REAL_TYPES.has(k.type)) return 'real'
  if (VEC_TYPES.has(k.type)) return 'vec3'
  return 'text'
}

/** The keys of a class that are edited as typed properties: every defined key, in .ent order, except `hidden` (keys the form edits elsewhere). */
export const propertyKeys = (def?: EntityClassDef, hidden: ReadonlySet<string> = new Set()): EntityKeyDef[] => (def?.keys ?? []).filter((k) => !hidden.has(k.key))

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
/** color255 stores "r g b" as 0..255 integers. */
export const formatColor255 = (c: readonly [number, number, number]): string => c.map((x) => String(Math.round(clamp01(x) * 255))).join(' ')

/** A typed number inside the .ent's range; integers are rounded. NaN stays NaN (the caller keeps the old value). */
export function clampValue(k: EntityKeyDef, raw: number): number {
  if (!Number.isFinite(raw)) return NaN
  let v = fieldKind(k) === 'integer' ? Math.round(raw) : raw
  if (k.min !== undefined) v = Math.max(k.min, v)
  if (k.max !== undefined) v = Math.min(k.max, v)
  return v
}

/** "x y z" -> three numbers; missing or bad components are null. */
export function parseVec3(v: string): [number | null, number | null, number | null] {
  const p = v.trim() ? v.trim().split(/[\s,]+/).map(Number) : []
  return [0, 1, 2].map((i) => (Number.isFinite(p[i]) ? p[i] : null)) as [number | null, number | null, number | null]
}

export const currentValue = (e: Entity, k: EntityKeyDef): string => e.keyvalues?.[k.key] ?? k.default ?? ''
export const isDefault = (e: Entity, k: EntityKeyDef): boolean => e.keyvalues?.[k.key] === undefined || e.keyvalues[k.key] === k.default
export const isChecked = (v: string): boolean => { const n = Number(v); return Number.isFinite(n) ? n !== 0 : v.trim() !== '' && v.trim().toLowerCase() !== 'false' }

const num = (v: number): string => String(+v.toFixed(6))
const isNumber = (v: string): boolean => v.trim() !== '' && Number.isFinite(Number(v))

/** The value widget of one non-reference key. `onChange(null)` = remove the key (back to the default). */
function widgetFor(k: EntityKeyDef, value: string, isSet: boolean, onChange: (value: string | null) => void, revert: () => string): HTMLElement {
  const kind = fieldKind(k)
  if (kind === 'boolean') {
    const cb = h('input', { type: 'checkbox' })
    cb.checked = isChecked(value)
    cb.addEventListener('change', () => onChange(cb.checked ? '1' : '0'))
    return cb
  }
  if (kind === 'asset') {
    const row = h('div', { class: 'asset' })
    const inp = h('input', { value, ...(k.default === undefined && !isSet ? { placeholder: 'unset' } : {}) })
    inp.addEventListener('keydown', (ev) => ev.stopPropagation())
    inp.addEventListener('change', () => onChange(inp.value === '' && k.default === undefined ? null : inp.value))
    const browse = h('button', { class: 'browse', title: `Choose the file (${k.name}) in the game folder`, ...(assetPicker ? {} : { disabled: '' }) }, '…')
    browse.addEventListener('click', (ev) => {
      ev.preventDefault()
      void assetPicker?.(k, inp.value).then((p) => { if (p !== null && p !== '') { inp.value = p; onChange(p) } })
    })
    row.append(inp, browse)
    return row
  }
  if (kind === 'color01' || kind === 'color255') {
    const raw = parseColor1(value)
    const c = raw ? (kind === 'color255' ? (raw.map((x) => x / 255) as [number, number, number]) : raw) : [1, 1, 1] as [number, number, number]
    const pick = h('input', { type: 'color', value: colorToHex(c), title: `${k.name}: ${value || '(unset)'}` })
    pick.addEventListener('change', () => { const n = hexToColor(pick.value); if (n) onChange(kind === 'color255' ? formatColor255(n) : formatColor(n)) })
    return pick
  }
  if (kind === 'list') {
    const items = k.items!
    const sel = h('select', {}, ...items.map((i) => h('option', { value: i.value }, i.name)))
    if (!isSet && !items.some((i) => i.value === value)) sel.prepend(h('option', { value: '' }, '—'))
    else if (!items.some((i) => i.value === value)) sel.prepend(h('option', { value }, value)) // keep values the definition doesn't list
    sel.value = value
    sel.addEventListener('change', () => onChange(sel.value === '' && !isSet ? null : sel.value))
    return sel
  }
  if ((kind === 'integer' || kind === 'real') && (value === '' || isNumber(value))) {
    const inp = h('input', { type: 'number', value: value === '' ? '' : num(Number(value)), step: k.step !== undefined ? String(k.step) : kind === 'integer' ? '1' : 'any' })
    if (k.min !== undefined) inp.min = String(k.min)
    if (k.max !== undefined) inp.max = String(k.max)
    inp.addEventListener('keydown', (ev) => ev.stopPropagation())
    inp.addEventListener('change', () => {
      if (inp.value.trim() === '') { onChange(null); return }
      const v = clampValue(k, Number(inp.value))
      if (Number.isNaN(v)) { inp.value = revert(); return }
      inp.value = num(v)
      onChange(num(v))
    })
    return inp
  }
  if (kind === 'vec3' && (value.trim() === '' || (value.trim().split(/[\s,]+/).length === 3 && parseVec3(value).every((x) => x !== null)))) {
    const cur = parseVec3(value)
    const row = h('div', { class: 'vec' })
    const inputs = ([0, 1, 2] as const).map((i) => {
      const inp = h('input', { type: 'number', step: k.step !== undefined ? String(k.step) : 'any', value: cur[i] === null ? '' : num(cur[i]!) })
      inp.addEventListener('keydown', (ev) => ev.stopPropagation())
      row.append(inp)
      return inp
    })
    for (const inp of inputs) inp.addEventListener('change', () => {
      const vals = inputs.map((x) => (x.value.trim() === '' ? 0 : Number(x.value)))
      if (!vals.every(Number.isFinite)) { const back = parseVec3(revert()); inputs.forEach((x, i) => (x.value = back[i] === null ? '' : num(back[i]!))); return }
      onChange(vals.map(num).join(' '))
    })
    return row
  }
  const inp = h('input', { value, ...(k.default === undefined && !isSet ? { placeholder: 'unset' } : {}) })
  inp.addEventListener('keydown', (ev) => ev.stopPropagation())
  inp.addEventListener('change', () => onChange(inp.value === '' && k.default === undefined ? null : inp.value))
  return inp
}

/** The editor of one non-reference key: label, widget, reset. `onChange(null)` = back to the default (the key is removed). */
export function propertyField(k: EntityKeyDef, e: Entity, ro: boolean, onChange: (value: string | null) => void): HTMLElement {
  const isSet = e.keyvalues?.[k.key] !== undefined
  const widget = widgetFor(k, currentValue(e, k), isSet, onChange, () => currentValue(e, k))
  const hasDefault = k.default !== undefined
  const reset = h('button', { class: 'reset', title: hasDefault ? `Back to the default (${k.default})` : 'Remove the key' }, hasDefault ? '↺' : '×')
  if (isDefault(e, k)) reset.setAttribute('disabled', '')
  reset.addEventListener('click', () => onChange(null))
  if (ro) { for (const x of [widget, ...widget.querySelectorAll('input')]) x.setAttribute('disabled', ''); reset.setAttribute('disabled', '') }
  const title = [k.key !== k.name ? `${k.key} (${k.type})` : k.type, k.description].filter(Boolean).join('\n')
  return h('label', { class: `field prop${isDefault(e, k) ? '' : ' changed'}`, title }, h('span', {}, k.name), widget, reset)
}
