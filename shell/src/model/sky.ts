// Sky (REQUIREMENTS §28, DESIGN §12 "Sky"): skyboxes as the engine loads and samples them, and the level's sky settings.
//
// A texture whose name ends in "cube" is a skybox: the engine (SdlWasmEngine Quake3Bsp + TextureCube.hpp) loads GameData/env/<last part of the name>,
// six faces <base>_lf _rt _up _dn _ft _bk as the cube faces +X -X +Y -Y +Z -Z (_up turned 90 degrees clockwise, _dn counter-clockwise on load), or
// one equirectangular panorama, and bsp/fs_bsp_cube samples it with the view direction (world = editor space: metres, Y up) with x negated.
// The Core's core/sky.h has the same mapping (cubemapLookup / panoramaLookup) for the sky light; both are tested for the same directions.
import type { Entity } from './document'
import type { Vec3 } from './math'

/** The last part of the name ends in "cube" (case-sensitive, as the engine checks it). */
export const isCubemapPath = (path: string | undefined): boolean => !!path && (path.split('/').pop() ?? '').endsWith('cube')

/**
 * A sky texture: a skybox, or a texture the built-in BSP rules give the `sky` surface flag (`*sky*`, core bsp_compile.cpp). Sky faces get no lightmap,
 * block no light and are drawn by direction in the views (the engine draws only skyboxes that way).
 */
export const isSkyTexture = (path: string | undefined): boolean => !!path && (isCubemapPath(path) || path.toLowerCase().endsWith('cube') || path.toLowerCase().includes('sky'))
/** A skybox of the assets' env folder as the Core lists it ("env/<name>"). */
export const isEnvSkybox = (path: string): boolean => path.startsWith('env/') && isCubemapPath(path)

/** The face files in the engine's order, the WebGL cube face each one becomes, and how it is turned on load (TextureCube.hpp DecodeFromFiles). */
export const SKY_FACES: { suffix: string; target: 'px' | 'nx' | 'py' | 'ny' | 'pz' | 'nz'; turn: 0 | 1 | -1 }[] = [
  { suffix: '_lf', target: 'px', turn: 0 },
  { suffix: '_rt', target: 'nx', turn: 0 },
  { suffix: '_up', target: 'py', turn: 1 },   // 90 degrees clockwise
  { suffix: '_dn', target: 'ny', turn: -1 },  // 90 degrees counter-clockwise
  { suffix: '_ft', target: 'pz', turn: 0 },
  { suffix: '_bk', target: 'nz', turn: 0 },
]

/** The direction the engine samples the cube with: fs_bsp_cube's `dir * vec3(-1, 1, 1)` (worldOrientation = identity). */
export const engineCubeDir = (d: Vec3): Vec3 => [-d[0], d[1], d[2]]

/**
 * Which face file (index into SKY_FACES) and where in it (u right, v down, 0..1) the engine samples for a view direction: the standard cube face
 * selection (GL / D3D / bgfx) of engineCubeDir, then the load-time turn of _up / _dn undone. Matches core/sky.cpp cubemapLookup.
 */
export function cubemapLookup(dir: Vec3): { face: number; u: number; v: number } {
  const [x, y, z] = engineCubeDir(dir)
  const ax = Math.abs(x), ay = Math.abs(y), az = Math.abs(z)
  let face: number, sc: number, tc: number, ma: number
  if (ax >= ay && ax >= az) { ma = ax; if (x > 0) { face = 0; sc = -z; tc = -y } else { face = 1; sc = z; tc = -y } }
  else if (ay >= az) { ma = ay; if (y > 0) { face = 2; sc = x; tc = z } else { face = 3; sc = x; tc = -z } }
  else { ma = az; if (z > 0) { face = 4; sc = x; tc = -y } else { face = 5; sc = -x; tc = -y } }
  if (ma <= 0) return { face, u: 0, v: 0 }
  const u = (sc / ma + 1) / 2, v = (tc / ma + 1) / 2
  if (face === 2) return { face, u: v, v: 1 - u }
  if (face === 3) return { face, u: 1 - v, v: u }
  return { face, u, v }
}

