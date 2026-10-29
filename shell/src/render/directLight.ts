// Direct light prediction on the GPU (DESIGN 6.11). When the document changes, the Core needs seconds to bring the lightmaps up to date, but the direct light of
// the change (a brush that moves, a light that moves) can be computed here at once: shadow maps of the lights, rendered into the lightmap atlas of the
// triangles whose light may differ. The Core stays the owner of everything else (sky and rect lights, the bounces, ambient occlusion): what is shown is
//
//     shown = max(0, core total + ao * (predicted direct - core direct layer))      where a prediction exists,
//
// so the Core's own noise, its stale state and the parts the Shell does not model cancel out exactly, and the rest of the picture is the Core's.
// A prediction is dropped when the Core has caught up (its bake converged after it saw the edit); a Core tile never has to be held back.
import type { LightmapStore, DirtyRect } from './lightmapStore'
import { packRect } from './lightmapStore'
import type { World } from '../model/derived'
import type { EntityClassDef } from '../net/protocol'
import type { PredLight, PredScene, SceneBoxes } from './directLightPlan'
import { CORE_FLOATS, affectedTriangles, changedBoxes, changedLights, collectPredScene, predictorMesh, staleBoxes, triInfo } from './directLightPlan'
import type { TriInfo } from './directLightPlan'
import { affectTimings } from './directLightPlan'

const CUBE_RES = 512
const SUN_RES = 1024
const MAX_LIGHTS = 8
const CELL = 64               // metres: receivers are processed in groups of this size (the sun's shadow map covers one group)
const BATCH_TRIS = 6000
const FRAME_BUDGET_MS = 4
const FRAME_MAX_BATCHES = 6   // each batch renders shadow maps: the GPU should not be handed more than a few per frame (a drag makes a new prediction every few frames)

// Cube faces: look direction w, right r, up u (view space x = dot(d, r), y = dot(d, u), depth along w). The shader has the same table.
const FACES: { w: [number, number, number]; r: [number, number, number]; u: [number, number, number] }[] = [
  { w: [1, 0, 0], r: [0, 0, -1], u: [0, 1, 0] }, { w: [-1, 0, 0], r: [0, 0, 1], u: [0, 1, 0] },
  { w: [0, 1, 0], r: [1, 0, 0], u: [0, 0, -1] }, { w: [0, -1, 0], r: [1, 0, 0], u: [0, 0, 1] },
  { w: [0, 0, 1], r: [1, 0, 0], u: [0, 1, 0] }, { w: [0, 0, -1], r: [-1, 0, 0], u: [0, 1, 0] },
]

const SHADOW_VS = `#version 300 es
layout(location = 0) in vec3 aPos; uniform mat4 uVP; out vec3 vW;
void main() { vW = aPos; gl_Position = uVP * vec4(aPos, 1.0); }`
const SHADOW_FS = `#version 300 es
precision highp float; in vec3 vW; uniform vec3 uRef; uniform vec3 uDir; uniform float uMode; layout(location = 0) out float o;
void main() { o = uMode < 0.5 ? length(vW - uRef) : dot(vW, uDir); }`

const RECV_VS = `#version 300 es
layout(location = 0) in vec3 aPos; layout(location = 1) in vec3 aNrm; layout(location = 2) in vec2 aLm; out vec3 vPos; out vec3 vNrm;
void main() { vPos = aPos; vNrm = aNrm; gl_Position = vec4(aLm * 2.0 - 1.0, 0.0, 1.0); }`
const RECV_FS = `#version 300 es
precision highp float; precision highp sampler2DArray;
in vec3 vPos; in vec3 vNrm; layout(location = 0) out vec4 o;
uniform int uCount; uniform vec3 uAmbient; uniform sampler2DArray uPt; uniform sampler2DArray uSun;
uniform vec4 uLPos[${MAX_LIGHTS}];     // xyz position, radius
uniform vec4 uLDir[${MAX_LIGHTS}];     // xyz forward, kind (0 point, 1 spot, 2 directional)
uniform vec4 uLCol[${MAX_LIGHTS}];     // rgb colour x brightness, size
uniform vec4 uLCone[${MAX_LIGHTS}];    // cos inner, cos outer, casts shadows, slot (point: first of 6 layers of uPt; directional: layer of uSun)
uniform vec4 uSunB[${MAX_LIGHTS}];     // directional: ortho bounds in light space (minR, maxR, minU, maxU)
uniform vec3 uSunR[${MAX_LIGHTS}]; uniform vec3 uSunU[${MAX_LIGHTS}]; uniform vec2 uSunW[${MAX_LIGHTS}]; // basis right / up; depth origin and 1 / range
const float PI = 3.14159265358979;
const vec3 FW[6] = vec3[6](vec3(1,0,0), vec3(-1,0,0), vec3(0,1,0), vec3(0,-1,0), vec3(0,0,1), vec3(0,0,-1));
const vec3 FR[6] = vec3[6](vec3(0,0,-1), vec3(0,0,1), vec3(1,0,0), vec3(1,0,0), vec3(1,0,0), vec3(-1,0,0));
const vec3 FU[6] = vec3[6](vec3(0,1,0), vec3(0,1,0), vec3(0,0,-1), vec3(0,0,1), vec3(0,1,0), vec3(0,1,0));
float window(float d, float r) { float q = d / r; float w = clamp(1.0 - q * q * q * q, 0.0, 1.0); return w * w; }

// lit fraction of a point-light cube: percentage closer filtering with a kernel that follows the source's penumbra (PCSS)
float pointShadow(int li, vec3 p, vec3 n) {
  vec3 L = uLPos[li].xyz, d = p - L;
  float dist = length(d);
  vec3 a = abs(d);
  int f = a.x >= a.y && a.x >= a.z ? (d.x > 0.0 ? 0 : 1) : (a.y >= a.z ? (d.y > 0.0 ? 2 : 3) : (d.z > 0.0 ? 4 : 5));
  float texW = 2.0 * dist / float(${CUBE_RES});             // world size of a shadow texel at the receiver
  vec3 po = p + n * (texW * 1.5 + 0.01);
  vec3 dd = po - L; float z = dot(dd, FW[f]);
  if (z <= 1e-4) return 1.0;
  vec2 uv = vec2(dot(dd, FR[f]), dot(dd, FU[f])) / z * 0.5 + 0.5;
  float recv = length(dd);
  int layer = int(uLCone[li].w) + f;
  vec2 tc = uv * float(${CUBE_RES});
  float size = uLCol[li].w;
  // blocker search
  float sum = 0.0, cnt = 0.0;
  for (int j = -2; j <= 2; j++) for (int i = -2; i <= 2; i++) {
    float s = texelFetch(uPt, ivec3(clamp(ivec2(floor(tc)) + ivec2(i, j) * 2, ivec2(0), ivec2(${CUBE_RES - 1})), layer), 0).r;
    if (s < recv - texW) { sum += s; cnt += 1.0; }
  }
  if (cnt < 0.5) return 1.0;
  float blocker = sum / cnt;
  float pen = size * (recv - blocker) / max(blocker, 0.05) / texW;      // penumbra width in shadow texels
  float k = clamp(pen * 0.5, 0.75, 7.0);
  float lit = 0.0;
  for (int j = -2; j <= 2; j++) for (int i = -2; i <= 2; i++) {
    vec2 o2 = vec2(float(i), float(j)) * k * 0.5;
    float s = texelFetch(uPt, ivec3(clamp(ivec2(floor(tc + o2)), ivec2(0), ivec2(${CUBE_RES - 1})), layer), 0).r;
    lit += s >= recv - texW ? 1.0 : 0.0;
  }
  return lit / 25.0;
}

float sunShadow(int li, vec3 p, vec3 n) {
  vec4 b = uSunB[li];
  float texW = (b.y - b.x) / float(${SUN_RES});
  vec3 po = p + n * (texW * 1.5 + 0.01);
  vec2 uv = vec2((dot(po, uSunR[li]) - b.x) / (b.y - b.x), (dot(po, uSunU[li]) - b.z) / (b.w - b.z));
  if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) return 1.0;
  float recv = dot(po, uLDir[li].xyz);
  int layer = int(uLCone[li].w);
  vec2 tc = uv * float(${SUN_RES});
  float tanHalf = tan(radians(uLCol[li].w * 0.5));
  float sum = 0.0, cnt = 0.0;
  for (int j = -2; j <= 2; j++) for (int i = -2; i <= 2; i++) {
    float s = texelFetch(uSun, ivec3(clamp(ivec2(floor(tc)) + ivec2(i, j) * 2, ivec2(0), ivec2(${SUN_RES - 1})), layer), 0).r;
    if (s < recv - texW) { sum += s; cnt += 1.0; }
  }
  if (cnt < 0.5) return 1.0;
  float pen = 2.0 * tanHalf * (recv - sum / cnt) / texW;
  float k = clamp(pen * 0.5, 0.75, 7.0);
  float lit = 0.0;
  for (int j = -2; j <= 2; j++) for (int i = -2; i <= 2; i++) {
    vec2 o2 = vec2(float(i), float(j)) * k * 0.5;
    float s = texelFetch(uSun, ivec3(clamp(ivec2(floor(tc + o2)), ivec2(0), ivec2(${SUN_RES - 1})), layer), 0).r;
    lit += s >= recv - texW ? 1.0 : 0.0;
  }
  return lit / 25.0;
}

void main() {
  vec3 n = normalize(vNrm), p = vPos, sum = uAmbient;
  for (int li = 0; li < ${MAX_LIGHTS}; li++) {
    if (li >= uCount) break;
    float kind = uLDir[li].w;
    vec3 v;
    if (kind > 1.5) {
      v = uLCol[li].rgb * max(0.0, dot(n, -uLDir[li].xyz));
      if (v.x != 0.0 || v.y != 0.0 || v.z != 0.0) { if (uLCone[li].z > 0.5) v *= sunShadow(li, p, n); }
    } else {
      vec3 dv = uLPos[li].xyz - p; float dist = length(dv);
      if (dist >= uLPos[li].w) continue;
      vec3 l = dist > 1e-9 ? dv / dist : n;
      float cosT = max(0.0, dot(n, l)), dm = max(dist, 0.05);
      float s = window(dist, uLPos[li].w) / (dm * dm) * cosT;
      if (kind > 0.5) { float t = clamp((dot(uLDir[li].xyz, -l) - uLCone[li].y) / (uLCone[li].x - uLCone[li].y), 0.0, 1.0); s *= t * t; }
      v = uLCol[li].rgb * s;
      if ((v.x != 0.0 || v.y != 0.0 || v.z != 0.0) && uLCone[li].z > 0.5) v *= pointShadow(li, p, n);
    }
    sum += v;
  }
  o = vec4(sum, 1.0);
}`

