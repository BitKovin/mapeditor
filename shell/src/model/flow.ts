// Flow (REQUIREMENTS §6 B5, user's example: a curved road). The UVs of an already textured face continue across a shared edge onto its neighbour, as if the
// neighbour were unfolded flat into the first face's plane: nothing is stretched, and the texture meets itself along every edge. The target face (and the
// faces in between on the shortest chain of neighbours) receive the same texture and an editable projection of their own ('free' kind).
import type { Brush, Face } from './document'
import { faceNormal } from './geometry'
import type { Vec3 } from './math'
import { projectionFromAffine } from './uv'
import { Aff, affineFromCorners, faceAffine } from './uvmesh'

const ev = (a: Aff, p: Vec3): number => a[0] * p[0] + a[1] * p[1] + a[2] * p[2] + a[3]
const sub3 = (a: Vec3, b: Vec3): Vec3 => [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
const dot3 = (a: Vec3, b: Vec3): number => a[0] * b[0] + a[1] * b[1] + a[2] * b[2]

/** Faces sharing a full edge with each face (by vertex index pairs). */
function neighbours(b: Brush): Map<number, { face: number; edge: [number, number] }[]> {
  const byEdge = new Map<string, number[]>()
  for (const f of b.faces) f.loop.forEach((v, k) => {
    const w = f.loop[(k + 1) % f.loop.length]
    const key = v < w ? `${v}_${w}` : `${w}_${v}`
    byEdge.set(key, [...(byEdge.get(key) ?? []), f.id])
  })
  const out = new Map<number, { face: number; edge: [number, number] }[]>()
  for (const [key, ids] of byEdge) {
    if (ids.length !== 2) continue
    const [v, w] = key.split('_').map(Number) as [number, number]
    out.set(ids[0], [...(out.get(ids[0]) ?? []), { face: ids[1], edge: [v, w] }])
    out.set(ids[1], [...(out.get(ids[1]) ?? []), { face: ids[0], edge: [v, w] }])
  }
  return out
}

/** UVs of face `B` unfolded across the edge it shares with the solved face `A` (whose map is `m`). */
function unfold(b: Brush, A: Face, B: Face, edge: [number, number], m: { u: Aff; v: Aff }): [number, number][] {
  const p = b.vertices[edge[0]], q = b.vertices[edge[1]]
  const el = Math.hypot(...sub3(q, p))
  const e: Vec3 = [(q[0] - p[0]) / el, (q[1] - p[1]) / el, (q[2] - p[2]) / el]
  const c = A.loop.reduce<Vec3>((s, i) => [s[0] + b.vertices[i][0] / A.loop.length, s[1] + b.vertices[i][1] / A.loop.length, s[2] + b.vertices[i][2] / A.loop.length], [0, 0, 0])
  const cp = sub3(c, p), along = dot3(cp, e)
  const inward = sub3(cp, [e[0] * along, e[1] * along, e[2] * along])
  const il = Math.hypot(...inward)
  const mA: Vec3 = [inward[0] / il, inward[1] / il, inward[2] / il]
  return B.loop.map((vi) => {
    const x = b.vertices[vi]
    const xp = sub3(x, p), s = dot3(xp, e)
    const w = sub3(xp, [e[0] * s, e[1] * s, e[2] * s])
    const d = Math.hypot(...w)
    const y: Vec3 = [p[0] + e[0] * s - mA[0] * d, p[1] + e[1] * s - mA[1] * d, p[2] + e[2] * s - mA[2] * d]
    return [ev(m.u, y), ev(m.v, y)] as [number, number]
  })
}

export interface FlowResult { brush: Brush; /** faces that received UVs (the targets and the faces between), in flow order */ flowed: number[] }

/**
 * Continues the UVs of face `sourceId` onto `targetIds` of the same brush along the shortest chains of edge-sharing faces. null when a target cannot be
 * reached (different brush islands) or the source does not exist. The source face is unchanged; flowed faces take the source's texture and repeat override.
 */
export function flowFaces(b: Brush, sourceId: number, targetIds: number[]): FlowResult | null {
  const src = b.faces.find((f) => f.id === sourceId)
  if (!src || targetIds.length === 0) return null
  const adj = neighbours(b)
  // breadth first from the source over the whole brush
  const parent = new Map<number, { from: number; edge: [number, number] }>(), order: number[] = [sourceId], seen = new Set([sourceId])
  for (let i = 0; i < order.length; i++) for (const nb of adj.get(order[i]) ?? []) if (!seen.has(nb.face)) { seen.add(nb.face); parent.set(nb.face, { from: order[i], edge: nb.edge }); order.push(nb.face) }
  const needed = new Set<number>()
  for (const t of targetIds) {
    if (t === sourceId) continue
    if (!seen.has(t)) return null
    for (let f = t; f !== sourceId; f = parent.get(f)!.from) needed.add(f)
  }
  const maps = new Map<number, { u: Aff; v: Aff }>([[sourceId, faceAffine(b, src)]])
  const flowed: number[] = [], byId = new Map<number, Face>()
  for (const id of order) {
    if (!needed.has(id)) continue
    const par = parent.get(id)!
    const A = byId.get(par.from) ?? src, B = b.faces.find((f) => f.id === id)!
    const uvs = unfold(b, A, B, par.edge, maps.get(par.from)!)
    const n = faceNormal(b, B)
    const m = affineFromCorners(B.loop.map((i) => b.vertices[i]), uvs, n, 1e-6)
    if (!m) return null
    maps.set(id, m)
    const proj = projectionFromAffine(n, [m.u[0], m.u[1], m.u[2]], [m.v[0], m.v[1], m.v[2]], m.u[3], m.v[3])
    const nf: Face = { ...B, texture: src.texture, uvs, ...(proj ? { projection: proj } : {}) }
    if (src.repeat) nf.repeat = src.repeat; else delete nf.repeat
    byId.set(id, nf)
    flowed.push(id)
  }
  return { brush: { ...b, faces: b.faces.map((f) => byId.get(f.id) ?? f) }, flowed }
}
