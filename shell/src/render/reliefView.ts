// Small WebGL2 renderer of the relief editor's viewport: the textured relief mesh (two-sided, back faces tinted red), faint neighbouring repeats, the tile
// outline and grid on the surface plane, element overlays (selected faces, edges, vertices) and axes. Independent of the map Renderer: its own context, own
// texture upload, nothing shared.
import type { Vec3 } from '../model/math'

export interface ReliefScene {
  /** interleaved position(3) normal(3) uv(2) triangles of the mesh */
  tris: Float32Array
  /** plain xyz triangles of the selected faces */
  selTris: number[]
  /** xyz line pairs: all edges, selected edges */
  edges: number[]; selEdges: number[]
  /** xyz points: all vertices, selected vertices */
  points: number[]; selPoints: number[]
  /** the tile outline and grid on the surface plane (xyz line pairs) */
  outline: number[]; grid: number[]
  /** width / height of one repeat, metres */
  tile: { w: number; h: number }
  textured: boolean
  ghosts: boolean
  axes: number
  /** extra lines drawn on top (loop-cut preview) */
  overlay: number[]
}

const VS_MESH = `#version 300 es
layout(location=0) in vec3 aPos; layout(location=1) in vec3 aNrm; layout(location=2) in vec2 aUv;
uniform mat4 uVP; out vec3 vN; out vec2 vUv;
void main(){ vN = aNrm; vUv = aUv; gl_Position = uVP * vec4(aPos, 1.0); }`
const FS_MESH = `#version 300 es
precision mediump float;
in vec3 vN; in vec2 vUv; uniform sampler2D uTex; uniform float uHasTex; uniform float uAlpha;
out vec4 o;
void main(){
  vec3 base = uHasTex > 0.5 ? texture(uTex, vUv).rgb : vec3(0.62);
  vec3 n = normalize(vN);
  vec3 L = normalize(vec3(-0.45, 0.6, 0.65));
  float lit = 0.42 + 0.58 * max(dot(n, L), 0.0);
  vec3 c = base * lit;
  if (!gl_FrontFacing) c = mix(c, vec3(0.85, 0.12, 0.12), 0.55);
  o = vec4(c, uAlpha);
}`
const VS_LINE = `#version 300 es
layout(location=0) in vec3 aPos; uniform mat4 uVP; uniform float uSize;
void main(){ gl_Position = uVP * vec4(aPos, 1.0); gl_PointSize = uSize; }`
const FS_LINE = `#version 300 es
precision mediump float; uniform vec4 uColor; out vec4 o; void main(){ o = uColor; }`

export class ReliefView {
  private gl: WebGL2RenderingContext
  private mesh: WebGLProgram
  private line: WebGLProgram
  private buf: WebGLBuffer
  private tex: WebGLTexture | null = null
  private texSource: unknown = null

  constructor(readonly canvas: HTMLCanvasElement) {
    const gl = canvas.getContext('webgl2', { antialias: true })
    if (!gl) throw new Error('WebGL2 is not available')
    this.gl = gl
    this.mesh = this.program(VS_MESH, FS_MESH)
    this.line = this.program(VS_LINE, FS_LINE)
    this.buf = gl.createBuffer()!
  }

