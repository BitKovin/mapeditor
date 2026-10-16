// Level settings panel (REQUIREMENTS §22 P2): light brightness, ambient light, colour correction, baked ambient occlusion, optimisation, the
// bake quality multiplier and the sky (REQUIREMENTS §28: Auto sky, margin, skybox). Every value is a worldspawn key (model/levelSettings.ts); an empty field = the default. Each change is one undo step.
import type { IContentRenderer } from 'dockview-core'
import { LEVEL_SECTIONS, LevelField, ambientKey, ambientOf, colorOfHex, hexOf, levelKeyFor, levelValue, worldspawnOf } from '../model/levelSettings'
import { setLevelKey, setLevelKeys } from '../state/actions'
import type { Store } from '../state/store'
import type { TextureCache } from '../state/textures'
import { AUTO_SKY_OPTIONS, AutoSky, SKY_AUTO_KEY, SKY_MARGIN_DEFAULT, SKY_MARGIN_KEY, SKY_MARGIN_MAX, SKY_TEXTURE_KEY, autoSkyOf, isCubemapPath, isEnvSkybox, skyMarginKey, skyMarginOf } from '../model/sky'
import { fmt, h } from './dom'
import { pickTexture } from './texturePropsWindow'

/** What the Sky section needs to offer the skyboxes (the texture list and the thumbnails); without it the skybox is typed by name. */
export interface LevelSkyEnv { textures: TextureCache; listTextures: () => Promise<string[]> }

export class LevelSettingsPanel implements IContentRenderer {
  readonly element = h('div', { class: 'panel form level-settings' })
  private unsub: (() => void) | null = null
  private sig = ''
  constructor(private store: Store, private skyEnv?: LevelSkyEnv) {}
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
    rows.push(...this.skyRows(world, ro, reset))
    rows.push(h('div', { class: 'dim' }, 'Stored in the map as worldspawn keys (Details shows them too). Light, ambient occlusion and optimisation re-bake the lighting; colour correction only changes how it is shown.'))
    this.element.replaceChildren(...rows)
  }

  /** Sky (REQUIREMENTS §28 K1 / K9): Auto sky (off / box / box per area), its margin and the level's skybox. */
  private skyRows(world: ReturnType<typeof worldspawnOf>, ro: boolean, reset: (key: string, has: boolean) => HTMLElement): HTMLElement[] {
    const stop = (e: Event) => e.stopPropagation()
    const mode = h('select', { title: 'Encloses the level in sky faces on BSP export (the map itself is not changed)' }) as HTMLSelectElement
    for (const o of AUTO_SKY_OPTIONS) { const opt = h('option', { value: o.value, title: o.title }, o.label) as HTMLOptionElement; opt.selected = o.value === autoSkyOf(world); mode.append(opt) }
    if (ro) mode.setAttribute('disabled', '')
    mode.addEventListener('change', () => void this.setAutoSky(mode.value as AutoSky))
    const hasMargin = world?.keyvalues?.[SKY_MARGIN_KEY] !== undefined
    const margin = h('input', { type: 'number', min: '0', max: String(SKY_MARGIN_MAX), step: '1', value: hasMargin ? fmt(skyMarginOf(world)) : '', placeholder: fmt(SKY_MARGIN_DEFAULT), title: "Space between the level's geometry and its sky box, in metres" }) as HTMLInputElement
    if (ro) margin.setAttribute('disabled', '')
    margin.addEventListener('keydown', stop)
    margin.addEventListener('change', () => setLevelKey(this.store, SKY_MARGIN_KEY, skyMarginKey(margin.value)))
    const tex = world?.keyvalues?.[SKY_TEXTURE_KEY] ?? ''
    const name = h('input', { type: 'text', value: tex, placeholder: 'the first skybox in env/', title: "The level's skybox (env/<name>_cube): the automatic sky and the light of light_sky entities" }) as HTMLInputElement
    if (ro) name.setAttribute('disabled', '')
    name.addEventListener('keydown', (e) => { e.stopPropagation(); if ((e as KeyboardEvent).key === 'Enter') name.blur() })
    name.addEventListener('change', () => setLevelKey(this.store, SKY_TEXTURE_KEY, name.value.trim() || null))
    const choose = h('button', { title: 'Choose a skybox from the env folder' }, 'Choose...')
    if (ro || !this.skyEnv) choose.setAttribute('disabled', '')
    choose.addEventListener('click', () => void this.chooseSky())
    const rows: HTMLElement[] = [
      h('div', { class: 'section' }, 'Sky'),
      h('div', { class: 'dim' }, 'Retro sky: on export the level is enclosed in sky faces, drawn by direction from the skybox; sky faces get no lightmap and block no light.'),
      h('label', { class: 'field' }, h('span', {}, 'Auto sky'), h('span', { class: 'row-split' }, mode, reset(SKY_AUTO_KEY, autoSkyOf(world) !== 'off'))),
      h('label', { class: 'field' }, h('span', {}, 'Margin (m)'), h('span', { class: 'row-split' }, margin, reset(SKY_MARGIN_KEY, hasMargin))),
      h('label', { class: 'field' }, h('span', {}, 'Skybox'), h('span', { class: 'row-split' }, name, choose, reset(SKY_TEXTURE_KEY, tex !== ''))),
    ]
    if (tex && !isCubemapPath(tex)) rows.push(h('div', { class: 'dim' }, 'Not a skybox: the engine draws only textures named <name>_cube (images in env/) as sky.'))
    rows.push(h('div', { class: 'dim' }, 'The skybox also gives light_sky entities their light per direction (colour x brightness x the sky).'))
    return rows
  }

  /** Turning Auto sky on without a skybox picks the first one of env/ in the same undo step, so the views can show it. */
  async setAutoSky(mode: AutoSky): Promise<void> {
    if (mode === 'off') { setLevelKey(this.store, SKY_AUTO_KEY, null); return }
    const world = worldspawnOf(this.store.state.doc)
    let tex: string | null = world?.keyvalues?.[SKY_TEXTURE_KEY] ?? null
    if (!tex && this.skyEnv) { try { tex = (await this.skyEnv.listTextures()).filter(isEnvSkybox).sort()[0] ?? null } catch { tex = null } }
    setLevelKeys(this.store, 'Level: auto sky', tex ? { [SKY_AUTO_KEY]: mode, [SKY_TEXTURE_KEY]: tex } : { [SKY_AUTO_KEY]: mode })
  }

  private async chooseSky(): Promise<void> {
    if (!this.skyEnv) return
    const env = { textures: this.skyEnv.textures, listTextures: this.skyEnv.listTextures, save: async () => {}, activeTexture: () => '' }
    const p = await pickTexture(env, 'Skybox of the level', undefined, isCubemapPath)
    if (p) setLevelKey(this.store, SKY_TEXTURE_KEY, p)
  }
}
