// Editor preferences (Settings > Editor). Core stores them in settings.json as an opaque object (`editor`); this module owns their
// meaning: defaults, validation of whatever the file holds, and what a new viewport starts with.
import type { ViewportSettings } from '../render/viewportSettings'
import { GRID_SIZES, ROT_SNAPS, SCALE_SNAPS } from '../render/viewportSettings'

export interface EditorPrefs {
  /** new viewports */
  gridSize: number; snapEnabled: boolean; rotSnap: number; scaleSnap: number; flySpeed: number; showGrid: boolean
  /** at start-up */
  uvLock: boolean; autoDetail: boolean
}

export const DEFAULT_PREFS: EditorPrefs = { gridSize: 0.5, snapEnabled: true, rotSnap: 15, scaleSnap: 0.1, flySpeed: 8, showGrid: true, uvLock: true, autoDetail: true }

/** The preferences in `raw` (from settings.json), anything missing or out of range replaced by its default. */
export function editorPrefs(raw: Record<string, unknown> | null | undefined): EditorPrefs {
  const r = raw ?? {}
  const num = (k: keyof EditorPrefs, ok: (v: number) => boolean) => (typeof r[k] === 'number' && ok(r[k] as number) ? (r[k] as number) : (DEFAULT_PREFS[k] as number))
  const bool = (k: keyof EditorPrefs) => (typeof r[k] === 'boolean' ? (r[k] as boolean) : (DEFAULT_PREFS[k] as boolean))
  return {
    gridSize: num('gridSize', (v) => GRID_SIZES.includes(v)), snapEnabled: bool('snapEnabled'), rotSnap: num('rotSnap', (v) => ROT_SNAPS.includes(v)),
    scaleSnap: num('scaleSnap', (v) => SCALE_SNAPS.includes(v)), flySpeed: num('flySpeed', (v) => v >= 0.1 && v <= 10000), showGrid: bool('showGrid'),
    uvLock: bool('uvLock'), autoDetail: bool('autoDetail'),
  }
}

/** The part of the preferences a viewport takes over when it is created. */
export function viewportDefaults(p: EditorPrefs): Partial<ViewportSettings> {
  return { gridSize: p.gridSize, snapEnabled: p.snapEnabled, rotSnap: p.rotSnap, scaleSnap: p.scaleSnap, flySpeed: p.flySpeed, showGrid: p.showGrid }
}
