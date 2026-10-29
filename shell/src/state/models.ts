// Models for entities (model_path): .glb files are read as bytes and parsed here (model/gltf.ts), .obj files are read by Core (read_model), which also
// finds their textures. Loaded once per path and shared by every viewport; a model that is still loading or cannot be read leaves the entity's box as it is.
import type { Box } from '../model/entityBox'
import type { ModelData, ModelPrimitive } from '../model/gltf'
import { parseGlb } from '../model/gltf'
import { setModelBoundsProvider } from '../model/modelEntity'
import { base64ToBytes } from './textures'

/** What Core's read_model answers (core/include/core/protocol.h). */
export interface ObjResponse { bounds: number[]; warnings?: string[]; groups: { texture: string; positions: string; uvs: string; normals: string }[] }
export interface ModelLoader {
  readFile(path: string): Promise<Uint8Array>
  readModel(path: string): Promise<ObjResponse>
}
/** An image of a .glb, decoded for the GPU (null when it cannot be decoded). */
export type DecodedImage = ImageBitmap | HTMLCanvasElement | null
export type ModelSlot =
  | { status: 'loading' }
  | { status: 'error'; error: string }
  | { status: 'ready'; model: ModelData; images: DecodedImage[]; warnings: string[] }

const floats = (b64: string): Float32Array => {
  const bytes = base64ToBytes(b64)
  return new Float32Array(bytes.buffer, bytes.byteOffset, Math.floor(bytes.byteLength / 4))
}

/** The model of a read_model answer: one primitive per material, already in model space. */
export function modelFromObj(r: ObjResponse): ModelData {
  const primitives: ModelPrimitive[] = r.groups.map((g) => {
    const positions = floats(g.positions)
    return {
      positions, normals: floats(g.normals), uvs: floats(g.uvs), indices: Uint32Array.from({ length: positions.length / 3 }, (_, i) => i),
      joints: null, weights: null, node: -1, skin: -1, color: [1, 1, 1, 1], image: -1, texture: g.texture, alphaMode: 'OPAQUE', cutoff: 0.5,
    }
  })
  const b = r.bounds
  const bounds: Box = b.length === 6 ? { min: [b[0], b[1], b[2]], max: [b[3], b[4], b[5]] } : { min: [0, 0, 0], max: [0, 0, 0] }
  return { primitives, nodes: [], roots: [], skins: [], animations: [], images: [], bounds }
}

async function decode(mime: string, bytes: Uint8Array): Promise<DecodedImage> {
  try {
    if (!bytes.length || typeof createImageBitmap !== 'function') return null
    return await createImageBitmap(new Blob([bytes as BlobPart], { type: mime || 'image/png' }), { premultiplyAlpha: 'none' })
  } catch { return null }
}

export class ModelStore {
  private slots = new Map<string, ModelSlot>()
  private listeners = new Set<() => void>()
  private loader: ModelLoader | null = null

  /** The transport to Core (installed by the app once it is connected). */
  configure(loader: ModelLoader | null): void { this.loader = loader }
  onChange(fn: () => void): () => void { this.listeners.add(fn); return () => void this.listeners.delete(fn) }
  /** Forget everything (another game folder, or files changed on disk); the next ask reads again. */
  clear(): void { this.slots.clear(); this.listeners.forEach((f) => f()) }

  /** The slot of a model; the first ask starts loading it. */
  get(path: string): ModelSlot {
    let slot = this.slots.get(path)
    if (slot) return slot
    slot = { status: 'loading' } as ModelSlot
    this.slots.set(path, slot)
    void this.load(path)
    return slot
  }
  /** Rest pose bounds of a model that is ready (model space). */
  boundsOf(path: string): Box | null {
    const s = this.get(path)
    return s.status === 'ready' ? s.model.bounds : null
  }
  private set(path: string, slot: ModelSlot): void {
    if (this.slots.get(path) === undefined) return // cleared meanwhile
    this.slots.set(path, slot)
    this.listeners.forEach((f) => f())
  }
  private async load(path: string): Promise<void> {
    const ext = path.slice(path.lastIndexOf('.')).toLowerCase()
    try {
      if (!this.loader) throw new Error('not connected to Core')
      if (ext === '.glb') {
        const bytes = await this.loader.readFile(path)
        const model = parseGlb(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength) as ArrayBuffer)
        const images = await Promise.all(model.images.map((im) => decode(im.mime, im.bytes)))
        this.set(path, { status: 'ready', model, images, warnings: [] })
      } else if (ext === '.obj') {
        const r = await this.loader.readModel(path)
        this.set(path, { status: 'ready', model: modelFromObj(r), images: [], warnings: r.warnings ?? [] })
      } else if (ext === '.gltf') throw new Error('save the model as a binary .glb')
      else throw new Error(`unsupported model format ${ext || '(none)'}: use .glb (or .obj for misc_model)`)
    } catch (e) {
      this.set(path, { status: 'error', error: (e as Error).message })
    }
  }
}

/** The one store the app and the viewports share. */
export const modelStore = new ModelStore()
setModelBoundsProvider((p) => modelStore.boundsOf(p))