const COMPOSE_VS = `#version 300 es
uniform vec4 uRect; // x, y, w, h in texels of the page
uniform float uAtlas;
void main() {
  vec2 q = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
  vec2 px = uRect.xy + q * uRect.zw;
  gl_Position = vec4(px / uAtlas * 2.0 - 1.0, 0.0, 1.0);
}`
// A texel that was not predicted but lies in the gutter of a predicted chart (no chart covers it: uMask 0) takes the nearest predicted value within two texels, like
// the Core's own gutter: the bicubic lookup next to a chart edge reads two texels beyond it.
const COMPOSE_FS = `#version 300 es
precision highp float; uniform sampler2D uCore; uniform sampler2D uDc; uniform sampler2D uP; uniform sampler2D uMask; uniform float uAtlas; layout(location = 0) out vec4 o;
void main() {
  ivec2 c = ivec2(gl_FragCoord.xy);
  vec4 T = texelFetch(uCore, c, 0), P = texelFetch(uP, c, 0);
  vec3 pr = P.rgb;
  bool valid = P.a >= 0.5;
  if (!valid) {
    if (texelFetch(uMask, c, 0).r > 0.5) { o = T; return; }
    float best = 1e9; int hi = int(uAtlas) - 1;
    for (int dy = -2; dy <= 2; dy++) for (int dx = -2; dx <= 2; dx++) {
      vec4 Q = texelFetch(uP, clamp(c + ivec2(dx, dy), ivec2(0), ivec2(hi)), 0);
      float dd = float(dx * dx + dy * dy);
      if (Q.a >= 0.5 && dd < best) { best = dd; pr = Q.rgb; valid = true; }
    }
    if (!valid) { o = T; return; }
  }
  vec4 D = texelFetch(uDc, c, 0);
  float ao = D.a > 0.0 ? D.a : 1.0;
  o = vec4(max(vec3(0.0), T.rgb + ao * (pr - D.rgb)), T.a);
}`
const MASK_FS = `#version 300 es
precision highp float; in vec3 vPos; in vec3 vNrm; layout(location = 0) out vec4 o; void main() { o = vec4(1.0); }`

interface PageTex { core: WebGLTexture; dc: WebGLTexture; p: WebGLTexture; mask: WebGLTexture; pFbo: WebGLFramebuffer }
interface Batch { tris: Uint32Array; sphere: [number, number, number, number] }

const cross = (a: number[], b: number[]): number[] => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
const norm = (a: number[]): number[] => { const l = Math.hypot(a[0], a[1], a[2]) || 1; return [a[0] / l, a[1] / l, a[2] / l] }

export interface PredictorTarget {
  /** The renderer's lightmap texture array and where page `p` of the base layer lives in it (layer index); null when it has none. */
  lmTex: WebGLTexture | null
  layerOf(page: number): number
}

