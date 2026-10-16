// @vitest-environment jsdom
import { afterEach, describe, expect, it, vi } from 'vitest'
import { makeBox } from '../src/model/geometry'
import { findEntity } from '../src/model/document'
import { defaultProps, texEnv } from '../src/model/texprops'
import { projectionOf } from '../src/model/uv'
import { chooseHotspot, faceSize } from '../src/model/hotspots'
import { addDrawnBrush } from '../src/state/actions'
import { TextureCache } from '../src/state/textures'
import { TexturePanel } from '../src/ui/texturePanel'
import { makeApp } from './helpers'

afterEach(() => { texEnv.setProvider(null); document.body.replaceChildren() })

function setup() {
  const app = makeApp(); app.sync()
  addDrawnBrush(app.store, makeBox('b1', [0, 0, 0], [4, 2, 1], 'dev/wall'))
  app.store.select({ entityId: 'e_worldspawn', brushId: 'b1', faceId: 1 })
  const textures = new TextureCache(async () => { throw new Error('none') })
  texEnv.setProvider(() => ({ path: 'dev/wall', width: 256, height: 128, ppm: 64, props: defaultProps() })) // 4 x 2 m natural
  const open = vi.fn()
  const panel = new TexturePanel(app.store, textures, open, () => 0.5)
  document.body.append(panel.element); panel.init()
  const face = () => findEntity(app.store.state.doc, 'e_worldspawn')!.brushes![0].faces.find((f) => f.id === 1)!
  const row = (label: string) => [...panel.element.querySelectorAll('.si-row')].find((r) => r.querySelector('.si-label')?.textContent === label) as HTMLElement
  const field = (label: string) => row(label).querySelector('input[type=number]') as HTMLInputElement
  const set = (i: HTMLInputElement, v: string) => { i.value = v; i.dispatchEvent(new Event('change')) }
  const button = (text: string) => [...panel.element.querySelectorAll('button')].find((b) => b.textContent === text) as HTMLButtonElement
  return { app, panel, open, face, row, field, set, button }
}

describe('Surface Inspector', () => {
  it('shows shift in texels and stretch as a multiple of the natural size', () => {
    const { field } = setup()
    expect(field('Horizontal stretch').value).toBe('1')
    expect(field('Vertical shift').value).toBe('0')
  })
  it('editing the fields writes the projection (shift in texels, stretch relative)', () => {
    const { field, set, face } = setup()
    set(field('Horizontal shift'), '64')
    expect(projectionOf(face()).shift[0]).toBeCloseTo(0.25, 9)
    set(field('Horizontal stretch'), '2')
    expect(projectionOf(face()).scale[0]).toBe(8)
    set(field('Rotate'), '45')
    expect(projectionOf(face()).rotate).toBe(45)
    expect(face().uvs).toBeTruthy()
  })
  it('the mirror button negates one axis; a second press restores it', () => {
    const { row, face } = setup()
    const m = row('Vertical stretch').querySelector('.si-mirror') as HTMLButtonElement
    m.click(); expect(projectionOf(face()).scale[1]).toBe(-2)
    ;(row('Vertical stretch').querySelector('.si-mirror') as HTMLButtonElement).click(); expect(projectionOf(face()).scale[1]).toBe(2)
  })
  it('Match Grid uses the viewport grid, Reset restores the natural size', () => {
    const { field, set, button, face } = setup()
    set(field('Horizontal stretch'), '0.77')
    button('Match Grid').click()
    expect(projectionOf(face()).scale[0] % 0.5).toBeCloseTo(0, 9)
    button('Reset').click()
    expect(projectionOf(face()).scale).toEqual([4, 2])
  })
  it('Width / Height / Fit use the repeat counts', () => {
    const { button, face, panel } = setup()
    button('Fit').click()
    const p = projectionOf(face())
    expect(p.scale[0]).toBeGreaterThan(0); expect(p.scale[1]).toBeGreaterThan(0)
    expect(panel.element.querySelectorAll('input[title^="Repeats"]').length).toBe(2)
  })
  it('the step fields drive the spinners; Properties opens the texture window', () => {
    const { row, field, set, button, open, app } = setup()
    set(row('Horizontal shift').querySelector('.si-step input') as HTMLInputElement, '8')
    expect(app.store.state.texSteps.shiftU).toBe(8)
    expect(field('Horizontal shift').step).toBe('8')
    button('Properties...').click()
    expect(open).toHaveBeenCalledWith('dev/wall')
  })
  it('Ortho and Cam change the projection kind; Axial returns to the classic one; Shear is a field', () => {
    const { face, button, field, set, app } = setup()
    button('Ortho').click()
    expect(projectionOf(face()).kind).toBe('ortho')
    button('Axial').click()
    expect(projectionOf(face()).kind).toBeUndefined()
    button('Cam').click() // the test panel has no camera: refused with a message, nothing changes
    expect(projectionOf(face()).kind).toBeUndefined()
    expect(app.store.state.message).toBe('No viewport camera')
    set(field('Shear'), '0.25')
    expect(projectionOf(face()).shear).toBe(0.25)
    set(field('Shear'), '0')
    expect(projectionOf(face()).shear).toBeUndefined()
  })
  it('typing a texture name and pressing Enter applies it at once', () => {
    const { panel, face } = setup()
    const name = panel.element.querySelector('.si-name') as HTMLInputElement
    name.value = 'dev/metal'
    name.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))
    expect(face().texture).toBe('dev/metal')
  })
})

describe('per-face repetition override', () => {
  it('buttons write face.repeat, "Texture" / "Auto" clear it, and an empty override is removed', () => {
    const s = setup()
    const group = (label: string) => s.row(label) as HTMLElement
    const click = (label: string, text: string, nth = 0) => ([...group(label).querySelectorAll('button')].filter((b) => b.textContent === text)[nth] as HTMLButtonElement).click()
    click('Horizontal', 'Mirror')
    expect(s.face().repeat).toEqual({ u: { mode: 'mirror' } })
    click('Vertical', 'Clamp')
    expect(s.face().repeat).toEqual({ u: { mode: 'mirror' }, v: { edge: 'clamp' } })
    click('Horizontal', 'Texture')
    click('Vertical', 'Auto')
    expect('repeat' in s.face()).toBe(false)
  })
})

describe('hotspot button', () => {
  it('applies the best-fitting hotspot of the texture; says so when there are none', () => {
    const s = setup()
    s.button('Hotspot').click()
    expect(s.app.store.state.message).toMatch(/no hotspots/)
    const props = defaultProps(); props.hotspots = [{ x: 0, y: 0, w: 1, h: 0.25 }, { x: 0, y: 0.25, w: 0.5, h: 0.75 }]
    texEnv.setProvider(() => ({ path: 'dev/wall', width: 256, height: 128, ppm: 64, props })) // 4 x 2 m; the face is 4 x 2 m
    s.button('Hotspot').click()
    const uv = s.face().uvs!
    const brush = findEntity(s.app.store.state.doc, 'e_worldspawn')!.brushes![0]
    const k = chooseHotspot(props.hotspots, { width: 256, height: 128, ppm: 64 }, faceSize(brush, s.face()))
    const hs = props.hotspots[k]
    // the face shows exactly the chosen rectangle (pulled in by the edge inset)
    expect(Math.max(...uv.map((q) => q[0])) - Math.min(...uv.map((q) => q[0]))).toBeCloseTo(hs.w, 1)
    expect(Math.max(...uv.map((q) => q[1])) - Math.min(...uv.map((q) => q[1]))).toBeCloseTo(hs.h, 1)
  })
})
