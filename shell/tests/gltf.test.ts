import { describe, expect, it } from 'vitest'
import { animationIndex, isAnimated, parseGlb, poseModel } from '../src/model/gltf'
import type { Entity } from '../src/model/document'
import { entityModelBox, modelMatrix, modelPathOf, modelScaleOf, setModelBoundsProvider } from '../src/model/modelEntity'
import { pointBox } from '../src/model/pointBox'
import { boxFromQuake } from '../src/model/entityBox'
import { modelFromObj } from '../src/state/models'

/** A .glb with one triangle skinned to two joints (the second joint is a child of the first) and an animation lifting the second joint by 2 m over a second. */
function skinnedGlb(): ArrayBuffer {
  const f32 = (...v: number[]) => new Uint8Array(new Float32Array(v).buffer)
  const parts: Uint8Array[] = []
  const views: object[] = [], accessors: object[] = []
  const add = (data: Uint8Array, accessor: object): number => {
    const offset = parts.reduce((n, p) => n + p.length, 0)
    parts.push(data)
    views.push({ buffer: 0, byteOffset: offset, byteLength: data.length })
    accessors.push({ bufferView: views.length - 1, ...accessor })
    return accessors.length - 1
  }
  const position = add(f32(0, 0, 0, 1, 0, 0, 0, 1, 0), { componentType: 5126, count: 3, type: 'VEC3' })
  const joints = add(new Uint8Array([0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0]), { componentType: 5121, count: 3, type: 'VEC4' })
  const weights = add(f32(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0), { componentType: 5126, count: 3, type: 'VEC4' })
  const index = add(new Uint8Array(new Uint16Array([0, 1, 2]).buffer), { componentType: 5123, count: 3, type: 'SCALAR' })
  const ibm = add(f32(...[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1], ...[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]), { componentType: 5126, count: 2, type: 'MAT4' })
  const times = add(f32(0, 1), { componentType: 5126, count: 2, type: 'SCALAR' })
  const lift = add(f32(0, 0, 0, 0, 2, 0), { componentType: 5126, count: 2, type: 'VEC3' })
  const json = {
    asset: { version: '2.0' },
    scene: 0, scenes: [{ nodes: [0, 1] }],
    nodes: [{ mesh: 0, skin: 0 }, { children: [2] }, {}],
    meshes: [{ primitives: [{ attributes: { POSITION: position, JOINTS_0: joints, WEIGHTS_0: weights }, indices: index }] }],
    skins: [{ joints: [1, 2], inverseBindMatrices: ibm }],
    animations: [{ name: 'Lift', samplers: [{ input: times, output: lift }], channels: [{ sampler: 0, target: { node: 2, path: 'translation' } }] }],
    accessors, bufferViews: views, buffers: [{ byteLength: parts.reduce((n, p) => n + p.length, 0) }],
  }
  const bin = new Uint8Array(parts.reduce((n, p) => n + p.length, 0) + 3 & ~3)
  let o = 0
  for (const p of parts) { bin.set(p, o); o += p.length }
  const j = new TextEncoder().encode(JSON.stringify(json))
  const jsonChunk = new Uint8Array(j.length + ((4 - (j.length % 4)) % 4)).fill(0x20)
  jsonChunk.set(j)
  const total = 12 + 8 + jsonChunk.length + 8 + bin.length
  const out = new ArrayBuffer(total), v = new DataView(out)
  v.setUint32(0, 0x46546c67, true); v.setUint32(4, 2, true); v.setUint32(8, total, true)
  v.setUint32(12, jsonChunk.length, true); v.setUint32(16, 0x4e4f534a, true); new Uint8Array(out, 20, jsonChunk.length).set(jsonChunk)
  const b = 20 + jsonChunk.length
  v.setUint32(b, bin.length, true); v.setUint32(b + 4, 0x004e4942, true); new Uint8Array(out, b + 8, bin.length).set(bin)
  return out
}

