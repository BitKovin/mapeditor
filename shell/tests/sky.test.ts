// @vitest-environment jsdom
// Sky (REQUIREMENTS §28): the engine's skybox mapping, skyboxes in the texture cache and the browser, the Level settings' Sky section.
import { afterEach, describe, expect, it } from 'vitest'
import { SKY_FACES, autoSkyOf, cubemapLookup, engineCubeDir, isCubemapPath, isEnvSkybox, isSkyTexture, levelSkyTexture, panoramaLookup, skyMarginKey, skyMarginOf } from '../src/model/sky'
import type { Vec3 } from '../src/model/math'
import { TextureCache } from '../src/state/textures'
import { ContentBrowserPanel, SKY_FOLDER } from '../src/ui/contentBrowser'
import { LevelSettingsPanel } from '../src/ui/levelSettingsPanel'
import { viewportSky } from '../src/ui/viewport'
import { worldspawnOf } from '../src/model/levelSettings'
import { setLevelKey } from '../src/state/actions'
import { addDrawnBrush } from '../src/state/actions'
import { makeBox } from '../src/model/geometry'
import { makeApp } from './helpers'

class IO { observe() {} disconnect() {} unobserve() {} }
;(globalThis as any).IntersectionObserver ??= IO
afterEach(() => document.body.replaceChildren())

// The engine's code, transcribed (SdlWasmEngine TextureCube.hpp): the direction a pixel of a generated face slot shows (bgfxFaceDir), and the plain
// GL / D3D cube face selection: what fs_bsp_cube's textureCube does with its direction.
const bgfxFaceDir = (slot: number, u: number, v: number): Vec3 =>
  [[-1, -v, u], [1, -v, -u], [u, 1, v], [u, -1, -v], [u, -v, 1], [-u, -v, -1]][slot] as Vec3
function glCube([x, y, z]: Vec3): { face: number; u: number; v: number } {
  const ax = Math.abs(x), ay = Math.abs(y), az = Math.abs(z)
  const [face, sc, tc, ma] = ax >= ay && ax >= az ? (x > 0 ? [0, -z, -y, ax] : [1, z, -y, ax]) : ay >= az ? (y > 0 ? [2, x, z, ay] : [3, x, -z, ay]) : (z > 0 ? [4, x, -y, az] : [5, -x, -y, az])
  return { face, u: (sc / ma + 1) / 2, v: (tc / ma + 1) / 2 }
}
const sphere = (n: number): Vec3[] => Array.from({ length: n }, (_, i) => { const z = 1 - (2 * (i + 0.5)) / n, r = Math.sqrt(1 - z * z), p = i * 2.399963; return [r * Math.cos(p), z, r * Math.sin(p)] as Vec3 })

