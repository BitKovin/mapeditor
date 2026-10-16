// @vitest-environment jsdom
import { describe, expect, it } from 'vitest'
import { MAX_PARALLEL_LOADS, TextureCache, TexImage } from '../src/state/textures'
import { texEnv } from '../src/model/texprops'
import { fitRect } from '../src/ui/fitImage'
import { ThumbnailLoader } from '../src/ui/textureThumbs'

const image = (width = 2, height = 2): TexImage => ({ width, height, source: {} as ImageData })
const settle = () => new Promise((r) => setTimeout(r, 0))

/** A cache whose fetches wait until the test lets them finish. */
function gated() {
  const started: string[] = [], pending = new Map<string, () => void>()
  const cache = new TextureCache((path) => { started.push(path); return new Promise((res) => pending.set(path, () => res({ ext: '.png', bytes: new Uint8Array([1]) }))) }, async () => image())
  const finish = async (path: string) => { pending.get(path)!(); pending.delete(path); await settle() }
  return { cache, started, finish }
}

describe('texture loading: a few at a time, map textures first, thumbnails cancellable', () => {
  it('loads at most MAX_PARALLEL_LOADS at once and serves the map before thumbnails', async () => {
    const { cache, started, finish } = gated()
    for (let i = 0; i < 6; i++) cache.request(`thumb${i}`)
    cache.get('wall')
    expect(started).toEqual(['thumb0', 'thumb1', 'thumb2', 'thumb3'].slice(0, MAX_PARALLEL_LOADS))
    await finish('thumb0')
    expect(started.at(-1)).toBe('wall') // the free slot goes to the map's texture, not to thumb4
  })
  it('a thumbnail scrolled away before its turn is dropped; the map asking for it promotes it', async () => {
    const { cache, started, finish } = gated()
    for (let i = 0; i < 6; i++) cache.request(`t${i}`)
    cache.cancel('t4')
    expect(cache.peek('t4')).toBeUndefined()
    cache.get('t5') // now wanted by the map
    await finish('t0')
    expect(started).toContain('t5')
    await finish('t1'); await finish('t2'); await finish('t3'); await finish('t5')
    expect(started).not.toContain('t4')
  })
  it('a thumbnail-only texture does not make the viewports rebuild; a map texture does, once per burst', async () => {
    const cache = new TextureCache(async () => ({ ext: '.png', bytes: new Uint8Array([1]) }), async () => image())
    let changes = 0, loads = 0
    cache.onChange(() => changes++); cache.onLoad(() => loads++)
    const rev = texEnv.rev
    cache.request('thumb'); await settle()
    expect([changes, loads, texEnv.rev]).toEqual([0, 1, rev])
    cache.get('a'); cache.get('b'); cache.get('c'); await settle()
    expect(changes).toBe(1)
    expect(texEnv.rev).toBeGreaterThan(rev)
  })
})

describe('thumbnails keep the aspect ratio', () => {
  it('fits and centres any size into the box', () => {
    expect(fitRect(256, 64, 84, 84)).toEqual({ x: 0, y: 31.5, w: 84, h: 21 })
    expect(fitRect(64, 128, 84, 84)).toEqual({ x: 21, y: 0, w: 42, h: 84 })
    expect(fitRect(512, 512, 84, 84)).toEqual({ x: 0, y: 0, w: 84, h: 84 })
  })
  it('the loader asks only for tiles it shows and drops the rest on reset', async () => {
    const { cache, started } = gated()
    const loader = new ThumbnailLoader(cache, document.body) // no IntersectionObserver in jsdom: every tile counts as shown
    for (let i = 0; i < 6; i++) loader.canvas(`t${i}`)
    expect(started).toHaveLength(MAX_PARALLEL_LOADS)
    loader.reset()
    expect(cache.peek('t5')).toBeUndefined() // still waiting: dropped
    expect(cache.peek('t0')).toEqual({ status: 'loading' }) // already being read: finishes
    loader.dispose()
  })
})
