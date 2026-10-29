// glTF 2.0 binary (.glb) reader and CPU skinning, for the model preview of entities (model_path / model_animation). Pure: no GL, no DOM.
//
// Metres, Y up, +Z forward, right handed: the editor's own conventions, so a model is drawn as it is. Supported: meshes of triangle lists / strips / fans,
// node hierarchies (TRS or matrix), skins (JOINTS_0 / WEIGHTS_0), animations (translation / rotation / scale; LINEAR, STEP and CUBICSPLINE), the base
// colour of the material (factor and texture) and the alpha mode. Not supported: morph targets, sparse accessors, KHR extensions other than the
// pass-through of quantised attributes (normalised integers are read as such).
import { mat4, quat } from 'gl-matrix'
import type { Box } from './entityBox'

export interface ModelPrimitive {
  positions: Float32Array   // rest pose, model space, 3 per vertex (the node's transform is NOT applied for skinned primitives, see poseModel)
  normals: Float32Array
  uvs: Float32Array         // 2 per vertex, v grows down the image (the glTF and WebGL convention)
  indices: Uint32Array
  joints: Uint16Array | null  // 4 per vertex
  weights: Float32Array | null
  node: number              // the node that carries the mesh (-1: none, positions are final)
  skin: number              // index into ModelData.skins, -1 = none
  color: [number, number, number, number]
  image: number             // index into ModelData.images (an image inside the file), -1 = none
  texture: string           // a texture path for the editor's texture cache ("/models/x/y"), '' = none (an .obj's material)
  alphaMode: 'OPAQUE' | 'MASK' | 'BLEND'
  cutoff: number
}
export interface ModelNode { parent: number; children: number[]; t: [number, number, number]; r: [number, number, number, number]; s: [number, number, number]; matrix: Float32Array | null }
export interface ModelSkin { joints: number[]; inverseBind: Float32Array }  // 16 floats per joint
export interface ModelChannel { node: number; path: 'translation' | 'rotation' | 'scale'; interp: 'LINEAR' | 'STEP' | 'CUBICSPLINE'; times: Float32Array; values: Float32Array }
export interface ModelAnimation { name: string; duration: number; channels: ModelChannel[] }
export interface ModelImage { mime: string; bytes: Uint8Array }

export interface ModelData {
  primitives: ModelPrimitive[]
  nodes: ModelNode[]
  roots: number[]
  skins: ModelSkin[]
  animations: ModelAnimation[]
  images: ModelImage[]
  /** The rest pose bounds in model space (what the editor shows as the entity's box). */
  bounds: Box
}

const COMPONENT: Record<number, { size: number; read: (v: DataView, o: number) => number; norm: number }> = {
  5120: { size: 1, read: (v, o) => v.getInt8(o), norm: 127 },
  5121: { size: 1, read: (v, o) => v.getUint8(o), norm: 255 },
  5122: { size: 2, read: (v, o) => v.getInt16(o, true), norm: 32767 },
  5123: { size: 2, read: (v, o) => v.getUint16(o, true), norm: 65535 },
  5125: { size: 4, read: (v, o) => v.getUint32(o, true), norm: 4294967295 },
  5126: { size: 4, read: (v, o) => v.getFloat32(o, true), norm: 1 },
}
const WIDTH: Record<string, number> = { SCALAR: 1, VEC2: 2, VEC3: 3, VEC4: 4, MAT2: 4, MAT3: 9, MAT4: 16 }