describe('the engine skybox mapping (model/sky.ts, the same as core/sky.cpp)', () => {
  it('known directions: north = _bk, east = _rt, south = _ft, west = _lf, up = _up, down = _dn, each at its centre', () => {
    const cases: [Vec3, string][] = [[[0, 0, -1], '_bk'], [[1, 0, 0], '_rt'], [[0, 0, 1], '_ft'], [[-1, 0, 0], '_lf'], [[0, 1, 0], '_up'], [[0, -1, 0], '_dn']]
    for (const [d, suffix] of cases) {
      const t = cubemapLookup(d)
      expect(SKY_FACES[t.face].suffix).toBe(suffix)
      expect(t.u).toBeCloseTo(0.5, 12); expect(t.v).toBeCloseTo(0.5, 12)
    }
  })
  it('the faces are upright and not mirrored seen from inside; _up has west at its top and south at its right', () => {
    expect(cubemapLookup([0.2, 0, -1]).u).toBeGreaterThan(0.5)   // facing north, east is on the right
    expect(cubemapLookup([0, 0.2, -1]).v).toBeLessThan(0.5)      // and up is above
    expect(cubemapLookup([1, 0, 0.2]).u).toBeGreaterThan(0.5)    // facing east, south is on the right
    expect(cubemapLookup([0, 0, 1]).face).toBe(4)
    expect(cubemapLookup([-0.2, 0, 1]).u).toBeGreaterThan(0.5)   // facing south, west is on the right
    const top = cubemapLookup([-0.2, 1, 0]), right = cubemapLookup([0, 1, 0.2])
    expect(top.face).toBe(2); expect(top.v).toBeLessThan(0.5); expect(top.u).toBeCloseTo(0.5, 12)
    expect(right.u).toBeGreaterThan(0.5); expect(right.v).toBeCloseTo(0.5, 12)
  })
  it('the viewport upload (SKY_FACES targets and turns + the x flip) gives exactly the cube lookup the engine does', () => {
    // GL samples the uploaded face; SKY_FACES says which file went there and how it was turned: turning the file's pixel the same way must land on the
    // pixel GL picks for engineCubeDir(d)
    const target = ['px', 'nx', 'py', 'ny', 'pz', 'nz']
    for (const d of sphere(300)) {
      const file = cubemapLookup(d), gl = glCube(engineCubeDir(d))
      expect(target[gl.face]).toBe(SKY_FACES[file.face].target)
      const turn = SKY_FACES[file.face].turn
      // clockwise: the file's (u, v) moves to (1 - v, u); counter-clockwise: to (v, 1 - u)
      const up = turn === 1 ? [1 - file.v, file.u] : turn === -1 ? [file.v, 1 - file.u] : [file.u, file.v]
      expect(up[0]).toBeCloseTo(gl.u, 9); expect(up[1]).toBeCloseTo(gl.v, 9)
    }
  })
  it('a panorama: north in the middle, east at three quarters, up at the top; and the engine converting it into faces lands on the same point', () => {
    expect(panoramaLookup([0, 0, -1])).toEqual({ u: 0.5, v: 0.5 })
    expect(panoramaLookup([1, 0, 0]).u).toBeCloseTo(0.75, 12)
    expect(panoramaLookup([0, 1, 0]).v).toBeCloseTo(0, 12)
    // DecodeFromPanorama: slot k written as _lf _rt _up (turned cw) _dn (turned ccw) _bk _ft, then loaded as faces; sampleEquirect lon = atan2(x, z)
    const slotOfFile = [0, 1, 2, 3, 5, 4]
    for (const d of sphere(300)) {
      const t = cubemapLookup(d)
      const [ru, rv] = t.face === 2 ? [t.v, 1 - t.u] : t.face === 3 ? [1 - t.v, t.u] : [t.u, t.v]
      const p = bgfxFaceDir(slotOfFile[t.face], ru * 2 - 1, rv * 2 - 1), l = Math.hypot(...p)
      const pu = Math.atan2(p[0], p[2]) / (2 * Math.PI) + 0.5, pv = 0.5 - Math.asin(p[1] / l) / Math.PI
      const mine = panoramaLookup(d)
      const du = Math.abs(pu - mine.u)
      expect(Math.min(du, 1 - du)).toBeLessThan(1e-9)
      expect(pv).toBeCloseTo(mine.v, 9)
    }
  })
  it('names: skyboxes end in "cube" (the engine loads env/<last part>); sky textures also match *sky*', () => {
    expect(isCubemapPath('env/day_cube')).toBe(true)
    expect(isCubemapPath('any/folder/day_cube')).toBe(true)
    expect(isCubemapPath('env/cube_day')).toBe(false)
    expect(isEnvSkybox('env/day_cube')).toBe(true)
    expect(isEnvSkybox('sky/day_cube')).toBe(false)
    expect(isSkyTexture('sky/blue')).toBe(true)
    expect(isSkyTexture('dev/wall')).toBe(false)
  })
})

describe('level sky settings', () => {
  const w = (kv: Record<string, string>) => ({ id: 'w', classname: 'worldspawn', keyvalues: kv })
  it('mode, margin and the sky texture as the Core picks it', () => {
    expect(autoSkyOf(w({}))).toBe('off')
    expect(autoSkyOf(w({ _sky_auto: 'areas' }))).toBe('areas')
    expect(autoSkyOf(w({ _sky_auto: 'bogus' }))).toBe('off')
    expect(skyMarginOf(w({}))).toBe(8)
    expect(skyMarginOf(w({ _sky_margin: '5000' }))).toBe(1000)
    expect(skyMarginKey('8')).toBeNull()
    expect(skyMarginKey('12.5')).toBe('12.5')
    expect(skyMarginKey('-3')).toBe('0')
    expect(levelSkyTexture(w({ _sky_texture: 'env/b_cube' }), ['env/a_cube'])).toBe('env/b_cube')
    expect(levelSkyTexture(w({ _sky_auto: 'box' }), ['dev/wall', 'env/z_cube', 'env/a_cube'])).toBe('env/a_cube')
    expect(levelSkyTexture(w({}), ['env/a_cube'])).toBe('')
  })
  it('the views show the level sky behind the geometry only with Auto sky on and a skybox chosen', () => {
    const doc = (kv: Record<string, string>) => ({ entities: [w(kv)] }) as any
    expect(viewportSky(doc({ _sky_auto: 'box', _sky_texture: 'env/day_cube' }))).toBe('env/day_cube')
    expect(viewportSky(doc({ _sky_texture: 'env/day_cube' }))).toBeUndefined()
    expect(viewportSky(doc({ _sky_auto: 'box', _sky_texture: 'sky/blue' }))).toBeUndefined()
  })
})

