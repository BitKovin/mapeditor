// Texture transparency (REQUIREMENTS §23): stored form, resolution against the image's alpha (none / cutout / glass), what the views draw.
import { describe, expect, it } from 'vitest'
import { AlphaKind, defaultProps, drawTransparency, effectiveOpacity, parseProps, parseTransparency, propsToJson, resolvedTransparency, TexInfo, Transparency } from '../src/model/texprops'
import { classifyAlpha } from '../src/state/textures'

const T = (over: Partial<Transparency> = {}): Transparency => ({ mode: 'auto', cutoff: 0.5, opacity: 0.5, tint: 1, ...over })
const info = (over: Partial<Transparency>, alpha: AlphaKind): TexInfo => {
  const props = defaultProps()
  props.transparency = T(over)
  return { path: 't', width: 4, height: 4, ppm: 64, props, alpha }
}
const rgba = (alphas: number[]) => alphas.flatMap((a) => [10, 20, 30, a])

describe('transparency props', () => {
  it('defaults to auto, round-trips through the stored JSON, and a damaged value keeps the defaults', () => {
    expect(defaultProps().transparency).toEqual(T())
    const p = defaultProps(); p.transparency = T({ mode: 'translucent', cutoff: 0.3, opacity: 0.25, tint: 0.5 })
    expect(parseProps(propsToJson(p)).transparency).toEqual(p.transparency)
    expect(parseTransparency({ mode: 'glass', opacity: 7, tint: 'x' })).toEqual(T({ opacity: 1 }))
    expect(parseTransparency(null).mode).toBe('auto')
  })
  it('classifies alpha like the Core: none, on / off holes, or half transparent glass', () => {
    expect(classifyAlpha(rgba([255, 250, 241]))).toBe('none')
    expect(classifyAlpha(rgba([0, 255, 0, 255, 0, 255, 0, 128, 255]))).toBe('cutout')
    expect(classifyAlpha(rgba([120, 120, 120, 255, 255]))).toBe('translucent')
    expect(classifyAlpha(rgba([200, 200, 200, 200]))).toBe('translucent')
  })
  it('auto follows the image: glass alpha -> translucent with the painted alpha as opacity; explicit modes as set', () => {
    expect(resolvedTransparency(T(), 'translucent')).toBe('translucent')
    expect(effectiveOpacity(T(), 'translucent')).toBe(1)
    expect(resolvedTransparency(T(), 'cutout')).toBe('cutout')
    expect(resolvedTransparency(T(), 'none')).toBe('opaque')
    expect(resolvedTransparency(T({ mode: 'cutout' }), 'none')).toBe('opaque')
    expect(resolvedTransparency(T({ mode: 'cutout' }), 'translucent')).toBe('cutout')
    expect(resolvedTransparency(T({ mode: 'opaque' }), 'translucent')).toBe('opaque')
    expect(resolvedTransparency(T({ mode: 'translucent' }), 'none')).toBe('translucent')
    expect(effectiveOpacity(T({ mode: 'translucent', opacity: 0.3 }), 'translucent')).toBe(0.3)
  })
  it('what the views draw: mode, cutoff, opacity', () => {
    expect(drawTransparency(undefined)).toEqual([0, 0, 0])
    expect(drawTransparency(info({}, 'none'))).toEqual([0, 0, 0])
    expect(drawTransparency(info({ cutoff: 0.3 }, 'cutout'))).toEqual([1, 0.3, 0])
    expect(drawTransparency(info({}, 'translucent'))).toEqual([2, 0.5, 1])   // glass: blended by its own alpha
    expect(drawTransparency(info({ mode: 'translucent', opacity: 0.4 }, 'none'))).toEqual([2, 0.5, 0.4])
  })
})
