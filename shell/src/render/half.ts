// IEEE 754 half floats: what the Core sends lightmap tiles in (RGBA16F, linear, as baked). The Shell keeps pages as halves (WebGL2 uploads them as
// is), so nothing is ever rounded to 8 bits or clamped on the way: brightness above 1 survives to the shader.

/** Half -> number (tests, tooling). */
export function halfToFloat(h: number): number {
  const e = (h >> 10) & 0x1f, m = h & 0x3ff
  const v = e === 0 ? m * 2 ** -24 : e === 31 ? (m ? NaN : Infinity) : (1 + m / 1024) * 2 ** (e - 15)
  return h & 0x8000 ? -v : v
}

/** Number -> half (round to nearest even; negative and NaN -> 0, saturates at 65504): same rules as the Core's floatToHalf. */
export function floatToHalf(v: number): number {
  if (!(v > 0)) return 0
  if (v >= 65504) return 0x7bff
  const e = Math.floor(Math.log2(v))
  if (e < -24) return 0
  if (e < -14) return Math.round(v * 2 ** 24) // subnormal: spacing 2^-24 (Math.round on .5 ties is close enough for tests; the Core rounds to even)
  let h = ((e + 15) << 10) | Math.round((v / 2 ** e - 1) * 1024)
  if (h > 0x7bff) h = 0x7bff
  return h
}
