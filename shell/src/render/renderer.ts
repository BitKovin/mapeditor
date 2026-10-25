// WebGL2 renderer. Unlit only (DESIGN.md §6.2): flat colour per texture path with a fixed
// normal-based tint for readability -- no lighting. Geometry is uploaded per Brush OBJECT and
// cached in a WeakMap; edits are immutable, so an unchanged brush keeps its GPU buffers.
import type { Brush, Entity } from '../model/document'
import type { CarvedMesh } from '../model/derived'
import type { MeshSel, SelMode } from '../model/meshTopology'
import { buildTopology, parseEdge } from '../model/meshTopology'
import { carvedKey } from '../model/derived'
import { classOf, isBrushEntity, isEditorOnly, originOf, showsCustomOrigin } from '../model/document'
import { faceNormal, triangulateFace } from '../model/geometry'
import { uvsForDraw } from '../model/uv'
import { drawTransparency, texEnv } from '../model/texprops'
import { newBudget, planHasRelief, planWithRelief, reliefOf, reliefTriangle } from '../model/reliefApply'
import { HEAT_BUCKETS, bucketColor, faceHeat, heatBucket } from '../model/texelDensity'
import { brushTextureSignature, lerpVec3, planForFace, retexture } from '../model/uvmesh'
import type { TextureCache } from '../state/textures'
import { LightmapStore, LIT_VERTEX_FLOATS, packRect } from './lightmapStore'
import type { LightvolStore } from './lightvolStore'
import { styleValue } from './lightStyles'
import { entityLightKey } from './lightvolStore'
import { buildProbePreview, PREVIEW_STRIDE, PROBE_PREVIEW_MAX } from './lightvolPreview'
import { DENSITY_HIGH, DENSITY_LOW, DENSITY_STRIDE, densityOf } from './lightmapDensity'
import type { ProbePreview } from './lightvolPreview'
import type { LitDraw } from './litTextured'
import { LIT_TEX_FLOATS, buildEntityDecals, buildLitDecals } from './litTextured'
import type { EntityDecalGroup } from './litTextured'
import type { DecalEntry, World } from '../model/derived'
import type { Vec3 } from '../model/math'
import type { Selection } from '../state/store'
import { OrthoCamera } from './camera'
import type { Camera } from './camera'
import { orthoGrid } from './orthoGrid'
import { AXIS_DIR, Axis, ringLines } from './gizmo'
import type { Box } from '../model/entityBox'
import { boxFromQuake } from '../model/entityBox'
import type { OrientedBox } from '../model/pointBox'
import { cornersOf, drawsBox, pointBox, toWorld, worldCorners } from '../model/pointBox'
import type { ViewportSettings } from './viewportSettings'
import { SKY_FACES, SKY_GLSL, isSkyTexture } from '../model/sky'
import { mat4 } from 'gl-matrix'

/** `instanceId` = the top-level prefab instance this content belongs to (undefined for the document's own entities). */
/** `instance` = the full instance path of prefab content ("a/b" for nested; undefined for the document's own entities), the key under which Core files its carved result. */
export interface DrawEntity { entity: Entity; readonly: boolean; instanceId?: string; instance?: string }
export interface Frame {
  camera: Camera
  /** Marker colour for a point-entity class (from the game's entity definitions); undefined = default. */
  entityColor?: (classname: string) => [number, number, number] | undefined
  /** Bounding box of a point-entity class relative to its origin (editor space, from the .ent definition); a default box when absent. */
  entityBox?: (classname: string) => Box
  /** A half-transparent preview of something about to be placed (a prefab's content or a point entity). The entities are in their own local space; `offset` moves them to where they would land. */
  ghost?: { entities: Entity[]; offset: Vec3 } | null
  /** Render to a fixed pixel size instead of the canvas's CSS size (preview icons). */
  pixelSize?: [number, number]
  /** Clear colour; the viewport colour when absent. */
  background?: [number, number, number, number]
  settings: ViewportSettings
  /** The Core's live lighting; drawn when `settings.renderMode` is 'lightingOnly' and it has a mesh. */
  lighting?: LightmapStore
  /** The usable decals as derived fragments: the Lit view lights them with the face they lie on. */
  decalEntries?: DecalEntry[]
  /** Seconds for light style animation (a monotonic clock); defaults to performance.now() / 1000. */
  time?: number
  /** Light volume results (entity light), for brush entities that are not lightmapped: Lit and Lighting-only. */
  lightvol?: LightvolStore
  /** The document and prefab content: the Lit view projects albedo textures onto the lit mesh with the faces' own projections. */
  world?: World
  /** Boolean results (keyed `entityId/brushId`) drawn instead of the authored brush. */
  carved?: Map<string, CarvedMesh>
  entities: DrawEntity[]
  selection: Selection | null
  /** `dir` set = a single handle along that direction (face drag) instead of the three axes. */
  gizmo: { pivot: Vec3; length: number; active: Axis | null; dir?: Vec3; mode?: 'move' | 'rotate' | 'scale' } | null
  /** Entity annotations (decal projector boxes, angle arrows): flat line endpoints, drawn depth-tested. */
  annotations?: number[]
  /** Coloured line overlays (entity direction arrows, link arrows): depth-tested, drawn after the annotations. */
  overlays?: { color: [number, number, number]; lines: number[] }[]
  /** Wire preview while drawing (flat array of line endpoints). */
  preview: number[] | null
  /** Mesh edit mode: the brush being edited, which element kind is active, and what is selected. */
  mesh?: { brush: Brush; mode: SelMode; sel: MeshSel } | null
  /** Decal meshes (derived), drawn just above the surfaces they project onto. */
  decals?: CarvedMesh[]
  /** Every selected item (multi-selection); when absent only `selection` is highlighted. */
  selections?: { entityId: string; brushId?: string; instanceId?: string }[]
  /** The selected face, drawn as a translucent overlay. */
  highlight: { brush: Brush; faceId: number } | null
  /** The face under the cursor while Alt is held (an Alt + drag resizes its brush). */
  hoverFace?: { brush: Brush; faceId: number } | null
  /** Handles drawn over everything (entity origin / angle handles): coloured lines. */
  handles?: { color: [number, number, number]; lines: number[] }[]
  /** Size of the selection (REQUIREMENTS §33 H7): text at world points, drawn over everything. */
  sizeLabels?: { text: string; at: Vec3 }[]
  /** The level's sky (a skybox) drawn behind everything in perspective views (Auto sky, REQUIREMENTS §28 K10); absent = the plain background. */
  sky?: string
}

// Texture transparency in the views (REQUIREMENTS §23), the same rules the bake uses: uTransp = (mode 0 opaque | 1 cutout | 2 translucent, cutoff,
// opacity). A cutout texel below the cutoff is not drawn; a translucent face is drawn blended with opacity x alpha (after the opaque geometry).
const TRANSP_GLSL = `
float transparency(float a) {
  if (uTransp.x > 0.5 && uTransp.x < 1.5 && a < uTransp.y) discard;
  return uTransp.x > 1.5 ? uTransp.z * a : 1.0;
}`
const MESH_VS = `#version 300 es
uniform mat4 uViewProj; layout(location = 0) in vec3 aPos; layout(location = 1) in vec3 aNormal; layout(location = 2) in vec2 aUv; out vec3 vN; out vec2 vUv;
void main() { vN = aNormal; vUv = aUv; gl_Position = uViewProj * vec4(aPos, 1.0); }`
const MESH_FS = `#version 300 es
precision mediump float; uniform vec4 uColor; uniform sampler2D uTex; uniform float uHasTex; uniform float uAlphaTex; uniform vec3 uTransp; in vec3 vN; in vec2 vUv; out vec4 o;
${TRANSP_GLSL}
void main() {
  float s = 0.7 + 0.3 * abs(dot(normalize(vN), normalize(vec3(0.35, 0.85, 0.4)))); // unlit: just a readability tint
  vec4 t = texture(uTex, vUv);
  vec3 base = mix(uColor.rgb, t.rgb, uHasTex);
  float a = uColor.a * mix(1.0, t.a, uAlphaTex * uHasTex); // decals take their alpha from the image
  o = vec4(base * s, a * transparency(t.a * uHasTex + (1.0 - uHasTex)));
}`
// 'Lighting only': the Core's lit mesh (positions + lightmap coordinates) sampled from the lightmap page array. The pixels are already
// sRGB-encoded 8 bit values (retro low dynamic range), so they go to the framebuffer as they are.
// ---- lit views (Lighting-only, Lit) --------------------------------------------------------------------------------------------------------------
// Lightmap pages are linear half floats exactly as baked (RGBA16F): nothing is clamped, scaled or encoded on the way, so brightness above 1 reaches the
// shader. `uExposure` is the level's own exposure setting (1 = none), `uBrightness` its brightness offset added after it (0 = none); the only clamp is the display's, in linearToSrgb, at the very end.
//
// Light styles (core/light_styles.h): every light style is a layer of the same pages, stored as extra slices of one texture array (slice = slot * pageCount +
// page; slot 0 is the base, the other slots are compact, only for styles that exist). A chart carries its layers as a bit mask (styles 1..63 = bit style-1,
// 21 bits per float, per vertex): the fragment samples the base and the layers in its mask:  light = sum over layers of  value(style, time) * lightmap_layer.
const SRGB_GLSL = `
vec3 srgbToLinear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c)); }
vec3 linearToSrgb(vec3 c) { c = clamp(c, 0.0, 1.0); return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c)); }`
const LAYERS_GLSL = `
uniform sampler2DArray uLightmap; uniform float uPages; uniform float uSlot[64]; uniform float uW[64];
// Bicubic B-spline lookup from 4 bilinear fetches (GPU Gems 2, ch. 20): smooth across texel borders, where bilinear's slope jumps and soft
// shadows show creases on the texel grid (REQUIREMENTS §30). It reads up to 2 texels around a chart's edge: the layout's padding is 2.
// The engine's fs_bsp.sc samples rBSP v2 lightmaps the same way.
vec3 lightmapTexel(vec3 uvp) {
  vec2 size = vec2(textureSize(uLightmap, 0).xy);
  vec2 p = uvp.xy * size - 0.5, f = fract(p), i = p - f;
  vec2 f2 = f * f, f3 = f2 * f;
  vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0, w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
  vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0, w3 = f3 / 6.0;
  vec2 g0 = w0 + w1, g1 = w2 + w3;
  vec2 a = (i - 0.5 + w1 / g0) / size, b = (i + 1.5 + w3 / g1) / size;  // texel i-1+w1/g0 and i+1+w3/g1, as coordinates
  return g0.y * (g0.x * texture(uLightmap, vec3(a.x, a.y, uvp.z)).rgb + g1.x * texture(uLightmap, vec3(b.x, a.y, uvp.z)).rgb)
       + g1.y * (g0.x * texture(uLightmap, vec3(a.x, b.y, uvp.z)).rgb + g1.x * texture(uLightmap, vec3(b.x, b.y, uvp.z)).rgb);
}
vec3 lightmapLight(vec3 uvp, vec3 mask) {
  vec3 sum = lightmapTexel(uvp) * uW[0];
  int m0 = int(mask.x + 0.5), m1 = int(mask.y + 0.5), m2 = int(mask.z + 0.5);
  if ((m0 | m1 | m2) == 0) return sum;
  for (int k = 1; k <= 63; k++) {
    int w = k <= 21 ? m0 : (k <= 42 ? m1 : m2);
    if (((w >> ((k - 1) % 21)) & 1) == 0) continue;
    float slot = uSlot[k];
    if (slot < 0.0) continue;
    sum += lightmapTexel(vec3(uvp.xy, uvp.z + slot * uPages)) * uW[k];
  }
  return sum;
}`
// Relief on lit faces (litTextured.ts displaceLit): the lightmap is the flat face's, so a relief slope takes the light of the face scaled by a brightness factor
// that travels in the length of the vertex normal (flat vertices carry a unit normal: factor 1; a relief triangle has one normal and one factor).
const SHADE_GLSL = `
float reliefShade(vec3 n) { float l = length(n); return l < 1e-3 ? 1.0 : min(l, 1.0); }`
const LIT_VS = `#version 300 es
${SHADE_GLSL}
uniform mat4 uViewProj; layout(location = 0) in vec3 aPos; layout(location = 1) in vec3 aLm; layout(location = 2) in vec3 aMask; layout(location = 4) in vec3 aNrm; out vec3 vLm; flat out vec3 vMask; flat out float vShade;
void main() { vLm = aLm; vMask = aMask; vShade = reliefShade(aNrm); gl_Position = uViewProj * vec4(aPos, 1.0); }`
const LIT_FS = `#version 300 es
precision highp float; precision highp sampler2DArray; uniform float uExposure; uniform float uBrightness; in vec3 vLm; flat in vec3 vMask; flat in float vShade; out vec4 o;
${SRGB_GLSL}
${LAYERS_GLSL}
void main() { o = vec4(linearToSrgb(lightmapLight(vLm, vMask) * vShade * uExposure + uBrightness), 1.0); }`
// 'Lit': albedo x light multiplied in linear light (the texture is sRGB, the light is linear HDR), encoded to sRGB at the end. Lightmapped surfaces take the
// light from the lightmap layers as stored; entities that are not lightmapped take, per style layer, ambient + directed * max(0, n . dir) from the Core's light
// volume (the x2 range decoded, nothing else applied), per vertex normal, like the engine's dynamic lighting does. No tone mapping, no brightness constants.
const LITTEX_VS = `#version 300 es
${SHADE_GLSL}
uniform mat4 uViewProj; layout(location = 0) in vec3 aPos; layout(location = 1) in vec3 aLm; layout(location = 2) in vec2 aUv; layout(location = 3) in vec3 aMask; layout(location = 4) in vec3 aNrm;
out vec3 vLm; out vec2 vUv; flat out vec3 vMask; flat out float vShade;
void main() { vLm = aLm; vUv = aUv; vMask = aMask; vShade = reliefShade(aNrm); gl_Position = uViewProj * vec4(aPos, 1.0); }`
const LITTEX_FS = `#version 300 es
precision highp float; precision highp sampler2DArray; uniform sampler2D uTex; uniform float uHasTex; uniform float uAlphaTex; uniform float uExposure; uniform float uBrightness; uniform vec3 uColor; uniform vec3 uTransp; in vec3 vLm; in vec2 vUv; flat in vec3 vMask; flat in float vShade; out vec4 o;
${SRGB_GLSL}
${LAYERS_GLSL}
${TRANSP_GLSL}
void main() {
  vec4 t = texture(uTex, vUv);
  vec3 albedo = srgbToLinear(mix(uColor, t.rgb, uHasTex));
  o = vec4(linearToSrgb(albedo * lightmapLight(vLm, vMask) * vShade * uExposure + uBrightness), mix(1.0, t.a, uAlphaTex) * transparency(t.a * uHasTex + (1.0 - uHasTex))); // decals blend by their texture's alpha
}`
const ENTLIT_FS = `#version 300 es
precision highp float; uniform vec3 uColor; uniform sampler2D uTex; uniform float uHasTex; uniform float uAlphaTex; uniform int uN; uniform vec3 uAmb[8]; uniform vec3 uDc[8]; uniform vec3 uDir; uniform float uExposure; uniform float uBrightness; uniform vec3 uTransp; in vec3 vN; in vec2 vUv; out vec4 o;
${SRGB_GLSL}
${TRANSP_GLSL}
void main() {
  vec4 t = texture(uTex, vUv);
  vec3 albedo = srgbToLinear(mix(uColor, t.rgb, uHasTex));
  vec3 n = normalize(vN), light = vec3(0.0);
  for (int i = 0; i < 8; i++) { if (i >= uN) break; light += uAmb[i] + uDc[i] * max(0.0, dot(n, uDir)); } // weights (style values) are already in uAmb / uDc; ONE direction for every layer (REQUIREMENTS §27 G4)
  o = vec4(linearToSrgb(albedo * light * uExposure + uBrightness), mix(1.0, t.a, uAlphaTex) * transparency(t.a * uHasTex + (1.0 - uHasTex)));
}`
// Sky faces (REQUIREMENTS §28 K10): the skybox sampled by the view direction, exactly as the engine's bsp/fs_bsp_cube does (model/sky.ts), no light,
// no fog. SKYBG draws the level's sky behind everything (a full-screen triangle at the far plane).
const SKY_VS = `#version 300 es
uniform mat4 uViewProj; uniform vec3 uEye; layout(location = 0) in vec3 aPos; out vec3 vDir;
void main() { vDir = aPos - uEye; gl_Position = uViewProj * vec4(aPos, 1.0); }`
const SKY_FS = `#version 300 es
precision highp float; uniform samplerCube uCube; uniform sampler2D uPano; uniform float uIsPano; in vec3 vDir; out vec4 o;
${SKY_GLSL}
void main() { o = vec4(skyColor(vDir, uCube, uPano, uIsPano), 1.0); }`
const SKYBG_VS = `#version 300 es
layout(location = 0) in vec2 aCorner; out vec2 vNdc;
void main() { vNdc = aCorner; gl_Position = vec4(aCorner, 1.0, 1.0); }`
const SKYBG_FS = `#version 300 es
precision highp float; uniform mat4 uInvViewProj; uniform vec3 uEye; uniform samplerCube uCube; uniform sampler2D uPano; uniform float uIsPano; in vec2 vNdc; out vec4 o;
${SKY_GLSL}
void main() { vec4 p = uInvViewProj * vec4(vNdc, 1.0, 1.0); o = vec4(skyColor(p.xyz / p.w - uEye, uCube, uPano, uIsPano), 1.0); }`
const LINE_VS = `#version 300 es
uniform mat4 uViewProj; in vec3 aPos;
void main() { gl_Position = uViewProj * vec4(aPos, 1.0); }`
const LINE_FS = `#version 300 es
precision mediump float; uniform vec4 uColor; out vec4 o;
void main() { o = uColor; }`