export class DirectLightPredictor {
  readonly supported: boolean
  private gl: WebGL2RenderingContext
  private progShadow: WebGLProgram | null = null
  private progRecv: WebGLProgram | null = null
  private progCompose: WebGLProgram | null = null
  private progMask: WebGLProgram | null = null
  private pages = new Map<number, PageTex>()
  private occVbo: WebGLBuffer | null = null
  private occVao: WebGLVertexArrayObject | null = null
  private occCount = 0
  private rcvVbo: WebGLBuffer | null = null
  private rcvVao: WebGLVertexArrayObject | null = null
  private composeVao: WebGLVertexArrayObject | null = null
  private ptTex: WebGLTexture | null = null
  private sunTex: WebGLTexture | null = null
  private fbo: WebGLFramebuffer | null = null
  private depthRbs = new Map<number, WebGLRenderbuffer>()
  private ptFbos: (WebGLFramebuffer | null)[] = []
  private sunFbos: (WebGLFramebuffer | null)[] = []
  // state of the prediction
  private mesh: Float32Array | null = null
  private info: TriInfo | null = null
  private predScene: PredScene | null = null
  private predBoxes: SceneBoxes | null = null
  private meshSerial = -1
  private serial = -1
  private cells = new Set<string>()
  private queue: Batch[] = []
  private generation = 0
  private cubeCache = new Map<string, number>()     // light key -> first layer of its cube in uPt (valid for `generation`)
  private cubeSlots: (string | null)[] = new Array(MAX_LIGHTS).fill(null)
  private nextSlot = 0
  private lastPredictAt = 0
  private planMesh: { mesh: Float32Array; analysis: unknown; plain: Float32Array; overlay: number } | null = null
  private memo: { doc: unknown; prefabs: unknown; defs: unknown; analysis: unknown; mesh: unknown; size: number; scene: PredScene; boxes: SceneBoxes } | null = null
  private sawUnconverged = false
  /** What the last update did, for tests and the profile overlay. */
  /** Test hook: keep predictions when the Core has converged (to compare them with what it computed). */
  debugHold = false
  /** Test hook: only these lights (ids) are evaluated. */
  debugOnly: string[] | null = null
  stats = { predictions: 0, batches: 0, triangles: 0, lastMs: 0, pending: 0, activePages: 0, sceneMs: 0, planMs: 0, uploadMs: 0, batchMs: 0, infoMs: 0, pageMs: 0, meshMs: 0, affectMs: 0, enqueueMs: 0, affect: {} as Record<string, number>, slow: [] as number[][] }

  constructor(gl: WebGL2RenderingContext) {
    this.gl = gl
    this.supported = !!gl.getExtension('EXT_color_buffer_float')
    gl.getExtension('EXT_float_blend')
  }

  /** True while a prediction is shown for `page`: the renderer must then route that page's tile uploads through `handleRect`. */
  pageActive(page: number): boolean { return this.pages.has(page) }
  get active(): boolean { return this.pages.size > 0 || this.queue.length > 0 }
  /** Work is left: the viewport must draw another frame (it only draws when something asks). */
  get wantsFrame(): boolean { return this.queue.length > 0 }

  /** The renderer draws into the canvas without binding anything itself: leave the default framebuffer, no vertex array and texture unit 0 behind. */
  private restoreState(): void {
    const gl = this.gl
    gl.bindFramebuffer(gl.FRAMEBUFFER, null)
    gl.bindVertexArray(null)
    gl.activeTexture(gl.TEXTURE0)
    gl.disable(gl.SCISSOR_TEST)
  }

