// Smooth edges (REQUIREMENTS §33 H6). A brush keeps the edges marked smooth as pairs of face ids (`smooth: [[faceA, faceB], ...]`): face ids survive
// the editing tools (clipper, face drag, mesh tools), vertex indices do not. A pair smooths every edge its two faces share; a pair whose faces no
// longer meet does nothing (Core: core/src/smoothing.cpp, which shades and exports from it).
import type { Brush } from './document'
import { buildTopology, edgeKey, parseEdge } from './meshTopology'

export type FacePair = [number, number]

const isPair = (x: unknown): x is FacePair => Array.isArray(x) && x.length === 2 && x.every((n) => Number.isInteger(n) && n >= 0)
const pairKey = (a: number, b: number): string => (a < b ? `${a}|${b}` : `${b}|${a}`)

/** The brush's marked face pairs (malformed entries dropped). */
export function smoothPairs(b: Brush): FacePair[] {
  return Array.isArray(b.smooth) ? (b.smooth as unknown[]).filter(isPair) : []
}

/** Vertex edge keys (`a-b`) of every smooth edge of the brush. */
export function smoothEdgeKeys(b: Brush): Set<string> {
  const pairs = new Set(smoothPairs(b).map(([x, y]) => pairKey(x, y)))
  const out = new Set<string>()
  if (!pairs.size) return out
  for (const [k, e] of buildTopology(b).edges) {
    if (e.faces.length !== 2) continue
    if (pairs.has(pairKey(b.faces[e.faces[0]].id, b.faces[e.faces[1]].id))) out.add(k)
  }
  return out
}

/**
 * Marks (`on`) or clears the given vertex edges (`a-b`): each edge names the two faces that meet there. Pairs whose faces no longer exist or no longer
 * meet are dropped on the way. Returns the brush itself when nothing changed.
 */
export function markSmooth(b: Brush, edgeKeys: Iterable<string>, on: boolean): Brush {
  const t = buildTopology(b)
  const meets = new Set<string>()
  for (const e of t.edges.values()) if (e.faces.length === 2) meets.add(pairKey(b.faces[e.faces[0]].id, b.faces[e.faces[1]].id))
  const cur = new Map<string, FacePair>()
  for (const [x, y] of smoothPairs(b)) if (meets.has(pairKey(x, y))) cur.set(pairKey(x, y), x < y ? [x, y] : [y, x])
  for (const k of edgeKeys) {
    const [a, c] = parseEdge(k)
    const e = t.edges.get(edgeKey(a, c))
    if (!e || e.faces.length !== 2) continue
    const x = b.faces[e.faces[0]].id, y = b.faces[e.faces[1]].id
    if (on) cur.set(pairKey(x, y), x < y ? [x, y] : [y, x])
    else cur.delete(pairKey(x, y))
  }
  const next = [...cur.values()].sort((p, q) => p[0] - q[0] || p[1] - q[1])
  const before = smoothPairs(b)
  if (next.length === before.length && next.every((p, i) => p[0] === before[i][0] && p[1] === before[i][1]) && (next.length > 0 || b.smooth === undefined)) return b
  const { smooth: _old, ...rest } = b
  void _old
  return next.length ? { ...(rest as Brush), smooth: next } : (rest as Brush)
}

/** Every edge between two faces both in `faceIds` marked smooth (used by shapes: the sides of a cylinder, a whole sphere). */
export function smoothBetween(b: Brush, faceIds: Iterable<number>): Brush {
  const ids = new Set(faceIds)
  const keys: string[] = []
  for (const [k, e] of buildTopology(b).edges) if (e.faces.length === 2 && ids.has(b.faces[e.faces[0]].id) && ids.has(b.faces[e.faces[1]].id)) keys.push(k)
  return markSmooth(b, keys, true)
}
