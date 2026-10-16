// Minimal TGA decoder (uncompressed + RLE; 8-bit grey, 15/16-bit, 24-bit, 32-bit; colour-mapped
// not supported). Browsers can't decode TGA, and Quake 3 maps use it heavily.
export interface Rgba { width: number; height: number; data: Uint8ClampedArray }

export function decodeTga(bytes: Uint8Array): Rgba {
  if (bytes.length < 18) throw new Error('TGA: file too short')
  const idLen = bytes[0], cmType = bytes[1], type = bytes[2]
  const width = bytes[12] | (bytes[13] << 8), height = bytes[14] | (bytes[15] << 8)
  const bpp = bytes[16], desc = bytes[17]
  if (cmType !== 0 || ![2, 3, 10, 11].includes(type)) throw new Error(`TGA: unsupported type ${type}`)
  if (![8, 16, 24, 32].includes(bpp)) throw new Error(`TGA: unsupported bit depth ${bpp}`)
  if (!width || !height || width * height > 64 * 1024 * 1024) throw new Error('TGA: bad dimensions')
  const bytesPP = bpp / 8, rle = type >= 9
  const px = new Uint8Array(width * height * bytesPP)
  let src = 18 + idLen
  if (!rle) {
    if (bytes.length < src + px.length) throw new Error('TGA: truncated')
    px.set(bytes.subarray(src, src + px.length))
  } else {
    let dst = 0
    while (dst < px.length) {
      if (src >= bytes.length) throw new Error('TGA: truncated RLE data')
      const h = bytes[src++], count = (h & 0x7f) + 1
      if (h & 0x80) {
        if (src + bytesPP > bytes.length) throw new Error('TGA: truncated RLE data')
        for (let i = 0; i < count && dst < px.length; i++) for (let k = 0; k < bytesPP; k++) px[dst++] = bytes[src + k]
        src += bytesPP
      } else {
        const n = Math.min(count * bytesPP, px.length - dst)
        if (src + n > bytes.length) throw new Error('TGA: truncated RLE data')
        px.set(bytes.subarray(src, src + n), dst); dst += n; src += n
      }
    }
  }
  const out = new Uint8ClampedArray(width * height * 4)
  const topLeft = (desc & 0x20) !== 0
  for (let y = 0; y < height; y++) {
    const row = topLeft ? y : height - 1 - y
    for (let x = 0; x < width; x++) {
      const s = (y * width + x) * bytesPP, d = (row * width + x) * 4
      if (bpp === 8) out[d] = out[d + 1] = out[d + 2] = px[s], out[d + 3] = 255
      else if (bpp === 16) {
        const v = px[s] | (px[s + 1] << 8)
        out[d] = ((v >> 10) & 31) * 255 / 31; out[d + 1] = ((v >> 5) & 31) * 255 / 31; out[d + 2] = (v & 31) * 255 / 31; out[d + 3] = 255
      } else out[d] = px[s + 2], out[d + 1] = px[s + 1], out[d + 2] = px[s], out[d + 3] = bpp === 32 ? px[s + 3] : 255
    }
  }
  return { width, height, data: out }
}
