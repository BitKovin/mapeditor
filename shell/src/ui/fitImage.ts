/** Where an sw x sh image goes inside a W x H box so it fits whole and keeps its aspect ratio (centred; never enlarged past `maxScale`). */
export function fitRect(sw: number, sh: number, W: number, H: number, maxScale = Infinity): { x: number; y: number; w: number; h: number } {
  if (!(sw > 0 && sh > 0)) return { x: 0, y: 0, w: W, h: H }
  const k = Math.min(W / sw, H / sh, maxScale)
  const w = sw * k, h = sh * k
  return { x: (W - w) / 2, y: (H - h) / 2, w, h }
}

/** Draws `src` (sw x sh) into the whole of `g`'s W x H canvas: background, then the image fitted and centred. */
export function drawFitted(g: CanvasRenderingContext2D, src: CanvasImageSource, sw: number, sh: number, W: number, H: number, background = '#2a2a2a'): void {
  g.fillStyle = background
  g.fillRect(0, 0, W, H)
  const r = fitRect(sw, sh, W, H)
  g.imageSmoothingQuality = 'medium'
  g.drawImage(src, r.x, r.y, r.w, r.h)
}
