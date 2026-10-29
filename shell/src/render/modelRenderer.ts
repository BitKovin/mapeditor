// Draws the models of entities (model_path; state/models.ts) in the editor views: textured, flat shaded, replacing the entity's box. Animated .glb models
// are posed on the CPU (model/gltf.ts poseModel) once per frame and animation, however many entities show them.
import type { Entity } from '../model/document'
import type { ModelData, ModelPrimitive, PosedPrimitive } from '../model/gltf'
import { animationIndex, isAnimated, poseModel } from '../model/gltf'
import { modelAnimationOf, modelMatrix, modelPathOf } from '../model/modelEntity'
import type { DecodedImage } from '../state/models'
import { modelStore } from '../state/models'

export const MODEL_VS = `#version 300 es
uniform mat4 uViewProj; uniform mat4 uModel; layout(location = 0) in vec3 aPos; layout(location = 1) in vec3 aNormal; layout(location = 2) in vec2 aUv; out vec3 vN; out vec2 vUv;
void main() { vN = mat3(uModel) * aNormal; vUv = aUv; gl_Position = uViewProj * (uModel * vec4(aPos, 1.0)); }`

interface GpuPrim { vao: WebGLVertexArrayObject; pos: WebGLBuffer; nrm: WebGLBuffer; count: number }
/** One model in one animation: its buffers and the pose they hold. */
interface GpuPose { prims: GpuPrim[]; posed: PosedPrimitive[]; anim: number; animated: boolean; frame: number }

export interface ModelLookups {
  /** The GL texture of an editor texture path once it has loaded, else null. */
  texture(path: string): WebGLTexture | null
  /** (mode 0 opaque | 1 cutout | 2 translucent, cutoff, opacity) of an editor texture, as the brush faces use. */
  transparency(path: string): [number, number, number]
}

/** What the renderer asks of a frame's entities. */
export interface ModelDrawOptions {
  /** Entities drawn by something else (the Core's lit mesh shows misc_model in the lit views). */
  skip?: (e: Entity) => boolean
  /** Seconds, the clock the animations play on. */
  time: number
  /** Which entities are in view (a box test); undefined = all. */
  visible?: (e: Entity, bounds: { min: [number, number, number]; max: [number, number, number] }) => boolean
}

export class ModelDrawer {
  private poses = new WeakMap<ModelData, Map<number, GpuPose>>()
  private images = new WeakMap<object, WebGLTexture>()
  private frameNo = 0
  /** True after a draw when something animated is on screen: the viewport keeps asking for frames. */
  animating = false

  constructor(private gl: WebGL2RenderingContext, private prog: WebGLProgram, private look: ModelLookups) {}