/** Parses a .glb file. Throws a readable Error for anything that is not a usable binary glTF 2.0. */
export function parseGlb(buf: ArrayBuffer): ModelData {
  const view = new DataView(buf)
  if (buf.byteLength < 20 || view.getUint32(0, true) !== 0x46546c67) throw new Error('not a binary glTF (.glb) file')
  if (view.getUint32(4, true) !== 2) throw new Error('only glTF 2.0 is supported')
  let json: any = null
  let bin: Uint8Array | null = null
  for (let o = 12; o + 8 <= Math.min(buf.byteLength, view.getUint32(8, true)); ) {
    const len = view.getUint32(o, true), type = view.getUint32(o + 4, true)
    const start = o + 8
    if (type === 0x4e4f534a) json = JSON.parse(new TextDecoder().decode(new Uint8Array(buf, start, len)))
    else if (type === 0x004e4942 && !bin) bin = new Uint8Array(buf, start, len)
    o = start + len + ((4 - (len % 4)) % 4)
  }
  if (!json) throw new Error('the .glb has no JSON chunk')
  return parseGltf(json, bin)
}

export function parseGltf(g: any, bin: Uint8Array | null): ModelData {
  const bufferViews: any[] = g.bufferViews ?? [], accessors: any[] = g.accessors ?? []
  const bytesOf = (bufferIndex: number): Uint8Array => {
    const b = g.buffers?.[bufferIndex]
    if (bufferIndex === 0 && bin) return bin
    if (b?.uri?.startsWith('data:')) return base64(b.uri.slice(b.uri.indexOf(',') + 1))
    throw new Error('the glTF refers to an external buffer; use a .glb')
  }
  const viewBytes = (viewIndex: number): Uint8Array => {
    const bv = bufferViews[viewIndex]
    if (!bv) throw new Error('bad bufferView')
    const b = bytesOf(bv.buffer ?? 0)
    return b.subarray(bv.byteOffset ?? 0, (bv.byteOffset ?? 0) + bv.byteLength)
  }
  /** An accessor as plain numbers: floats (normalised integers scaled to 0..1 / -1..1) or raw integers. */
  const read = (index: number, ints = false): { data: Float32Array | Uint32Array; width: number; count: number } => {
    const a = accessors[index]
    if (!a) throw new Error('bad accessor')
    const comp = COMPONENT[a.componentType], width = WIDTH[a.type]
    if (!comp || !width) throw new Error('unsupported accessor')
    const count: number = a.count
    const out = ints ? new Uint32Array(count * width) : new Float32Array(count * width)
    if (a.bufferView !== undefined) {
      const bytes = viewBytes(a.bufferView)
      const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
      const stride = bufferViews[a.bufferView].byteStride || comp.size * width
      const scale = !ints && a.normalized ? 1 / comp.norm : 1
      for (let i = 0; i < count; i++)
        for (let k = 0; k < width; k++) {
          const v = comp.read(dv, (a.byteOffset ?? 0) + i * stride + k * comp.size)
          out[i * width + k] = a.componentType === 5126 || !a.normalized ? v : Math.max(-1, v * scale)
        }
    }
    return { data: out, width, count }
  }

  // images (the bytes only; the viewer decodes them)
  const images: ModelImage[] = (g.images ?? []).map((im: any): ModelImage => {
    if (im.bufferView !== undefined) return { mime: im.mimeType ?? 'image/png', bytes: viewBytes(im.bufferView).slice() }
    if (typeof im.uri === 'string' && im.uri.startsWith('data:')) return { mime: im.uri.slice(5, im.uri.indexOf(';')), bytes: base64(im.uri.slice(im.uri.indexOf(',') + 1)) }
    return { mime: '', bytes: new Uint8Array(0) } // an external file: not available from a .glb
  })

  const nodes: ModelNode[] = (g.nodes ?? []).map((n: any): ModelNode => ({
    parent: -1, children: n.children ?? [],
    t: n.translation ?? [0, 0, 0], r: n.rotation ?? [0, 0, 0, 1], s: n.scale ?? [1, 1, 1],
    matrix: Array.isArray(n.matrix) && n.matrix.length === 16 ? Float32Array.from(n.matrix) : null,
  }))
  nodes.forEach((n, i) => n.children.forEach((c) => { if (nodes[c]) nodes[c].parent = i }))
  const sceneRoots: number[] | undefined = g.scenes?.[g.scene ?? 0]?.nodes
  const roots = sceneRoots?.length ? sceneRoots : nodes.map((_, i) => i).filter((i) => nodes[i].parent < 0)

  const skins: ModelSkin[] = (g.skins ?? []).map((s: any): ModelSkin => {
    const joints: number[] = s.joints ?? []
    const ibm = s.inverseBindMatrices !== undefined ? (read(s.inverseBindMatrices).data as Float32Array) : null
    const inverseBind = new Float32Array(joints.length * 16)
    for (let j = 0; j < joints.length; j++) {
      if (ibm) inverseBind.set(ibm.subarray(j * 16, j * 16 + 16), j * 16)
      else mat4.identity(inverseBind.subarray(j * 16, j * 16 + 16) as unknown as mat4)
    }
    return { joints, inverseBind }
  })

  const primitives: ModelPrimitive[] = []
  const reachable = new Set<number>()
  const visit = (i: number) => { if (reachable.has(i) || !nodes[i]) return; reachable.add(i); nodes[i].children.forEach(visit) }
  roots.forEach(visit)
  for (const ni of reachable) {
    const meshIndex = g.nodes[ni].mesh
    if (meshIndex === undefined) continue
    const skin: number = g.nodes[ni].skin ?? -1
    for (const p of g.meshes?.[meshIndex]?.primitives ?? []) {
      const prim = readPrimitive(p, read, g, skin, ni)
      if (prim) primitives.push(prim)
    }
  }

  const animations: ModelAnimation[] = (g.animations ?? []).map((a: any, i: number): ModelAnimation => {
    const channels: ModelChannel[] = []
    let duration = 0
    for (const c of a.channels ?? []) {
      const sampler = a.samplers?.[c.sampler]
      const path = c.target?.path
      if (!sampler || c.target?.node === undefined || (path !== 'translation' && path !== 'rotation' && path !== 'scale')) continue
      const times = read(sampler.input).data as Float32Array, values = read(sampler.output).data as Float32Array
      const interp = sampler.interpolation === 'STEP' ? 'STEP' : sampler.interpolation === 'CUBICSPLINE' ? 'CUBICSPLINE' : 'LINEAR'
      if (times.length) duration = Math.max(duration, times[times.length - 1])
      channels.push({ node: c.target.node, path, interp, times, values })
    }
    return { name: typeof a.name === 'string' && a.name ? a.name : `animation ${i}`, duration, channels }
  })

  const model: ModelData = { primitives, nodes, roots, skins, animations, images, bounds: { min: [0, 0, 0], max: [0, 0, 0] } }
  model.bounds = boundsOfPose(poseModel(model, -1, 0))
  return model
}