  // ---- GL helpers --------------------------------------------------------------------------------------------------------------
  private program(vs: string, fs: string): WebGLProgram {
    const gl = this.gl
    const mk = (type: number, src: string) => {
      const s = gl.createShader(type)!
      gl.shaderSource(s, src); gl.compileShader(s)
      if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(`direct light shader: ${gl.getShaderInfoLog(s)}`)
      return s
    }
    const p = gl.createProgram()!
    gl.attachShader(p, mk(gl.VERTEX_SHADER, vs)); gl.attachShader(p, mk(gl.FRAGMENT_SHADER, fs)); gl.linkProgram(p)
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(`direct light program: ${gl.getProgramInfoLog(p)}`)
    return p
  }
  private tex2d(atlas: number): WebGLTexture {
    const gl = this.gl, t = gl.createTexture()!
    gl.bindTexture(gl.TEXTURE_2D, t)
    gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA16F, atlas, atlas)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST)
    return t
  }
  private ensurePrograms(): void {
    if (this.progShadow) return
    this.progShadow = this.program(SHADOW_VS, SHADOW_FS)
    this.progRecv = this.program(RECV_VS, RECV_FS)
    this.progCompose = this.program(COMPOSE_VS, COMPOSE_FS)
    this.progMask = this.program(RECV_VS, MASK_FS)
    const gl = this.gl
    this.occVao = gl.createVertexArray()!; this.occVbo = gl.createBuffer()!
    this.rcvVao = gl.createVertexArray()!; this.rcvVbo = gl.createBuffer()!
    this.composeVao = gl.createVertexArray()!
    this.fbo = gl.createFramebuffer()!
    gl.bindVertexArray(this.rcvVao); gl.bindBuffer(gl.ARRAY_BUFFER, this.rcvVbo)
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 32, 0)
    gl.enableVertexAttribArray(1); gl.vertexAttribPointer(1, 3, gl.FLOAT, false, 32, 12)
    gl.enableVertexAttribArray(2); gl.vertexAttribPointer(2, 2, gl.FLOAT, false, 32, 24)
    gl.bindVertexArray(null)
  }
  private ensureShadowTargets(): void {
    const gl = this.gl
    if (this.ptTex) return
    this.ptTex = gl.createTexture()!
    gl.bindTexture(gl.TEXTURE_2D_ARRAY, this.ptTex)
    gl.texStorage3D(gl.TEXTURE_2D_ARRAY, 1, gl.R32F, CUBE_RES, CUBE_RES, MAX_LIGHTS * 6)
    gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MIN_FILTER, gl.NEAREST); gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MAG_FILTER, gl.NEAREST)
    this.sunTex = gl.createTexture()!
    gl.bindTexture(gl.TEXTURE_2D_ARRAY, this.sunTex)
    gl.texStorage3D(gl.TEXTURE_2D_ARRAY, 1, gl.R32F, SUN_RES, SUN_RES, 2)
    gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MIN_FILTER, gl.NEAREST); gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MAG_FILTER, gl.NEAREST)
    this.ptFbos = new Array(MAX_LIGHTS * 6).fill(null)
    this.sunFbos = [null, null]
  }
  private depthFor(size: number): WebGLRenderbuffer {
    const gl = this.gl
    let rb = this.depthRbs.get(size)
    if (!rb) {
      rb = gl.createRenderbuffer()!
      gl.bindRenderbuffer(gl.RENDERBUFFER, rb)
      gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH_COMPONENT24, size, size)
      this.depthRbs.set(size, rb)
    }
    return rb
  }
  private layerFbo(tex: WebGLTexture, layer: number, cache: (WebGLFramebuffer | null)[], size: number): WebGLFramebuffer {
    const gl = this.gl
    let f = cache[layer]
    if (!f) { f = gl.createFramebuffer()!; cache[layer] = f }
    gl.bindFramebuffer(gl.FRAMEBUFFER, f)
    gl.framebufferTextureLayer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, tex, 0, layer)
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.RENDERBUFFER, this.depthFor(size))
    return f
  }

  // ---- pages ---------------------------------------------------------------------------------------------------------------------
  private ensurePage(l: LightmapStore, page: number): PageTex {
    let t = this.pages.get(page)
    if (t) return t
    const gl = this.gl, atlas = l.atlas
    gl.pixelStorei(gl.UNPACK_ROW_LENGTH, 0); gl.pixelStorei(gl.UNPACK_SKIP_PIXELS, 0); gl.pixelStorei(gl.UNPACK_SKIP_ROWS, 0)
    const core = this.tex2d(atlas), dc = this.tex2d(atlas), p = this.tex2d(atlas)
    gl.bindTexture(gl.TEXTURE_2D, core); gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, atlas, atlas, gl.RGBA, gl.HALF_FLOAT, l.pages[page])
    gl.bindTexture(gl.TEXTURE_2D, dc); gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, atlas, atlas, gl.RGBA, gl.HALF_FLOAT, l.directPages[page])
    const pFbo = gl.createFramebuffer()!
    gl.bindFramebuffer(gl.FRAMEBUFFER, pFbo)
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, p, 0)
    gl.clearBufferfv(gl.COLOR, 0, [0, 0, 0, 0])
    // which texels some chart covers (every triangle of the page, exact): what is not covered is gutter
    const mask = this.tex2d(atlas)
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.fbo)
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, mask, 0)
    gl.viewport(0, 0, atlas, atlas)
    gl.clearBufferfv(gl.COLOR, 0, [0, 0, 0, 0])
    const all: number[] = []
    const info = this.info!
    for (let i = 0; i < info.count; i++) if (info.page[i] === page) all.push(i)
    const rc = buildReceivers(this.mesh!, all, atlas)
    gl.disable(gl.DEPTH_TEST); gl.disable(gl.BLEND); gl.disable(gl.CULL_FACE); gl.disable(gl.SCISSOR_TEST)
    gl.useProgram(this.progMask)
    gl.bindVertexArray(this.rcvVao); gl.bindBuffer(gl.ARRAY_BUFFER, this.rcvVbo)
    gl.bufferData(gl.ARRAY_BUFFER, rc.data, gl.STREAM_DRAW)
    gl.drawArrays(gl.TRIANGLES, 0, rc.count)
    gl.bindVertexArray(null)
    t = { core, dc, p, mask, pFbo }
    this.pages.set(page, t)
    return t
  }
  private freePage(t: PageTex): void {
    const gl = this.gl
    gl.deleteTexture(t.core); gl.deleteTexture(t.dc); gl.deleteTexture(t.p); gl.deleteTexture(t.mask); gl.deleteFramebuffer(t.pFbo)
  }

  /** Writes the composed light of `rect` of page `page` into the renderer's lightmap texture. */
  private compose(target: PredictorTarget, page: number, x: number, y: number, w: number, h: number, atlas: number): void {
    const t = this.pages.get(page)
    if (!t || !target.lmTex) return
    const gl = this.gl
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.fbo)
    gl.framebufferTextureLayer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, target.lmTex, 0, target.layerOf(page))
    gl.viewport(0, 0, atlas, atlas)
    gl.disable(gl.DEPTH_TEST); gl.disable(gl.BLEND); gl.disable(gl.CULL_FACE); gl.disable(gl.SCISSOR_TEST)
    gl.useProgram(this.progCompose)
    gl.uniform4f(gl.getUniformLocation(this.progCompose!, 'uRect'), x, y, w, h)
    gl.uniform1f(gl.getUniformLocation(this.progCompose!, 'uAtlas'), atlas)
    gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, t.core); gl.uniform1i(gl.getUniformLocation(this.progCompose!, 'uCore'), 0)
    gl.activeTexture(gl.TEXTURE1); gl.bindTexture(gl.TEXTURE_2D, t.dc); gl.uniform1i(gl.getUniformLocation(this.progCompose!, 'uDc'), 1)
    gl.activeTexture(gl.TEXTURE2); gl.bindTexture(gl.TEXTURE_2D, t.p); gl.uniform1i(gl.getUniformLocation(this.progCompose!, 'uP'), 2)
    gl.activeTexture(gl.TEXTURE3); gl.bindTexture(gl.TEXTURE_2D, t.mask); gl.uniform1i(gl.getUniformLocation(this.progCompose!, 'uMask'), 3)
    gl.bindVertexArray(this.composeVao)
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4)
    gl.bindVertexArray(null)
    gl.activeTexture(gl.TEXTURE0)
  }

  /**
   * A tile of the Core arrived for a page that shows a prediction: keep the Core's copy up to date and compose again. Returns false when the page has no
   * prediction (the renderer then uploads into its texture itself).
   */
  handleRect(l: LightmapStore, target: PredictorTarget, r: DirtyRect): boolean {
    const t = this.pages.get(r.page)
    if (!t || (r.style ?? 0) !== 0 || !this.supported) return false
    const gl = this.gl
    gl.pixelStorei(gl.UNPACK_ROW_LENGTH, 0); gl.pixelStorei(gl.UNPACK_SKIP_PIXELS, 0); gl.pixelStorei(gl.UNPACK_SKIP_ROWS, 0)
    if (r.direct) {
      const src = packRect(l.directPages[r.page], l.atlas, r)
      gl.bindTexture(gl.TEXTURE_2D, t.dc); gl.texSubImage2D(gl.TEXTURE_2D, 0, r.x, r.y, r.w, r.h, gl.RGBA, gl.HALF_FLOAT, src)
    } else {
      const src = packRect(l.pages[r.page], l.atlas, r)
      gl.bindTexture(gl.TEXTURE_2D, t.core); gl.texSubImage2D(gl.TEXTURE_2D, 0, r.x, r.y, r.w, r.h, gl.RGBA, gl.HALF_FLOAT, src)
    }
    this.compose(target, r.page, r.x, r.y, r.w, r.h, l.atlas)
    this.restoreState()
    return true
  }

  /** Drops every prediction: the lightmap texture gets the Core's pages back. */
  releaseAll(l: LightmapStore | null, target: PredictorTarget | null, restore: boolean): void {
    const gl = this.gl
    for (const [page, t] of this.pages) {
      if (restore && l && target?.lmTex && l.pages[page]) {
        gl.bindTexture(gl.TEXTURE_2D_ARRAY, target.lmTex)
        gl.pixelStorei(gl.UNPACK_ROW_LENGTH, 0); gl.pixelStorei(gl.UNPACK_SKIP_PIXELS, 0); gl.pixelStorei(gl.UNPACK_SKIP_ROWS, 0); gl.pixelStorei(gl.UNPACK_SKIP_IMAGES, 0); gl.pixelStorei(gl.UNPACK_IMAGE_HEIGHT, 0)
        gl.texSubImage3D(gl.TEXTURE_2D_ARRAY, 0, 0, 0, target.layerOf(page), l.atlas, l.atlas, 1, gl.RGBA, gl.HALF_FLOAT, l.pages[page])
      }
      this.freePage(t)
    }
    this.pages.clear()
    this.queue = []
    this.cells.clear()
    this.stats.activePages = 0; this.stats.pending = 0
    this.restoreState()
  }

  // ---- the per frame entry -------------------------------------------------------------------------------------------------------
  /**
   * Call once per frame before drawing, in a lighting view with a lit mesh. Finds what the document changed since the last call, queues the triangles whose direct
   * light may differ and works through the queue for a few milliseconds. `camera`: nearest receivers first.
   */
  update(l: LightmapStore, world: World, defs: readonly EntityClassDef[], target: PredictorTarget, camera: [number, number, number]): void {
    if (!this.supported) return
    const t0 = performance.now()
    if (!l.directLayer || !l.ready || !l.mesh) { if (this.active) this.releaseAll(l, target, false); this.predScene = null; return }
    if (l.serial !== this.serial) { this.serial = l.serial; this.releaseAll(l, target, false); this.predScene = null } // the texture was rebuilt: it holds the Core's pages
    const analysis = l.staleness(world)
    // the mesh of the prediction (not the relief mesh of the Lighting-only view: displaced geometry is not needed for light, and costs 0.5 s to build)
    let pm = this.planMesh
    if (!pm || pm.mesh !== l.mesh || pm.analysis !== analysis) {
      const m0 = performance.now()
      const built = predictorMesh(l.mesh, l.faces, analysis)
      pm = { mesh: l.mesh, analysis, plain: built.mesh, overlay: built.overlayTriangles }
      this.planMesh = pm
      this.stats.meshMs = performance.now() - m0
    }
    const plain = pm.plain
    // what the document says, recomputed only when it changed (this runs every frame)
    const memo = this.memo
    let scene: PredScene, boxes: SceneBoxes
    if (memo && memo.doc === world.doc && memo.prefabs === world.prefabs && memo.defs === defs && memo.analysis === analysis && memo.mesh === l.mesh && memo.size === (l.status?.defaultSourceSize ?? 0.25)) {
      scene = memo.scene; boxes = memo.boxes
    } else {
      const p0 = performance.now()
      scene = collectPredScene(world, defs, { sourceSize: l.status?.defaultSourceSize ?? 0.25, sunSize: l.status?.defaultSunSize ?? 0.5 })
      boxes = staleBoxes(l.mesh, l.faces, analysis)
      this.memo = { doc: world.doc, prefabs: world.prefabs, defs, analysis, mesh: l.mesh, size: l.status?.defaultSourceSize ?? 0.25, scene, boxes }
      this.stats.sceneMs = performance.now() - p0
    }
    const status = l.status
    // the Core caught up: its converged bake saw the edit (it was unconverged since the prediction), so what it shows is final
    if (status) {
      if (!status.converged) this.sawUnconverged = true
      if (!this.debugHold && status.converged && this.active && this.queue.length === 0 && ((this.sawUnconverged && performance.now() - this.lastPredictAt > 400) || performance.now() - this.lastPredictAt > 30000)) {
        this.releaseAll(l, target, true)
        this.predScene = scene; this.predBoxes = boxes; this.sawUnconverged = false
        return
      }
    }
    const meshChanged = l.meshSerial !== this.meshSerial
    if (plain !== this.mesh || meshChanged) {
      const u0 = performance.now()
      this.mesh = plain
      this.info = triInfo(plain)
      this.stats.infoMs = performance.now() - u0
      this.uploadOccluders(plain)
      this.stats.uploadMs = performance.now() - u0
    }
    if (!this.predScene || !this.predBoxes) {
      this.predScene = scene; this.predBoxes = boxes; this.meshSerial = l.meshSerial
      // the Core has not baked this mesh yet (its pages are empty, the AO factor reads as 1): predict the direct light of everything, nearest first
      if (status && !status.converged) {
        this.generation++
        this.cubeCache.clear(); this.cubeSlots.fill(null)
        this.enqueue(Uint32Array.from({ length: this.info!.count }, (_, i) => i), this.info!, camera)
        this.lastPredictAt = performance.now(); this.sawUnconverged = false; this.stats.predictions++
        this.run(l, target, scene, t0)
        this.restoreState()
        this.stats.pending = this.queue.length; this.stats.activePages = this.pages.size
      }
      return
    }
    const info = this.info!
    const overlayTris = pm.overlay
    const forcedFrom = Math.max(0, info.count - overlayTris)
    if (meshChanged) {
      // a new Core mesh: the atlas may have been laid out again, so what was predicted there is meaningless. Predict again where it was, in the new layout.
      this.meshSerial = l.meshSerial
      if (this.pages.size > 0 || this.queue.length > 0) {
        const keep = new Set(this.cells)
        this.releaseAll(l, target, true)
        this.cells = keep
        this.enqueue(this.trianglesInCells(info, keep), info, camera)
        this.generation++
      }
    }
    const lightsChanged = changedLights(this.predScene.lights, scene.lights)
    const ambientChanged = this.predScene.ambient.some((v, i) => v !== scene.ambient[i])
    const boxesChanged = changedBoxes(this.predBoxes, boxes)
    if (lightsChanged.length > 0 || ambientChanged || boxesChanged.length > 0) {
      const q0 = performance.now()
      const all = [...scene.lights, ...this.predScene.lights]
      const a0 = performance.now()
      const tris = ambientChanged ? Uint32Array.from({ length: info.count }, (_, i) => i)
        : affectedTriangles(info, { boxes: boxesChanged, lights: lightsChanged }, all, forcedFrom)
      this.stats.affectMs = performance.now() - a0
      this.stats.affect = { ...affectTimings, boxesN: boxesChanged.length, lightsN: lightsChanged.length }
      this.predScene = scene; this.predBoxes = boxes
      this.generation++
      this.cubeCache.clear(); this.cubeSlots.fill(null)
      if (tris.length > 0) { this.enqueue(tris, info, camera); this.lastPredictAt = performance.now(); this.sawUnconverged = false; this.stats.predictions++ }
      this.stats.planMs = performance.now() - q0
    }
    const r0 = performance.now()
    this.run(l, target, scene, t0)
    this.restoreState()
    this.stats.lastMs = performance.now() - t0
    if (this.stats.lastMs > 20) this.stats.slow.push([Math.round(t0), Math.round(this.stats.lastMs), Math.round(r0 - t0), this.stats.batches, this.pages.size])
    if (this.stats.slow.length > 40) this.stats.slow.shift()
    this.stats.pending = this.queue.length
    this.stats.activePages = this.pages.size
  }

  private uploadOccluders(mesh: Float32Array): void {
    this.ensurePrograms()
    const gl = this.gl
    const n = Math.floor(mesh.length / CORE_FLOATS)
    gl.bindVertexArray(this.occVao); gl.bindBuffer(gl.ARRAY_BUFFER, this.occVbo)
    gl.bufferData(gl.ARRAY_BUFFER, mesh, gl.DYNAMIC_DRAW)
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 3, gl.FLOAT, false, CORE_FLOATS * 4, 0)
    gl.bindVertexArray(null)
    this.occCount = n
  }

  private trianglesInCells(info: TriInfo, cells: Set<string>): Uint32Array {
    const out: number[] = []
    for (let t = 0; t < info.count; t++) if (cells.has(cellKey(info.sphere[t * 4], info.sphere[t * 4 + 1], info.sphere[t * 4 + 2]))) out.push(t)
    return Uint32Array.from(out)
  }

  /** Splits triangles into batches: by 64 m cell (one sun shadow map covers a cell), nearest to the camera first, at most BATCH_TRIS each. */
  private enqueue(tris: Uint32Array, info: TriInfo, camera: [number, number, number]): void {
    const groups = new Map<string, number[]>()
    for (const t of tris) {
      const x = info.sphere[t * 4], y = info.sphere[t * 4 + 1], z = info.sphere[t * 4 + 2]
      this.cells.add(cellKey(x, y, z))
      const k = `${Math.floor(x / CELL)},${Math.floor(z / CELL)},${Math.floor(y / CELL)}`
      const g = groups.get(k) ?? []; groups.set(k, g); g.push(t)
    }
    const batches: Batch[] = []
    for (const g of groups.values())
      for (let i = 0; i < g.length; i += BATCH_TRIS) {
        const part = Uint32Array.from(g.slice(i, i + BATCH_TRIS))
        let cx = 0, cy = 0, cz = 0, r = 0
        for (const t of part) { cx += info.sphere[t * 4]; cy += info.sphere[t * 4 + 1]; cz += info.sphere[t * 4 + 2] }
        cx /= part.length; cy /= part.length; cz /= part.length
        for (const t of part) r = Math.max(r, Math.hypot(info.sphere[t * 4] - cx, info.sphere[t * 4 + 1] - cy, info.sphere[t * 4 + 2] - cz) + info.sphere[t * 4 + 3])
        batches.push({ tris: part, sphere: [cx, cy, cz, r] })
      }
    const d = (b: Batch) => Math.hypot(b.sphere[0] - camera[0], b.sphere[1] - camera[1], b.sphere[2] - camera[2]) - b.sphere[3]
    // batches queued earlier that are still waiting join the new ones, nearest first
    this.queue = [...this.queue, ...batches].sort((a, b) => d(b) - d(a)) // popped from the end: nearest last
  }

  private run(l: LightmapStore, target: PredictorTarget, scene: PredScene, t0: number): void {
    // always one batch (progress), then more while the time of this frame's work so far allows. A page the batch needs that does not exist yet is made first,
    // one per frame (two uploads of a whole page and its coverage mask are a few milliseconds each)
    let first = true, done = 0
    while (this.queue.length > 0 && (first || (performance.now() - t0 < FRAME_BUDGET_MS && done < FRAME_MAX_BATCHES))) {
      first = false
      done++
      const next = this.queue[this.queue.length - 1]
      const missing = new Set<number>()
      for (const t of next.tris) { const pg = this.info!.page[t]; if (!this.pages.has(pg)) missing.add(pg) }
      if (missing.size > 0) {
        const e0 = performance.now()
        this.ensurePage(l, missing.values().next().value as number)
        this.stats.pageMs = performance.now() - e0
        if (missing.size > 1) break // the rest next frame
      }
      const b0 = performance.now()
      const b = this.queue.pop()!
      this.predictBatch(l, target, scene, b)
      this.stats.batches++
      this.stats.triangles += b.tris.length
      this.stats.batchMs = performance.now() - b0
    }
  }

  // ---- one batch --------------------------------------------------------------------------------------------------------------------
  private predictBatch(l: LightmapStore, target: PredictorTarget, scene: PredScene, b: Batch): void {
    const gl = this.gl, mesh = this.mesh!
    this.ensurePrograms(); this.ensureShadowTargets()
    // the lights that can reach the batch
    const reaching: PredLight[] = []
    for (const li of scene.lights) {
      if (li.kind === 'directional' || Math.hypot(b.sphere[0] - li.position[0], b.sphere[1] - li.position[1], b.sphere[2] - li.position[2]) - b.sphere[3] <= li.radius) reaching.push(li)
    }
    const used = reaching.filter((x) => !this.debugOnly || this.debugOnly.includes(x.id)).slice(0, MAX_LIGHTS)
    // shadow maps
    const params = { pos: new Float32Array(MAX_LIGHTS * 4), dir: new Float32Array(MAX_LIGHTS * 4), col: new Float32Array(MAX_LIGHTS * 4), cone: new Float32Array(MAX_LIGHTS * 4),
      sunB: new Float32Array(MAX_LIGHTS * 4), sunR: new Float32Array(MAX_LIGHTS * 3), sunU: new Float32Array(MAX_LIGHTS * 3), sunW: new Float32Array(MAX_LIGHTS * 2) }
    let sunLayer = 0
    const cached = new Map<string, number>()
    used.forEach((li, i) => {
      params.pos.set([...li.position, li.radius], i * 4)
      params.dir.set([...li.forward, li.kind === 'directional' ? 2 : li.kind === 'spot' ? 1 : 0], i * 4)
      params.col.set([...li.color, li.size], i * 4)
      let slot = 0, casts = li.castShadows ? 1 : 0
      if (li.castShadows && li.kind !== 'directional') slot = this.cubeFor(li, cached)
      else if (li.castShadows) {
        if (sunLayer < 2) { slot = sunLayer++; this.renderSun(li, b, slot, params, i) } else casts = 0 // a third sun is not shadowed
      }
      params.cone.set([li.cosInner, li.cosOuter, casts, slot], i * 4)
    })
    // receivers of the batch, per page
    const byPage = new Map<number, number[]>()
    for (const t of b.tris) { const pg = this.info!.page[t]; const g = byPage.get(pg) ?? []; byPage.set(pg, g); g.push(t) }
    const atlas = l.atlas
    const P = this.progRecv!
    // the receiver program and its uniforms: set again for every page, because creating a page and composing use other programs
    const bindReceiverProgram = (): void => {
      gl.disable(gl.DEPTH_TEST); gl.disable(gl.CULL_FACE); gl.disable(gl.BLEND); gl.disable(gl.SCISSOR_TEST)
      gl.useProgram(P)
      gl.uniform1i(gl.getUniformLocation(P, 'uCount'), used.length)
      gl.uniform3f(gl.getUniformLocation(P, 'uAmbient'), scene.ambient[0], scene.ambient[1], scene.ambient[2])
      gl.uniform4fv(gl.getUniformLocation(P, 'uLPos'), params.pos); gl.uniform4fv(gl.getUniformLocation(P, 'uLDir'), params.dir)
      gl.uniform4fv(gl.getUniformLocation(P, 'uLCol'), params.col); gl.uniform4fv(gl.getUniformLocation(P, 'uLCone'), params.cone)
      gl.uniform4fv(gl.getUniformLocation(P, 'uSunB'), params.sunB); gl.uniform3fv(gl.getUniformLocation(P, 'uSunR'), params.sunR)
      gl.uniform3fv(gl.getUniformLocation(P, 'uSunU'), params.sunU)
      gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D_ARRAY, this.ptTex); gl.uniform1i(gl.getUniformLocation(P, 'uPt'), 0)
      gl.activeTexture(gl.TEXTURE1); gl.bindTexture(gl.TEXTURE_2D_ARRAY, this.sunTex); gl.uniform1i(gl.getUniformLocation(P, 'uSun'), 1)
    }
    for (const [page, tris] of byPage) {
      const pt = this.ensurePage(l, page)
      const { data, count, rect } = buildReceivers(mesh, tris, atlas)
      if (count === 0) continue
      bindReceiverProgram()
      gl.bindFramebuffer(gl.FRAMEBUFFER, pt.pFbo)
      gl.viewport(0, 0, atlas, atlas)
      gl.bindVertexArray(this.rcvVao); gl.bindBuffer(gl.ARRAY_BUFFER, this.rcvVbo)
      gl.bufferData(gl.ARRAY_BUFFER, data, gl.STREAM_DRAW)
      gl.drawArrays(gl.TRIANGLES, 0, count)
      gl.bindVertexArray(null)
      this.compose(target, page, rect[0], rect[1], rect[2], rect[3], atlas)
    }
    gl.activeTexture(gl.TEXTURE0)
  }

  /** The cube of a point / spot light in the shadow array: rendered when it is not there yet for this edit. Returns its first layer. */
  private cubeFor(li: PredLight, cached: Map<string, number>): number {
    const key = li.id
    const here = cached.get(key)
    if (here !== undefined) return here
    const have = this.cubeCache.get(key)
    if (have !== undefined && this.cubeSlots[have / 6] === key) { cached.set(key, have); return have }
    const slot = this.nextSlot; this.nextSlot = (this.nextSlot + 1) % MAX_LIGHTS
    this.cubeSlots[slot] = key; this.cubeCache.set(key, slot * 6)
    cached.set(key, slot * 6)
    const gl = this.gl
    gl.useProgram(this.progShadow)
    gl.enable(gl.DEPTH_TEST); gl.depthFunc(gl.LESS); gl.depthMask(true); gl.disable(gl.CULL_FACE); gl.disable(gl.BLEND); gl.disable(gl.SCISSOR_TEST)
    const near = 0.02, far = Math.max(li.radius, near * 2)
    for (let f = 0; f < 6; f++) {
      const fb = this.layerFbo(this.ptTex!, slot * 6 + f, this.ptFbos, CUBE_RES)
      gl.bindFramebuffer(gl.FRAMEBUFFER, fb)
      gl.viewport(0, 0, CUBE_RES, CUBE_RES)
      gl.clearBufferfv(gl.COLOR, 0, [1e9, 0, 0, 0]); gl.clearBufferfv(gl.DEPTH, 0, [1])
      const F = FACES[f], L = li.position
      const rows = [F.r, F.u, [-F.w[0], -F.w[1], -F.w[2]]]
      // view: rows r, u, -w and the translation; projection 90 degrees. Column major.
      const vp = new Float32Array(16)
      const m = (row: number[], k: number) => row[k]
      const t = rows.map((r) => -(r[0] * L[0] + r[1] * L[1] + r[2] * L[2]))
      const a = (far + near) / (near - far), c = (2 * far * near) / (near - far)
      // clip = (x_v, y_v, a z_v + c, -z_v) with view space (x_v, y_v, z_v) = (r . d, u . d, -w . d), d = p - L
      for (let col = 0; col < 3; col++) {
        vp[col * 4 + 0] = m(rows[0], col); vp[col * 4 + 1] = m(rows[1], col)
        vp[col * 4 + 2] = a * m(rows[2], col); vp[col * 4 + 3] = -m(rows[2], col)
      }
      vp[12] = t[0]; vp[13] = t[1]; vp[14] = a * t[2] + c; vp[15] = -t[2]
      gl.uniformMatrix4fv(gl.getUniformLocation(this.progShadow!, 'uVP'), false, vp)
      gl.uniform3f(gl.getUniformLocation(this.progShadow!, 'uRef'), L[0], L[1], L[2])
      gl.uniform3f(gl.getUniformLocation(this.progShadow!, 'uDir'), 0, 0, 0)
      gl.uniform1f(gl.getUniformLocation(this.progShadow!, 'uMode'), 0)
      gl.bindVertexArray(this.occVao)
      gl.drawArrays(gl.TRIANGLES, 0, this.occCount)
    }
    gl.bindVertexArray(null)
    gl.disable(gl.DEPTH_TEST)
    return slot * 6
  }

  /** The sun's orthographic shadow map over the batch (light space bounds from the batch's sphere, widened by what can shadow it). */
  private renderSun(li: PredLight, b: Batch, layer: number, params: { sunB: Float32Array; sunR: Float32Array; sunU: Float32Array; sunW: Float32Array }, i: number): void {
    const gl = this.gl
    const w = norm([li.forward[0], li.forward[1], li.forward[2]])
    const up = Math.abs(w[1]) < 0.95 ? [0, 1, 0] : [1, 0, 0]
    const r = norm(cross(w, up)), u = cross(r, w)
    const c = b.sphere, rad = b.sphere[3] + 1
    const cr = c[0] * r[0] + c[1] * r[1] + c[2] * r[2], cu = c[0] * u[0] + c[1] * u[1] + c[2] * u[2]
    // depth range along the light: everything that can lie between the sun and the batch (the whole occluder set's extent along w)
    const lo = -4096, hi = 4096
    params.sunB.set([cr - rad, cr + rad, cu - rad, cu + rad], i * 4)
    params.sunR.set(r, i * 3); params.sunU.set(u, i * 3); params.sunW.set([lo, 1 / (hi - lo)], i * 2)
    const fb = this.layerFbo(this.sunTex!, layer, this.sunFbos, SUN_RES)
    gl.bindFramebuffer(gl.FRAMEBUFFER, fb)
    gl.viewport(0, 0, SUN_RES, SUN_RES)
    gl.enable(gl.DEPTH_TEST); gl.depthFunc(gl.LESS); gl.depthMask(true); gl.disable(gl.CULL_FACE); gl.disable(gl.BLEND); gl.disable(gl.SCISSOR_TEST)
    gl.clearBufferfv(gl.COLOR, 0, [1e9, 0, 0, 0]); gl.clearBufferfv(gl.DEPTH, 0, [1])
    gl.useProgram(this.progShadow)
    // ortho: x = (dot(p, r) - minR) / (maxR - minR) * 2 - 1, y likewise, z = (dot(p, w) - lo) / (hi - lo) * 2 - 1
    const sx = 2 / (2 * rad), sz = 2 / (hi - lo)
    const vp = new Float32Array(16)
    vp[0] = sx * r[0]; vp[4] = sx * r[1]; vp[8] = sx * r[2]; vp[12] = -sx * (cr)
    vp[1] = sx * u[0]; vp[5] = sx * u[1]; vp[9] = sx * u[2]; vp[13] = -sx * (cu)
    vp[2] = sz * w[0]; vp[6] = sz * w[1]; vp[10] = sz * w[2]; vp[14] = -sz * lo - 1
    vp[3] = 0; vp[7] = 0; vp[11] = 0; vp[15] = 1
    gl.uniformMatrix4fv(gl.getUniformLocation(this.progShadow!, 'uVP'), false, vp)
    gl.uniform3f(gl.getUniformLocation(this.progShadow!, 'uRef'), 0, 0, 0)
    gl.uniform3f(gl.getUniformLocation(this.progShadow!, 'uDir'), w[0], w[1], w[2])
    gl.uniform1f(gl.getUniformLocation(this.progShadow!, 'uMode'), 1)
    gl.bindVertexArray(this.occVao)
    gl.drawArrays(gl.TRIANGLES, 0, this.occCount)
    gl.bindVertexArray(null)
    gl.disable(gl.DEPTH_TEST)
  }

  /** Test / profiling hook: predicts every triangle now (the whole queue, ignoring the frame budget). */
  debugPredictAll(l: LightmapStore, world: World, defs: readonly EntityClassDef[], target: PredictorTarget, camera: [number, number, number]): { ms: number; triangles: number; batches: number } {
    const t0 = performance.now()
    if (!l.mesh || !l.directLayer) return { ms: 0, triangles: 0, batches: 0 }
    const plain = predictorMesh(l.mesh, l.faces, l.staleness(world)).mesh
    this.mesh = plain; this.info = triInfo(plain); this.uploadOccluders(plain)
    const scene = collectPredScene(world, defs, { sourceSize: l.status?.defaultSourceSize ?? 0.25, sunSize: l.status?.defaultSunSize ?? 0.5 })
    this.predScene = scene; this.predBoxes = staleBoxes(l.mesh!, l.faces, l.staleness(world))
    this.generation++; this.cubeCache.clear(); this.cubeSlots.fill(null)
    this.enqueue(Uint32Array.from({ length: this.info.count }, (_, i) => i), this.info, camera)
    const batches = this.queue.length
    while (this.queue.length > 0) this.predictBatch(l, target, scene, this.queue.pop()!)
    this.gl.finish()
    this.restoreState()
    this.lastPredictAt = performance.now()
    return { ms: performance.now() - t0, triangles: this.info.count, batches }
  }
  /** Test hook: the predicted direct light (P) against the Core's direct layer over the texels both have: mean of (P / D) and the RMS difference relative to the mean D, per colour channel summed. */
  debugCompare(l: LightmapStore, regionPages?: number[]): { texels: number; meanRatio: number; relRms: number; meanD: number; meanP: number; p50: number; p90: number; p99: number; within15: number; worst: { page: number; x: number; y: number; p: number[]; d: number[] }[] } {
    const gl = this.gl, atlas = l.atlas
    let n = 0, sumRatio = 0, sumSq = 0, sumD = 0, sumP = 0
    const rel: number[] = []
    const worst: { page: number; x: number; y: number; p: number[]; d: number[]; e: number }[] = []
    const half = (h: number) => { const e = (h >> 10) & 31, m = h & 1023; const v = e === 0 ? m * 2 ** -24 : (1 + m / 1024) * 2 ** (e - 15); return h & 0x8000 ? -v : v }
    for (const [page, t] of this.pages) {
      if (regionPages && !regionPages.includes(page)) continue
      const buf = new Float32Array(atlas * atlas * 4)
      gl.bindFramebuffer(gl.FRAMEBUFFER, t.pFbo)
      gl.readPixels(0, 0, atlas, atlas, gl.RGBA, gl.FLOAT, buf)
      const d = l.directPages[page]
      for (let i = 0; i < atlas * atlas; i++) {
        if (buf[i * 4 + 3] < 0.5 || d[i * 4 + 3] === 0) continue
        // only the texels the Core has a sample for are comparable: the Core's alpha is its AO factor for all of its texels, the gutter included
        const p = [buf[i * 4], buf[i * 4 + 1], buf[i * 4 + 2]], dv = [half(d[i * 4]), half(d[i * 4 + 1]), half(d[i * 4 + 2])]
        const pl = (p[0] + p[1] + p[2]) / 3, dl = (dv[0] + dv[1] + dv[2]) / 3
        if (dl < 1e-4 && pl < 1e-4) continue
        rel.push(Math.abs(pl - dl) / Math.max(dl, 0.02))
        n++; sumD += dl; sumP += pl; sumRatio += pl / Math.max(dl, 1e-3); sumSq += (pl - dl) * (pl - dl)
        const e = Math.abs(pl - dl)
        if (worst.length < 6 || e > worst[worst.length - 1].e) { worst.push({ page, x: i % atlas, y: Math.floor(i / atlas), p, d: dv, e }); worst.sort((a, b) => b.e - a.e); if (worst.length > 6) worst.pop() }
      }
    }
    this.restoreState()
    rel.sort((a, b) => a - b)
    const q = (f: number) => (rel.length ? rel[Math.min(rel.length - 1, Math.floor(rel.length * f))] : 0)
    return { texels: n, p50: q(0.5), p90: q(0.9), p99: q(0.99), within15: rel.length ? rel.filter((x) => x < 0.15).length / rel.length : 0, meanRatio: n ? sumRatio / n : 0, relRms: n ? Math.sqrt(sumSq / n) / Math.max(sumD / n, 1e-6) : 0, meanD: n ? sumD / n : 0, meanP: n ? sumP / n : 0, worst: worst.map(({ e, ...w }) => w) }
  }

  dispose(): void {
    this.releaseAll(null, null, false)
    const gl = this.gl
    for (const p of [this.progShadow, this.progRecv, this.progCompose, this.progMask]) if (p) gl.deleteProgram(p)
    for (const t of [this.ptTex, this.sunTex]) if (t) gl.deleteTexture(t)
    for (const f of [...this.ptFbos, ...this.sunFbos, this.fbo]) if (f) gl.deleteFramebuffer(f)
    for (const b of [this.occVbo, this.rcvVbo]) if (b) gl.deleteBuffer(b)
    for (const rb of this.depthRbs.values()) gl.deleteRenderbuffer(rb)
  }
}

