// Cutting faces apart without moving anything (REQUIREMENTS §21 E2, E5): connect vertices (J), subdivide / connect edges, knife.
// A planar convex face cut along a chord between two of its corners gives two planar convex faces, so a valid brush stays valid.
// The first piece of a face keeps its id; the others get new ids and keep the face's UV map (finish(..., splitFrom)).
import type { Brush, Face } from './document'
import { Vec3, add, scale, sub } from './math'
import { cloneFace, edgeKey, finish, nextFaceId, parseEdge, selFromEdges, selFromVerts } from './meshTopology'
import type { OpResult } from './meshOps'

const fail = (error: string): { error: string } => ({ error })

/**
 * Cuts a face along chords (pairs of vertex indices of its loop). Chords must not cross; each is applied to the piece holding both ends.
 * Chords between neighbours of a piece (already an edge) are skipped. Returns the pieces (the first keeps the face's id) and the chords made.
 */
export function splitFace(f: Face, chords: [number, number][], newId: () => number): { pieces: Face[]; made: [number, number][] } {
  let pieces: Face[] = [f]
  const made: [number, number][] = []
  for (const [a, c] of chords) {
    const k = pieces.findIndex((p) => p.loop.includes(a) && p.loop.includes(c))
    if (k < 0) continue
    const p = pieces[k], n = p.loop.length, i = p.loop.indexOf(a), j = p.loop.indexOf(c)
    if ((i + 1) % n === j || (j + 1) % n === i || i === j) continue
    const arc = (from: number, to: number): number[] => { const out: number[] = []; for (let x = from; ; x = (x + 1) % n) { out.push(p.loop[x]); if (x === to) break } return out }
    const A = arc(i, j), B = arc(j, i)
    pieces = [...pieces.slice(0, k), { ...p, loop: A }, cloneFace(p, newId(), B), ...pieces.slice(k + 1)]
    made.push([a, c])
  }
  return { pieces, made }
}

/** Rebuilds the brush with every face replaced by its pieces; finishes with UV maps kept per piece. */
function rebuild(b: Brush, vertices: Vec3[], perFace: (f: Face, newId: () => number) => Face[], selEdges: (nb: Brush) => string[]): OpResult {
  let next = nextFaceId(b)
  const splitFrom = new Map<number, number>()
  const faces: Face[] = []
  for (const f of b.faces) {
    const pieces = perFace(f, () => next++)
    for (const p of pieces) if (p.id !== f.id) splitFrom.set(p.id, f.id)
    faces.push(...pieces)
  }
  const nb: Brush = { ...b, vertices, faces }
  const r = finish(nb, selFromEdges(nb, selEdges(nb)), b, splitFrom)
  return { brush: r.brush, sel: r.sel }
}

/**
 * J: every face holding two or more selected vertices that are not all neighbours is cut between them (between cyclically consecutive
 * selected corners; three or more give a middle face). The new edges are selected.
 */
export function connectVerts(b: Brush, verts: number[]): OpResult {
  const vs = new Set(verts)
  if (vs.size < 2) return fail('Connect: select two or more vertices that share a face')
  const chords: string[] = []
  const r = rebuild(b, b.vertices, (f, newId) => {
    const at = f.loop.filter((v) => vs.has(v))
    if (at.length < 2) return [f]
    const pairs: [number, number][] = at.length === 2 ? [[at[0], at[1]]] : at.map((v, i) => [v, at[(i + 1) % at.length]] as [number, number])
    const { pieces, made } = splitFace(f, pairs, newId)
    for (const [a, c] of made) chords.push(edgeKey(a, c))
    return pieces
  }, () => chords)
  if (!chords.length) return fail('Connect: no face holds two selected vertices that are not already joined by an edge')
  return r
}

/**
 * Subdivide: each selected edge gets `cuts` evenly spaced new vertices (every face on it takes them). Inside a face where two or more
 * selected edges were split, the new vertices are joined (Scythe "Connect edges"): two edges = parallel cuts (the k-th point of one to the
 * matching point of the other); more edges with one cut = their midpoints joined around a middle face. The new edges are selected; with
 * nothing to join, the pieces of the split edges are.
 */