function readPrimitive(p: any, read: (i: number, ints?: boolean) => { data: Float32Array | Uint32Array; width: number; count: number }, g: any, skin: number, node: number): ModelPrimitive | null {
  const attr = p.attributes ?? {}
  if (attr.POSITION === undefined) return null
  const pos = read(attr.POSITION).data as Float32Array
  const vertices = pos.length / 3
  let indices: Uint32Array
  const mode = p.mode ?? 4
  if (p.indices !== undefined) indices = read(p.indices, true).data as Uint32Array
  else indices = Uint32Array.from({ length: vertices }, (_, i) => i)
  if (mode === 5) { // strip
    const out: number[] = []
    for (let i = 0; i + 2 < indices.length; i++) out.push(...(i % 2 ? [indices[i + 1], indices[i], indices[i + 2]] : [indices[i], indices[i + 1], indices[i + 2]]))
    indices = Uint32Array.from(out)
  } else if (mode === 6) { // fan
    const out: number[] = []
    for (let i = 1; i + 1 < indices.length; i++) out.push(indices[0], indices[i], indices[i + 1])
    indices = Uint32Array.from(out)
  } else if (mode !== 4) return null // points and lines
  indices = indices.subarray(0, indices.length - (indices.length % 3))
  const uvs = attr.TEXCOORD_0 !== undefined ? (read(attr.TEXCOORD_0).data as Float32Array) : new Float32Array(vertices * 2)
  const normals = attr.NORMAL !== undefined ? (read(attr.NORMAL).data as Float32Array) : vertexNormals(pos, indices)
  const joints = attr.JOINTS_0 !== undefined && attr.WEIGHTS_0 !== undefined && skin >= 0 ? Uint16Array.from(read(attr.JOINTS_0, true).data) : null
  const weights = joints ? (read(attr.WEIGHTS_0).data as Float32Array) : null
  const mat = g.materials?.[p.material]
  const pbr = mat?.pbrMetallicRoughness
  const texIndex = pbr?.baseColorTexture?.index
  const image: number = texIndex !== undefined ? g.textures?.[texIndex]?.source ?? -1 : -1
  return {
    positions: pos, normals, uvs, indices, joints, weights, node, skin: joints ? skin : -1,
    color: pbr?.baseColorFactor ?? [1, 1, 1, 1], image, texture: '',
    alphaMode: mat?.alphaMode === 'MASK' ? 'MASK' : mat?.alphaMode === 'BLEND' ? 'BLEND' : 'OPAQUE', cutoff: mat?.alphaCutoff ?? 0.5,
  }
}