// Light volume sample preview: per-vertex colour (the probe's light for that normal), smoothly interpolated over the sphere. The normal is
// re-normalised per pixel for a gentle fixed key-light cue so a probe with no directed light still reads as a sphere; lines have a zero normal.
const PROBE_VS = `#version 300 es
uniform mat4 uViewProj; in vec3 aPos; in vec3 aCol; in vec3 aNrm; out vec3 vCol; out vec3 vNrm;
void main() { vCol = aCol; vNrm = aNrm; gl_Position = uViewProj * vec4(aPos, 1.0); }`
const PROBE_FS = `#version 300 es
precision mediump float; in vec3 vCol; in vec3 vNrm; out vec4 o;
void main() {
  float l = dot(vNrm, vNrm);
  float shade = l > 0.0 ? 0.7 + 0.3 * dot(normalize(vNrm), normalize(vec3(0.35, 0.8, 0.5))) : 1.0;
  o = vec4(vCol * shade, 1.0);
}`

// Lightmap density overlay: the ramp equals lightmapDensity.ts densityColor; the checker is one cell per lightmap texel.
const DENS_VS = `#version 300 es
uniform mat4 uViewProj; in vec3 aPos; in vec3 aData; out vec3 vData;
void main() { vData = aData; gl_Position = uViewProj * vec4(aPos, 1.0); }`
const DENS_FS = `#version 300 es
precision highp float; in vec3 vData; out vec4 o;
vec3 ramp(float d) {
  if (!(d > 0.0)) return vec3(0.5);
  float t = clamp(log2(d / ${DENSITY_LOW.toFixed(1)}) / log2(${(DENSITY_HIGH / DENSITY_LOW).toFixed(1)}), 0.0, 1.0) * 4.0;
  vec3 s0 = vec3(0.15, 0.25, 0.95), s1 = vec3(0.1, 0.8, 0.9), s2 = vec3(0.2, 0.85, 0.25), s3 = vec3(0.95, 0.85, 0.15), s4 = vec3(0.95, 0.2, 0.15);
  if (t < 1.0) return mix(s0, s1, t);
  if (t < 2.0) return mix(s1, s2, t - 1.0);
  if (t < 3.0) return mix(s2, s3, t - 2.0);
  return mix(s3, s4, t - 3.0);
}
void main() {
  vec2 c = floor(vData.xy);
  float k = mod(c.x + c.y, 2.0);
  o = vec4(ramp(vData.z) * mix(0.62, 1.0, k), 1.0);
}`

// Class-name labels: a screen-aligned quad anchored at a world position. Every corner shares the anchor's
// depth, so the whole label is depth-tested as one point and walls in front of the entity hide it.
const LABEL_VS = `#version 300 es
uniform mat4 uViewProj; uniform vec3 uAnchor; uniform vec2 uSize; in vec2 aCorner; out vec2 vUv;
void main() {
  vec4 c = uViewProj * vec4(uAnchor, 1.0);
  c.xy += aCorner * uSize * c.w; // uSize is in clip units (2 * pixels / viewport), so it stays the same size on screen
  vUv = aCorner * 0.5 + 0.5; vUv.y = 1.0 - vUv.y;
  gl_Position = c;
}`
const LABEL_FS = `#version 300 es
precision mediump float; uniform sampler2D uTex; in vec2 vUv; out vec4 o;
void main() { o = texture(uTex, vUv); }`

interface Group { texture: string; first: number; count: number }
interface GpuBrush { vao: WebGLVertexArrayObject; buf: WebGLBuffer; count: number; groups: Group[]; evao: WebGLVertexArrayObject; ebuf: WebGLBuffer; ecount: number; /** world box of the vertices: min xyz, max xyz (the draw skips what is outside the view) */ box: [number, number, number, number, number, number]; /** texture environment revision / signature the UVs were built for */ rev?: number; sig?: string }

/** The six planes (a b c d, normal pointing in) of the view frustum of a column-major view-projection matrix. */
export function frustumPlanes(m: ArrayLike<number>): Float32Array {
  const out = new Float32Array(24)
  const row = (i: number, j: number) => m[j * 4 + i]
  const set = (k: number, sign: number, r: number) => { for (let j = 0; j < 4; j++) out[k * 4 + j] = row(3, j) + sign * row(r, j) }
  set(0, 1, 0); set(1, -1, 0); set(2, 1, 1); set(3, -1, 1); set(4, 1, 2); set(5, -1, 2)
  return out
}

/** False when the box lies completely outside one of the planes. */
export function boxInFrustum(planes: Float32Array, b: ArrayLike<number>): boolean {
  for (let k = 0; k < 6; k++) {
    const a = planes[k * 4], bb = planes[k * 4 + 1], c = planes[k * 4 + 2], d = planes[k * 4 + 3]
    if (a * (a >= 0 ? b[3] : b[0]) + bb * (bb >= 0 ? b[4] : b[1]) + c * (c >= 0 ? b[5] : b[2]) + d < 0) return false
  }
  return true
}

/** Stable colour per texture path so different surfaces are distinguishable without loading images. */
export function textureColor(texture: string | undefined): [number, number, number] {
  let h = 2166136261
  for (const ch of texture ?? 'dev/grey') h = Math.imul(h ^ ch.charCodeAt(0), 16777619)
  const hue = ((h >>> 0) % 360) / 360
  const f = (n: number) => { const k = (n + hue * 12) % 12; return 0.62 - 0.22 * Math.max(-1, Math.min(k - 3, 9 - k, 1)) }
  return [f(0), f(8), f(4)]
}

export class Renderer {
  private gl: WebGL2RenderingContext
  private mesh: WebGLProgram
  private line: WebGLProgram
  private probeProg: WebGLProgram
  private densProg: WebGLProgram
  private densVao: WebGLVertexArrayObject
  private densBuf: WebGLBuffer
  private densMeshSerial = -1
  private densCount = 0
  private probeVao: WebGLVertexArrayObject
  private probeBuf: WebGLBuffer
  private probeLineBuf: WebGLBuffer
  private probeCache: { serial: number; exposure: number; center?: [number, number, number]; preview: ProbePreview } | null = null
  private cache = new WeakMap<Brush, GpuBrush>()
  private carvedCache = new WeakMap<CarvedMesh, GpuBrush>()
  private dyn: WebGLBuffer
  private dynVao: WebGLVertexArrayObject
  private grid: { minor: Float32Array; major: Float32Array }