describe('glb model', () => {
  const model = parseGlb(skinnedGlb())
  it('reads the mesh, the skin and the animation; the bounds are the rest pose', () => {
    expect(model.primitives).toHaveLength(1)
    expect(model.primitives[0].skin).toBe(0)
    expect(model.animations.map((a) => a.name)).toEqual(['Lift'])
    expect(model.animations[0].duration).toBe(1)
    expect(model.bounds).toEqual({ min: [0, 0, 0], max: [1, 1, 0] })
  })
  it('finds an animation by name, ignoring case, or by index; an unknown or empty name is the rest pose', () => {
    expect(animationIndex(model, 'Lift')).toBe(0)
    expect(animationIndex(model, 'lift')).toBe(0)
    expect(animationIndex(model, '0')).toBe(0)
    expect(animationIndex(model, 'walk')).toBe(-1)
    expect(animationIndex(model, '')).toBe(-1)
    expect(isAnimated(model, 0)).toBe(true)
    expect(isAnimated(model, -1)).toBe(false)
  })
  it('skins the vertices with the joints at the time of the animation, looping', () => {
    const rest = poseModel(model, -1, 0)[0].positions
    expect(Array.from(rest)).toEqual([0, 0, 0, 1, 0, 0, 0, 1, 0])
    const half = poseModel(model, 0, 0.5)[0].positions // joint 2 is 1 m up: only the third vertex follows it
    expect(Array.from(half)).toEqual([0, 0, 0, 1, 0, 0, 0, 2, 0])
    const looped = poseModel(model, 0, 1.5)[0].positions
    expect(looped[7]).toBeCloseTo(2)
    const into = poseModel(model, 0, 0.25, poseModel(model, -1, 0))
    expect(into[0].positions[7]).toBeCloseTo(1.5) // reuses the buffers it is given
  })
  it('rejects things that are not a glb', () => {
    expect(() => parseGlb(new ArrayBuffer(8))).toThrow(/glb/)
  })
})

describe('entity model placement', () => {
  const e = (kv: Record<string, string>, over: Partial<Entity> = {}): Entity => ({ id: 'e', classname: 'npc_guard', origin: [1, 2, 3], keyvalues: kv, ...over })
  it('model_path is relative, slashes forward; brush entities have none', () => {
    expect(modelPathOf(e({ model_path: ' models\\guard.glb ' }))).toBe('models/guard.glb')
    expect(modelPathOf(e({}))).toBe('')
    expect(modelPathOf(e({ model_path: 'a.glb' }, { brushes: [{ id: 'b', vertices: [], faces: [] }] }))).toBe('')
  })
  it('model_scale is one number or three, times modelscale and modelscale_vec', () => {
    expect(modelScaleOf(e({}))).toEqual([1, 1, 1])
    expect(modelScaleOf(e({ model_scale: '2' }))).toEqual([2, 2, 2])
    expect(modelScaleOf(e({ model_scale: '1 2 3', modelscale_vec: '2 2 2' }))).toEqual([2, 4, 6])
    expect(modelScaleOf(e({ model_scale: 'abc' }))).toEqual([1, 1, 1])
  })
  it('the matrix is translate * rotate(angles) * scale (column major)', () => {
    const m = modelMatrix(e({ model_scale: '2' }, { angles: [0, 90, 0] }))
    expect(Array.from(m.slice(12, 15))).toEqual([1, 2, 3])
    // yaw 90 turns the model's +X (scaled by 2) into -Z
    expect(m[0]).toBeCloseTo(0); expect(m[2]).toBeCloseTo(-2)
  })
  it('a loaded model replaces the class box in pointBox, scaled, and the class box is used until then', () => {
    const cls = boxFromQuake([-16, -16, -16, 16, 16, 16])
    const guard = e({ model_path: 'models/guard.glb', model_scale: '2' })
    expect(pointBox(guard, () => cls).min).toEqual(cls.min)
    setModelBoundsProvider((p) => (p === 'models/guard.glb' ? { min: [-0.5, 0, -0.5], max: [0.5, 1.8, 0.5] } : null))
    try {
      expect(entityModelBox(guard)).toEqual({ min: [-1, 0, -1], max: [1, 3.6, 1] })
      const b = pointBox(guard, () => cls)
      expect(b.max).toEqual([1, 3.6, 1])
      expect(b.origin).toEqual([1, 2, 3])
      expect(pointBox(e({}), () => cls).min).toEqual(cls.min) // no model_path: the class box
    } finally { setModelBoundsProvider(() => null) }
  })
})

describe('obj model from Core', () => {
  it('builds one primitive per material with its texture path', () => {
    const f32 = (v: number[]) => btoa(String.fromCharCode(...new Uint8Array(new Float32Array(v).buffer)))
    const m = modelFromObj({ bounds: [0, 0, 0, 1, 1, 0], groups: [{ texture: '/models/q', positions: f32([0, 0, 0, 1, 0, 0, 1, 1, 0]), uvs: f32([0, 1, 1, 1, 1, 0]), normals: f32([0, 0, 1, 0, 0, 1, 0, 0, 1]) }] })
    expect(m.primitives).toHaveLength(1)
    expect(m.primitives[0].texture).toBe('/models/q')
    expect(Array.from(m.primitives[0].indices)).toEqual([0, 1, 2])
    expect(m.bounds.max).toEqual([1, 1, 0])
  })
})