function vertexNormals(pos: Float32Array, idx: Uint32Array): Float32Array {
  const n = new Float32Array(pos.length)
  for (let i = 0; i + 2 < idx.length; i += 3) {
    const a = idx[i] * 3, b = idx[i + 1] * 3, c = idx[i + 2] * 3
    const ux = pos[b] - pos[a], uy = pos[b + 1] - pos[a + 1], uz = pos[b + 2] - pos[a + 2]
    const vx = pos[c] - pos[a], vy = pos[c + 1] - pos[a + 1], vz = pos[c + 2] - pos[a + 2]
    const x = uy * vz - uz * vy, y = uz * vx - ux * vz, z = ux * vy - uy * vx
    for (const o of [a, b, c]) { n[o] += x; n[o + 1] += y; n[o + 2] += z }
  }
  for (let i = 0; i < n.length; i += 3) {
    const l = Math.hypot(n[i], n[i + 1], n[i + 2]) || 1
    n[i] /= l; n[i + 1] /= l; n[i + 2] /= l
  }
  return n
}

function base64(b64: string): Uint8Array {
  const s = atob(b64)
  const out = new Uint8Array(s.length)
  for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i)
  return out
}

// ---- posing --------------------------------------------------------------------------------------------------------------------------------

export interface PosedPrimitive { positions: Float32Array; normals: Float32Array }

/** Index of the animation `name` names (exact, then ignoring case; a plain number is an index), -1 for none / not found. */
export function animationIndex(model: ModelData, name: string): number {
  const n = name.trim()
  if (!n || !model.animations.length) return -1
  let i = model.animations.findIndex((a) => a.name === n)
  if (i < 0) i = model.animations.findIndex((a) => a.name.toLowerCase() === n.toLowerCase())
  if (i < 0 && /^\d+$/.test(n) && Number(n) < model.animations.length) i = Number(n)
  return i
}

/** True when posing at different times gives different results (a skinned primitive or a node animation exists). */
export const isAnimated = (model: ModelData, anim: number): boolean => anim >= 0 && anim < model.animations.length && model.animations[anim].duration > 0 && model.animations[anim].channels.length > 0

