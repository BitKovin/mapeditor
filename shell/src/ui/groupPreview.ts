// Draws a texture group on a canvas as a wall of a chosen size (REQUIREMENTS §20 T8). It is drawn from the same solver and the same cell maps as the
// viewports (model/layout.ts, model/layoutPlan.ts), so the preview shows exactly the layout a face gets. Not covered: relief depth (see the viewports).
import { LayoutResult, solveLayout, Facing } from '../model/layout'
import { cellMap } from '../model/layoutPlan'
import { previewPatternMatrix, previewScale } from '../model/groupTools'
import { ParamValue, TexGroup } from '../model/texgroup'
import { naturalSize, texEnv } from '../model/texprops'
import type { TextureCache } from '../state/textures'

export interface PreviewState { w: number; h: number; facing: Facing; params?: Record<string, ParamValue>; seed: number }
export interface PreviewOpts { outlines?: boolean; labels?: boolean; margin?: number; /** index of a cell to mark */ highlight?: number }

const tiltFor = (f: Facing): number => (f === 'up' ? 0 : f === 'down' ? 180 : 90)
const hue = (s: string): number => { let x = 0; for (let i = 0; i < s.length; i++) x = (x * 31 + s.charCodeAt(i)) % 360; return x }

/** Returns the solved layout (its warnings are shown by the caller). */
export function drawGroupPreview(canvas: HTMLCanvasElement, group: TexGroup, st: PreviewState, textures: TextureCache, opts: PreviewOpts = {}): LayoutResult {
  const g = canvas.getContext('2d')
  const result = solveLayout({ group, params: st.params, w: st.w, h: st.h, facing: st.facing, tilt: tiltFor(st.facing), seed: st.seed, info: (p) => texEnv.info(p) })
  if (!g) return result
  const m = opts.margin ?? 8
  g.setTransform(1, 0, 0, 1, 0, 0)
  g.fillStyle = '#161616'; g.fillRect(0, 0, canvas.width, canvas.height)
  const s = previewScale(canvas.width, canvas.height, st.w, st.h, m)
  if (!(s > 0)) return result
  g.translate(m, m)
  for (const c of result.cells) {
    const x = s * c.x0, y = s * (st.h - c.y1), w = s * (c.x1 - c.x0), h = s * (c.y1 - c.y0)
    const info = texEnv.info(c.texture), tex = textures.get(c.texture)
    g.save()
    g.beginPath(); g.rect(x, y, w, h); g.clip()
    if (info && tex.status === 'ready') {
      try {
        const pat = g.createPattern(tex.image.source as CanvasImageSource, 'repeat')
        if (pat) { pat.setTransform(new DOMMatrix(previewPatternMatrix(cellMap(c, naturalSize(info), st.w, st.h), info.width, info.height, s, st.h))); g.fillStyle = pat; g.fillRect(x, y, w, h) }
      } catch { g.fillStyle = '#444'; g.fillRect(x, y, w, h) }
    } else { g.fillStyle = tex.status === 'missing' ? '#5a2a2a' : `hsl(${hue(c.slot || c.texture)} 25% 28%)`; g.fillRect(x, y, w, h) }
    g.restore()
    if (opts.outlines !== false) { g.strokeStyle = 'rgba(255,255,255,.35)'; g.lineWidth = 1; g.strokeRect(x + 0.5, y + 0.5, w - 1, h - 1) }
    if (opts.labels && w > 34 && h > 14) { g.fillStyle = 'rgba(0,0,0,.6)'; g.fillRect(x + 2, y + 2, Math.min(w - 4, g.measureText(c.slot).width + 6), 13); g.fillStyle = '#fff'; g.font = '10px sans-serif'; g.fillText(c.slot || 'fallback', x + 5, y + 12) }
  }
  const hc = opts.highlight !== undefined ? result.cells[opts.highlight] : undefined
  if (hc) { g.strokeStyle = '#ffd34d'; g.lineWidth = 2.5; g.strokeRect(s * hc.x0 + 1, s * (st.h - hc.y1) + 1, s * (hc.x1 - hc.x0) - 2, s * (hc.y1 - hc.y0) - 2) }
  g.strokeStyle = '#0a84e8'; g.lineWidth = 1.5; g.strokeRect(0, 0, s * st.w, s * st.h)
  g.setTransform(1, 0, 0, 1, 0, 0)
  return result
}
