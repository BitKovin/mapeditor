// @vitest-environment jsdom
import { afterEach, describe, expect, it } from 'vitest'
import { findEntity } from '../src/model/document'
import { faceUvPolygon, polygonsBounds } from '../src/model/ftt'
import { makeBox } from '../src/model/geometry'
import { defaultProps, texEnv } from '../src/model/texprops'
import { addDrawnBrush } from '../src/state/actions'
import { TextureCache } from '../src/state/textures'
import { faceMapKey } from '../src/render/lightmapStore'
import { openFastTextureTool, toggleFastTextureTool } from '../src/ui/fastTextureTool'
import { makeApp } from './helpers'

afterEach(() => { texEnv.setProvider(null); document.body.replaceChildren() })

function setup() {
  const app = makeApp(); app.sync()
  texEnv.setProvider((path) => ({ path, width: 64, height: 64, ppm: 64, props: defaultProps() }))
  addDrawnBrush(app.store, makeBox('b1', [0, 0, 0], [4, 2, 1], 'dev/wall'))
  app.store.select({ entityId: 'e_worldspawn', brushId: 'b1', faceId: 1 })
  const textures = new TextureCache(async () => { throw new Error('none') })
  const env = { store: app.store, textures }
  const brush = () => findEntity(app.store.state.doc, 'e_worldspawn')!.brushes![0]
  const poly = () => faceUvPolygon(brush(), brush().faces.find((f) => f.id === 1)!)
  const btn = (t: string) => [...document.querySelectorAll('.fft-win button')].find((b) => b.textContent === t) as HTMLButtonElement
  const canvas = () => document.querySelector('.fft-win canvas') as HTMLCanvasElement
  canvas // referenced lazily
  return { app, env, brush, poly, btn, canvas }
}
const ptr = (el: HTMLElement, type: string, x: number, y: number, init: MouseEventInit = {}) => el.dispatchEvent(new MouseEvent(type, { clientX: x, clientY: y, button: 0, bubbles: true, ...init }))

