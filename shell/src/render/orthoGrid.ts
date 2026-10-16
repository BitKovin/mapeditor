// Grid for the orthographic views: lines in the plane through the world origin that faces the camera, covering the visible
// area. The step is the grid size, doubled until neighbouring lines are at least `minPx` apart.
import type { OrthoCamera } from './camera'

export interface OrthoGridLines { minor: number[]; major: number[]; axisH: number[]; axisV: number[]; step: number }

export function orthoGrid(cam: OrthoCamera, gridSize: number, viewportHeightPx: number, aspect: number, minPx = 8): OrthoGridLines {
  const { right, up, forward } = cam.basis
  const mpp = cam.metresPerPixel(viewportHeightPx)
  let step = Math.max(1e-3, gridSize)
  while (step / mpp < minPx) step *= 2
  // visible extent along the screen axes, centred on the target
  const hh = cam.halfHeight, hw = hh * aspect
  const cr = cam.target[0] * right[0] + cam.target[1] * right[1] + cam.target[2] * right[2]
  const cu = cam.target[0] * up[0] + cam.target[1] * up[1] + cam.target[2] * up[2]
  const pt = (r: number, u: number): number[] => [right[0] * r + up[0] * u, right[1] * r + up[1] * u, right[2] * r + up[2] * u].map((x, i) => (forward[i] !== 0 ? 0 : x))
  const out: OrthoGridLines = { minor: [], major: [], axisH: [], axisV: [], step }
  const r0 = Math.floor((cr - hw) / step), r1 = Math.ceil((cr + hw) / step)
  const u0 = Math.floor((cu - hh) / step), u1 = Math.ceil((cu + hh) / step)
  if ((r1 - r0 + 1) * (u1 - u0 + 1) > 20000) return out
  const major = 10 // every 10th line
  for (let i = r0; i <= r1; i++) {
    const t = i === 0 ? out.axisV : i % major === 0 ? out.major : out.minor
    t.push(...pt(i * step, cu - hh - step), ...pt(i * step, cu + hh + step))
  }
  for (let j = u0; j <= u1; j++) {
    const t = j === 0 ? out.axisH : j % major === 0 ? out.major : out.minor
    t.push(...pt(cr - hw - step, j * step), ...pt(cr + hw + step, j * step))
  }
  return out
}