  private imageTexture(img: DecodedImage): WebGLTexture | null {
    if (!img) return null
    const have = this.images.get(img)
    if (have) return have
    const gl = this.gl, t = gl.createTexture()!
    gl.bindTexture(gl.TEXTURE_2D, t)
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false)
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false)
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, img as TexImageSource)
    gl.generateMipmap(gl.TEXTURE_2D)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR_MIPMAP_LINEAR)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.REPEAT)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.REPEAT)
    this.images.set(img, t)
    return t
  }

  private pose(model: ModelData, anim: number, time: number): GpuPose {
    const gl = this.gl
    let byAnim = this.poses.get(model)
    if (!byAnim) this.poses.set(model, (byAnim = new Map()))
    let gp = byAnim.get(anim)
    const animated = isAnimated(model, anim)
    if (gp && gp.frame === this.frameNo) return gp
    if (gp && !gp.animated) return gp // a rest pose never changes
    const posed = poseModel(model, anim, time, gp?.posed)
    if (!gp) {
      const prims = model.primitives.map((p, i): GpuPrim => {
        const vao = gl.createVertexArray()!, pos = gl.createBuffer()!, nrm = gl.createBuffer()!, uv = gl.createBuffer()!, idx = gl.createBuffer()!
        gl.bindVertexArray(vao)
        const usage = animated ? gl.DYNAMIC_DRAW : gl.STATIC_DRAW
        gl.bindBuffer(gl.ARRAY_BUFFER, pos); gl.bufferData(gl.ARRAY_BUFFER, posed[i].positions, usage)
        gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0)
        gl.bindBuffer(gl.ARRAY_BUFFER, nrm); gl.bufferData(gl.ARRAY_BUFFER, posed[i].normals, usage)
        gl.enableVertexAttribArray(1); gl.vertexAttribPointer(1, 3, gl.FLOAT, false, 0, 0)
        gl.bindBuffer(gl.ARRAY_BUFFER, uv); gl.bufferData(gl.ARRAY_BUFFER, p.uvs, gl.STATIC_DRAW)
        gl.enableVertexAttribArray(2); gl.vertexAttribPointer(2, 2, gl.FLOAT, false, 0, 0)
        gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, idx); gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, p.indices, gl.STATIC_DRAW)
        gl.bindVertexArray(null)
        return { vao, pos, nrm, count: p.indices.length }
      })
      gp = { prims, posed, anim, animated, frame: this.frameNo }
      byAnim.set(anim, gp)
    } else {
      gp.posed = posed
      gp.frame = this.frameNo
      gp.prims.forEach((g, i) => {
        gl.bindBuffer(gl.ARRAY_BUFFER, g.pos); gl.bufferSubData(gl.ARRAY_BUFFER, 0, posed[i].positions)
        gl.bindBuffer(gl.ARRAY_BUFFER, g.nrm); gl.bufferSubData(gl.ARRAY_BUFFER, 0, posed[i].normals)
      })
    }
    return gp
  }

  /** Draws the models of `entities`; returns the entities that showed one (their box is not drawn). The caller has set the viewport, depth state and the view. */
  draw(vp: Float32Array, entities: readonly Entity[], opts: ModelDrawOptions): Set<Entity> {
    const gl = this.gl
    this.frameNo++
    this.animating = false
    const shown = new Set<Entity>()
    type Draw = { gp: GpuPose; prim: ModelPrimitive; index: number; m: Float32Array; images: DecodedImage[] }
    const opaque: Draw[] = [], blended: Draw[] = []
    for (const e of entities) {
      const path = modelPathOf(e)
      if (!path) continue
      const slot = modelStore.get(path)
      if (slot.status !== 'ready') continue
      shown.add(e)
      if (opts.skip?.(e)) continue
      const model = slot.model
      const anim = animationIndex(model, modelAnimationOf(e))
      if (opts.visible && !opts.visible(e, model.bounds)) continue
      const gp = this.pose(model, anim, opts.time)
      if (gp.animated) this.animating = true
      const m = modelMatrix(e)
      model.primitives.forEach((prim, index) => (prim.alphaMode === 'BLEND' ? blended : opaque).push({ gp, prim, index, m, images: slot.images }))
    }
    if (!opaque.length && !blended.length) return shown
    gl.useProgram(this.prog)
    gl.uniformMatrix4fv(gl.getUniformLocation(this.prog, 'uViewProj'), false, vp)
    const uModel = gl.getUniformLocation(this.prog, 'uModel'), uColor = gl.getUniformLocation(this.prog, 'uColor'), uHasTex = gl.getUniformLocation(this.prog, 'uHasTex')
    const uTransp = gl.getUniformLocation(this.prog, 'uTransp')
    gl.uniform1i(gl.getUniformLocation(this.prog, 'uTex'), 0)
    gl.uniform1f(gl.getUniformLocation(this.prog, 'uAlphaTex'), 0)
    gl.activeTexture(gl.TEXTURE0)
    gl.disable(gl.CULL_FACE) // glTF materials can be double sided and a mirrored scale flips the winding: models are drawn from both sides
    gl.enable(gl.POLYGON_OFFSET_FILL); gl.polygonOffset(1, 1)
    const pass = (list: Draw[], blend: boolean) => {
      gl.depthMask(!blend)
      for (const d of list) {
        const { prim } = d
        let tex: WebGLTexture | null = null
        let tp: [number, number, number] = [0, 0, 0]
        if (prim.image >= 0) tex = this.imageTexture(d.images[prim.image] ?? null)
        else if (prim.texture) { tex = this.look.texture(prim.texture); tp = this.look.transparency(prim.texture) }
        if (prim.alphaMode === 'MASK') tp = [1, prim.cutoff, 0]
        else if (prim.alphaMode === 'BLEND') tp = [2, 0, 1]
        if (tex) gl.bindTexture(gl.TEXTURE_2D, tex)
        gl.uniform1f(uHasTex, tex ? 1 : 0)
        gl.uniform4f(uColor, prim.color[0], prim.color[1], prim.color[2], prim.alphaMode === 'BLEND' ? prim.color[3] : 1)
        gl.uniform3f(uTransp, tp[0], tp[1], tp[2])
        gl.uniformMatrix4fv(uModel, false, d.m)
        const g = d.gp.prims[d.index]
        gl.bindVertexArray(g.vao)
        gl.drawElements(gl.TRIANGLES, g.count, gl.UNSIGNED_INT, 0)
      }
    }
    pass(opaque, false)
    pass(blended, true)
    gl.bindVertexArray(null)
    gl.uniform3f(uTransp, 0, 0, 0)
    gl.depthMask(true)
    gl.disable(gl.POLYGON_OFFSET_FILL)
    return shown
  }
}
