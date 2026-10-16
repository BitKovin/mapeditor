/** True when a screen position is outside the browser window (a drag ended elsewhere on the desktop). */
export function isOutsideWindow(sx: number, sy: number, w: Pick<Window, 'screenX' | 'screenY' | 'outerWidth' | 'outerHeight'> = window): boolean {
  if (sx === 0 && sy === 0) return false // some browsers report 0,0 when they do not know the position: do not guess
  return sx < w.screenX || sy < w.screenY || sx > w.screenX + w.outerWidth || sy > w.screenY + w.outerHeight
}