describe('Fast Texture Tool window', () => {
  it('opens non modal, shows the island, toggles with the shortcut and closes with the x', () => {
    const { env } = setup()
    toggleFastTextureTool(env)
    expect(document.querySelector('.fft-win')).toBeTruthy(); expect(document.querySelector('.modal-backdrop')).toBeNull()
    expect(document.querySelector('.fft-win')!.textContent).toMatch(/1 face of b1/)
    openFastTextureTool(env); expect(document.querySelectorAll('.fft-win')).toHaveLength(1) // a second open does nothing
    toggleFastTextureTool(env); expect(document.querySelector('.fft-win')).toBeNull()
    openFastTextureTool(env); (document.querySelector('.fft-x') as HTMLElement).click(); expect(document.querySelector('.fft-win')).toBeNull()
  })

  it('says what to do without a selection and refuses buttons', () => {
    const { app, env, btn } = setup(); app.store.select(null); openFastTextureTool(env)
    expect(document.querySelector('.fft-win')!.textContent).toMatch(/Select faces/)
    btn('Mirror U').click(); expect(document.querySelector('.fft-win')!.textContent).toMatch(/Select faces first/)
  })

  it('Mirror U / Rot 90 act on the island as one undoable step each', () => {
    const { app, env, btn, poly } = setup(); openFastTextureTool(env)
    const before = poly(), b0 = polygonsBounds([before])!, n = app.store.state.history.length
    btn('Mirror U').click()
    const after = poly(), b1 = polygonsBounds([after])!
    before.forEach((p, i) => expect(after[i][0]).toBeCloseTo(b0.min[0] + b0.max[0] - p[0], 6))
    expect(b1.min[0]).toBeCloseTo(b0.min[0], 6); expect(app.store.state.history).toHaveLength(n + 1); expect(app.store.state.history.at(-1)!.label).toMatch(/Mirror/)
    btn('Rot +90').click()
    const b2 = polygonsBounds([poly()])!
    expect(b2.max[0] - b2.min[0]).toBeCloseTo(b0.max[1] - b0.min[1], 6)
    app.store.undo(); app.store.undo()
    poly().forEach((p, i) => expect(p[0]).toBeCloseTo(before[i][0], 6))
  })

  it('dragging the body moves the island live with ONE history entry; Escape cancels', () => {
    const { app, env, poly, canvas } = setup(); openFastTextureTool(env)
    const c = canvas(); c.getBoundingClientRect = () => ({ left: 0, top: 0, width: 600, height: 420, right: 600, bottom: 420, x: 0, y: 0, toJSON: () => ({}) })
    const before = poly(), n = app.store.state.history.length
    ptr(c, 'pointerdown', 300, 210); ptr(c, 'pointermove', 360, 210)
    const mid = poly(); expect(mid[0][0]).toBeGreaterThan(before[0][0]); expect(mid[0][1]).toBeCloseTo(before[0][1], 6)
    expect(app.store.state.history).toHaveLength(n) // not yet
    ptr(c, 'pointerup', 360, 210)
    expect(app.store.state.history).toHaveLength(n + 1); expect(app.store.state.history.at(-1)!.label).toBe('Fast texture')
    // escape restores
    const moved = poly()
    ptr(c, 'pointerdown', 300, 210); ptr(c, 'pointermove', 340, 240)
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))
    poly().forEach((p, i) => expect(p[0]).toBeCloseTo(moved[i][0], 6)); expect(app.store.state.history).toHaveLength(n + 1)
  })

  it('Align edge turns a slanted UV edge straight; Follow Quads needs several faces; Ortho is one undo step', () => {
    const { app, env, btn, poly, canvas } = setup(); openFastTextureTool(env)
    const c = canvas(); c.getBoundingClientRect = () => ({ left: 0, top: 0, width: 600, height: 420, right: 600, bottom: 420, x: 0, y: 0, toJSON: () => ({}) })
    // slant the island by dragging the round rotate handle (above the top edge) sideways, then align an edge by clicking it
    const b = polygonsBounds([poly()])!, w = b.max[0] - b.min[0], h = b.max[1] - b.min[1]
    const s = Math.min((600 - 110) / w, (420 - 110) / h) // the window's initial fit
    const cx = 300, top = 210 - (h * s) / 2
    ptr(c, 'pointerdown', cx, top - 24); ptr(c, 'pointermove', cx + 40, top - 20); ptr(c, 'pointerup', cx + 40, top - 20)
    const slanted = poly(), e = [slanted[1][0] - slanted[0][0], slanted[1][1] - slanted[0][1]]
    expect(Math.abs(e[1])).toBeGreaterThan(1e-3) // no longer horizontal
    btn('Align edge').click()
    const mid = [(slanted[0][0] + slanted[1][0]) / 2, (slanted[0][1] + slanted[1][1]) / 2]
    const nb = polygonsBounds([slanted])!, nc = [(nb.min[0] + nb.max[0]) / 2, (nb.min[1] + nb.max[1]) / 2]
    const s2 = s // zoom unchanged by the drag (the island stayed on screen)
    ptr(c, 'pointerdown', 300 + (mid[0] - nc[0]) * s2, 210 + (mid[1] - nc[1]) * s2); ptr(c, 'pointerup', 0, 0)
    const aligned = poly()
    expect(Math.abs(aligned[1][1] - aligned[0][1]) < 1e-6 || Math.abs(aligned[1][0] - aligned[0][0]) < 1e-6).toBe(true)
    expect(app.store.state.history.at(-1)!.label).toBe('Align UV edge')
    btn('Follow Quads').click(); expect(document.querySelector('.fft-win')!.textContent).toMatch(/needs two or more faces/)
    btn('Ortho').click(); expect(app.store.state.history.at(-1)!.label).toBe('Project ortho')
  })

  it('follows the selection: selecting a whole brush shows all its faces', () => {
    const { app, env } = setup(); openFastTextureTool(env)
    app.store.select({ entityId: 'e_worldspawn', brushId: 'b1' })
    expect(document.querySelector('.fft-win')!.textContent).toMatch(/6 faces of b1/)
  })
})

