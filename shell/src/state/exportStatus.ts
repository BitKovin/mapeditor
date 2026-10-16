// BSP export status for the status bar (REQUIREMENTS §25 E9): what the Core reports while it exports and when it is done.
import type { Inbound } from '../net/protocol'

export type ExportProgress = Extract<Inbound, { type: 'export_progress' }>
export type ExportResult = Extract<Inbound, { type: 'export_result' }>

export const progressMessage = (m: ExportProgress): string => `Exporting BSP: ${m.stage} ${Math.round(Math.min(1, Math.max(0, m.fraction)) * 100)}%`

export function resultMessage(m: ExportResult): string {
  if (m.cancelled) return 'BSP export cancelled'
  if (!m.ok) return `BSP export failed: ${m.error}`
  const s = m.stats
  const parts = [`${s.surfaces} surfaces`, `${s.brushes} brushes`, `${s.lightmapPages} lightmap page${s.lightmapPages === 1 ? '' : 's'}`, `${s.clusters} vis cells of ${fmt(s.visCell)} m`]
  if (s.gridPoints) parts.push(`light grid ${s.gridPoints} points`)
  const warn = m.warnings.length ? ` · ${m.warnings.length} warning${m.warnings.length === 1 ? '' : 's'}: ${m.warnings.slice(0, 3).join('; ')}${m.warnings.length > 3 ? '; …' : ''}` : ''
  return `Exported ${m.bsp} (lightmaps in ${m.lightmaps}/) in ${fmt(m.seconds)} s: ${parts.join(', ')}${warn}`
}

const fmt = (v: number): string => (Math.round(v * 10) / 10).toString()
