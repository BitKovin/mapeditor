import { describe, expect, it } from 'vitest'
import { readFileSync } from 'node:fs'
import { decodeTga } from '../src/model/tga'
import { TextureCache, TexImage } from '../src/state/textures'
import { texEnv } from '../src/model/texprops'

const tga = (type: number, w: number, h: number, bpp: number, desc: number, body: number[]) =>
  Uint8Array.from([0, 0, type, 0, 0, 0, 0, 0, 0, 0, 0, 0, w & 255, w >> 8, h & 255, h >> 8, bpp, desc, ...body])

describe('decodeTga', () => {
  it('reads an uncompressed 24-bit bottom-left image (BGR -> RGBA, flipped to top-down)', () => {
    // 2x2, bottom-left origin: rows are stored bottom row first
    const img = decodeTga(tga(2, 2, 2, 24, 0, [0, 0, 255, 0, 255, 0, /* bottom: red, green */ 255, 0, 0, 255, 255, 255 /* top: blue, white */]))
    expect([img.width, img.height]).toEqual([2, 2])
    const px = (x: number, y: number) => [...img.data.slice((y * 2 + x) * 4, (y * 2 + x) * 4 + 4)]
    expect(px(0, 0)).toEqual([0, 0, 255, 255])     // top-left = blue
    expect(px(1, 0)).toEqual([255, 255, 255, 255]) // top-right = white
    expect(px(0, 1)).toEqual([255, 0, 0, 255])     // bottom-left = red
    expect(px(1, 1)).toEqual([0, 255, 0, 255])     // bottom-right = green
  })
  it('honours the top-left origin flag and 32-bit alpha', () => {
    const img = decodeTga(tga(2, 1, 2, 32, 0x28, [1, 2, 3, 128, 4, 5, 6, 255]))
    expect([...img.data.slice(0, 4)]).toEqual([3, 2, 1, 128]) // first stored row is the top row
    expect([...img.data.slice(4, 8)]).toEqual([6, 5, 4, 255])
  })
  it('decodes RLE runs and raw packets', () => {
    // 4x1: a 3-pixel run of red, then 1 raw pixel of blue
    const img = decodeTga(tga(10, 4, 1, 24, 0x20, [0x82, 0, 0, 255, 0x00, 255, 0, 0]))
    expect([...img.data]).toEqual([255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 0, 0, 255, 255])
  })
  it('rejects truncated and unsupported files', () => {
    expect(() => decodeTga(new Uint8Array(4))).toThrow(/too short/)
    expect(() => decodeTga(tga(2, 2, 2, 24, 0, [0, 0, 0]))).toThrow(/truncated/)
    expect(() => decodeTga(tga(1, 2, 2, 8, 0, []))).toThrow(/unsupported/)
    expect(() => decodeTga(tga(10, 4, 1, 24, 0, [0x82, 0, 0]))).toThrow(/truncated/)
  })
  it('decodes the real demo TGA shipped with the assets', () => {
    const bytes = readFileSync(new URL('../../core/assets/textures/dev/tga_test.tga', import.meta.url))
    const img = decodeTga(new Uint8Array(bytes))
    expect([img.width, img.height]).toEqual([64, 64])
    expect([...img.data.slice(0, 4)]).toEqual([120, 90, 0, 255]) // top-left corner marker colour
    expect([...img.data.slice((32 * 64 + 32) * 4, (32 * 64 + 32) * 4 + 4)]).toEqual([230, 200, 60, 255]) // centre
  })
})

describe('TextureCache', () => {
  const image: TexImage = { width: 2, height: 2, source: {} as ImageData }
  it('loads once, shares the result, and notifies', async () => {
    let fetches = 0, notified = 0
    const cache = new TextureCache(async () => { fetches++; return { ext: '.png', bytes: new Uint8Array([1]) } }, async () => image)
    cache.onChange(() => notified++)
    expect(cache.get('a')).toEqual({ status: 'loading' })
    cache.get('a'); cache.get('a')
    await new Promise((r) => setTimeout(r, 5))
    expect(cache.get('a')).toMatchObject({ status: 'ready', image })
    expect([fetches, notified]).toEqual([1, 1])
  })
  it('remembers failures instead of retrying every frame', async () => {
    let fetches = 0
    const cache = new TextureCache(async () => { fetches++; throw new Error('nope') }, async () => image)
    cache.get('x'); await new Promise((r) => setTimeout(r, 5))
    cache.get('x'); cache.get('x')
    expect(cache.get('x')).toMatchObject({ status: 'missing', error: 'nope' })
    expect(fetches).toBe(1)
  })
  it('carries the texture properties and the effective pixels per meter, and is the texture environment', async () => {
    const cache = new TextureCache(async () => ({ ext: '.png', bytes: new Uint8Array([1]), props: { pixelsPerMeter: 128, repeat: { v: { mode: 'mirror' } } } }), async () => image)
    expect(cache.info('a')).toBeUndefined() // loading
    await new Promise((r) => setTimeout(r, 5))
    expect(cache.info('a')).toMatchObject({ width: 2, height: 2, ppm: 128 })
    expect(texEnv.info('a')?.props.repeat.v.mode).toBe('mirror')
  })
  it('a texture without properties takes the profile default; changing it or the properties bumps the revision', async () => {
    const cache = new TextureCache(async () => ({ ext: '.png', bytes: new Uint8Array([1]) }), async () => image)
    cache.get('a'); await new Promise((r) => setTimeout(r, 5))
    expect(cache.info('a')?.ppm).toBe(64)
    const r0 = texEnv.rev
    cache.setDefaultPpm(32)
    expect(cache.info('a')?.ppm).toBe(32)
    expect(texEnv.rev).toBeGreaterThan(r0)
    const r1 = texEnv.rev
    cache.setProps('a', { ...cache.propsOf('a'), pixelsPerMeter: 100 })
    expect(cache.info('a')?.ppm).toBe(100)
    expect(texEnv.rev).toBeGreaterThan(r1)
  })
})