  private lit: WebGLProgram
  private litTexProg: WebGLProgram
  private entLit: WebGLProgram
  private litTexVao: WebGLVertexArrayObject | null = null
  private litTexVbo: WebGLBuffer | null = null
  private litTexDraw: LitDraw | null = null
  // decals of the Lit view: on lightmapped faces (lit mesh layout) and on faces of entities lit from the light volume
  private decalLit: { entries: DecalEntry[]; faceMaps: unknown; draw: LitDraw; vao: WebGLVertexArrayObject; vbo: WebGLBuffer } | null = null
  private decalEnt: { entries: DecalEntry[]; faceMaps: unknown; groups: EntityDecalGroup[]; vao: WebGLVertexArrayObject; vbo: WebGLBuffer } | null = null
  private litVao: WebGLVertexArrayObject | null = null
  private litVbo: WebGLBuffer | null = null
  private litCount = 0
  private litPlain: Float32Array | null = null
  private litSerial = -1
  private litMeshSerial = -1
  private litCursor = 0
  private lmTex: WebGLTexture | null = null
  private lmStyles: number[] = []   // style of every slot of the texture array (slot 0 = base)
  private lmPages = 0
  private styleW = new Float32Array(64)
  private styleSlot = new Float32Array(64)
  private glTex = new Map<string, WebGLTexture>()
  private label: WebGLProgram
  private labelVao: WebGLVertexArrayObject
  private skyProg: WebGLProgram
  private skyBgProg: WebGLProgram
  private skyBgVao: WebGLVertexArrayObject
  /** GL textures of the skyboxes: a cube map (faces) or a 2D panorama */
  private skyTex = new Map<string, { tex: WebGLTexture; pano: boolean }>()
  private labelTex = new Map<string, { tex: WebGLTexture; w: number; h: number }>()

