// Loop, ring and path selection for Mesh mode (REQUIREMENTS §21 E1). Pure: brush + picked element -> the elements to select.
// Edges are `a-b` keys (a < b), faces are ids, vertices are indices, as everywhere in meshTopology.
import type { Brush } from './document'
import { MeshSel, SelMode, Topology, buildTopology, edgeKey, edgeLength, faceById, parseEdge, selFromEdges, selFromFaces, selFromVerts } from './meshTopology'
import { edgeRing } from './meshOps'

/** Undirected edges at every vertex. */
function vertexEdges(t: Topology): Map<number, string[]> {
  const out = new Map<number, string[]>()
  for (const [k, e] of t.edges) for (const v of [e.a, e.b]) { const l = out.get(v); if (l) l.push(k); else out.set(v, [k]) }
  return out
}

/**
 * The edge loop through `edge`: from each end, walk on through a vertex with exactly four edges to the edge that shares no face with
 * the one we arrived on (Blender's rule); along an open border, walk on to the other border edge. Stops anywhere else.
 */
export function edgeLoop(b: Brush, edge: string): string[] {
  const t = buildTopology(b)
  if (!t.edges.has(edge)) return []
  const ve = vertexEdges(t)
  const next = (cur: string, at: number): string | null => {
    const curFaces = t.edges.get(cur)!.faces
    const around = (ve.get(at) ?? []).filter((k) => k !== cur)
    if (curFaces.length === 1) { // open border: the other border edge at this vertex, if there is exactly one
      const border = around.filter((k) => t.edges.get(k)!.faces.length === 1)
      return border.length === 1 ? border[0] : null
    }
    if (around.length !== 3) return null
    const opposite = around.filter((k) => !t.edges.get(k)!.faces.some((f) => curFaces.includes(f)))
    return opposite.length === 1 ? opposite[0] : null
  }
  const out = [edge], seen = new Set(out)
  const [a0, b0] = parseEdge(edge)
  for (const start of [b0, a0]) {
    let cur = edge, at = start
    for (let guard = 0; guard < 100000; guard++) {
      const n = next(cur, at)
      if (!n || seen.has(n)) break
      seen.add(n)
      if (start === b0) out.push(n); else out.unshift(n)
      const [p, q] = parseEdge(n)
      at = p === at ? q : p; cur = n
    }
  }
  return out
}

/** The edges a loop cut through `edge` would cross (the edge ring). */
export function edgeRingEdges(b: Brush, edge: string): string[] {
  const r = edgeRing(b, edge)
  return r ? r.ring.map((pq) => { const [p, q] = pq.split('>').map(Number); return edgeKey(p, q) }) : []
}

/** The face loop that crosses `edge` of face `faceId`: the quads its ring passes through (the face itself always). */
export function faceLoop(b: Brush, faceId: number, edge: string): number[] {
  const f = faceById(b, faceId)
  if (!f) return []
  const ring = new Set(edgeRingEdges(b, edge))
  const out = [faceId]
  for (const g of b.faces) {
    if (g.id === faceId || g.loop.length !== 4) continue
    const hits = g.loop.filter((v, i) => ring.has(edgeKey(v, g.loop[(i + 1) % 4]))).length
    if (hits >= 2) out.push(g.id)
  }
  return out
}

/** Shortest chain of vertices from `from` to `to` along edges (by length), both ends included; [] when not connected. */
export function shortestVertexPath(b: Brush, from: number, to: number): number[] {
  if (from === to) return [from]
  const t = buildTopology(b), ve = vertexEdges(t)
  const dist = new Map<number, number>([[from, 0]]), prev = new Map<number, number>(), done = new Set<number>()
  // a brush has few vertices: a plain O(n^2) Dijkstra is enough
  for (;;) {
    let u = -1, best = Infinity
    for (const [v, d] of dist) if (!done.has(v) && d < best) { best = d; u = v }
    if (u < 0) return []
    if (u === to) break
    done.add(u)
    for (const k of ve.get(u) ?? []) {
      const [p, q] = parseEdge(k), w = p === u ? q : p, d = best + edgeLength(b, k)
      if (d < (dist.get(w) ?? Infinity) - 1e-12) { dist.set(w, d); prev.set(w, u) }
    }
  }
  const path = [to]
  while (path[0] !== from) path.unshift(prev.get(path[0])!)
  return path
}

