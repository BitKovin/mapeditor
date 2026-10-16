import { afterEach, describe, expect, it } from 'vitest'
import { buildCarvedMesh } from '../src/model/derived'
import type { DerivedEntry, World } from '../src/model/derived'
import { makeBox } from '../src/model/geometry'
import type { MapDocument } from '../src/model/document'
import { buildPreset } from '../src/model/relief'
import { defaultProps, texEnv } from '../src/model/texprops'

afterEach(() => texEnv.setProvider(null))

describe('relief in carved meshes', () => {
  it('a textured fragment with a relief is drawn with depth, a plain one stays flat', () => {
    const b = makeBox('b', [0, 0, 0], [2, 2, 2], 'wall/win')
    const top = b.faces.find((f) => Math.abs(f.loop.reduce((s, i) => s + b.vertices[i][1], 0) / f.loop.length - 2) < 1e-9)!
    const verts = top.loop.flatMap((i) => b.vertices[i])
    const entry = { entityId: 'w', brushId: 'b', instance: '', subtractors: [], fragments: [{ brush: 'b', face: top.id, v: verts }] } as unknown as DerivedEntry
    const world: World = { doc: { entities: [{ id: 'w', brushes: [b] }] } as unknown as MapDocument, prefabs: [] }
    const info = (relief: ReturnType<typeof buildPreset> | null) => (path: string) => ({ path, width: 128, height: 128, ppm: 64, props: { ...defaultProps(), relief } })
    texEnv.setProvider(info(null))
    const flat = buildCarvedMesh(entry, world).textures.get('wall/win')!
    texEnv.setProvider(info(buildPreset('recess', 2, 2)))
    const deep = buildCarvedMesh(entry, world).textures.get('wall/win')!
    const ys = (a: number[]) => { const o: number[] = []; for (let i = 1; i < a.length; i += 8) o.push(a[i]); return o }
    expect(Math.max(...ys(flat)) - Math.min(...ys(flat))).toBeCloseTo(0, 9)
    expect(Math.min(...ys(deep))).toBeLessThan(2 - 0.01) // the top face is y = 2: the recess sinks below it
    expect(Math.max(...ys(deep))).toBeLessThanOrEqual(2 + 1e-9)
    expect(deep.length).toBeGreaterThan(flat.length)
  })
})
