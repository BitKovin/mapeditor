// The game's export format (REQUIREMENTS §31 X2): game_profile.json "exporter", announced by the Core's `profile` message.
import type { DialogFilter } from '../net/protocol'

export interface ExportFormat { id: string; name: string; extension: string }

export const RBSP_FORMAT: ExportFormat = { id: 'rbsp', name: 'rBSP', extension: '.bsp' }

/** The format the profile message names; rBSP from an older Core that does not send one. */
export function formatFromProfile(m: { exporter?: unknown; exporters?: unknown }): ExportFormat {
  const list = Array.isArray(m.exporters) ? m.exporters.filter((e): e is ExportFormat =>
    !!e && typeof e.id === 'string' && typeof e.name === 'string' && typeof e.extension === 'string') : []
  return list.find((e) => e.id === m.exporter) ?? RBSP_FORMAT
}

/** `p` with the format's extension instead of a map's or another export's ("maps/a.map.json" -> "maps/a.obj"). */
export function withFormatExt(p: string, f: ExportFormat): string {
  if (p.toLowerCase().endsWith(f.extension)) return p
  return p.replace(/\.(map\.)?json$|\.map$|\.[a-z0-9]{1,8}$/i, '') + f.extension
}

export const formatFilters = (f: ExportFormat): DialogFilter[] => [{ name: `${f.name} (*${f.extension})`, pattern: `*${f.extension}` }]