const cellKey = (x: number, y: number, z: number): string => `${Math.floor(x / 8)},${Math.floor(y / 8)},${Math.floor(z / 8)}`

/**
 * The receiver geometry of triangles of one page: position (3), normal (3), lightmap uv (2) per vertex. Also the rectangle (x, y, w, h in texels) the result
 * covers, with a margin for the bicubic lookup (the gutter beyond a chart is filled from the nearest texel when the light is composed).
 */
export function buildReceivers(mesh: Float32Array, tris: readonly number[] | Uint32Array, atlas: number): { data: Float32Array; count: number; rect: [number, number, number, number] } {
  const exact: number[] = []
  let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity
  for (const t of tris) {
    const o = t * 3 * CORE_FLOATS
    const P: number[][] = [], N: number[][] = [], U: number[][] = []
    for (let k = 0; k < 3; k++) {
      const v = o + k * CORE_FLOATS
      P.push([mesh[v], mesh[v + 1], mesh[v + 2]])
      const nx = mesh[v + 3], ny = mesh[v + 4], nz = mesh[v + 5], nl = Math.hypot(nx, ny, nz) || 1
      N.push([nx / nl, ny / nl, nz / nl])
      U.push([mesh[v + 6] * atlas, mesh[v + 7] * atlas])
    }
    const area = (U[1][0] - U[0][0]) * (U[2][1] - U[0][1]) - (U[2][0] - U[0][0]) * (U[1][1] - U[0][1])
    if (Math.abs(area) < 1e-9) continue
    for (let k = 0; k < 3; k++) { minX = Math.min(minX, U[k][0]); maxX = Math.max(maxX, U[k][0]); minY = Math.min(minY, U[k][1]); maxY = Math.max(maxY, U[k][1]) }
    const push = (out: number[], u: number[][]) => {
      for (let k = 0; k < 3; k++) {
        // barycentric weights of u[k] in the exact triangle: the attributes continue the plane
        const px = u[k][0], py = u[k][1]
        const w1 = ((px - U[0][0]) * (U[2][1] - U[0][1]) - (py - U[0][1]) * (U[2][0] - U[0][0])) / area
        const w2 = ((U[1][0] - U[0][0]) * (py - U[0][1]) - (U[1][1] - U[0][1]) * (px - U[0][0])) / area
        const w0 = 1 - w1 - w2
        out.push(P[0][0] * w0 + P[1][0] * w1 + P[2][0] * w2, P[0][1] * w0 + P[1][1] * w1 + P[2][1] * w2, P[0][2] * w0 + P[1][2] * w1 + P[2][2] * w2,
          N[0][0] * w0 + N[1][0] * w1 + N[2][0] * w2, N[0][1] * w0 + N[1][1] * w1 + N[2][1] * w2, N[0][2] * w0 + N[1][2] * w1 + N[2][2] * w2, px / atlas, py / atlas)
      }
    }
    push(exact, U)
  }
  const data = Float32Array.from(exact)
  const m = 3
  const x0 = Math.max(0, Math.floor(minX) - m), y0 = Math.max(0, Math.floor(minY) - m)
  const x1 = Math.min(atlas, Math.ceil(maxX) + m), y1 = Math.min(atlas, Math.ceil(maxY) + m)
  return { data, count: data.length / 8, rect: [x0, y0, Math.max(0, x1 - x0), Math.max(0, y1 - y0)] }
}
