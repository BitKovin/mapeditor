// Level settings (REQUIREMENTS §22 P2): stored as worldspawn keys, so they are saved with the map and the Core reads them on every edit
// (core/lights.h levelSettingsOf). A key that is absent means "default" (for the optimisation keys: the Core's lighting.json value).
import type { Entity, MapDocument } from './document'
import { classOf } from './document'

export interface LevelField {
  key: string
  label: string
  /** what an absent key means: a number, or null = "the Core's lighting.json value" */
  dflt: number | null
  min: number
  max: number
  step: number
  int?: boolean
  hint: string
}
export interface LevelSection { title: string; note?: string; fields: LevelField[] }

export const LEVEL_SECTIONS: LevelSection[] = [
  { title: 'Light', fields: [
    { key: '_light_scale', label: 'General brightness', dflt: 1, min: 0, max: 100, step: 0.1, hint: 'Multiplies the brightness of every light entity (not the ambient light)' },
  ] },
  { title: 'Colour correction', note: 'Display only: how the Lit view (and the engine) shows the baked light. Changing it re-bakes nothing.', fields: [
    { key: '_cc_exposure', label: 'Exposure', dflt: null, min: 0.05, max: 20, step: 0.05, hint: 'Multiplies the shown light; empty = the Core\'s lighting.json exposure' },
    { key: '_cc_brightness', label: 'Brightness', dflt: 0, min: -1, max: 1, step: 0.01, hint: 'Added to the shown light after the exposure (linear)' },
  ] },
  { title: 'Ambient occlusion (baked)', note: 'Darkens light in corners and creases. Radius 0 = off. Darker: raise Strength or Exponent, or lower Distance falloff.', fields: [
    { key: '_ao_radius', label: 'Radius (m)', dflt: 0, min: 0, max: 64, step: 0.1, hint: 'How far rays look for nearby geometry' },
    { key: '_ao_power', label: 'Exponent', dflt: 1, min: 0.1, max: 10, step: 0.1, hint: 'Curve of the darkening: higher = darker, wider shadows; below 1 = softer' },
    { key: '_ao_intensity', label: 'Intensity', dflt: 1, min: 0, max: 1, step: 0.05, hint: '0 = no darkening, 1 = full' },
    { key: '_ao_strength', label: 'Strength', dflt: 1, min: 0, max: 16, step: 0.1, hint: 'Multiplies the occlusion before the exponent: 2 or more turns tight corners fully dark' },
    { key: '_ao_falloff', label: 'Distance falloff', dflt: 1, min: 0, max: 8, step: 0.1, hint: 'How a hit fades with its distance: 0 = every hit within the radius counts fully (strongest), 1 = linear, higher = only very near geometry' },
    { key: '_ao_rays', label: 'Rays', dflt: 32, min: 1, max: 1024, step: 8, int: true, hint: 'Occlusion rays per texel (ray traced on the GPU when it supports ray queries): more = smoother' },
  ] },
  { title: 'Quality falloff (outside the playable area)', note: 'Needs an editor_playable_area volume (Place tab). Lightmap detail drops in cascades with the distance from it: full density up to Radius, then each cascade reaches Range times further and has Density times the texels per metre of the one before. The light volume only covers the area plus the margin.', fields: [
    { key: '_lod_radius', label: 'Full detail radius (m)', dflt: 16, min: 1, max: 1024, step: 1, hint: 'Surfaces this close to the playable area keep the full lightmap density' },
    { key: '_lod_levels', label: 'Cascades', dflt: 4, min: 0, max: 8, step: 1, int: true, hint: 'Number of lower-detail cascades beyond the full-detail one; 0 = no falloff. Surfaces further out stay in the last cascade' },
    { key: '_lod_range', label: 'Range factor', dflt: 2, min: 1.1, max: 8, step: 0.1, hint: 'Each cascade reaches this many times further than the previous one (cascade k ends at Radius x Range^k)' },
    { key: '_lod_density', label: 'Density per cascade', dflt: 0.5, min: 0.05, max: 1, step: 0.05, hint: 'Lightmap texels per metre of a cascade relative to the one before (0.5 = half, so a quarter in the second)' },
    { key: '_lod_cull_unseen', label: 'Cull unseen surfaces', dflt: 1, min: 0, max: 1, step: 1, int: true, hint: '1 = surfaces that nothing in the playable area can see (inside far closed buildings, backs of far walls) get the lowest cascade straight away' },
    { key: '_lightvol_margin', label: 'Light volume margin (m)', dflt: 2, min: 0, max: 256, step: 0.5, hint: 'The light volume covers the playable area grown by this much' },
  ] },
  { title: 'Export', note: 'Only the export uses these.', fields: [
    { key: '_lightmap_jpg', label: 'Lightmaps as .jpg when they fit', dflt: 1, min: 0, max: 1, step: 1, int: true, hint: '1 (default) = pages whose light is nothing above 1 and survives 8 bits are written as lm_NNNN.jpg, grouped apart from the .exr pages (the engine has to find .jpg pages)' },
    { key: '_lightmap_jpg_tolerance', label: '.jpg error allowed (levels)', dflt: 8, min: 0.5, max: 64, step: 0.5, hint: 'How far a chart may differ in a .jpg, in 8-bit display levels after the exposure; a chart that differs more stays in the .exr pages. Near black one byte of linear light is already 12 levels: lower this and the dark charts stay .exr' },
    { key: '_lightmap_jpg_quality', label: '.jpg quality', dflt: 98, min: 50, max: 100, step: 1, int: true, hint: 'JPEG quality of the lightmap pages' },
  ] },
  { title: 'Optimisation', note: 'Empty = the Core\'s lighting.json value.', fields: [
    { key: '_lightvol_grid', label: 'Lightvol grid size (m)', dflt: null, min: 0.25, max: 64, step: 0.25, hint: 'Spacing of the light volume probes' },
    { key: '_rays_per_texel', label: 'Rays per lightmap pixel', dflt: null, min: 1, max: 65536, step: 16, int: true, hint: 'Bounce-light gather rays per texel over the whole bake' },
    { key: '_rays_per_probe', label: 'Rays per lightvol probe', dflt: null, min: 1, max: 1024, step: 8, int: true, hint: 'Gather rays per light volume probe' },
    { key: '_supersample', label: 'Supersampling per lightmap pixel', dflt: null, min: 1, max: 8, step: 1, int: true, hint: 'Sub-texel positions for the direct light (anti-aliased shadow edges)' },
    { key: '_bake_quality', label: 'Bake quality multiplier', dflt: 2, min: 0.25, max: 16, step: 0.25, hint: 'Multiplies the ray counts of the final bake (export); the live view bakes at x1' },
  ] },
]
export const LEVEL_FIELDS: LevelField[] = LEVEL_SECTIONS.flatMap((s) => s.fields)

