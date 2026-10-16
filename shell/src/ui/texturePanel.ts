import type { IContentRenderer } from 'dockview-core'
import { faceDensity } from '../model/texelDensity'
import { texEnv } from '../model/texprops'
import { fitAxis, fitBoth, justify, matchGrid, mirrorAxis, projectAxial, projectCam, projectOrtho, withShear, resetSurface, shiftTexels, stretchOf, surfaceBasis, withShiftTexels, withStretch } from '../model/surface'
import { LIGHTMAP_SCALE_MAX, LIGHTMAP_SCALE_MIN, lightmapScaleOf, projectionOf, defaultProjection } from '../model/uv'
import { applyHotspot, applyTexture, editLightmapScale, faceTargets, setFaceRepeat, surfaceEdit } from '../state/actions'
import { parseFaceRepeat } from '../model/texprops'
import type { Store, TexSteps } from '../state/store'
import type { TextureCache } from '../state/textures'
import { fmt, h } from './dom'
import { layoutSection } from './layoutSection'
import { groupEnv } from '../model/texgroup'

/**
 * Surface Inspector (REQUIREMENTS §6, §19 B1; layout after NetRadiant, without an Apply button): texture, shift (texels) and stretch of both axes,
 * each with its own step, rotate, Match Grid, Texturing (Width / Height / Reset / Fit with repeat counts), Project, density readout and lightmap density.
 * It edits whatever is selected: one face, every face of a brush, or many.
 */
export class TexturePanel implements IContentRenderer {
  readonly element = h('div', { class: 'panel' })
  private unsub: (() => void) | null = null
  private sig = ''
  private fit: [number, number] = [1, 1]
  /** `openProperties` opens the Texture Properties window; `gridSize` is the active viewport's grid (Match Grid). */
  constructor(private store: Store, private textures: TextureCache, private openProperties: (path: string) => void = () => {}, private gridSize: () => number = () => 0.25, private camera: () => { right: [number, number, number]; up: [number, number, number] } | null = () => null, private openRelief: (path: string) => void = () => {}, private openGroup: (path: string) => void = () => {}, private openFtt: () => void = () => {}) {}
  init(): void { this.unsub = this.store.subscribe(() => this.refresh()); this.textures.onChange(() => { this.sig = ''; this.refresh() }); this.refresh() }
  dispose(): void { this.unsub?.() }

