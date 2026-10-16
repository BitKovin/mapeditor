// Picking inside one brush for mesh edit mode: nearest vertex / edge under the cursor in screen space, the
// face under a ray, and rectangle selection. Editing is "x-ray": hidden elements can be picked too, like
// Blender's X-ray mode, so selecting what you cannot see never needs a camera move.
import type { Brush } from '../model/document'
import { triangulateFace } from '../model/geometry'
import { Vec3 } from '../model/math'
import { MeshSel, SelMode, buildTopology, edgeKey, parseEdge, selFromEdges, selFromFaces, selFromVerts } from '../model/meshTopology'
import { rayTriangle } from '../render/picking'
import type { Ray } from '../render/camera'

export interface Screen { x: number; y: number; depth: number }
export type Project = (p: Vec3) => Screen | null

export function pickVertex(b: Brush, project: Project, mx: number, my: number, radius = 10): number | null {
  let best: number | null = null, bestD = radius * radius
  b.vertices.forEach((p, i) => {
    const s = project(p)
    if (!s) return
    const d = (s.x - mx) ** 2 + (s.y - my) ** 2
    if (d < bestD) { bestD = d; best = i }
  })
  return best
}

export function pickEdge(b: Brush, project: Project, mx: number, my: number, radius = 8, only?: Set<string>): string | null {
  let best: string | null = null, bestD = radius * radius
  for (const [k, e] of buildTopology(b).edges) {
    if (only && !only.has(k)) continue
    const a = project(b.vertices[e.a]), c = project(b.vertices[e.b])
    if (!a || !c) continue
    const dx = c.x - a.x, dy = c.y - a.y, l2 = dx * dx + dy * dy
    const t = l2 < 1e-9 ? 0 : Math.max(0, Math.min(1, ((mx - a.x) * dx + (my - a.y) * dy) / l2))
    const d = (a.x + dx * t - mx) ** 2 + (a.y + dy * t - my) ** 2
    if (d < bestD) { bestD = d; best = k }
  }
  return best
}

/** The edge of face `faceId` nearest to the cursor on screen, however far (face loop selection picks its direction by it). */
export function nearestFaceEdge(b: Brush, faceId: number, project: Project, mx: number, my: number): string | null {
  const f = b.faces.find((x) => x.id === faceId)
  if (!f) return null
  return pickEdge(b, project, mx, my, Infinity, new Set(f.loop.map((v, i) => edgeKey(v, f.loop[(i + 1) % f.loop.length]))))
}

/** Id of the nearest face along the ray (front-most, as in any solid view). */
export function pickFaceId(b: Brush, ray: Ray): number | null {
  let best: number | null = null, bestT = Infinity
  for (const f of b.faces)
    for (const [i, j, k] of triangulateFace(b, f)) {
      const t = rayTriangle(ray, b.vertices[i], b.vertices[j], b.vertices[k])
      if (t !== null && t < bestT) { bestT = t; best = f.id }
    }
  return best
}

/** Everything of the current element kind whose screen position lies in the rectangle. */
export function boxSelect(b: Brush, project: Project, mode: SelMode, x0: number, y0: number, x1: number, y1: number): MeshSel {
  const [lx, hx] = [Math.min(x0, x1), Math.max(x0, x1)], [ly, hy] = [Math.min(y0, y1), Math.max(y0, y1)]
  const inside = (p: Vec3): boolean => { const s = project(p); return !!s && s.x >= lx && s.x <= hx && s.y >= ly && s.y <= hy }
  if (mode === 'vertex') return selFromVerts(b, b.vertices.map((p, i) => (inside(p) ? i : -1)).filter((i) => i >= 0))
  if (mode === 'edge') {
    const keys: string[] = []
    for (const k of buildTopology(b).edges.keys()) { const [a, c] = parseEdge(k); if (inside(b.vertices[a]) && inside(b.vertices[c])) keys.push(edgeKey(a, c)) }
    return selFromEdges(b, keys)
  }
  const ids = b.faces.filter((f) => f.loop.every((v) => inside(b.vertices[v]))).map((f) => f.id)
  return selFromFaces(b, ids)
}

/** Union / toggle of two selections in the given element mode (shift = toggle, like Blender's extend). */
export function combineSel(b: Brush, base: MeshSel, add: MeshSel, mode: SelMode, toggle: boolean): MeshSel {
  const pick = (m: MeshSel): (string | number)[] => (mode === 'vertex' ? m.verts : mode === 'edge' ? m.edges : m.faces)
  const cur = new Set(pick(base)), next = pick(add)
  const out = new Set(cur)
  for (const x of next) { if (toggle && cur.has(x)) out.delete(x); else out.add(x) }
  const list = [...out]
  return mode === 'vertex' ? selFromVerts(b, list as number[]) : mode === 'edge' ? selFromEdges(b, list as string[]) : selFromFaces(b, list as number[])
}