  constructor(private canvas: HTMLCanvasElement, private textures: TextureCache) {
    const gl = canvas.getContext('webgl2', { antialias: true })
    if (!gl) throw new Error('WebGL2 is not available in this browser')
    this.gl = gl
    this.mesh = this.program(MESH_VS, MESH_FS)
    this.line = this.program(LINE_VS, LINE_FS)
    this.probeProg = this.program(PROBE_VS, PROBE_FS)
    this.densProg = this.program(DENS_VS, DENS_FS)
    this.densVao = gl.createVertexArray()!
    this.densBuf = gl.createBuffer()!
    this.probeVao = gl.createVertexArray()!
    this.probeBuf = gl.createBuffer()!
    this.probeLineBuf = gl.createBuffer()!
    this.lit = this.program(LIT_VS, LIT_FS)
    this.litTexProg = this.program(LITTEX_VS, LITTEX_FS)
    this.entLit = this.program(MESH_VS, ENTLIT_FS)
    this.label = this.program(LABEL_VS, LABEL_FS)
    this.skyProg = this.program(SKY_VS, SKY_FS)
    this.skyBgProg = this.program(SKYBG_VS, SKYBG_FS)
    this.skyBgVao = gl.createVertexArray()!
    gl.bindVertexArray(this.skyBgVao)
    gl.bindBuffer(gl.ARRAY_BUFFER, gl.createBuffer()!)
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 3, -1, -1, 3]), gl.STATIC_DRAW)
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0)
    gl.bindVertexArray(null)
    this.dyn = gl.createBuffer()!
    this.dynVao = gl.createVertexArray()!
    gl.bindVertexArray(this.dynVao)
    gl.bindBuffer(gl.ARRAY_BUFFER, this.dyn)
    const loc = gl.getAttribLocation(this.line, 'aPos')
    gl.enableVertexAttribArray(loc)
    gl.vertexAttribPointer(loc, 3, gl.FLOAT, false, 0, 0)
    gl.bindVertexArray(null)
    this.labelVao = gl.createVertexArray()!
    gl.bindVertexArray(this.labelVao)
    gl.bindBuffer(gl.ARRAY_BUFFER, gl.createBuffer()!)
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW)
    const cl = gl.getAttribLocation(this.label, 'aCorner')
    gl.enableVertexAttribArray(cl); gl.vertexAttribPointer(cl, 2, gl.FLOAT, false, 0, 0)
    gl.bindVertexArray(null)
    this.grid = buildGrid(50)
  }

  private program(vs: string, fs: string): WebGLProgram {
    const gl = this.gl
    const compile = (type: number, src: string) => {
      const s = gl.createShader(type)!
      gl.shaderSource(s, src); gl.compileShader(s)
      if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s) ?? 'shader compile failed')
      return s
    }
    const p = gl.createProgram()!
    gl.attachShader(p, compile(gl.VERTEX_SHADER, vs)); gl.attachShader(p, compile(gl.FRAGMENT_SHADER, fs))
    gl.linkProgram(p)
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p) ?? 'program link failed')
    return p
  }

  private upload(b: Brush): GpuBrush {
    let g = this.cache.get(b)
    if (g && g.rev === texEnv.rev) return g
    // a texture changed (loaded, resized, properties edited): rebuild only when one of THIS brush's textures is affected
    const sig = brushTextureSignature(b)
    if (g && g.sig === sig) { g.rev = texEnv.rev; return g }
    if (g) this.freeGpu(g)
    // one vertex run per texture, so a brush with several textures is a few draw calls
    const byTex = new Map<string, number[]>()
    const edges: number[] = []
    const budget = newBudget()
    for (const f of b.faces) {
      const n = faceNormal(b, f)
      const tris = triangulateFace(b, f)
      const plan = planForFace(b, f) // repetition behaviour: the triangles are cut along the fold lines, the cut points are invisible vertices
      const relief = reliefOf(f.texture) // texture relief: repeated over the face, pushed along its normal (model/reliefApply.ts)
      if (plan) {
        const pts = tris.map(([a, c, d]) => [b.vertices[a], b.vertices[c], b.vertices[d]] as [Vec3, Vec3, Vec3])
        if (planHasRelief(plan)) planWithRelief(plan, pts, n, byTex, budget) // every piece takes the relief of the texture it samples, placed by the piece's own uv
        else for (const o of retexture(plan, pts)) {
          const list = byTex.get(o.texture) ?? []
          byTex.set(o.texture, list)
          for (const v of o.verts) list.push(...lerpVec3(pts[o.src], v.w), ...n, v.uv[0], v.uv[1])
        }
      } else if (relief) {
        const uv = uvsForDraw(b, f)
        const list = byTex.get(f.texture ?? 'dev/grey') ?? []
        byTex.set(f.texture ?? 'dev/grey', list)
        for (const [a, c, d] of tris) {
          const k = [a, c, d].map((i) => f.loop.indexOf(i))
          const got = reliefTriangle([b.vertices[a], b.vertices[c], b.vertices[d]], [uv[k[0]], uv[k[1]], uv[k[2]]], n, relief, budget)
          if (got) { for (const x of got) list.push(x); continue }
          for (const i of [a, c, d]) { const q = f.loop.indexOf(i); list.push(...b.vertices[i], ...n, uv[q][0], uv[q][1]) }
        }
      } else {
        const uv = uvsForDraw(b, f)
        const list = byTex.get(f.texture ?? 'dev/grey') ?? []
        byTex.set(f.texture ?? 'dev/grey', list)
        for (const [a, c, d] of tris) for (const i of [a, c, d]) {
          const k = f.loop.indexOf(i)
          list.push(...b.vertices[i], ...n, uv[k][0], uv[k][1])
        }
      }
      for (let i = 0; i < f.loop.length; i++) edges.push(...b.vertices[f.loop[i]], ...b.vertices[f.loop[(i + 1) % f.loop.length]])
    }
    g = this.gpu(byTex, edges)
    g.rev = texEnv.rev; g.sig = sig
    this.cache.set(b, g)
    return g
  }

  private freeGpu(g: GpuBrush): void {
    const gl = this.gl
    gl.deleteVertexArray(g.vao); gl.deleteBuffer(g.buf); gl.deleteVertexArray(g.evao); gl.deleteBuffer(g.ebuf)
  }

  private uploadCarved(m: CarvedMesh): GpuBrush {
    let g = this.carvedCache.get(m)
    if (!g) { g = this.gpu(m.textures, m.edges); this.carvedCache.set(m, g) }
    return g
  }

  private gpu(byTex: Map<string, number[]>, edges: number[]): GpuBrush {
    const gl = this.gl
    const tri: number[] = [], groups: Group[] = []
    // appended in a loop: `tri.push(...verts)` passes every number as an argument and throws (call stack) beyond ~120k of them, i.e. ~15k vertices of one texture: the level then stopped drawing
    for (const [texture, verts] of byTex) { groups.push({ texture, first: tri.length / 8, count: verts.length / 8 }); for (let i = 0; i < verts.length; i++) tri.push(verts[i]) }
    const mkMesh = () => {
      const vao = gl.createVertexArray()!, buf = gl.createBuffer()!
      gl.bindVertexArray(vao); gl.bindBuffer(gl.ARRAY_BUFFER, buf)
      gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(tri), gl.STATIC_DRAW)
      const attr = (name: string, size: number, offset: number) => {
        const l = gl.getAttribLocation(this.mesh, name)
        gl.enableVertexAttribArray(l); gl.vertexAttribPointer(l, size, gl.FLOAT, false, 32, offset)
      }
      attr('aPos', 3, 0); attr('aNormal', 3, 12); attr('aUv', 2, 24)
      gl.bindVertexArray(null)
      return { vao, buf }
    }
    const mkEdges = () => {
      const vao = gl.createVertexArray()!, buf = gl.createBuffer()!
      gl.bindVertexArray(vao); gl.bindBuffer(gl.ARRAY_BUFFER, buf)
      gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(edges), gl.STATIC_DRAW)
      const p = gl.getAttribLocation(this.line, 'aPos')
      gl.enableVertexAttribArray(p); gl.vertexAttribPointer(p, 3, gl.FLOAT, false, 12, 0)
      gl.bindVertexArray(null)
      return { vao, buf }
    }
    const m = mkMesh(), e = mkEdges()
    const box: [number, number, number, number, number, number] = [Infinity, Infinity, Infinity, -Infinity, -Infinity, -Infinity]
    for (let i = 0; i + 2 < tri.length; i += 8) for (let k = 0; k < 3; k++) { box[k] = Math.min(box[k], tri[i + k]); box[k + 3] = Math.max(box[k + 3], tri[i + k]) }
    return { vao: m.vao, buf: m.buf, count: tri.length / 8, groups, evao: e.vao, ebuf: e.buf, ecount: edges.length / 3, box }
  }

  /** The GL texture for a path if it has finished loading; otherwise null (flat colour is used meanwhile). */
  private texture(path: string): WebGLTexture | null {
    const have = this.glTex.get(path)
    if (have) return have
    const st = this.textures.get(path) // starts loading on first ask
    if (st.status !== 'ready') return null
    const gl = this.gl, t = gl.createTexture()!
    gl.bindTexture(gl.TEXTURE_2D, t)
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false)
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, st.image.source as TexImageSource)
    gl.generateMipmap(gl.TEXTURE_2D)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR_MIPMAP_LINEAR)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.REPEAT)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.REPEAT)
    this.glTex.set(path, t)
    return t
  }

  /**
   * The GL texture of a skybox once its images arrived: the six faces as a cube map, each put where the engine puts it (model/sky.ts SKY_FACES: the
   * file order, _up turned clockwise, _dn counter-clockwise, all scaled to the first face's size), or the panorama as a 2D texture. No mip maps, linear
   * filtering, like the engine's CubemapTexture.
   */
  private skyTexture(path: string): { tex: WebGLTexture; pano: boolean } | null {
    const have = this.skyTex.get(path)
    if (have) return have
    const sky = this.textures.sky(path)
    if (!sky) return null
    const gl = this.gl, tex = gl.createTexture()!
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false)
    if (sky.panorama) {
      gl.bindTexture(gl.TEXTURE_2D, tex)
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, sky.faces[0].source as TexImageSource)
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR)
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.REPEAT); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE)
    } else {
      const size = Math.max(1, sky.faces[0].width)
      gl.bindTexture(gl.TEXTURE_CUBE_MAP, tex)
      SKY_FACES.forEach((f, i) => {
        const img = sky.faces[i]
        if (!img) return
        const c = Object.assign(document.createElement('canvas'), { width: size, height: size })
        const g = c.getContext('2d')
        if (!g) return
        g.translate(size / 2, size / 2)
        g.rotate((f.turn * Math.PI) / 2)
        g.drawImage(img.source as CanvasImageSource, -size / 2, -size / 2, size, size)
        gl.texImage2D(gl.TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, c)
      })
      gl.texParameteri(gl.TEXTURE_CUBE_MAP, gl.TEXTURE_MIN_FILTER, gl.LINEAR); gl.texParameteri(gl.TEXTURE_CUBE_MAP, gl.TEXTURE_MAG_FILTER, gl.LINEAR)
      gl.texParameteri(gl.TEXTURE_CUBE_MAP, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE); gl.texParameteri(gl.TEXTURE_CUBE_MAP, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE)
    }
    const out = { tex, pano: sky.panorama }
    this.skyTex.set(path, out)
    return out
  }

  /** Binds a skybox for the sky programs (cube on unit 2, panorama on unit 3); false while it is not loaded. */
  private bindSky(P: WebGLProgram, path: string): boolean {
    const gl = this.gl, t = this.skyTexture(path)
    if (!t) return false
    gl.uniform1i(gl.getUniformLocation(P, 'uCube'), 2)
    gl.uniform1i(gl.getUniformLocation(P, 'uPano'), 3)
    gl.uniform1f(gl.getUniformLocation(P, 'uIsPano'), t.pano ? 1 : 0)
    gl.activeTexture(gl.TEXTURE2); gl.bindTexture(gl.TEXTURE_CUBE_MAP, t.pano ? null : t.tex)
    gl.activeTexture(gl.TEXTURE3); gl.bindTexture(gl.TEXTURE_2D, t.pano ? t.tex : null)
    gl.activeTexture(gl.TEXTURE0)
    return true
  }

  private tris(vp: Float32Array, data: number[], color: [number, number, number, number]): void {
    const gl = this.gl
    gl.useProgram(this.line)
    gl.uniformMatrix4fv(gl.getUniformLocation(this.line, 'uViewProj'), false, vp)
    gl.uniform4f(gl.getUniformLocation(this.line, 'uColor'), ...color)
    gl.bindVertexArray(this.dynVao); gl.bindBuffer(gl.ARRAY_BUFFER, this.dyn)
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(data), gl.DYNAMIC_DRAW)
    gl.drawArrays(gl.TRIANGLES, 0, data.length / 3)
    gl.bindVertexArray(null)
  }

  private lines(vp: Float32Array, data: Float32Array | number[], color: [number, number, number, number]): void {
    if (data.length === 0) return
    const gl = this.gl
    gl.useProgram(this.line)
    gl.uniformMatrix4fv(gl.getUniformLocation(this.line, 'uViewProj'), false, vp)
    gl.uniform4f(gl.getUniformLocation(this.line, 'uColor'), ...color)
    gl.bindVertexArray(this.dynVao); gl.bindBuffer(gl.ARRAY_BUFFER, this.dyn)
    gl.bufferData(gl.ARRAY_BUFFER, data instanceof Float32Array ? data : new Float32Array(data), gl.DYNAMIC_DRAW)
    gl.drawArrays(gl.LINES, 0, data.length / 3)
    gl.bindVertexArray(null)
  }

  private heatCache = new WeakMap<Brush, { rev: number; buckets: number[][] }>()
  /** Texel density heat map (View > Texel density heat map): every face tinted by authored vs actual texture pixels per meter (model/texelDensity.ts). */
  private drawTexelHeat(vp: Float32Array, frame: Frame): void {
    const gl = this.gl
    const all: number[][] = Array.from({ length: HEAT_BUCKETS }, () => [])
    for (const de of frame.entities) for (const b of de.entity.brushes ?? []) {
      if (b.op === 'subtract') continue
      let c = this.heatCache.get(b)
      if (!c || c.rev !== texEnv.rev) {
        const buckets: number[][] = Array.from({ length: HEAT_BUCKETS }, () => [])
        for (const f of b.faces) {
          const e = faceHeat(b, f)
          if (e === null) continue
          const list = buckets[heatBucket(e)]
          for (const [a, cc, d] of triangulateFace(b, f)) list.push(...b.vertices[a], ...b.vertices[cc], ...b.vertices[d])
        }
        c = { rev: texEnv.rev, buckets }
        this.heatCache.set(b, c)
      }
      c.buckets.forEach((l, i) => { for (let k = 0; k < l.length; k++) all[i].push(l[k]) })
    }
    gl.enable(gl.POLYGON_OFFSET_FILL); gl.polygonOffset(-1, -1)
    all.forEach((data, i) => { if (data.length) { const col = bucketColor(i); this.tris(vp, data, [col[0], col[1], col[2], 0.7]) } })
    gl.disable(gl.POLYGON_OFFSET_FILL)
  }

  /** Lightmap density (View > Lightmap density): the Core's lit mesh as a texel checker coloured by texels per metre, over the scene. */
  private drawLightmapDensity(vp: Float32Array, frame: Frame): void {
    const store = frame.lighting
    if (!store?.ready) return
    const gl = this.gl
    const d = densityOf(store)
    if (!d) return
    if (this.densMeshSerial !== store.meshSerial) {
      this.densMeshSerial = store.meshSerial
      this.densCount = d.data.length / DENSITY_STRIDE
      gl.bindBuffer(gl.ARRAY_BUFFER, this.densBuf); gl.bufferData(gl.ARRAY_BUFFER, d.data, gl.STATIC_DRAW)
    }
    if (this.densCount === 0) return
    gl.useProgram(this.densProg)
    gl.uniformMatrix4fv(gl.getUniformLocation(this.densProg, 'uViewProj'), false, vp)
    const aPos = gl.getAttribLocation(this.densProg, 'aPos'), aData = gl.getAttribLocation(this.densProg, 'aData')
    gl.bindVertexArray(this.densVao)
    gl.bindBuffer(gl.ARRAY_BUFFER, this.densBuf)
    gl.enableVertexAttribArray(aPos); gl.vertexAttribPointer(aPos, 3, gl.FLOAT, false, DENSITY_STRIDE * 4, 0)
    gl.enableVertexAttribArray(aData); gl.vertexAttribPointer(aData, 3, gl.FLOAT, false, DENSITY_STRIDE * 4, 12)
    gl.enable(gl.POLYGON_OFFSET_FILL); gl.polygonOffset(-1, -1)  // on top of the surface it describes
    gl.drawArrays(gl.TRIANGLES, 0, this.densCount)
    gl.disable(gl.POLYGON_OFFSET_FILL)
    gl.bindVertexArray(null)
  }

  /** Light volume samples (View > Lightvol samples): a smooth-shaded sphere per baked probe coloured by its light, and a line towards its directed light. */
  private drawLightvolSamples(vp: Float32Array, frame: Frame): void {
    const store = frame.lightvol
    if (!store?.ready) return
    const gl = this.gl
    const exposure = frame.lighting?.exposure ?? 1
    // a grid with more probes than the preview draws shows the ones around the view's target, rebuilt when the target has moved a few cells away
    const local = store.probeCount > PROBE_PREVIEW_MAX
    const target = frame.camera.target
    const moved = local && this.probeCache?.center ? Math.hypot(target[0] - this.probeCache.center[0], target[1] - this.probeCache.center[1], target[2] - this.probeCache.center[2]) > store.cell * 5 : false
    if (!this.probeCache || this.probeCache.serial !== store.serial || this.probeCache.exposure !== exposure || (local && (!this.probeCache.center || moved))) {
      const center = local ? ([target[0], target[1], target[2]] as [number, number, number]) : undefined
      this.probeCache = { serial: store.serial, exposure, center, preview: buildProbePreview(store, exposure, PROBE_PREVIEW_MAX, center) }
      gl.bindBuffer(gl.ARRAY_BUFFER, this.probeBuf); gl.bufferData(gl.ARRAY_BUFFER, this.probeCache.preview.tris, gl.STATIC_DRAW)
      gl.bindBuffer(gl.ARRAY_BUFFER, this.probeLineBuf); gl.bufferData(gl.ARRAY_BUFFER, this.probeCache.preview.lines, gl.STATIC_DRAW)
    }
    const { tris, lines } = this.probeCache.preview
    gl.useProgram(this.probeProg)
    gl.uniformMatrix4fv(gl.getUniformLocation(this.probeProg, 'uViewProj'), false, vp)
    const aPos = gl.getAttribLocation(this.probeProg, 'aPos'), aCol = gl.getAttribLocation(this.probeProg, 'aCol'), aNrm = gl.getAttribLocation(this.probeProg, 'aNrm')
    gl.bindVertexArray(this.probeVao)
    const bind = (buf: WebGLBuffer) => {
      gl.bindBuffer(gl.ARRAY_BUFFER, buf)
      gl.enableVertexAttribArray(aPos); gl.vertexAttribPointer(aPos, 3, gl.FLOAT, false, PREVIEW_STRIDE * 4, 0)
      gl.enableVertexAttribArray(aCol); gl.vertexAttribPointer(aCol, 3, gl.FLOAT, false, PREVIEW_STRIDE * 4, 12)
      gl.enableVertexAttribArray(aNrm); gl.vertexAttribPointer(aNrm, 3, gl.FLOAT, false, PREVIEW_STRIDE * 4, 24)
    }
    if (tris.length) { bind(this.probeBuf); gl.drawArrays(gl.TRIANGLES, 0, tris.length / PREVIEW_STRIDE) }
    if (lines.length) { bind(this.probeLineBuf); gl.drawArrays(gl.LINES, 0, lines.length / PREVIEW_STRIDE) }
    gl.bindVertexArray(null)
  }

  /** Mesh edit overlay: every edge and vertex of the brush (x-ray), selected ones in orange, selected faces tinted. */
  private drawMesh(vp: Float32Array, frame: Frame): void {
    const gl = this.gl, { brush, mode, sel } = frame.mesh!
    const selV = new Set(sel.verts), selE = new Set(sel.edges), selF = new Set(sel.faces)
    const plain: number[] = [], hot: number[] = [], tri: number[] = []
    const t = buildTopology(brush)
    for (const [k, e] of t.edges) {
      const seg = [...brush.vertices[e.a], ...brush.vertices[e.b]]
      if (mode !== 'vertex' && selE.has(k)) hot.push(...seg)
      else plain.push(...seg)
    }
    for (const f of brush.faces) if (selF.has(f.id)) for (const [a, c, d] of triangulateFace(brush, f)) tri.push(...brush.vertices[a], ...brush.vertices[c], ...brush.vertices[d])
    const dots: number[] = [], hotDots: number[] = []
    if (mode === 'vertex') brush.vertices.forEach((p, i) => (selV.has(i) ? hotDots : dots).push(...crossLines(p, frame.camera.distanceTo(p) * (selV.has(i) ? 0.014 : 0.009))))
    gl.disable(gl.DEPTH_TEST)
    if (tri.length) this.tris(vp, tri, [1, 0.6, 0.15, 0.3])
    this.lines(vp, plain, mode === 'vertex' ? [0.45, 0.75, 1, 0.75] : [0.45, 0.75, 1, 0.9])
    this.lines(vp, hot, [1, 0.62, 0.15, 1])
    this.lines(vp, dots, [0.8, 0.95, 1, 1])
    this.lines(vp, hotDots, [1, 0.62, 0.15, 1])
    gl.enable(gl.DEPTH_TEST)
    void parseEdge
  }

  /** The text of a label as a texture (white with a dark outline so it reads on any background), cached per text and pixel ratio. */
  private labelTexture(text: string, dpr: number): { tex: WebGLTexture; w: number; h: number } {
    const key = `${dpr}|${text}`
    const have = this.labelTex.get(key)
    if (have) return have
    const gl = this.gl, font = `600 ${Math.round(12 * dpr)}px sans-serif`
    const cv = document.createElement('canvas'), ctx = cv.getContext('2d')!
    ctx.font = font
    const pad = Math.ceil(3 * dpr), w = Math.ceil(ctx.measureText(text).width) + pad * 2, h = Math.ceil(12 * dpr * 1.4) + pad * 2
    cv.width = w; cv.height = h
    ctx.font = font; ctx.textAlign = 'center'; ctx.textBaseline = 'middle'
    ctx.lineJoin = 'round'; ctx.lineWidth = Math.max(2, 3 * dpr); ctx.strokeStyle = 'rgba(0,0,0,0.85)'
    ctx.strokeText(text, w / 2, h / 2); ctx.fillStyle = '#fff'; ctx.fillText(text, w / 2, h / 2)
    const tex = gl.createTexture()!
    gl.bindTexture(gl.TEXTURE_2D, tex)
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false)
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, cv)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE)
    const l = { tex, w, h }
    this.labelTex.set(key, l)
    return l
  }

  /** Class names written on point-entity boxes. Depth-tested at the box centre, so anything solid in front hides the text. */
  private drawLabels(vp: Float32Array, labels: { text: string; at: Vec3 }[], viewW: number, viewH: number, dpr: number): void {
    if (!labels.length) return
    const gl = this.gl
    gl.useProgram(this.label)
    gl.uniformMatrix4fv(gl.getUniformLocation(this.label, 'uViewProj'), false, vp)
    gl.uniform1i(gl.getUniformLocation(this.label, 'uTex'), 0)
    gl.activeTexture(gl.TEXTURE0)
    gl.bindVertexArray(this.labelVao)
    const uAnchor = gl.getUniformLocation(this.label, 'uAnchor'), uSize = gl.getUniformLocation(this.label, 'uSize')
    for (const { text, at } of labels) {
      const t = this.labelTexture(text, dpr)
      gl.bindTexture(gl.TEXTURE_2D, t.tex)
      gl.uniform3f(uAnchor, at[0], at[1], at[2])
      gl.uniform2f(uSize, t.w / viewW, t.h / viewH) // half-extent in clip units = (pixels / 2) * 2 / viewport
      gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4)
    }
    gl.bindVertexArray(null)
  }

  /** The thing being dragged in, half transparent, exactly where it would land. Content keeps its own coordinates; the offset goes into the matrix so cached GPU buffers are reused every frame. */
  private drawGhost(vp: Float32Array, frame: Frame, ghost: NonNullable<Frame['ghost']>): void {
    const gl = this.gl, m = translatedMatrix(vp, ghost.offset)
    gl.enable(gl.BLEND); gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA)
    gl.disable(gl.POLYGON_OFFSET_FILL)
    gl.depthMask(false)
    if (frame.settings.backfaceCulling) { gl.enable(gl.CULL_FACE); gl.cullFace(gl.BACK) }
    gl.useProgram(this.mesh)
    gl.uniformMatrix4fv(gl.getUniformLocation(this.mesh, 'uViewProj'), false, m)
    gl.uniform1i(gl.getUniformLocation(this.mesh, 'uTex'), 0)
    gl.uniform1f(gl.getUniformLocation(this.mesh, 'uAlphaTex'), 0)
    gl.activeTexture(gl.TEXTURE0)
    const uColor = gl.getUniformLocation(this.mesh, 'uColor'), uHasTex = gl.getUniformLocation(this.mesh, 'uHasTex')
    for (const entity of ghost.entities)
      for (const b of entity.brushes ?? []) {
        if (b.op === 'subtract') continue
        const g = this.upload(b)
        gl.bindVertexArray(g.vao)
        for (const grp of g.groups) {
          const [r, gg, bl] = textureColor(grp.texture)
          const tex = this.texture(grp.texture)
          if (tex) gl.bindTexture(gl.TEXTURE_2D, tex)
          gl.uniform1f(uHasTex, tex ? 1 : 0)
          gl.uniform4f(uColor, r, gg, bl, GHOST_ALPHA)
          gl.drawArrays(gl.TRIANGLES, grp.first, grp.count)
        }
      }
    gl.bindVertexArray(null)
    gl.disable(gl.CULL_FACE) // box fills are two-sided
    for (const entity of ghost.entities) {
      if (isBrushEntity(entity) || classOf(entity) === 'worldspawn') continue
      const c = frame.entityColor?.(classOf(entity)) ?? DEFAULT_MARKER
      const corners = worldCorners(pointBox(entity, (n) => boxOf(frame, n)))  // turned with the entity's angles
      this.tris(m, cornerTriangles(corners), [c[0], c[1], c[2], GHOST_ALPHA * 0.7])
      this.lines(m, cornerLines(corners), [c[0], c[1], c[2], 0.95])
    }
    gl.depthMask(true)
  }

  /** Brings the GPU copy of the lit mesh and the lightmap pages up to date with the store (rebuilds after a new mesh, else only changed tiles). */
  private syncLighting(l: LightmapStore): void {
    const gl = this.gl
    if (l.serial !== this.litSerial) {
      this.litSerial = l.serial
      this.litMeshSerial = l.meshSerial
      if (this.lmTex) { gl.deleteTexture(this.lmTex); this.lmTex = null }
      if (!l.ready || !l.mesh) { this.litCount = 0; return }
      if (!this.litVao) {
        this.litVao = gl.createVertexArray()!; this.litVbo = gl.createBuffer()!
        gl.bindVertexArray(this.litVao); gl.bindBuffer(gl.ARRAY_BUFFER, this.litVbo)
        const stride = LIT_VERTEX_FLOATS * 4
        gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 3, gl.FLOAT, false, stride, 0)
        gl.enableVertexAttribArray(1); gl.vertexAttribPointer(1, 3, gl.FLOAT, false, stride, 6 * 4)
        gl.enableVertexAttribArray(2); gl.vertexAttribPointer(2, 3, gl.FLOAT, false, stride, 9 * 4)
        gl.enableVertexAttribArray(4); gl.vertexAttribPointer(4, 3, gl.FLOAT, false, stride, 3 * 4) // normal: its length is the relief brightness factor
      }
      this.litPlain = null // the vertex buffer is filled by syncLitPlain
      // one texture array: slot * pageCount + page. Every style layer that has pixels gets a slot (the API guarantees at least 256 slices).
      const maxSlots = Math.max(1, Math.floor(gl.getParameter(gl.MAX_ARRAY_TEXTURE_LAYERS) / Math.max(1, l.pageCount)))
      this.lmStyles = l.styles.slice(0, maxSlots)
      this.lmPages = l.pageCount
      this.lmTex = gl.createTexture()!
      gl.bindTexture(gl.TEXTURE_2D_ARRAY, this.lmTex)
      gl.texStorage3D(gl.TEXTURE_2D_ARRAY, 1, gl.RGBA16F, l.atlas, l.atlas, l.pageCount * this.lmStyles.length) // linear HDR as baked; RGBA16F is filterable in core WebGL2
      gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MIN_FILTER, gl.LINEAR); gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MAG_FILTER, gl.LINEAR)
      gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE); gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE)
      // A fresh texture is empty: upload every page of every layer. (dirtySince(0) reports `full` with no rectangles once the log was reset by a new
      // mesh, so using its rectangles here left every tile black that did not refine afterwards.)
      this.uploadRects(l, l.fullRects())
      this.litCursor = l.dirtySince(Number.MAX_SAFE_INTEGER).cursor
      return
    }
    if (!this.lmTex) return
    if (l.meshSerial !== this.litMeshSerial) {
      // a new mesh over the pages we already have (keepPages): the geometry moved, the texture stays; the vertex buffer is refilled by syncLitPlain
      this.litMeshSerial = l.meshSerial
      this.litPlain = null
    }
    const d = l.dirtySince(this.litCursor)
    this.litCursor = d.cursor
    if (d.full) this.uploadRects(l, l.fullRects())
    else if (d.rects.length) this.uploadRects(l, d.rects)
  }

  /** Lighting-only view: uploads the mesh to draw (the Core's, or with changed brushes replaced by their current faces) when it changed. */
  private syncLitPlain(l: LightmapStore, world: World | undefined): void {
    const gl = this.gl
    if (!this.litVao || !this.litVbo) return
    const mesh = l.plainMesh(world)
    if (!mesh || mesh === this.litPlain) return
    this.litPlain = mesh
    gl.bindVertexArray(this.litVao); gl.bindBuffer(gl.ARRAY_BUFFER, this.litVbo)
    gl.bufferData(gl.ARRAY_BUFFER, mesh, gl.STATIC_DRAW)
    this.litCount = mesh.length / LIT_VERTEX_FLOATS
  }

  /** Per frame: the style value of every layer at `seconds`, and where each style sits in the texture array. */
  private styleUniforms(l: LightmapStore, seconds: number): void {
    this.styleW.fill(1)
    this.styleSlot.fill(-1)
    this.lmStyles.forEach((st, slot) => { this.styleSlot[st] = slot; this.styleW[st] = styleValue(l.patterns, st, seconds, l.styleHz) })
  }

  private setLayerUniforms(P: WebGLProgram, l: LightmapStore): void {
    const gl = this.gl
    gl.uniform1i(gl.getUniformLocation(P, 'uLightmap'), 0)
    gl.uniform1f(gl.getUniformLocation(P, 'uPages'), this.lmPages)
    gl.uniform1fv(gl.getUniformLocation(P, 'uSlot'), this.styleSlot)
    gl.uniform1fv(gl.getUniformLocation(P, 'uW'), this.styleW)
    gl.uniform1f(gl.getUniformLocation(P, 'uExposure'), l.exposure)
    gl.uniform1f(gl.getUniformLocation(P, 'uBrightness'), l.brightness)
  }

  /** Lit view: the lit mesh regrouped per texture with albedo coordinates (rebuilt when a new mesh arrived or the document changed). */
  private syncLitTextured(l: LightmapStore, world: World): void {
    const gl = this.gl
    const draw = l.litDraw(world)
    if (draw === this.litTexDraw) return
    this.litTexDraw = draw
    if (!draw || draw.vertexCount === 0) return
    if (!this.litTexVao) {
      this.litTexVao = gl.createVertexArray()!; this.litTexVbo = gl.createBuffer()!
      gl.bindVertexArray(this.litTexVao); gl.bindBuffer(gl.ARRAY_BUFFER, this.litTexVbo)
      const stride = LIT_TEX_FLOATS * 4
      gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 3, gl.FLOAT, false, stride, 0)
      gl.enableVertexAttribArray(1); gl.vertexAttribPointer(1, 3, gl.FLOAT, false, stride, 6 * 4)
      gl.enableVertexAttribArray(3); gl.vertexAttribPointer(3, 3, gl.FLOAT, false, stride, 9 * 4)
      gl.enableVertexAttribArray(2); gl.vertexAttribPointer(2, 2, gl.FLOAT, false, stride, 12 * 4)
      gl.enableVertexAttribArray(4); gl.vertexAttribPointer(4, 3, gl.FLOAT, false, stride, 3 * 4)
    }
    gl.bindVertexArray(this.litTexVao); gl.bindBuffer(gl.ARRAY_BUFFER, this.litTexVbo)
    gl.bufferData(gl.ARRAY_BUFFER, draw.data, gl.STATIC_DRAW)
  }

  /** Lit view decals: rebuilt when the decals or the face maps changed (a new lit mesh). */
  private syncLitDecals(l: LightmapStore, entries: DecalEntry[]): void {
    const gl = this.gl
    if (!this.decalLit || this.decalLit.entries !== entries || this.decalLit.faceMaps !== l.faceMaps) {
      if (this.decalLit) { gl.deleteVertexArray(this.decalLit.vao); gl.deleteBuffer(this.decalLit.vbo) }
      const draw = buildLitDecals(entries, l.faceMaps)
      const vao = gl.createVertexArray()!, vbo = gl.createBuffer()!
      gl.bindVertexArray(vao); gl.bindBuffer(gl.ARRAY_BUFFER, vbo)
      const stride = LIT_TEX_FLOATS * 4
      gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 3, gl.FLOAT, false, stride, 0)
      gl.enableVertexAttribArray(1); gl.vertexAttribPointer(1, 3, gl.FLOAT, false, stride, 6 * 4)
      gl.enableVertexAttribArray(3); gl.vertexAttribPointer(3, 3, gl.FLOAT, false, stride, 9 * 4)
      gl.enableVertexAttribArray(2); gl.vertexAttribPointer(2, 2, gl.FLOAT, false, stride, 12 * 4)
      gl.enableVertexAttribArray(4); gl.vertexAttribPointer(4, 3, gl.FLOAT, false, stride, 3 * 4)
      gl.bufferData(gl.ARRAY_BUFFER, draw.data, gl.STATIC_DRAW)
      this.decalLit = { entries, faceMaps: l.faceMaps, draw, vao, vbo }
    }
    if (!this.decalEnt || this.decalEnt.entries !== entries || this.decalEnt.faceMaps !== l.faceMaps) {
      if (this.decalEnt) { gl.deleteVertexArray(this.decalEnt.vao); gl.deleteBuffer(this.decalEnt.vbo) }
      const e = buildEntityDecals(entries, l.faceMaps)
      const vao = gl.createVertexArray()!, vbo = gl.createBuffer()!
      gl.bindVertexArray(vao); gl.bindBuffer(gl.ARRAY_BUFFER, vbo)
      gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 32, 0)
      gl.enableVertexAttribArray(1); gl.vertexAttribPointer(1, 3, gl.FLOAT, false, 32, 12)
      gl.enableVertexAttribArray(2); gl.vertexAttribPointer(2, 2, gl.FLOAT, false, 32, 24)
      gl.bufferData(gl.ARRAY_BUFFER, e.data, gl.STATIC_DRAW)
      this.decalEnt = { entries, faceMaps: l.faceMaps, groups: e.groups, vao, vbo }
    }
  }

  private uploadRects(l: LightmapStore, rects: { page: number; x: number; y: number; w: number; h: number; style?: number }[]): void {
    const gl = this.gl
    gl.bindTexture(gl.TEXTURE_2D_ARRAY, this.lmTex)
    // Each rectangle is copied into a tightly packed buffer and uploaded with default unpack state. Uploading straight out of the page with
    // UNPACK_ROW_LENGTH / SKIP_PIXELS / SKIP_ROWS fails in Chromium ("invalid unpack params combination") for every rectangle with
    // skipRows > 0 on a 3D texture, which silently dropped most tiles.
    gl.pixelStorei(gl.UNPACK_ROW_LENGTH, 0); gl.pixelStorei(gl.UNPACK_SKIP_PIXELS, 0); gl.pixelStorei(gl.UNPACK_SKIP_ROWS, 0)
    gl.pixelStorei(gl.UNPACK_SKIP_IMAGES, 0); gl.pixelStorei(gl.UNPACK_IMAGE_HEIGHT, 0)
    const atlas = l.atlas
    for (const r of rects) {
      const style = r.style ?? 0
      const slot = this.lmStyles.indexOf(style)
      const page = l.pagesOf(style)?.[r.page]
      if (slot < 0 || !page || r.w <= 0 || r.h <= 0) continue
      const src = packRect(page, atlas, r)
      gl.texSubImage3D(gl.TEXTURE_2D_ARRAY, 0, r.x, r.y, slot * this.lmPages + r.page, r.w, r.h, 1, gl.RGBA, gl.HALF_FLOAT, src)
    }
  }

  draw(frame: Frame): void {
    const gl = this.gl, c = this.canvas
    const dpr = frame.pixelSize ? 1 : window.devicePixelRatio || 1
    const w = frame.pixelSize ? frame.pixelSize[0] : Math.max(1, Math.floor(c.clientWidth * dpr)), h = frame.pixelSize ? frame.pixelSize[1] : Math.max(1, Math.floor(c.clientHeight * dpr))
    if (c.width !== w || c.height !== h) { c.width = w; c.height = h }
    gl.viewport(0, 0, w, h)
    gl.clearColor(...(frame.background ?? [0.105, 0.115, 0.135, 1])); gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT)
    gl.enable(gl.DEPTH_TEST); gl.depthFunc(gl.LEQUAL)
    // back faces are culled (default) so you see what the engine will draw; switch off in View to find inside-out faces
    if (frame.settings.backfaceCulling) { gl.enable(gl.CULL_FACE); gl.cullFace(gl.BACK); gl.frontFace(gl.CCW) } else gl.disable(gl.CULL_FACE)
    const vp = frame.camera.viewProj(w / h) as Float32Array
    const sel = frame.selection
    const lightingOnly = frame.settings.renderMode === 'lightingOnly', litView = frame.settings.renderMode === 'lit'
    // Wireframe (REQUIREMENTS §36 W1): edges only, every edge visible through everything, no fills, no sky
    const wire = frame.settings.renderMode === 'wireframe'
    const litMode = (lightingOnly || litView) && !!frame.lighting?.ready
    const texturedLit = litMode && litView && !!frame.world
    const nowSeconds = frame.time ?? performance.now() / 1000
    if (litMode) { this.syncLighting(frame.lighting!); this.styleUniforms(frame.lighting!, nowSeconds) }
    if (texturedLit) this.syncLitTextured(frame.lighting!, frame.world!)
    else if (litMode) this.syncLitPlain(frame.lighting!, frame.world)
    if (texturedLit && frame.decalEntries?.length) this.syncLitDecals(frame.lighting!, frame.decalEntries)

    if (frame.settings.showGrid && frame.camera instanceof OrthoCamera) {
      const g = orthoGrid(frame.camera, frame.settings.gridSize, h / dpr, w / h)
      this.lines(vp, g.minor, [1, 1, 1, 0.07])
      this.lines(vp, g.major, [1, 1, 1, 0.16])
      this.lines(vp, g.axisH, [0.85, 0.25, 0.25, 0.6])
      this.lines(vp, g.axisV, [0.3, 0.45, 0.9, 0.6])
    } else if (frame.settings.showGrid) {
      this.lines(vp, this.grid.minor, [1, 1, 1, 0.07])
      this.lines(vp, this.grid.major, [1, 1, 1, 0.16])
      this.lines(vp, [-50, 0, 0, 50, 0, 0], [0.85, 0.25, 0.25, 0.6])
      this.lines(vp, [0, 0, -50, 0, 0, 50], [0.3, 0.45, 0.9, 0.6])
    }

    // the level's sky behind everything (perspective views only: an orthographic view would show one colour)
    if (frame.sky && !wire && !(frame.camera instanceof OrthoCamera)) {
      gl.useProgram(this.skyBgProg)
      if (this.bindSky(this.skyBgProg, frame.sky)) {
        const inv = mat4.invert(mat4.create(), vp)
        if (inv) {
          gl.uniformMatrix4fv(gl.getUniformLocation(this.skyBgProg, 'uInvViewProj'), false, inv)
          gl.uniform3fv(gl.getUniformLocation(this.skyBgProg, 'uEye'), frame.camera.eye)
          gl.depthMask(false); gl.disable(gl.CULL_FACE)
          gl.bindVertexArray(this.skyBgVao); gl.drawArrays(gl.TRIANGLES, 0, 3)
          gl.depthMask(true)
          if (frame.settings.backfaceCulling) gl.enable(gl.CULL_FACE)
        }
      }
    }

    // opaque fills, pushed back a little so edges drawn on top don't z-fight
    gl.enable(gl.BLEND); gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA)
    gl.enable(gl.POLYGON_OFFSET_FILL); gl.polygonOffset(1, 1)
    gl.useProgram(this.mesh)
    gl.uniformMatrix4fv(gl.getUniformLocation(this.mesh, 'uViewProj'), false, vp)
    const uColor = gl.getUniformLocation(this.mesh, 'uColor'), uHasTex = gl.getUniformLocation(this.mesh, 'uHasTex')
    gl.uniform1i(gl.getUniformLocation(this.mesh, 'uTex'), 0)
    gl.activeTexture(gl.TEXTURE0)
    const subtractive: { g: GpuBrush }[] = []
    // translucent faces (REQUIREMENTS §23) are drawn after everything opaque, blended, without writing depth
    const translucent: (() => void)[] = []
    const uTransp = gl.getUniformLocation(this.mesh, 'uTransp')
    gl.uniform3f(uTransp, 0, 0, 0)
    const entityLit: { g: GpuBrush; lights: NonNullable<ReturnType<LightvolStore['entity']>> }[] = []
    // A big map has thousands of brushes: what is outside the view is not drawn, and a texture's colour, image and transparency are looked up once per
    // frame, not once per brush (the draw calls and their state changes were most of the frame on a map with 14k of them)
    const planes = frustumPlanes(vp)
    const texStates = new Map<string, { r: number; g: number; b: number; tex: WebGLTexture | null; tp: [number, number, number] }>()
    const texState = (path: string) => {
      let t = texStates.get(path)
      if (!t) { const [r, g, b] = textureColor(path); t = { r, g, b, tex: this.texture(path), tp: drawTransparency(this.textures.info(path)) }; texStates.set(path, t) }
      return t
    }
    let boundTex: WebGLTexture | null | undefined, lastHas = -1, lastCol = '', lastTp = ''
    const resetState = () => { boundTex = undefined; lastHas = -1; lastCol = ''; lastTp = '' }
    // sky faces (REQUIREMENTS §28 K10): never lit, not in the Core's lit mesh; drawn by direction after the other opaque faces
    const skyDraws: { vao: WebGLVertexArrayObject; grp: Group }[] = []
    for (const { entity, instance } of wire ? [] : frame.entities)
      for (const b of entity.brushes ?? []) {
        const carved = b.op !== 'subtract' ? frame.carved?.get(carvedKey(entity.id, b.id, instance)) : undefined
        const g = carved ? this.uploadCarved(carved) : this.upload(b)
        if (b.op === 'subtract') { subtractive.push({ g }); continue }
        for (const grp of g.groups) if (isSkyTexture(grp.texture)) skyDraws.push({ vao: g.vao, grp })
        if (isEditorOnly(classOf(entity))) { volumes.push({ g, color: frame.entityColor?.(classOf(entity)) ?? DEFAULT_MARKER }); continue }
        if (!boxInFrustum(planes, g.box)) continue
        if (litMode) {
          // lightmapped entities come from the Core's lit mesh below; the others are lit from the light volume; without it (not baked yet)
          // Lit shows them unlit, Lighting-only leaves them out
          if (frame.lighting!.litEntities.has(entityLightKey(instance ?? '', entity.id))) continue
          const lights = frame.lightvol?.entity(instance ?? '', entity.id)
          if (lights) { entityLit.push({ g, lights }); continue }
          if (lightingOnly) continue
        }
        gl.bindVertexArray(g.vao)
        for (const grp of g.groups) {
          if (isSkyTexture(grp.texture)) continue
          const { r, g: gg, b: bl, tex, tp } = texState(grp.texture)
          const one = () => {
            if (tex) gl.bindTexture(gl.TEXTURE_2D, tex)
            gl.uniform1f(uHasTex, tex ? 1 : 0)
            gl.uniform4f(uColor, r, gg, bl, 1) // prefab content is opaque too (it is only read-only, not a ghost)
            gl.uniform3f(uTransp, tp[0], tp[1], tp[2])
            gl.drawArrays(gl.TRIANGLES, grp.first, grp.count)
          }
          if (tp[0] === 2) { const vao = g.vao; translucent.push(() => { gl.useProgram(this.mesh); gl.activeTexture(gl.TEXTURE0); gl.bindVertexArray(vao); one() }); continue }
          // opaque: only what differs from the previous draw is set
          if (tex && tex !== boundTex) { gl.bindTexture(gl.TEXTURE_2D, tex); boundTex = tex }
          const has = tex ? 1 : 0
          if (has !== lastHas) { gl.uniform1f(uHasTex, has); lastHas = has }
          const col = `${r},${gg},${bl}`
          if (col !== lastCol) { gl.uniform4f(uColor, r, gg, bl, 1); lastCol = col }
          const tpKey = `${tp[0]},${tp[1]},${tp[2]}`
          if (tpKey !== lastTp) { gl.uniform3f(uTransp, tp[0], tp[1], tp[2]); lastTp = tpKey }
          gl.drawArrays(gl.TRIANGLES, grp.first, grp.count)
        }
      }
    resetState()
    gl.uniform3f(uTransp, 0, 0, 0)
    if (texturedLit && this.lmTex && this.litTexDraw && this.litTexVao) {
      gl.useProgram(this.litTexProg)
      gl.uniformMatrix4fv(gl.getUniformLocation(this.litTexProg, 'uViewProj'), false, vp)
      this.setLayerUniforms(this.litTexProg, frame.lighting!)
      gl.uniform1i(gl.getUniformLocation(this.litTexProg, 'uTex'), 1)
      gl.uniform1f(gl.getUniformLocation(this.litTexProg, 'uAlphaTex'), 0)
      const uHas = gl.getUniformLocation(this.litTexProg, 'uHasTex'), uCol = gl.getUniformLocation(this.litTexProg, 'uColor')
      gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D_ARRAY, this.lmTex)
      gl.bindVertexArray(this.litTexVao)
      const uTp = gl.getUniformLocation(this.litTexProg, 'uTransp'), P = this.litTexProg, vao = this.litTexVao, lm = this.lmTex
      for (const grp of this.litTexDraw.groups) {
        const [r, gg, bl] = textureColor(grp.texture)
        const tex = this.texture(grp.texture)
        const tp = drawTransparency(this.textures.info(grp.texture))
        const one = () => {
          gl.activeTexture(gl.TEXTURE1)
          if (tex) gl.bindTexture(gl.TEXTURE_2D, tex)
          gl.uniform1f(uHas, tex ? 1 : 0); gl.uniform3f(uCol, r, gg, bl)
          gl.uniform3f(uTp, tp[0], tp[1], tp[2])
          gl.drawArrays(gl.TRIANGLES, grp.first, grp.count)
        }
        if (tp[0] === 2) {
          translucent.push(() => { gl.useProgram(P); gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D_ARRAY, lm); gl.bindVertexArray(vao); one(); gl.activeTexture(gl.TEXTURE0); gl.uniform3f(uTp, 0, 0, 0) })
          continue
        }
        one()
      }
      gl.uniform3f(uTp, 0, 0, 0)
      gl.activeTexture(gl.TEXTURE0)
      gl.useProgram(this.mesh)
    } else if (litMode && this.litCount > 0 && this.lmTex) {
      gl.useProgram(this.lit)
      gl.uniformMatrix4fv(gl.getUniformLocation(this.lit, 'uViewProj'), false, vp)
      gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D_ARRAY, this.lmTex)
      this.setLayerUniforms(this.lit, frame.lighting!)
      gl.bindVertexArray(this.litVao); gl.drawArrays(gl.TRIANGLES, 0, this.litCount)
      gl.useProgram(this.mesh)
    }
    if (entityLit.length) {
      const P = this.entLit, ex = frame.lighting!.exposure
      gl.useProgram(P)
      gl.uniformMatrix4fv(gl.getUniformLocation(P, 'uViewProj'), false, vp)
      gl.uniform1i(gl.getUniformLocation(P, 'uTex'), 0)
      gl.uniform1f(gl.getUniformLocation(P, 'uExposure'), ex)
      gl.uniform1f(gl.getUniformLocation(P, 'uBrightness'), frame.lighting!.brightness)
      gl.uniform1f(gl.getUniformLocation(P, 'uAlphaTex'), 0)
      const uHas = gl.getUniformLocation(P, 'uHasTex'), uCol = gl.getUniformLocation(P, 'uColor'), uN = gl.getUniformLocation(P, 'uN')
      const uAmb = gl.getUniformLocation(P, 'uAmb'), uDc = gl.getUniformLocation(P, 'uDc'), uDir = gl.getUniformLocation(P, 'uDir'), uTp = gl.getUniformLocation(P, 'uTransp')
      const amb = new Float32Array(24), dc = new Float32Array(24)
      const lm = frame.lighting!
      gl.activeTexture(gl.TEXTURE0)
      for (const { g, lights } of entityLit) {
        // one probe per style layer, scaled by the style's value now (the shader sums them; at most 8 slots per surface in the engine)
        const n = Math.min(8, lights.length)
        for (let i = 0; i < n; i++) {
          const w = styleValue(lm.patterns, lights[i].s, nowSeconds, lm.styleHz)
          for (let c = 0; c < 3; c++) { amb[i * 3 + c] = lights[i].ambient[c] * w; dc[i * 3 + c] = lights[i].directed[c] * w }
        }
        gl.uniform1i(uN, n); gl.uniform3fv(uAmb, amb); gl.uniform3fv(uDc, dc); gl.uniform3fv(uDir, lights[0].dir)
        gl.bindVertexArray(g.vao)
        for (const grp of g.groups) {
          if (isSkyTexture(grp.texture)) continue
          const [r, gg, bl] = lightingOnly ? [1, 1, 1] : textureColor(grp.texture)
          const tex = lightingOnly ? null : this.texture(grp.texture)
          const tp = lightingOnly ? [0, 0, 0] : drawTransparency(this.textures.info(grp.texture)) // as the lightmapped faces: Lighting-only draws no texture
          const one = () => {
            if (tex) gl.bindTexture(gl.TEXTURE_2D, tex)
            gl.uniform1f(uHas, tex ? 1 : 0); gl.uniform3f(uCol, r, gg, bl)
            gl.uniform3f(uTp, tp[0], tp[1], tp[2])
            gl.drawArrays(gl.TRIANGLES, grp.first, grp.count)
          }
          if (tp[0] === 2) {
            const vao = g.vao, a = amb.slice(), d = dc.slice(), dir = lights[0].dir
            translucent.push(() => {
              gl.useProgram(P); gl.activeTexture(gl.TEXTURE0); gl.bindVertexArray(vao)
              gl.uniform1i(uN, n); gl.uniform3fv(uAmb, a); gl.uniform3fv(uDc, d); gl.uniform3fv(uDir, dir)
              one(); gl.uniform3f(uTp, 0, 0, 0)
            })
            continue
          }
          one()
        }
      }
      gl.uniform3f(uTp, 0, 0, 0)
      gl.useProgram(this.mesh)
    }
    if (skyDraws.length) {
      const P = this.skyProg
      gl.useProgram(P)
      gl.uniformMatrix4fv(gl.getUniformLocation(P, 'uViewProj'), false, vp)
      gl.uniform3fv(gl.getUniformLocation(P, 'uEye'), frame.camera.eye)
      for (const { vao, grp } of skyDraws) {
        if (!this.bindSky(P, grp.texture)) {
          // not loaded yet, or an ordinary 2D sky texture: its flat colour / image
          gl.useProgram(this.mesh)
          const tex = this.texture(grp.texture), [r, gg, bl] = textureColor(grp.texture)
          if (tex) gl.bindTexture(gl.TEXTURE_2D, tex)
          gl.uniform1f(uHasTex, tex ? 1 : 0); gl.uniform4f(uColor, r, gg, bl, 1); gl.uniform3f(uTransp, 0, 0, 0)
          gl.bindVertexArray(vao); gl.drawArrays(gl.TRIANGLES, grp.first, grp.count)
          gl.useProgram(P)
          continue
        }
        gl.bindVertexArray(vao); gl.drawArrays(gl.TRIANGLES, grp.first, grp.count)
      }
      gl.useProgram(this.mesh)
    }
    // Lit view decals: the projector's texture x the light of the face they lie on (the lightmap layers, or the light volume of a door), blended by alpha
    if (texturedLit && frame.decalEntries?.length && this.lmTex) {
      gl.depthMask(false); gl.polygonOffset(-2, -2)
      if (this.decalLit && this.decalLit.draw.vertexCount > 0) {
        const P = this.litTexProg
        gl.useProgram(P)
        gl.uniformMatrix4fv(gl.getUniformLocation(P, 'uViewProj'), false, vp)
        this.setLayerUniforms(P, frame.lighting!)
        gl.uniform1i(gl.getUniformLocation(P, 'uTex'), 1)
        gl.uniform1f(gl.getUniformLocation(P, 'uAlphaTex'), 1)
        const uHas = gl.getUniformLocation(P, 'uHasTex'), uCol = gl.getUniformLocation(P, 'uColor')
        gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D_ARRAY, this.lmTex)
        gl.bindVertexArray(this.decalLit.vao)
        for (const grp of this.decalLit.draw.groups) {
          const tex = this.texture(grp.texture)
          gl.activeTexture(gl.TEXTURE1)
          if (tex) gl.bindTexture(gl.TEXTURE_2D, tex)
          gl.uniform1f(uHas, tex ? 1 : 0); gl.uniform3f(uCol, 0.8, 0.8, 0.8)
          gl.drawArrays(gl.TRIANGLES, grp.first, grp.count)
        }
        gl.activeTexture(gl.TEXTURE0)
      }
      if (this.decalEnt && this.decalEnt.groups.length) {
        const P = this.entLit, lm = frame.lighting!
        gl.useProgram(P)
        gl.uniformMatrix4fv(gl.getUniformLocation(P, 'uViewProj'), false, vp)
        gl.uniform1i(gl.getUniformLocation(P, 'uTex'), 0)
        gl.uniform1f(gl.getUniformLocation(P, 'uExposure'), lm.exposure)
        gl.uniform1f(gl.getUniformLocation(P, 'uBrightness'), lm.brightness)
        gl.uniform1f(gl.getUniformLocation(P, 'uAlphaTex'), 1)
        const uHas = gl.getUniformLocation(P, 'uHasTex'), uCol = gl.getUniformLocation(P, 'uColor'), uN = gl.getUniformLocation(P, 'uN')
        const uAmb = gl.getUniformLocation(P, 'uAmb'), uDc = gl.getUniformLocation(P, 'uDc'), uDir = gl.getUniformLocation(P, 'uDir')
        const amb = new Float32Array(24), dc = new Float32Array(24)
        gl.bindVertexArray(this.decalEnt.vao)
        gl.activeTexture(gl.TEXTURE0)
        for (const grp of this.decalEnt.groups) {
          const lights = frame.lightvol?.entity(grp.instance, grp.entityId)
          if (!lights) continue // not baked yet
          const n = Math.min(8, lights.length)
          for (let i = 0; i < n; i++) {
            const w = styleValue(lm.patterns, lights[i].s, nowSeconds, lm.styleHz)
            for (let c = 0; c < 3; c++) { amb[i * 3 + c] = lights[i].ambient[c] * w; dc[i * 3 + c] = lights[i].directed[c] * w }
          }
          gl.uniform1i(uN, n); gl.uniform3fv(uAmb, amb); gl.uniform3fv(uDc, dc); gl.uniform3fv(uDir, lights[0].dir)
          const tex = this.texture(grp.texture)
          if (tex) gl.bindTexture(gl.TEXTURE_2D, tex)
          gl.uniform1f(uHas, tex ? 1 : 0); gl.uniform3f(uCol, 0.8, 0.8, 0.8)
          gl.drawArrays(gl.TRIANGLES, grp.first, grp.count)
        }
        gl.uniform1f(gl.getUniformLocation(P, 'uAlphaTex'), 0)
      }
      gl.useProgram(this.mesh)
      gl.polygonOffset(1, 1); gl.depthMask(true)
    }
    // decals: derived surfaces laid just above what they project onto
    if (frame.decals?.length && !litMode && !wire) {
      gl.depthMask(false); gl.polygonOffset(-2, -2)
      gl.uniform1f(gl.getUniformLocation(this.mesh, 'uAlphaTex'), 1)
      for (const m of frame.decals) {
        const g = this.uploadCarved(m)
        gl.bindVertexArray(g.vao)
        for (const grp of g.groups) {
          const tex = this.texture(grp.texture)
          if (tex) gl.bindTexture(gl.TEXTURE_2D, tex)
          gl.uniform1f(uHasTex, tex ? 1 : 0)
          gl.uniform4f(uColor, 0.8, 0.8, 0.8, 1)
          gl.drawArrays(gl.TRIANGLES, grp.first, grp.count)
        }
      }
      gl.uniform1f(gl.getUniformLocation(this.mesh, 'uAlphaTex'), 0)
      gl.polygonOffset(1, 1); gl.depthMask(true)
    }
    if (translucent.length) {
      gl.depthMask(false)
      for (const draw of translucent) draw()
      gl.useProgram(this.mesh)
      gl.uniform3f(uTransp, 0, 0, 0)
      gl.activeTexture(gl.TEXTURE0)
    }
    // subtractive brushes: translucent red volumes (the carving itself is derived data, DESIGN.md §5.6)
    gl.depthMask(false)
    gl.uniform1f(uHasTex, 0)
    gl.uniform4f(uColor, 0.95, 0.25, 0.25, 0.28)
    if (frame.settings.showSubtractive) for (const { g } of subtractive) { gl.bindVertexArray(g.vao); gl.drawArrays(gl.TRIANGLES, 0, g.count) }
    gl.depthMask(true)
    gl.disable(gl.POLYGON_OFFSET_FILL)
    if (frame.settings.showVolumes) for (const { g, color } of volumes) { gl.uniform4f(uColor, color[0], color[1], color[2], 0.2); gl.bindVertexArray(g.vao); gl.drawArrays(gl.TRIANGLES, 0, g.count) }

    // edges
    gl.useProgram(this.line)
    gl.uniformMatrix4fv(gl.getUniformLocation(this.line, 'uViewProj'), false, vp)
    const lColor = gl.getUniformLocation(this.line, 'uColor')
    const point = new Map<string, { color: [number, number, number]; lines: number[]; fill: number[] }>(), origins: number[] = []
    const labels: { text: string; at: Vec3 }[] = []
    const selInst = new Set((frame.selections ?? (sel ? [sel] : [])).flatMap((x) => (x.instanceId !== undefined ? [x.instanceId] : [])))
    for (const { entity, instanceId, instance } of frame.entities) {
      const inSelectedInstance = instanceId !== undefined && selInst.has(instanceId)
      const isSel = instanceId === undefined && sel?.instanceId === undefined && sel?.entityId === entity.id
      for (const b of entity.brushes ?? []) {
        const carved = b.op !== 'subtract' ? frame.carved?.get(carvedKey(entity.id, b.id, instance)) : undefined
        const g = carved ? this.uploadCarved(carved) : this.upload(b)
        const brushSel = inSelectedInstance || (instanceId === undefined && (frame.selections ? frame.selections.some((s) => s.instanceId === undefined && s.entityId === entity.id && (!s.brushId || s.brushId === b.id)) : isSel && (!sel!.brushId || sel!.brushId === b.id)))
        if (b.op === 'subtract' && !frame.settings.showSubtractive) continue
        const volume = isEditorOnly(classOf(entity))
        if (volume && !frame.settings.showVolumes) continue
        if (!volume && !brushSel && !frame.settings.showOutlines && !wire) continue
        if (brushSel) gl.uniform4f(lColor, 1, 0.62, 0.15, 1)
        else if (b.op === 'subtract') gl.uniform4f(lColor, 1, 0.4, 0.4, 0.9)
        else if (wire) { const c = classOf(entity) === 'worldspawn' ? [0.78, 0.82, 0.88] : frame.entityColor?.(classOf(entity)) ?? DEFAULT_MARKER; gl.uniform4f(lColor, c[0], c[1], c[2], 0.95) }
        else gl.uniform4f(lColor, 0, 0, 0, 0.55)
        if (wire) gl.disable(gl.DEPTH_TEST)
        gl.bindVertexArray(g.evao); gl.drawArrays(gl.LINES, 0, g.ecount)
        if (wire) gl.enable(gl.DEPTH_TEST)
      }
      if (!isBrushEntity(entity) && classOf(entity) !== 'worldspawn') {
        const picked = inSelectedInstance || (instanceId === undefined && (frame.selections ? frame.selections.some((x) => x.instanceId === undefined && x.entityId === entity.id) : isSel))
        const color: [number, number, number] = picked ? [1, 0.62, 0.15] : frame.entityColor?.(classOf(entity)) ?? DEFAULT_MARKER
        const key = color.join(',')
        let g = point.get(key)
        if (!g) point.set(key, (g = { color, lines: [], fill: [] }))
        const ob = pointBox(entity, (n) => boxOf(frame, n))  // the class box turned with the entity's angles
        if (drawsBox(entity)) { const corners = worldCorners(ob); g.lines.push(...cornerLines(corners)); g.fill.push(...cornerTriangles(corners)) } // lights and decals show their gizmo / bounds instead
        labels.push({ text: classOf(entity), at: toWorldCentre(ob) })
      }
      else if (frame.settings.showOrigins && showsCustomOrigin(entity)) origins.push(...crossLines(originOf(entity), 0.25))
    }
    gl.bindVertexArray(null)
    // point entities: translucent box (two-sided, depth-tested, no depth write) + outline + class name
    gl.disable(gl.CULL_FACE); gl.depthMask(false)
    if (!wire) for (const g of point.values()) if (g.fill.length) this.tris(vp, g.fill, [g.color[0], g.color[1], g.color[2], ENTITY_FILL_ALPHA])
    gl.depthMask(true)
    for (const g of point.values()) this.lines(vp, g.lines, [g.color[0], g.color[1], g.color[2], 1])
    gl.depthMask(false)
    this.drawLabels(vp, labels, w, h, dpr)
    gl.depthMask(true)
    this.lines(vp, origins, [0.3, 0.9, 0.95, 1])

    // selected face overlay and the box-draw preview
    const faceOverlay = (hl: { brush: Brush; faceId: number }, fill: [number, number, number, number], line: [number, number, number, number], depthTested = false) => {
      const { brush, faceId } = hl
      const f = brush.faces.find((x) => x.id === faceId)
      if (!f) return
      const tri: number[] = [], edge: number[] = []
      for (const [a, c, d] of triangulateFace(brush, f)) tri.push(...brush.vertices[a], ...brush.vertices[c], ...brush.vertices[d])
      for (let i = 0; i < f.loop.length; i++) edge.push(...brush.vertices[f.loop[i]], ...brush.vertices[f.loop[(i + 1) % f.loop.length]])
      if (depthTested) {
        // only where the face is seen (a big floor must not tint the walls in front of it): just above its own surface
        gl.depthMask(false); gl.enable(gl.POLYGON_OFFSET_FILL); gl.polygonOffset(-2, -2)
        this.tris(vp, tri, fill)
        gl.disable(gl.POLYGON_OFFSET_FILL); gl.depthMask(true)
        this.lines(vp, edge, line)
        return
      }
      gl.disable(gl.DEPTH_TEST)
      this.tris(vp, tri, fill)
      this.lines(vp, edge, line)
      gl.enable(gl.DEPTH_TEST)
    }
    if (frame.highlight) faceOverlay(frame.highlight, [0.15, 0.55, 1, 0.35], [0.35, 0.7, 1, 1])
    // the face an Alt + drag would grab (REQUIREMENTS §33 H8)
    if (frame.hoverFace) faceOverlay(frame.hoverFace, [1, 0.6, 0.1, 0.35], [1, 0.75, 0.3, 1], true)
    gl.disable(gl.CULL_FACE) // overlays are two-sided
    if (frame.mesh) this.drawMesh(vp, frame)
    if (frame.settings.showLightmapDensity && !wire) this.drawLightmapDensity(vp, frame)
    if (frame.settings.showTexelHeat && !wire) this.drawTexelHeat(vp, frame)
    if (frame.settings.showLightvol) this.drawLightvolSamples(vp, frame)
    if (frame.annotations?.length) this.lines(vp, frame.annotations, [0.95, 0.8, 0.25, 1])
    for (const o of frame.overlays ?? []) this.lines(vp, o.lines, [o.color[0], o.color[1], o.color[2], 1])
    if (frame.preview?.length) { gl.disable(gl.DEPTH_TEST); this.lines(vp, frame.preview, [0.35, 0.8, 1, 1]); gl.enable(gl.DEPTH_TEST) }
    if (frame.handles?.length) { gl.disable(gl.DEPTH_TEST); for (const o of frame.handles) this.lines(vp, o.lines, [o.color[0], o.color[1], o.color[2], 1]); gl.enable(gl.DEPTH_TEST) }
    if (frame.sizeLabels?.length) { gl.disable(gl.DEPTH_TEST); gl.depthMask(false); this.drawLabels(vp, frame.sizeLabels, w, h, dpr); gl.depthMask(true); gl.enable(gl.DEPTH_TEST) }
    if (frame.ghost) this.drawGhost(vp, frame, frame.ghost)

    // translate gizmo, drawn over everything
    if (frame.gizmo) {
      gl.disable(gl.DEPTH_TEST)
      const { pivot, length, active, dir, mode } = frame.gizmo
      const colors: [number, number, number, number][] = [[1, 0.3, 0.3, 1], [0.35, 0.95, 0.35, 1], [0.4, 0.55, 1, 1]]
      if (dir) {
        const tip: Vec3 = [pivot[0] + dir[0] * length, pivot[1] + dir[1] * length, pivot[2] + dir[2] * length]
        this.lines(vp, [...pivot, ...tip, ...arrowHead(tip, dir, length * 0.14)], active !== null ? [1, 1, 0.2, 1] : [0.2, 0.9, 0.95, 1])
      } else if (mode === 'rotate') {
        for (const axis of [0, 1, 2] as Axis[]) this.lines(vp, ringLines(pivot, axis, length), active === axis ? [1, 1, 0.2, 1] : colors[axis])
      } else if (mode === 'scale') {
        for (const axis of [0, 1, 2] as Axis[]) {
          const d = AXIS_DIR[axis], tip: Vec3 = [pivot[0] + d[0] * length, pivot[1] + d[1] * length, pivot[2] + d[2] * length]
          const k = length * 0.06, c: Vec3 = tip, col = active === axis ? [1, 1, 0.2, 1] as [number, number, number, number] : colors[axis]
          const e: number[] = []
          for (const [a, b] of [[0, 1], [1, 2], [2, 0]]) for (const sa of [-1, 1]) for (const sb of [-1, 1]) { // a cube's 12 edges
            const o = [0, 1, 2].find((x) => x !== a && x !== b)!
            for (const so of [-1, 1]) { const p: Vec3 = [c[0], c[1], c[2]]; p[a] += sa * k; p[b] += sb * k; const q: Vec3 = [...p]; p[o] -= so * k; q[o] += so * k; if (so === -1) e.push(...p, ...q) }
          }
          this.lines(vp, [...pivot, ...tip, ...e], col)
        }
      } else for (const axis of [0, 1, 2] as Axis[]) {
        const d = AXIS_DIR[axis], tip: Vec3 = [pivot[0] + d[0] * length, pivot[1] + d[1] * length, pivot[2] + d[2] * length]
        this.lines(vp, [...pivot, ...tip, ...arrowHead(tip, d, length * 0.14)], active === axis ? [1, 1, 0.2, 1] : colors[axis])
      }
      gl.enable(gl.DEPTH_TEST)
    }
  }

  dispose(): void { this.gl.getExtension('WEBGL_lose_context')?.loseContext() }
}

