// Level settings panel (REQUIREMENTS §22 P2): light brightness, ambient light, colour correction, baked ambient occlusion, optimisation and the
// bake quality multiplier. Every value is a worldspawn key (model/levelSettings.ts); an empty field = the default. Each change is one undo step.
import type { IContentRenderer } from 'dockview-core'
import { LEVEL_SECTIONS, LevelField, ambientKey, ambientOf, colorOfHex, hexOf, levelKeyFor, levelValue, worldspawnOf } from '../model/levelSettings'
import { setLevelKey } from '../state/actions'
import type { Store } from '../state/store'
import { fmt, h } from './dom'

export class LevelSettingsPanel implements IContentRenderer {
  readonly element = h('div', { class: 'panel form level-settings' })
  private unsub: (() => void) | null = null
  private sig = ''
  constructor(private store: Store) {}
  init(): void { this.unsub = this.store.subscribe(() => this.refresh()); this.refresh() }
  dispose(): void { this.unsub?.() }

  private refresh(): void {
    const world = worldspawnOf(this.store.state.doc)
    // rebuild only when the stored values changed, so typing in a field is never interrupted
    const sig = JSON.stringify([world?.id, world?.keyvalues ?? {}, this.store.canEdit])
    if (sig === this.sig) return
    this.sig = sig
    const ro = !this.store.canEdit
    const rows: HTMLElement[] = []
    const reset = (key: string, has: boolean) => {
      const b = h('button', { class: 'icon', title: 'Back to the default' }, '↺')
      if (!has || ro) b.setAttribute('disabled', '')
      b.addEventListener('click', () => setLevelKey(this.store, key, null))
      return b
    }
    const numberRow = (f: LevelField) => {
      const v = levelValue(world, f)
      const inp = h('input', { type: 'number', min: String(f.min), max: String(f.max), step: String(f.step), value: v === null ? '' : fmt(v), placeholder: f.dflt === null ? 'lighting.json' : fmt(f.dflt), title: f.hint })
      if (ro) inp.setAttribute('disabled', '')
      inp.addEventListener('keydown', (e) => e.stopPropagation())
      inp.addEventListener('change', () => setLevelKey(this.store, f.key, levelKeyFor(f, inp.value)))
      return h('label', { class: 'field', title: f.hint }, h('span', {}, f.label), h('span', { class: 'row-split' }, inp, reset(f.key, v !== null)))
    }
    for (const sec of LEVEL_SECTIONS) {
      rows.push(h('div', { class: 'section' }, sec.title))
      if (sec.note) rows.push(h('div', { class: 'dim' }, sec.note))
      for (const f of sec.fields) rows.push(numberRow(f))
      if (sec.title === 'Light') {
        // ambient light: a colour and an intensity, stored as worldspawn `_ambient` "r g b"
        const amb = ambientOf(world)
        const col = h('input', { type: 'color', value: hexOf(amb.color), title: 'Colour of the ambient light' })
        const k = h('input', { type: 'number', min: '0', max: '10', step: '0.01', value: fmt(amb.intensity), title: 'Brightness of the ambient light added to every texel; 0 = none' })
        if (ro) { col.setAttribute('disabled', ''); k.setAttribute('disabled', '') }
        k.addEventListener('keydown', (e) => e.stopPropagation())
        const apply = () => setLevelKey(this.store, '_ambient', ambientKey(colorOfHex(col.value), Number(k.value)))
        col.addEventListener('change', apply)
        k.addEventListener('change', apply)
        rows.push(h('label', { class: 'field', title: 'Light added to every texel (worldspawn _ambient)' }, h('span', {}, 'Ambient light'), h('span', { class: 'row-split' }, col, k, reset('_ambient', amb.intensity > 0))))
      }
    }
    rows.push(h('div', { class: 'dim' }, 'Stored in the map as worldspawn keys (Details shows them too). Light, ambient occlusion and optimisation re-bake the lighting; colour correction only changes how it is shown.'))
    this.element.replaceChildren(...rows)
  }
}