describe('Fast Texture Tool: Lightmap mode', () => {
  const bake = (app: ReturnType<typeof setup>['app'], faceId = 1) => {
    const lm = app.store.lightmap
    lm.atlas = 256; lm.pageCount = 1; lm.pages = [new Uint16Array(256 * 256 * 4)]; lm.mesh = new Float32Array(12); lm.triangleCount = 1
    lm.faceMaps = new Map([[faceMapKey('', 'e_worldspawn', 'b1', faceId), { u: [16 / 256, 0, 0, 10 / 256], v: [0, 16 / 256, 0, 20 / 256], pg: 0, k: [0, 0, 0] }]])
    app.store.select({ entityId: 'e_worldspawn', brushId: 'b1', faceId })
  }
  const lmBtn = () => [...document.querySelectorAll('.fft-win button')].find((b) => b.textContent === 'Lightmap') as HTMLButtonElement
  const uvBtn = () => [...document.querySelectorAll('.fft-win button')].find((b) => b.textContent === 'UV') as HTMLButtonElement
  const bars = () => [...document.querySelectorAll('.fft-win .fft-bar')] as HTMLElement[]

  it('switches the tool set: UV tools hide, density controls show; back again restores', () => {
    const { env } = setup(); openFastTextureTool(env)
    const [, uvTools, lmTools] = bars()
    expect(uvTools.style.display).toBe(''); expect(lmTools.style.display).toBe('none')
    lmBtn().click(); expect(uvTools.style.display).toBe('none'); expect(lmTools.style.display).toBe('')
    uvBtn().click(); expect(uvTools.style.display).toBe(''); expect(lmTools.style.display).toBe('none')
  })

  it('says why there is nothing to show: no lightmap, or no light data for the selection', () => {
    const { app, env } = setup(); openFastTextureTool(env); lmBtn().click()
    expect(document.querySelector('.fft-win')!.textContent).toMatch(/No lightmap yet/)
    bake(app, 2) // a map for face 2 only; face 1 is selected below
    app.store.select({ entityId: 'e_worldspawn', brushId: 'b1', faceId: 1 })
    expect(document.querySelector('.fft-win')!.textContent).toMatch(/no light data/)
  })

  it('shows page, size in texels and density of the selected face; density edits are undoable steps', () => {
    const { app, env, brush } = setup(); openFastTextureTool(env); lmBtn().click(); bake(app)
    const text = () => document.querySelector('.fft-win')!.textContent!
    expect(text()).toMatch(/face 1: page 0, \d+ x \d+ texels at \(10, 20\), 16 x 16 texels\/m \(density x1\)/)
    const x2 = [...document.querySelectorAll('.fft-win button')].find((b) => b.textContent === 'x2') as HTMLButtonElement
    x2.click(); expect(brush().faces.find((f) => f.id === 1)!.lightmapScale).toBe(2); expect(app.store.state.history.at(-1)!.label).toBe('Lightmap density')
    const half = [...document.querySelectorAll('.fft-win button')].find((b) => b.textContent === '/2') as HTMLButtonElement
    half.click(); expect(brush().faces.find((f) => f.id === 1)!.lightmapScale).toBeUndefined() // 1 is the default: not stored
    const input = document.querySelector('.fft-win input[type=number]') as HTMLInputElement; input.value = '4'; input.dispatchEvent(new Event('change'))
    expect(brush().faces.find((f) => f.id === 1)!.lightmapScale).toBe(4)
  })

  it('the canvas does not edit the UV island in this mode', () => {
    const { app, env, canvas } = setup(); openFastTextureTool(env); lmBtn().click(); bake(app)
    const c = canvas(); c.getBoundingClientRect = () => ({ left: 0, top: 0, width: 600, height: 420, right: 600, bottom: 420, x: 0, y: 0, toJSON: () => ({}) })
    const n = app.store.state.history.length
    ptr(c, 'pointerdown', 300, 210); ptr(c, 'pointermove', 360, 210); ptr(c, 'pointerup', 360, 210)
    expect(app.store.state.history).toHaveLength(n); expect(app.store.dragging).toBe(false)
  })
})