  private refresh(): void {
    const st = this.store.state
    const targets = faceTargets(st.doc, st.selection)
    const first = targets[0] && st.doc.entities.find((e) => e.id === targets[0].entityId)?.brushes?.find((b) => b.id === targets[0].brushId)?.faces.find((f) => f.id === targets[0].faceId)
    const sig = JSON.stringify([targets.length, first?.texture, first?.projection, first?.repeat, first?.layout, groupEnv.rev, first?.lightmapScale, st.activeTexture, this.store.canEdit, st.texSteps, texEnv.rev, this.fit])
    if (sig === this.sig) return
    if (this.element.contains(this.element.ownerDocument.activeElement) && (this.element.ownerDocument.activeElement as HTMLElement).tagName === 'INPUT') return
    this.sig = sig
    const ro = !this.store.canEdit || targets.length === 0
    const proj = first ? projectionOf(first) : defaultProjection()
    const basis = first ? surfaceBasis(first) : { texels: [64, 64] as [number, number], natural: [1, 1] as [number, number] }
    const texName = first?.texture ?? st.activeTexture

    const btn = (label: string, title: string, fn: () => void, extra = '') => {
      const b = h('button', { title, class: extra }, label)
      if (ro) b.setAttribute('disabled', '')
      b.addEventListener('click', fn)
      return b
    }
    const stop = (e: Event) => e.stopPropagation()
    /** value + spinner whose step is the neighbouring Step field */
    const num = (value: number, step: number, apply: (v: number) => void, title: string) => {
      const i = h('input', { type: 'number', step: String(step), value: fmt(value), title })
      if (ro) i.setAttribute('disabled', '')
      i.addEventListener('change', () => { const v = Number(i.value); if (Number.isFinite(v)) apply(v) })
      i.addEventListener('keydown', stop)
      return i
    }
    const stepField = (key: keyof TexSteps) => {
      const i = h('input', { type: 'number', step: key === 'rotate' || key === 'shiftU' || key === 'shiftV' ? '1' : '0.01', min: '0', value: fmt(st.texSteps[key]), title: 'Step of the spinner' })
      i.addEventListener('change', () => this.store.setTexStep(key, Number(i.value)))
      i.addEventListener('keydown', stop)
      return i
    }
    const row = (label: string, ...cells: HTMLElement[]) => h('div', { class: 'si-row' }, h('span', { class: 'si-label' }, label), ...cells)
    const stepCell = (key: keyof TexSteps) => h('label', { class: 'si-step' }, h('span', {}, 'Step'), stepField(key))

    const sh = shiftTexels(proj, basis.texels), sr = stretchOf(proj, basis.natural)
    const setShift = (axis: 0 | 1) => (v: number) => surfaceEdit(this.store, 'Shift texture', (p, _b, f) => withShiftTexels(p, axis, v, surfaceBasis(f).texels))
    const setStretch = (axis: 0 | 1) => (v: number) => surfaceEdit(this.store, 'Stretch texture', (p, _b, f) => withStretch(p, axis, v, surfaceBasis(f).natural))
    const mirror = (axis: 0 | 1, title: string) => btn('-', title, () => surfaceEdit(this.store, 'Mirror texture', (p) => mirrorAxis(p, axis)), 'si-mirror')

    // texture name: typing applies at once
    const name = h('input', { type: 'text', value: texName, class: 'si-name', title: 'Texture of the selection. Type a texture path and press Enter, or click a texture in the browser.' })
    if (ro) name.setAttribute('disabled', '')
    name.addEventListener('keydown', (e) => { e.stopPropagation(); if ((e as KeyboardEvent).key === 'Enter' && name.value.trim()) applyTexture(this.store, name.value.trim()) })
    const propsBtn = h('button', { title: 'Texture properties: pixels per meter, repetition behaviour, heat map' }, 'Properties...')
    propsBtn.addEventListener('click', () => this.openProperties(texName))
    const reliefBtn = h('button', { title: 'Relief: model how one repeat of the texture stands out of or sinks into the surface (windows, frames, cornices)' }, 'Relief...')
    reliefBtn.addEventListener('click', () => this.openRelief(texName))
    const fttBtn = h('button', { title: 'Fast Texture Tool (Shift+Q): edit the UV island of the selected faces by direct manipulation' }, 'Fast Texture Tool...')
    fttBtn.addEventListener('click', () => this.openFtt())

    const thumb = h('canvas', { width: '48', height: '48', class: 'tex-preview' })
    const tex = this.textures.get(texName)
    const g = thumb.getContext('2d')
    if (g) { g.fillStyle = '#2a2a2a'; g.fillRect(0, 0, 48, 48); if (tex.status === 'ready') g.drawImage(tex.image.source as CanvasImageSource, 0, 0, 48, 48) }

    // density readout
    let density = ''
    const info = texEnv.info(texName)
    if (first && info) {
      const [bb] = [st.doc.entities.find((e) => e.id === targets[0].entityId)?.brushes?.find((b) => b.id === targets[0].brushId)]
      if (bb) { const [du, dv] = faceDensity(bb, first, info.width, info.height); density = `${fmt(du)} x ${fmt(dv)} px/m (authored ${fmt(info.ppm)})` }
    }
    const who = targets.length === 0 ? 'Select a brush or a face' : targets.length === 1 ? '1 face' : `${targets.length} faces`

    const fitInput = (axis: 0 | 1) => {
      const i = h('input', { type: 'number', step: '0.25', min: '0.001', value: this.fit[axis].toFixed(3), title: axis === 0 ? 'Repeats across the face' : 'Repeats along the face' })
      i.addEventListener('change', () => { const v = Number(i.value); if (v > 0) { this.fit[axis] = v; this.sig = '' } })
      i.addEventListener('keydown', stop)
      return i
    }
    const lmScale = first ? lightmapScaleOf(first) : 1
    const lmEdit = (label: string, fn: (v: number) => number) => () => editLightmapScale(this.store, label, fn)
    const lmInput = h('input', { type: 'number', step: '0.05', value: fmt(lmScale) })
    if (ro) lmInput.setAttribute('disabled', '')
    lmInput.addEventListener('change', () => { const v = Number(lmInput.value); if (v > 0) lmEdit('Lightmap density', () => v)() })
    lmInput.addEventListener('keydown', stop)
    const soon = (label: string, why: string) => { const b = h('button', { title: why, disabled: '' }, label); return b }

    this.element.replaceChildren(h('div', { class: 'form surface' },
      h('div', { class: 'texhead' }, thumb, h('div', {}, h('div', { class: 'dim' }, who), density ? h('div', { class: 'dim', title: 'Texel density of the selected face against the texture\'s pixels per meter' }, density) : h('span'))),
      row('Texture', name, propsBtn, reliefBtn),
      h('div', { class: 'btns' }, fttBtn),
      row('Horizontal shift', num(sh[0], st.texSteps.shiftU, setShift(0), 'Texels'), stepCell('shiftU')),
      row('Vertical shift', num(sh[1], st.texSteps.shiftV, setShift(1), 'Texels'), stepCell('shiftV')),
      row('Horizontal stretch', num(sr[0], st.texSteps.stretchU, setStretch(0), '1 = natural size (pixels / pixels per meter)'), mirror(0, 'Mirror horizontally'), stepCell('stretchU')),
      row('Vertical stretch', num(sr[1], st.texSteps.stretchV, setStretch(1), '1 = natural size (pixels / pixels per meter)'), mirror(1, 'Mirror vertically'), stepCell('stretchV')),
      row('Shear', num(proj.shear ?? 0, st.texSteps.shearStep, (v) => surfaceEdit(this.store, 'Shear texture', (p) => withShear(p, v)), 'Slant: u += shear * v'), stepCell('shearStep')),
      row('Rotate', num(proj.rotate, st.texSteps.rotate, (v) => surfaceEdit(this.store, 'Rotate texture', (p) => ({ ...p, rotate: v })), 'Degrees'), stepCell('rotate')),
      h('div', { class: 'btns' }, btn('Match Grid', 'Make every repeat a whole number of grid cells wide', () => { const gs = this.gridSize(); surfaceEdit(this.store, 'Match grid', (p) => matchGrid(p, gs)) })),
      h('div', { class: 'section' }, 'Texturing'),
      h('div', { class: 'btns' },
        btn('Width', 'Fit the texture to the face width (once)', () => surfaceEdit(this.store, 'Fit width', (p, b, f) => fitAxis(b, f, p, 0, this.fit[0]))),
        btn('Height', 'Fit the texture to the face height (once)', () => surfaceEdit(this.store, 'Fit height', (p, b, f) => fitAxis(b, f, p, 1, this.fit[1]))),
        btn('Reset', 'The texture at its natural size, unrotated', () => surfaceEdit(this.store, 'Reset texture', (_p, b, f) => resetSurface(b, f))),
        btn('Hotspot', 'Show the hotspot of this texture that fits the face best (draw hotspots in the texture properties)', () => { const m = applyHotspot(this.store); if (m) this.store.setMessage(m) }),
        btn('Fit', 'Stretch the texture to cover the face the given number of times', () => surfaceEdit(this.store, 'Fit texture', (p, b, f) => fitBoth(b, f, p, [this.fit[0], this.fit[1]]))),
        fitInput(0), fitInput(1)),
      h('div', { class: 'section' }, 'Repetition of this face'),
      ...(['u', 'v'] as const).map((ax) => {
        const cur = parseFaceRepeat(first?.repeat)?.[ax]
        const group = <T extends string>(key: 'mode' | 'edge', opts: { v: T | null; label: string; title: string }[], value: T | undefined) => {
          const g = h('div', { class: 'seg' })
          for (const o of opts) {
            const b = h('button', { title: o.title, class: (o.v ?? undefined) === value ? 'on' : '' }, o.label)
            if (ro) b.setAttribute('disabled', '')
            b.addEventListener('click', () => setFaceRepeat(this.store, ax, { [key]: o.v }))
            g.append(b)
          }
          return g
        }
        return row(ax === 'u' ? 'Horizontal' : 'Vertical',
          group('mode', [{ v: null, label: 'Texture', title: "As set in the texture's properties" }, { v: 'repeat', label: 'Repeat', title: 'Plain tiling on this face' }, { v: 'mirror', label: 'Mirror', title: 'Mirror every second tile on this face' }], cur?.mode),
          group('edge', [{ v: null, label: 'Auto', title: "As the texture's edge setting (auto / wrap / clamp)" }, { v: 'wrap', label: 'Wrap', title: 'Edges continue each other (seamless)' }, { v: 'clamp', label: 'Clamp', title: 'Edges are borders: no blending with the opposite edge' }], cur?.edge))
      }),
      ...layoutSection(this.store, ro, (p) => this.openGroup(p)),
      h('div', { class: 'section' }, 'Justify'),
      h('div', { class: 'btns' }, ...(['top', 'bottom', 'left', 'right', 'center'] as const).map((e) =>
        btn(e[0].toUpperCase() + e.slice(1), e === 'center' ? 'Centre the texture on the face' : `The texture's ${e} edge touches the face's ${e} edge`, () => surfaceEdit(this.store, `Justify ${e}`, (p, b, f) => justify(b, f, p, e))))),
      h('div', { class: 'section' }, 'Project'),
      h('div', { class: 'btns' },
        btn('Axial', 'Project along the dominant axis again (clears rotation, shift and shear)', () => surfaceEdit(this.store, 'Project axial', (p) => projectAxial(p)), proj.kind === undefined ? 'on' : ''),
        btn('Ortho', 'Axes lie in the face: v follows its steepest slope (no stretching on sloped faces)', () => surfaceEdit(this.store, 'Project ortho', (p, b, f) => projectOrtho(b, f, p)), proj.kind === 'ortho' ? 'on' : ''),
        btn('Cam', 'Axes are the camera\'s right / up, frozen now', () => {
          const c = this.camera()
          if (!c) return this.store.setMessage('No viewport camera')
          let refused = 0
          surfaceEdit(this.store, 'Project from camera', (p, b, f) => { const r = projectCam(b, f, p, c); if (!r) refused++; return r ?? p })
          if (refused) this.store.setMessage(`${refused} face(s) are seen edge-on from here and were left unchanged`)
        }, proj.kind === 'cam' ? 'on' : '')),
      h('div', { class: 'section' }, 'Patch'),
      h('div', { class: 'btns' }, soon('Natural', 'Patches are not implemented yet')),
      h('div', { class: 'section' }, 'Lightmap texel density'),
      h('div', { class: 'row2' }, h('label', { class: 'numfield' }, h('span', {}, 'x'), lmInput)),
      h('div', { class: 'btns' }, btn('x2', 'Twice the lightmap texels per metre (four times the texels)', lmEdit('Lightmap density', (v) => v * 2)), btn('/2', 'Half the lightmap texels per metre', lmEdit('Lightmap density', (v) => v / 2)), btn('Reset', 'The level density (1)', lmEdit('Lightmap density', () => 1))),
      h('div', { class: 'dim' }, `Relative to the level's texels per metre; ${LIGHTMAP_SCALE_MIN} to ${LIGHTMAP_SCALE_MAX}. Faces with different densities do not share a lightmap chart.`),
      h('div', { class: 'dim' }, 'Keys: Shift+arrows shift, Ctrl+Shift+arrows scale, Shift+PageUp/PageDown rotate; Alt = 8x finer. UV lock is in the toolbar.')))
  }
}