function sampleChannel(c: ModelChannel, t: number, out: number[]): void {
  const width = c.path === 'rotation' ? 4 : 3
  const times = c.times, n = times.length
  const cubic = c.interp === 'CUBICSPLINE'
  const val = (k: number): number[] => { const o = cubic ? k * width * 3 + width : k * width; return Array.from(c.values.subarray(o, o + width)) }
  if (n === 0) return
  if (t <= times[0] || n === 1) { out.splice(0, width, ...val(0)); return }
  if (t >= times[n - 1]) { out.splice(0, width, ...val(n - 1)); return }
  let k = 0
  while (k + 2 < n && times[k + 1] <= t) k++
  const t0 = times[k], t1 = times[k + 1], dt = t1 - t0, u = dt > 0 ? (t - t0) / dt : 0
  if (c.interp === 'STEP') { out.splice(0, width, ...val(k)); return }
  if (c.interp === 'CUBICSPLINE') {
    const base = (i: number) => i * width * 3
    const inTan = (i: number) => c.values.subarray(base(i), base(i) + width), v = (i: number) => c.values.subarray(base(i) + width, base(i) + 2 * width), outTan = (i: number) => c.values.subarray(base(i) + 2 * width, base(i) + 3 * width)
    const u2 = u * u, u3 = u2 * u
    const h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2
    const r: number[] = []
    for (let j = 0; j < width; j++) r.push(h00 * v(k)[j] + h10 * dt * outTan(k)[j] + h01 * v(k + 1)[j] + h11 * dt * inTan(k + 1)[j])
    if (width === 4) { const l = Math.hypot(...r) || 1; for (let j = 0; j < 4; j++) r[j] /= l }
    out.splice(0, width, ...r)
    return
  }
  const a = val(k), b = val(k + 1)
  if (width === 4) out.splice(0, 4, ...(quat.slerp(quat.create(), a as unknown as quat, b as unknown as quat, u) as unknown as number[]))
  else out.splice(0, 3, a[0] + (b[0] - a[0]) * u, a[1] + (b[1] - a[1]) * u, a[2] + (b[2] - a[2]) * u)
}

/** World matrices of every node at `time` (seconds, looped) of animation `anim` (-1 = the rest pose). */
export function nodeWorlds(model: ModelData, anim: number, time: number): Float32Array[] {
  const n = model.nodes.length
  const t = model.nodes.map((x) => [...x.t]), r = model.nodes.map((x) => [...x.r]), s = model.nodes.map((x) => [...x.s])
  if (anim >= 0 && anim < model.animations.length) {
    const a = model.animations[anim]
    const tt = a.duration > 0 ? ((time % a.duration) + a.duration) % a.duration : 0
    for (const c of a.channels) {
      if (!model.nodes[c.node]) continue
      sampleChannel(c, tt, c.path === 'translation' ? t[c.node] : c.path === 'rotation' ? r[c.node] : s[c.node])
    }
  }
  const worlds: Float32Array[] = new Array(n)
  const done = new Uint8Array(n)
  const compute = (i: number): Float32Array => {
    if (done[i]) return worlds[i]
    const node = model.nodes[i]
    const local = node.matrix ? mat4.clone(node.matrix) : mat4.fromRotationTranslationScale(mat4.create(), r[i] as unknown as quat, t[i] as unknown as [number, number, number], s[i] as unknown as [number, number, number])
    const out = node.parent >= 0 ? mat4.multiply(mat4.create(), compute(node.parent), local) : local
    worlds[i] = out as Float32Array
    done[i] = 1
    return worlds[i]
  }
  for (let i = 0; i < n; i++) compute(i)
  return worlds
}

/**
 * The vertices of every primitive in model space at `time` of animation `anim` (-1 = rest pose). Skinned primitives follow their joints; the others
 * take their node's world matrix. `out` (the result of an earlier call for the same model) is overwritten instead of allocating.
 */