/** Where in a panorama (u right, v down) the engine samples: longitude atan2(x, -z) (north = the middle, east = three quarters), latitude asin(y). */
export function panoramaLookup(dir: Vec3): { u: number; v: number } {
  const l = Math.hypot(dir[0], dir[1], dir[2]) || 1
  let u = Math.atan2(dir[0], -dir[2]) / (2 * Math.PI) + 0.5
  if (u >= 1) u -= 1
  return { u, v: 0.5 - Math.asin(Math.max(-1, Math.min(1, dir[1] / l))) / Math.PI }
}

// GLSL of the same mapping for the viewports (renderer.ts). Cube faces are uploaded as SKY_FACES says, so a GL cube lookup with engineCubeDir is the
// engine's; a panorama is sampled with panoramaLookup's formula.
export const SKY_GLSL = `
vec3 skyColor(vec3 d, samplerCube cube, sampler2D pano, float isPano) {
  if (isPano > 0.5) {
    float u = atan(d.x, -d.z) / 6.28318530718 + 0.5;
    float v = 0.5 - asin(clamp(d.y / max(length(d), 1e-9), -1.0, 1.0)) / 3.14159265359;
    return textureGrad(pano, vec2(u, v), vec2(0.0), vec2(0.0)).rgb; // no mip jump at the seam
  }
  return texture(cube, vec3(-d.x, d.y, d.z)).rgb;
}`

// ---- level settings (worldspawn keys, read by the Core's skySettingsOf) --------------------------------------------------------------------------
export const SKY_AUTO_KEY = '_sky_auto', SKY_MARGIN_KEY = '_sky_margin', SKY_TEXTURE_KEY = '_sky_texture'
export const SKY_MARGIN_DEFAULT = 8, SKY_MARGIN_MAX = 1000
export type AutoSky = 'off' | 'box' | 'areas'
export const AUTO_SKY_OPTIONS: { value: AutoSky; label: string; title: string }[] = [
  { value: 'off', label: 'Off', title: 'No automatic sky' },
  { value: 'box', label: 'Box', title: 'One sky box around the whole level plus the margin (the floor is sky too)' },
  { value: 'areas', label: 'Box per area', title: 'One sky box per group of touching geometry, plus the margin; boxes that meet are merged' },
]
export const autoSkyOf = (world: Entity | undefined): AutoSky => { const v = world?.keyvalues?.[SKY_AUTO_KEY]; return v === 'box' || v === 'areas' ? v : 'off' }
export function skyMarginOf(world: Entity | undefined): number {
  const v = Number(world?.keyvalues?.[SKY_MARGIN_KEY])
  return world?.keyvalues?.[SKY_MARGIN_KEY]?.trim() && Number.isFinite(v) ? Math.min(SKY_MARGIN_MAX, Math.max(0, v)) : SKY_MARGIN_DEFAULT
}
/** What to store for a typed margin: null (remove the key) for empty input or the default. */
export function skyMarginKey(input: string): string | null {
  if (input.trim() === '') return null
  const v = Number(input)
  if (!Number.isFinite(v)) return null
  const c = Math.min(SKY_MARGIN_MAX, Math.max(0, v))
  return c === SKY_MARGIN_DEFAULT ? null : String(+c.toFixed(6))
}
/** The level's sky texture as the Core picks it: `_sky_texture`, else (Auto sky on) the first skybox, else none (''). */
export function levelSkyTexture(world: Entity | undefined, skyboxes: string[]): string {
  const t = world?.keyvalues?.[SKY_TEXTURE_KEY]?.trim()
  if (t) return t
  return autoSkyOf(world) !== 'off' ? (skyboxes.filter(isEnvSkybox).sort()[0] ?? '') : ''
}