function buildGrid(half: number): { minor: Float32Array; major: Float32Array } {
  const minor: number[] = [], major: number[] = []
  for (let i = -half; i <= half; i++) {
    const t = i % 10 === 0 ? major : minor
    t.push(i, 0, -half, i, 0, half, -half, 0, i, half, 0, i)
  }
  return { minor: new Float32Array(minor), major: new Float32Array(major) }
}

const DEFAULT_MARKER: [number, number, number] = [1, 0.85, 0.25]
const crossLines = (o: Vec3, r: number): number[] => [
  o[0] - r, o[1], o[2], o[0] + r, o[1], o[2], o[0], o[1] - r, o[2], o[0], o[1] + r, o[2], o[0], o[1], o[2] - r, o[0], o[1], o[2] + r,
]
const GHOST_ALPHA = 0.5
/** Fill of a placed point entity's box: the same as the drag preview's (GHOST_ALPHA * 0.7). */
const ENTITY_FILL_ALPHA = GHOST_ALPHA * 0.7
const DEFAULT_BOX: Box = boxFromQuake(null)
const boxOf = (frame: Frame, classname: string): Box => frame.entityBox?.(classname) ?? DEFAULT_BOX
/** The 12 edges of a box given by its eight corners (the order of `cornersOf`) as line endpoints. */
export function cornerLines(p: Vec3[]): number[] {
  const e = [[0, 1], [1, 2], [2, 3], [3, 0], [4, 5], [5, 6], [6, 7], [7, 4], [0, 4], [1, 5], [2, 6], [3, 7]]
  return e.flatMap(([a, b]) => [...p[a], ...p[b]])
}
/** The 12 triangles of a box given by its eight corners. */
export function cornerTriangles(p: Vec3[]): number[] {
  const q = [[0, 1, 2, 3], [4, 5, 6, 7], [0, 1, 5, 4], [3, 2, 6, 7], [0, 3, 7, 4], [1, 2, 6, 5]] // six quads
  return q.flatMap(([a, b, c, d]) => [...p[a], ...p[b], ...p[c], ...p[a], ...p[c], ...p[d]])
}
/** The 12 edges of an axis-aligned box as line endpoints. */
export const boxLines = (min: Vec3, max: Vec3): number[] => cornerLines(cornersOf(min, max))
/** The 12 triangles of an axis-aligned box. */
export const boxTriangles = (min: Vec3, max: Vec3): number[] => cornerTriangles(cornersOf(min, max))
/** The centre of an oriented box in world space (where the class name is written). */
const toWorldCentre = (b: OrientedBox): Vec3 => toWorld(b, [(b.min[0] + b.max[0]) / 2, (b.min[1] + b.max[1]) / 2, (b.min[2] + b.max[2]) / 2])
/** `vp` followed by a translation: the same matrix as drawing everything moved by `t`. */
export function translatedMatrix(vp: Float32Array, t: Vec3): Float32Array {
  const m = new Float32Array(vp)
  for (let r = 0; r < 4; r++) m[12 + r] = vp[12 + r] + vp[r] * t[0] + vp[4 + r] * t[1] + vp[8 + r] * t[2]
  return m
}
function arrowHead(tip: Vec3, d: Vec3, size: number): number[] {
  const u: Vec3 = d[0] ? [0, 1, 0] : [1, 0, 0], v: Vec3 = d[2] ? [0, 1, 0] : [0, 0, 1]
  const base: Vec3 = [tip[0] - d[0] * size * 2, tip[1] - d[1] * size * 2, tip[2] - d[2] * size * 2]
  const out: number[] = []
  for (const s of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
    const q = [base[0] + (u[0] * s[0] + v[0] * s[1]) * size, base[1] + (u[1] * s[0] + v[1] * s[1]) * size, base[2] + (u[2] * s[0] + v[2] * s[1]) * size]
    out.push(...tip, ...q)
  }
  return out
}