const skyFetch = async (path: string) => {
  if (!isCubemapPath(path)) return { ext: '.png', bytes: new Uint8Array([1]) }
  return { ext: '', bytes: new Uint8Array(0), sky: { panorama: false, images: SKY_FACES.map((f) => ({ ext: '.png', bytes: new TextEncoder().encode(f.suffix) })) } }
}
const fakeDecode = async (_ext: string, bytes: Uint8Array) => ({ width: 8, height: 8, source: { name: new TextDecoder().decode(bytes) } as any })

describe('skyboxes in the texture cache and the browser', () => {
  it('a skybox loads its six faces in the engine order and gets a preview', async () => {
    const t = new TextureCache(skyFetch, fakeDecode, (run) => run())
    t.get('env/day_cube')
    await new Promise((r) => setTimeout(r, 0))
    const sky = t.sky('env/day_cube')!
    expect(sky.panorama).toBe(false)
    expect(sky.faces.map((f) => (f.source as any).name)).toEqual(['_lf', '_rt', '_up', '_dn', '_ft', '_bk'])
    const st = t.peek('env/day_cube')
    expect(st?.status).toBe('ready')
    if (st?.status === 'ready') expect(st.image.width).toBeGreaterThan(0)  // the preview (no canvas here: the first face)
    t.get('dev/wall')
    await new Promise((r) => setTimeout(r, 0))
    expect(t.sky('dev/wall')).toBeUndefined()
  })
  it('the browser lists skyboxes under "Skyboxes" with a SKY badge', async () => {
    const app = makeApp()
    const textures = new TextureCache(skyFetch, fakeDecode, (run) => run())
    const cb = new ContentBrowserPanel(app.store, textures, async () => ['dev/wall', 'env/day_cube', 'sky/blue'])
    document.body.append(cb.element)
    cb.init()
    await cb.reload()
    const folders = [...cb.element.querySelectorAll('.folder')].map((f) => f.textContent)
    expect(folders).toContain('Skyboxes')
    expect(cb.element.querySelector('.tile[data-path="env/day_cube"] .tile-badge')?.textContent).toBe('SKY')
    expect(cb.element.querySelector('.tile[data-path="dev/wall"] .tile-badge')).toBeNull()
    ;([...cb.element.querySelectorAll('.folder')].find((f) => f.textContent === 'Skyboxes') as HTMLElement).click()
    const shown = [...cb.element.querySelectorAll('.tile')].map((t) => (t as HTMLElement).dataset.path)
    expect(shown).toEqual(['env/day_cube', 'sky/blue'])
    expect(SKY_FOLDER).not.toContain('/')
  })
})

describe('Level settings: Sky section', () => {
  it('Auto sky, margin and skybox are worldspawn keys; turning Auto sky on picks the first skybox in the same undo step', async () => {
    const app = makeApp(); app.sync(); addDrawnBrush(app.store, makeBox('b1', [0, 0, 0], [1, 1, 1]))
    const textures = new TextureCache(skyFetch, fakeDecode, (run) => run())
    const p = new LevelSettingsPanel(app.store, { textures, listTextures: async () => ['dev/wall', 'env/night_cube', 'env/day_cube'] })
    p.init()
    const field = (label: string) => [...p.element.querySelectorAll('label.field')].find((l) => l.querySelector('span')!.textContent === label)!
    const select = field('Auto sky').querySelector('select') as HTMLSelectElement
    expect([...select.options].map((o) => o.textContent)).toEqual(['Off', 'Box', 'Box per area'])
    await p.setAutoSky('areas')
    let world = worldspawnOf(app.store.state.doc)!
    expect(world.keyvalues!._sky_auto).toBe('areas')
    expect(world.keyvalues!._sky_texture).toBe('env/day_cube')
    app.store.undo()
    world = worldspawnOf(app.store.state.doc)!
    expect(world.keyvalues?._sky_auto).toBeUndefined()
    expect(world.keyvalues?._sky_texture).toBeUndefined()
    const margin = field('Margin (m)').querySelector('input') as HTMLInputElement
    margin.value = '20'; margin.dispatchEvent(new Event('change'))
    expect(worldspawnOf(app.store.state.doc)!.keyvalues!._sky_margin).toBe('20')
    const name = field('Skybox').querySelector('input') as HTMLInputElement
    name.value = 'env/night_cube'; name.dispatchEvent(new Event('change'))
    expect(worldspawnOf(app.store.state.doc)!.keyvalues!._sky_texture).toBe('env/night_cube')
    setLevelKey(app.store, '_sky_texture', 'sky/blue')
    expect(p.element.textContent).toContain('Not a skybox')
  })
})