/** Edges of the shortest connection between two edges: both edges and the shortest vertex chain joining their nearest ends. */
export function shortestEdgePath(b: Brush, from: string, to: string): string[] {
  if (from === to) return [from]
  let best: number[] | null = null, bestLen = Infinity
  for (const a of parseEdge(from)) for (const c of parseEdge(to)) {
    const p = shortestVertexPath(b, a, c)
    if (!p.length) continue
    let len = 0
    for (let i = 1; i < p.length; i++) len += edgeLength(b, edgeKey(p[i - 1], p[i]))
    if (len < bestLen - 1e-12) { bestLen = len; best = p }
  }
  if (!best) return []
  const out = [from]
  for (let i = 1; i < best.length; i++) { const k = edgeKey(best[i - 1], best[i]); if (!out.includes(k)) out.push(k) }
  if (!out.includes(to)) out.push(to)
  return out
}

/** Fewest faces from `from` to `to`, stepping across shared edges, both ends included; [] when not connected. */
export function shortestFacePath(b: Brush, from: number, to: number): number[] {
  if (from === to) return [from]
  const t = buildTopology(b)
  const idx = new Map(b.faces.map((f, i) => [f.id, i]))
  const s = idx.get(from), g = idx.get(to)
  if (s === undefined || g === undefined) return []
  const prev = new Map<number, number>([[s, -1]]), queue = [s]
  while (queue.length) {
    const fi = queue.shift()!
    if (fi === g) break
    const f = b.faces[fi]
    for (let i = 0; i < f.loop.length; i++) {
      for (const n of t.edges.get(edgeKey(f.loop[i], f.loop[(i + 1) % f.loop.length]))?.faces ?? []) {
        if (!prev.has(n)) { prev.set(n, fi); queue.push(n) }
      }
    }
  }
  if (!prev.has(g)) return []
  const path = [g]
  while (prev.get(path[0])! >= 0) path.unshift(prev.get(path[0])!)
  return path.map((i) => b.faces[i].id)
}

// ---- what a click in Mesh mode selects ----------------------------------------------------------
/** What is under the cursor: the vertex / edge near it (screen space), the face hit, and that face's edge nearest to the cursor. */
export interface MeshPickAt { vertex: number | null; edge: string | null; face: number | null; faceEdge: string | null }
/** The last element clicked (the start of a Ctrl+click path). */
export type ActiveElem = { mode: SelMode; id: number | string } | null
export interface ClickMods { shift: boolean; ctrl: boolean; alt: boolean }

const elems = (s: MeshSel, mode: SelMode): (number | string)[] => (mode === 'vertex' ? s.verts : mode === 'edge' ? s.edges : s.faces)
function selOf(b: Brush, mode: SelMode, ids: (number | string)[]): MeshSel {
  return mode === 'vertex' ? selFromVerts(b, ids as number[]) : mode === 'edge' ? selFromEdges(b, ids as string[]) : selFromFaces(b, ids as number[])
}

/**
 * The selection after a click (REQUIREMENTS §21 E1). `loop` = Alt held or a double-click. Alt: loop (Ctrl+Alt: ring), replacing the
 * selection unless Shift; Ctrl: shortest path from the active element, added to the selection; plain: the element (Shift toggles it).
 * Returns null when nothing usable is under the cursor (the caller then starts a box selection).
 */
export function clickSelect(b: Brush, mode: SelMode, at: MeshPickAt, mods: ClickMods, cur: MeshSel, active: ActiveElem, loop = mods.alt): { sel: MeshSel; active: ActiveElem } | null {
  const id: number | string | null = mode === 'vertex' ? at.vertex : mode === 'edge' ? at.edge : at.face
  if (loop) {
    let ids: (number | string)[]
    if (mode === 'face') {
      if (at.face === null || !at.faceEdge) return null
      ids = faceLoop(b, at.face, at.faceEdge)
    } else {
      if (!at.edge) return null
      const edges = mods.ctrl ? edgeRingEdges(b, at.edge) : edgeLoop(b, at.edge)
      ids = mode === 'edge' ? edges : [...new Set(edges.flatMap(parseEdge))]
    }
    const merged = mods.shift ? [...new Set([...elems(cur, mode), ...ids])] : ids
    return { sel: selOf(b, mode, merged), active: id !== null ? { mode, id } : active }
  }
  if (id === null) return null
  if (mods.ctrl) {
    const from = active && active.mode === mode ? active.id : null
    const path: (number | string)[] = from === null ? [id]
      : mode === 'vertex' ? shortestVertexPath(b, from as number, id as number)
      : mode === 'edge' ? shortestEdgePath(b, from as string, id as string)
      : shortestFacePath(b, from as number, id as number)
    return { sel: selOf(b, mode, [...new Set([...elems(cur, mode), ...(path.length ? path : [id])])]), active: { mode, id } }
  }
  if (mods.shift) {
    const have = elems(cur, mode), on = have.includes(id)
    return { sel: selOf(b, mode, on ? have.filter((x) => x !== id) : [...have, id]), active: on ? null : { mode, id } }
  }
  return { sel: selOf(b, mode, [id]), active: { mode, id } }
}