export function subdivideEdges(b: Brush, edgeKeys: string[], cuts = 1): OpResult {
  const es = [...new Set(edgeKeys)]
  if (!es.length) return fail('Subdivide: select one or more edges')
  cuts = Math.max(1, Math.min(64, Math.round(cuts)))
  const vertices = [...b.vertices]
  const along = new Map<string, number[]>() // edge -> new vertices from its lower index to its higher
  for (const k of es) {
    const [a, c] = parseEdge(k)
    if (!b.vertices[a] || !b.vertices[c]) continue
    const ids: number[] = []
    for (let i = 1; i <= cuts; i++) { ids.push(vertices.length); vertices.push(add(b.vertices[a], scale(sub(b.vertices[c], b.vertices[a]), i / (cuts + 1)))) }
    along.set(k, ids)
  }
  const chords: string[] = []
  const r = rebuild(b, vertices, (f, newId) => {
    const loop: number[] = [], runs: number[][] = []
    f.loop.forEach((v, i) => {
      loop.push(v)
      const w = f.loop[(i + 1) % f.loop.length], ids = along.get(edgeKey(v, w))
      if (!ids) return
      const run = v < w ? ids : [...ids].reverse()
      loop.push(...run); runs.push(run)
    })
    if (!runs.length) return [f]
    const g: Face = { ...f, loop }
    delete g.uvs // re-evaluated from the face's map by finish
    let pairs: [number, number][] = []
    if (runs.length === 2) pairs = runs[0].map((v, i) => [v, runs[1][runs[1].length - 1 - i]] as [number, number])
    else if (runs.length > 2 && cuts === 1) pairs = runs.map((r0, i) => [r0[0], runs[(i + 1) % runs.length][0]] as [number, number])
    const { pieces, made } = splitFace(g, pairs, newId)
    for (const [a, c] of made) chords.push(edgeKey(a, c))
    return pieces
  }, (nb) => {
    if (chords.length) return chords
    const fresh = new Set([...along.values()].flat())
    return selFromVerts(nb, [...fresh, ...es.flatMap(parseEdge)]).edges.filter((k) => parseEdge(k).some((v) => fresh.has(v)))
  })
  return r
}

// ---- knife (REQUIREMENTS §21 E5) ----------------------------------------------------------------
/** A knife point: an existing vertex, or a point on an edge at `t` (0..1 from the edge's lower vertex index to its higher). */
export type KnifePoint = { v: number } | { edge: string; t: number }

/** World position of a knife point. */
export function knifePos(b: Brush, p: KnifePoint): Vec3 {
  if ('v' in p) return b.vertices[p.v]
  const [a, c] = parseEdge(p.edge)
  return add(b.vertices[a], scale(sub(b.vertices[c], b.vertices[a]), p.t))
}

/**
 * Cuts the brush along the clicked points: points on edges become new vertices (every face on that edge takes them), and each pair of
 * consecutive points that lie on one face cuts that face. Consecutive points without a common face are skipped (the cut continues from the
 * later one). The new edges are selected.
 */
export function knifeCut(b: Brush, points: KnifePoint[]): OpResult {
  if (points.length < 2) return fail('Knife: click at least two points on edges or vertices')
  const vertices = [...b.vertices]
  const onEdge = new Map<string, { t: number; v: number }[]>() // edge -> new vertices sorted by t
  const ids: number[] = []
  for (const p of points) {
    if ('v' in p) { ids.push(p.v); continue }
    const t = Math.min(1, Math.max(0, p.t))
    const [a, c] = parseEdge(p.edge)
    if (t < 1e-6) { ids.push(a); continue }
    if (t > 1 - 1e-6) { ids.push(c); continue }
    const list = onEdge.get(p.edge) ?? []
    let hit = list.find((x) => Math.abs(x.t - t) < 1e-6)
    if (!hit) { hit = { t, v: vertices.length }; vertices.push(knifePos(b, { edge: p.edge, t })); list.push(hit); list.sort((x, y) => x.t - y.t); onEdge.set(p.edge, list) }
    ids.push(hit.v)
  }
  const chords: string[] = []
  const r = rebuild(b, vertices, (f, newId) => {
    const loop: number[] = []
    f.loop.forEach((v, i) => {
      loop.push(v)
      const w = f.loop[(i + 1) % f.loop.length], list = onEdge.get(edgeKey(v, w))
      if (list) loop.push(...(v < w ? list : [...list].reverse()).map((x) => x.v))
    })
    const g: Face = { ...f, loop }
    if (loop.length !== f.loop.length) delete g.uvs // re-evaluated from the face's map by finish
    const pairs: [number, number][] = []
    for (let i = 1; i < ids.length; i++) if (loop.includes(ids[i - 1]) && loop.includes(ids[i]) && ids[i - 1] !== ids[i]) pairs.push([ids[i - 1], ids[i]])
    if (!pairs.length) return loop.length === f.loop.length ? [f] : [g]
    const { pieces, made } = splitFace(g, pairs, newId)
    for (const [a, c] of made) chords.push(edgeKey(a, c))
    return pieces
  }, () => chords)
  if (!chords.length) return fail('Knife: no two consecutive points lie on the same face across it')
  return r
}