export function poseModel(model: ModelData, anim: number, time: number, out?: PosedPrimitive[]): PosedPrimitive[] {
  const worlds = nodeWorlds(model, anim, time)
  const skinMats = new Map<number, Float32Array>()
  const jointsOf = (skin: number): Float32Array => {
    let m = skinMats.get(skin)
    if (m) return m
    const s = model.skins[skin]
    m = new Float32Array(s.joints.length * 16)
    const tmp = mat4.create()
    s.joints.forEach((j, k) => {
      mat4.multiply(tmp, worlds[j] ?? mat4.create(), s.inverseBind.subarray(k * 16, k * 16 + 16) as unknown as mat4)
      m!.set(tmp, k * 16)
    })
    skinMats.set(skin, m)
    return m
  }
  return model.primitives.map((p, pi): PosedPrimitive => {
    const o = out?.[pi] ?? { positions: new Float32Array(p.positions.length), normals: new Float32Array(p.normals.length) }
    const P = p.positions, N = p.normals, vp = o.positions, vn = o.normals
    if (p.skin >= 0 && p.joints && p.weights) {
      const J = jointsOf(p.skin)
      for (let v = 0; v < P.length / 3; v++) {
        let x = 0, y = 0, z = 0, nx = 0, ny = 0, nz = 0
        const px = P[v * 3], py = P[v * 3 + 1], pz = P[v * 3 + 2], qx = N[v * 3], qy = N[v * 3 + 1], qz = N[v * 3 + 2]
        for (let k = 0; k < 4; k++) {
          const w = p.weights[v * 4 + k]
          if (!w) continue
          const m = (p.joints[v * 4 + k]) * 16
          x += w * (J[m] * px + J[m + 4] * py + J[m + 8] * pz + J[m + 12])
          y += w * (J[m + 1] * px + J[m + 5] * py + J[m + 9] * pz + J[m + 13])
          z += w * (J[m + 2] * px + J[m + 6] * py + J[m + 10] * pz + J[m + 14])
          nx += w * (J[m] * qx + J[m + 4] * qy + J[m + 8] * qz)
          ny += w * (J[m + 1] * qx + J[m + 5] * qy + J[m + 9] * qz)
          nz += w * (J[m + 2] * qx + J[m + 6] * qy + J[m + 10] * qz)
        }
        const l = Math.hypot(nx, ny, nz) || 1
        vp[v * 3] = x; vp[v * 3 + 1] = y; vp[v * 3 + 2] = z
        vn[v * 3] = nx / l; vn[v * 3 + 1] = ny / l; vn[v * 3 + 2] = nz / l
      }
    } else {
      const M = p.node >= 0 ? worlds[p.node] : mat4.create()
      for (let v = 0; v < P.length / 3; v++) {
        const px = P[v * 3], py = P[v * 3 + 1], pz = P[v * 3 + 2], qx = N[v * 3], qy = N[v * 3 + 1], qz = N[v * 3 + 2]
        vp[v * 3] = M[0] * px + M[4] * py + M[8] * pz + M[12]
        vp[v * 3 + 1] = M[1] * px + M[5] * py + M[9] * pz + M[13]
        vp[v * 3 + 2] = M[2] * px + M[6] * py + M[10] * pz + M[14]
        const nx = M[0] * qx + M[4] * qy + M[8] * qz, ny = M[1] * qx + M[5] * qy + M[9] * qz, nz = M[2] * qx + M[6] * qy + M[10] * qz
        const l = Math.hypot(nx, ny, nz) || 1
        vn[v * 3] = nx / l; vn[v * 3 + 1] = ny / l; vn[v * 3 + 2] = nz / l
      }
    }
    return o
  })
}

export function boundsOfPose(posed: PosedPrimitive[]): Box {
  const min: [number, number, number] = [Infinity, Infinity, Infinity], max: [number, number, number] = [-Infinity, -Infinity, -Infinity]
  for (const p of posed) for (let i = 0; i < p.positions.length; i += 3) for (let k = 0; k < 3; k++) { min[k] = Math.min(min[k], p.positions[i + k]); max[k] = Math.max(max[k], p.positions[i + k]) }
  return min[0] === Infinity ? { min: [0, 0, 0], max: [0, 0, 0] } : { min, max }
}
