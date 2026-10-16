// Light style playback (core/light_styles.h, from the reference engine): a style is a pattern of letters played at `hz` (10): 'a' = 0, 'm' = 1, 'z' = 25/12.
// Style 0 is the always-on base (1); a style without a pattern is constant 1. The patterns come from the Core (`light_styles` message): the Shell has no table.

export const STYLE_HZ = 10
export const MAX_STYLE_ID = 63

export function styleValue(patterns: readonly string[], style: number, seconds: number, hz = STYLE_HZ): number {
  if (!(style > 0) || style > MAX_STYLE_ID) return 1
  const p = patterns[style]
  if (!p) return 1
  const frame = Math.floor(seconds * hz)
  const c = p.charCodeAt((((frame % p.length) + p.length) % p.length))
  return (c - 97) / 12 // 'a' = 97, 'm' = 109
}

/** True when the style changes over time (a pattern with at least two different letters). */
export function styleAnimates(patterns: readonly string[], style: number): boolean {
  const p = style > 0 && style <= MAX_STYLE_ID ? patterns[style] : ''
  return !!p && new Set(p).size > 1
}
