// Does a texture tile? (REQUIREMENTS §19, edge handling.) With a REPEAT sampler the bilinear filter blends the last row with the first one at a tile
// edge. For a seamless texture that is exactly right; for a texture whose edges do not continue each other (a wall with a dark bottom row) it draws
// a line. We compare how different the two opposite edge lines are with how different adjacent interior lines normally are.
export interface EdgeSamples {
  /** First and last line of the axis (RGBA bytes along the line). */
  first: ArrayLike<number>
  last: ArrayLike<number>
  /** Adjacent line pairs from the interior (the "normal" difference between neighbouring lines). */
  interior: [ArrayLike<number>, ArrayLike<number>][]
}

/** Mean absolute RGB difference of two RGBA lines (0..255). */
export function lineDifference(a: ArrayLike<number>, b: ArrayLike<number>): number {
  const n = Math.min(a.length, b.length) >> 2
  if (n === 0) return 0
  let s = 0
  for (let i = 0; i < n; i++) s += Math.abs(a[4 * i] - b[4 * i]) + Math.abs(a[4 * i + 1] - b[4 * i + 1]) + Math.abs(a[4 * i + 2] - b[4 * i + 2])
  return s / (3 * n)
}

/** true = the opposite edges continue each other (seamless, wrapping is right); false = they are borders (clamp). */
export function edgesWrap(s: EdgeSamples): boolean {
  if (s.interior.length === 0) return true
  const seam = lineDifference(s.first, s.last)
  const base = s.interior.reduce((acc, [a, b]) => acc + lineDifference(a, b), 0) / s.interior.length
  return !(seam > 1.8 * base + 8)
}