  private program(vs: string, fs: string): WebGLProgram {
    const gl = this.gl, p = gl.createProgram()!
    for (const [type, src] of [[gl.VERTEX_SHADER, vs], [gl.FRAGMENT_SHADER, fs]] as const) {
      const s = gl.createShader(type)!
      gl.shaderSource(s, src); gl.compileShader(s)
      if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s) ?? 'shader error')
      gl.attachShader(p, s)
    }
    gl.linkProgram(p)
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p) ?? 'link error')
    return p
  }

  /** The texture shown on the mesh (null = grey). Uploaded again only when the source object changes. */
  setTexture(source: TexImageSource | null): void {
    if (source === this.texSource) return
    this.texSource = source
    const gl = this.gl
    if (this.tex) { gl.deleteTexture(this.tex); this.tex = null }
    if (!source) return
    const t = gl.createTexture()!
    gl.bindTexture(gl.TEXTURE_2D, t)
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false)
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, source)
    gl.generateMipmap(gl.TEXTURE_2D)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR_MIPMAP_LINEAR)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.REPEAT)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.REPEAT)
    this.tex = t
  }

  resize(): void {
    const dpr = Math.min(2, window.devicePixelRatio || 1)
    const w = Math.max(1, Math.round(this.canvas.clientWidth * dpr)), h = Math.max(1, Math.round(this.canvas.clientHeight * dpr))
    if (this.canvas.width !== w || this.canvas.height !== h) { this.canvas.width = w; this.canvas.height = h }
  }

  private lines(vp: Float32Array, data: number[], color: [number, number, number, number], mode: number, size = 1): void {
    if (data.length === 0) return
    const gl = this.gl
    gl.useProgram(this.line)
    gl.uniformMatrix4fv(gl.getUniformLocation(this.line, 'uVP'), false, vp)
    gl.uniform4f(gl.getUniformLocation(this.line, 'uColor'), ...color)
    gl.uniform1f(gl.getUniformLocation(this.line, 'uSize'), size * Math.min(2, window.devicePixelRatio || 1))
    gl.bindBuffer(gl.ARRAY_BUFFER, this.buf)
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(data), gl.DYNAMIC_DRAW)
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 12, 0)
    gl.disableVertexAttribArray(1); gl.disableVertexAttribArray(2)
    gl.drawArrays(mode, 0, data.length / 3)
  }

  private meshDraw(vp: Float32Array, data: Float32Array, alpha: number): void {
    if (data.length === 0) return
    const gl = this.gl
    gl.useProgram(this.mesh)
    gl.uniformMatrix4fv(gl.getUniformLocation(this.mesh, 'uVP'), false, vp)
    gl.uniform1f(gl.getUniformLocation(this.mesh, 'uHasTex'), this.tex ? 1 : 0)
    gl.uniform1f(gl.getUniformLocation(this.mesh, 'uAlpha'), alpha)
    gl.uniform1i(gl.getUniformLocation(this.mesh, 'uTex'), 0)
    if (this.tex) { gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, this.tex) }
    gl.bindBuffer(gl.ARRAY_BUFFER, this.buf)
    gl.bufferData(gl.ARRAY_BUFFER, data, gl.DYNAMIC_DRAW)
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 32, 0)
    gl.enableVertexAttribArray(1); gl.vertexAttribPointer(1, 3, gl.FLOAT, false, 32, 12)
    gl.enableVertexAttribArray(2); gl.vertexAttribPointer(2, 2, gl.FLOAT, false, 32, 24)
    gl.drawArrays(gl.TRIANGLES, 0, data.length / 8)
  }

  draw(vp: Float32Array, s: ReliefScene): void {
    this.resize()
    const gl = this.gl
    gl.viewport(0, 0, this.canvas.width, this.canvas.height)
    gl.clearColor(0.105, 0.113, 0.13, 1)
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT)
    gl.disable(gl.CULL_FACE)

    // faint neighbouring repeats: what the tile joins up with
    if (s.ghosts) {
      gl.enable(gl.BLEND); gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA)
      gl.disable(gl.DEPTH_TEST); gl.depthMask(false)
      const q: number[] = []
      const quad = (x: number, y: number): void => {
        const { w, h } = s.tile
        const c = [[x, y, 0, 0, 1], [x + w, y, 1, 0, 1], [x + w, y + h, 1, 1, 0], [x, y + h, 0, 1, 0]]
        const v = (i: number): void => { const p = c[i]; q.push(p[0], p[1], -0.0001, 0, 0, 1, p[2], p[4]) }
        for (const i of [0, 1, 2, 0, 2, 3]) v(i)
      }
      for (let dx = -1; dx <= 1; dx++) for (let dy = -1; dy <= 1; dy++) if (dx || dy) quad(dx * s.tile.w, dy * s.tile.h)
      this.meshDraw(vp, new Float32Array(q), 0.3)
      gl.depthMask(true)
    }

    gl.enable(gl.DEPTH_TEST); gl.depthFunc(gl.LEQUAL)
    gl.disable(gl.BLEND)
    gl.enable(gl.POLYGON_OFFSET_FILL); gl.polygonOffset(1, 1)
    const saved = this.tex
    if (!s.textured) this.tex = null
    this.meshDraw(vp, s.tris, 1)
    this.tex = saved
    gl.disable(gl.POLYGON_OFFSET_FILL)

    gl.enable(gl.BLEND); gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA)
    if (s.selTris.length) {
      gl.enable(gl.POLYGON_OFFSET_FILL); gl.polygonOffset(-1, -1)
      this.lines(vp, s.selTris, [1, 0.62, 0.12, 0.38], gl.TRIANGLES)
      gl.disable(gl.POLYGON_OFFSET_FILL)
    }
    // grid + outline on the surface plane
    this.lines(vp, s.grid, [1, 1, 1, 0.07], gl.LINES)
    this.lines(vp, s.outline, [0.3, 0.75, 1, 0.9], gl.LINES)
    // edges: dim where hidden, bright where visible
    gl.disable(gl.DEPTH_TEST)
    this.lines(vp, s.edges, [1, 1, 1, 0.16], gl.LINES)
    gl.enable(gl.DEPTH_TEST)
    this.lines(vp, s.edges, [0.95, 0.95, 1, 0.85], gl.LINES)
    gl.disable(gl.DEPTH_TEST)
    this.lines(vp, s.selEdges, [1, 0.62, 0.12, 1], gl.LINES)
    this.lines(vp, s.points, [0.9, 0.92, 1, 1], gl.POINTS, 6)
    this.lines(vp, s.selPoints, [1, 0.62, 0.12, 1], gl.POINTS, 8)
    this.lines(vp, s.overlay, [0.3, 1, 0.5, 1], gl.LINES)
    if (s.axes > 0) {
      const a = s.axes, o: Vec3 = [0, 0, 0]
      this.lines(vp, [...o, a, 0, 0], [0.95, 0.3, 0.3, 1], gl.LINES)
      this.lines(vp, [...o, 0, a, 0], [0.4, 0.85, 0.35, 1], gl.LINES)
      this.lines(vp, [...o, 0, 0, a], [0.35, 0.55, 1, 1], gl.LINES)
    }
    gl.disable(gl.BLEND)
  }

  dispose(): void {
    const gl = this.gl
    if (this.tex) gl.deleteTexture(this.tex)
    gl.deleteBuffer(this.buf); gl.deleteProgram(this.mesh); gl.deleteProgram(this.line)
    gl.getExtension('WEBGL_lose_context')?.loseContext()
  }
}