export const worldspawnOf = (doc: MapDocument): Entity | undefined => doc.entities.find((e) => classOf(e) === 'worldspawn')

/** The stored value of a field, or null when the key is absent or not a number (then the default applies). */
export function levelValue(world: Entity | undefined, f: LevelField): number | null {
  const raw = world?.keyvalues?.[f.key]
  if (raw === undefined || raw.trim() === '') return null
  const v = Number(raw)
  return Number.isFinite(v) ? v : null
}

/** What to store for a typed value: null (remove the key) for empty input or the default, else the clamped number as a string. */
export function levelKeyFor(f: LevelField, input: string): string | null {
  if (input.trim() === '') return null
  let v = Number(input)
  if (!Number.isFinite(v)) return null
  v = Math.min(f.max, Math.max(f.min, f.int ? Math.round(v) : v))
  if (f.dflt !== null && Math.abs(v - f.dflt) < 1e-12) return null
  return String(+v.toFixed(6))
}

/** worldspawn `_ambient` "r g b" as a colour (0..1 per channel, the brightest channel 1) and an intensity (its brightest channel). */
export function ambientOf(world: Entity | undefined): { color: [number, number, number]; intensity: number } {
  const parts = (world?.keyvalues?._ambient ?? '').trim().split(/\s+/).map(Number)
  if (parts.length !== 3 || !parts.every((x) => Number.isFinite(x) && x >= 0)) return { color: [1, 1, 1], intensity: 0 }
  const k = Math.max(...parts)
  return k > 0 ? { color: parts.map((x) => x / k) as [number, number, number], intensity: k } : { color: [1, 1, 1], intensity: 0 }
}

/** The `_ambient` value for a colour and intensity; null (remove the key) when the intensity is 0. */
export function ambientKey(color: [number, number, number], intensity: number): string | null {
  if (!(intensity > 0)) return null
  return color.map((c) => String(+(Math.max(0, c) * intensity).toFixed(6))).join(' ')
}

/** #rrggbb <-> 0..1 colour (sRGB bytes as stored: the light colour keys of this editor are plain 0..1 values, like `color`). */
export const hexOf = (c: [number, number, number]): string => '#' + c.map((x) => Math.round(Math.min(1, Math.max(0, x)) * 255).toString(16).padStart(2, '0')).join('')
export const colorOfHex = (hex: string): [number, number, number] => {
  const m = /^#?([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(hex)
  return m ? [parseInt(m[1], 16) / 255, parseInt(m[2], 16) / 255, parseInt(m[3], 16) / 255] : [1, 1, 1]
}
