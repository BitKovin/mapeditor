// Draws the models of entities (model_path; state/models.ts) in the editor views: textured, flat shaded, replacing the entity's box. Animated .glb models
// are posed on the CPU (model/gltf.ts poseModel) once per frame and animation, however many entities show them.
import type { Entity } from '../model/document'
import type { ModelData, ModelPrimitive, PosedPrimitive } from '../model/gltf'
import { animationIndex, isAnimated, poseModel } from '../model/gltf'
import { modelAnimationOf, modelMatrix, modelPathOf } from '../model/modelEntity'
import type { DecodedImage } from '../state/models'
import { modelStore } from '../state/models'
import type { EntityLight } from './lightvolStore'

export const MODEL_VS = `#version 300 es
uniform mat4 uViewProj; uniform mat4 uModel; layout(location = 0) in vec3 aPos; layout(location = 1) in vec3 aNormal; layout(location = 2) in vec2 aUv; out vec3 vN; out vec2 vUv;
void main() { vN = mat3(uModel) * aNormal; vUv = aUv; gl_Position = uViewProj * (uModel * vec4(aPos, 1.0)); }`

/**
 * The engine's own lighting of a model (GameData/shaders/source/fs_default.sc CalculateDirectionalDiffuse, light grid values as the engine reads them: byte / 255 * 2):
 *   colour = texture * 0.6 * (0.9 * ambient + directed * (0.5 * N.L + 0.5))
 * in the texture's own (display) space, no sRGB round trip and no level exposure - the brush faces are lit another way (lightmaps), models are not. Backfaces light
 * with the flipped normal. The engine's specular, rim light and dynamic point lights are not part of the preview.
 */
export const MODEL_LIT_FS = `#version 300 es
precision highp float; uniform vec3 uColor; uniform sampler2D uTex; uniform float uHasTex; uniform float uAlphaTex; uniform int uN; uniform vec3 uAmb[8]; uniform vec3 uDc[8]; uniform vec3 uDir; uniform vec3 uTransp; in vec3 vN; in vec2 vUv; out vec4 o;
float transparency(float a) {
  if (uTransp.x > 0.5 && uTransp.x < 1.5 && a < uTransp.y) discard;
  return uTransp.x > 1.5 ? uTransp.z * a : 1.0;
}
void main() {
  vec4 t = texture(uTex, vUv);
  vec3 albedo = mix(uColor, t.rgb, uHasTex);
  vec3 n = normalize(vN);
  if (!gl_FrontFacing) n = -n;
  float f = dot(n, normalize(uDir)) * 0.5 + 0.5;
  vec3 light = vec3(0.0);
  for (int i = 0; i < 8; i++) { if (i >= uN) break; light += uAmb[i] * 0.9 + uDc[i] * f; }
  o = vec4(albedo * light * 0.6, transparency(t.a * uHasTex + (1.0 - uHasTex)));
}`

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
  /** Light from the level's light volume (the lightgrid): a model is lit by the probes around it instead of drawn flat. Absent / null result = flat. */
  lit?: { sample: (p: [number, number, number]) => EntityLight[] | null; weight: (style: number) => number; exposure: number; brightness: number; lightingOnly: boolean }
  /** Which entities are in view (a box test); undefined = all. */
  visible?: (e: Entity, bounds: { min: [number, number, number]; max: [number, number, number] }) => boolean
}

export class ModelDrawer {
  private poses = new WeakMap<ModelData, Map<number, GpuPose>>()
  private images = new WeakMap<object, WebGLTexture>()
  private frameNo = 0
  /** True after a draw when something animated is on screen: the viewport keeps asking for frames. */
  animating = false

  constructor(private gl: WebGL2RenderingContext, private prog: WebGLProgram, private litProg: WebGLProgram, private look: ModelLookups) {}

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
    type Draw = { gp: GpuPose; prim: ModelPrimitive; index: number; m: Float32Array; images: DecodedImage[]; lights: EntityLight[] | null }
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
      let lights: EntityLight[] | null = null
      if (opts.lit) { // the probes around the middle of the model
        const b = model.bounds, c = [(b.min[0] + b.max[0]) / 2, (b.min[1] + b.max[1]) / 2, (b.min[2] + b.max[2]) / 2]
        lights = opts.lit.sample([m[0] * c[0] + m[4] * c[1] + m[8] * c[2] + m[12], m[1] * c[0] + m[5] * c[1] + m[9] * c[2] + m[13], m[2] * c[0] + m[6] * c[1] + m[10] * c[2] + m[14]])
      }
      model.primitives.forEach((prim, index) => (prim.alphaMode === 'BLEND' ? blended : opaque).push({ gp, prim, index, m, images: slot.images, lights }))
    }
    if (!opaque.length && !blended.length) return shown
    gl.disable(gl.CULL_FACE) // glTF materials can be double sided and a mirrored scale flips the winding: models are drawn from both sides
    gl.enable(gl.POLYGON_OFFSET_FILL); gl.polygonOffset(1, 1)
    // the flat program and the lightgrid one share the vertex shader: each has its own uniforms, set when a draw switches to it
    const locs = (prog: WebGLProgram) => ({
      model: gl.getUniformLocation(prog, 'uModel'), color: gl.getUniformLocation(prog, 'uColor'), has: gl.getUniformLocation(prog, 'uHasTex'), transp: gl.getUniformLocation(prog, 'uTransp'),
      n: gl.getUniformLocation(prog, 'uN'), amb: gl.getUniformLocation(prog, 'uAmb'), dc: gl.getUniformLocation(prog, 'uDc'), dir: gl.getUniformLocation(prog, 'uDir'),
    })
    const flat = locs(this.prog), litL = locs(this.litProg)
    const amb = new Float32Array(24), dc = new Float32Array(24)
    let current: WebGLProgram | null = null
    const use = (prog: WebGLProgram) => {
      if (current === prog) return
      current = prog
      gl.useProgram(prog)
      gl.uniformMatrix4fv(gl.getUniformLocation(prog, 'uViewProj'), false, vp)
      gl.uniform1i(gl.getUniformLocation(prog, 'uTex'), 0)
      gl.uniform1f(gl.getUniformLocation(prog, 'uAlphaTex'), 0)
      if (prog === this.litProg && opts.lit) { gl.uniform1f(gl.getUniformLocation(prog, 'uExposure'), opts.lit.exposure); gl.uniform1f(gl.getUniformLocation(prog, 'uBrightness'), opts.lit.brightness) }
    }
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
        const lit = d.lights && opts.lit ? d.lights : null
        use(lit ? this.litProg : this.prog)
        const L = lit ? litL : flat
        gl.activeTexture(gl.TEXTURE0)
        if (tex) gl.bindTexture(gl.TEXTURE_2D, tex)
        gl.uniform1f(L.has, tex && !(lit && opts.lit?.lightingOnly) ? 1 : 0) // Lighting-only shows the light alone
        const flatColor = opts.lit?.lightingOnly && lit ? [1, 1, 1, 1] : prim.color
        if (lit) {
          gl.uniform3f(L.color, flatColor[0], flatColor[1], flatColor[2])
          const n = Math.min(8, lit.length)
          for (let i = 0; i < n; i++) {
            const w = opts.lit!.weight(lit[i].s)
            for (let c = 0; c < 3; c++) { amb[i * 3 + c] = lit[i].ambient[c] * w; dc[i * 3 + c] = lit[i].directed[c] * w }
          }
          gl.uniform1i(L.n, n); gl.uniform3fv(L.amb, amb); gl.uniform3fv(L.dc, dc); gl.uniform3fv(L.dir, lit[0].dir)
        } else gl.uniform4f(L.color, prim.color[0], prim.color[1], prim.color[2], prim.alphaMode === 'BLEND' ? prim.color[3] : 1)
        gl.uniform3f(L.transp, tp[0], tp[1], tp[2])
        gl.uniformMatrix4fv(L.model, false, d.m)
        const g = d.gp.prims[d.index]
        gl.bindVertexArray(g.vao)
        gl.drawElements(gl.TRIANGLES, g.count, gl.UNSIGNED_INT, 0)
      }
    }
    pass(opaque, false)
    pass(blended, true)
    gl.bindVertexArray(null)
    for (const L of [flat, litL]) gl.uniform3f(L.transp, 0, 0, 0)
    gl.depthMask(true)
    gl.disable(gl.POLYGON_OFFSET_FILL)
    return shown
  }
}
