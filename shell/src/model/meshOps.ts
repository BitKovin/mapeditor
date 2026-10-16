// Direct mesh-editing operations (DESIGN.md §8). All pure: (brush, selection, parameter) -> new brush + new
// selection, or an error message. Nothing here knows about the store, the viewport or Core; the tools call
// these on every mouse move, always starting from the untouched brush, so a parameter of 0 gives back the input.
import type { Brush, Face } from './document'
import { Vec3, add, cross, dot, length, normalize, scale, sub } from './math'
import { faceNormal, signedVolume } from './geometry'
import { mapBrushPoints } from './transform'
import {
  MeshSel, Topology, averageNormal, buildTopology, centroid, cloneFace, edgeKey, faceById, finish, isClosedManifold, nextFaceId,
  parseEdge, selFromEdges, selFromFaces, selFromVerts,
} from './meshTopology'

export type OpResult = { brush: Brush; sel: MeshSel } | { error: string }
export const isError = (r: OpResult): r is { error: string } => 'error' in r

/** Closed, and every vertex has a single ring of faces around it (no bow-ties). */
export function isProperSolid(b: Brush): boolean {
  if (!isClosedManifold(b)) return false
  const t = buildTopology(b)
  for (const v of t.vertexFaces.keys()) if (!fanOf(b, t, v)) return false
  return true
}
const fail = (error: string): { error: string } => ({ error })
const same = (b: Brush, sel: MeshSel): OpResult => ({ brush: b, sel })

const faceIdx = (b: Brush, ids: number[]): number[] => ids.map((id) => b.faces.findIndex((f) => f.id === id)).filter((i) => i >= 0)

// ---- transform vertices ------------------------------------------------------------------------
/** Moves the selected vertices with `fn`; faces that stop being planar are split into triangles. `lockUv`: textures stay attached to the faces (the toolbar's UV lock). */
export function transformVerts(b: Brush, verts: number[], fn: (p: Vec3, i: number) => Vec3, lockUv = false): OpResult {
  const vs = new Set(verts)
  if (vs.size === 0) return fail('Nothing selected')
  const vertices = b.vertices.map((p, i) => (vs.has(i) ? fn(p, i) : p))
  const r = finish({ ...b, vertices }, selFromVerts({ ...b, vertices }, verts), lockUv ? b : undefined)
  return { brush: r.brush, sel: r.sel }
}

// ---- extrude -----------------------------------------------------------------------------------
/**
 * Region extrude: the selected faces are pushed `dist` along their average normal and side faces are
 * created around the region's boundary. The extruded faces keep their ids. dist === 0 is the identity. `along` (Shift+drag on the move
 * gizmo, REQUIREMENTS §21 E6): the offset as a vector instead of `dist` along the normal; a result that is not a valid brush is refused by the usual checks.
 */
export function extrudeFaces(b: Brush, faceIds: number[], dist: number, along?: Vec3): OpResult {
  const sel = selFromFaces(b, faceIds)
  if (sel.faces.length === 0) return fail('Select at least one face to extrude')
  if (along ? length(along) < 1e-9 : Math.abs(dist) < 1e-9) return same(b, sel)
  const t = buildTopology(b)
  const inSel = new Set(faceIdx(b, sel.faces))
  const regionFaces = [...inSel].map((i) => b.faces[i])
  const dir = averageNormal(b, regionFaces)
  if (length(dir) < 0.5) return fail('The selected faces face opposite ways; extrude them one region at a time')

  const vertices = [...b.vertices]
  const dup = new Map<number, number>()
  for (const f of regionFaces) for (const v of f.loop) if (!dup.has(v)) { dup.set(v, vertices.length); vertices.push(add(b.vertices[v], along ?? scale(dir, dist))) }

  const faces: Face[] = b.faces.map((f, fi) => (inSel.has(fi) ? { ...f, loop: f.loop.map((v) => dup.get(v)!) } : f))
  let next = nextFaceId(b)
  for (const fi of inSel) {
    const f = b.faces[fi]
    for (let i = 0; i < f.loop.length; i++) {
      const a = f.loop[i], c = f.loop[(i + 1) % f.loop.length]
      const across = t.half.get(`${c}>${a}`)
      if (across !== undefined && inSel.has(across)) continue // interior edge of the region
      // side wall takes its look from the face it grows out of (world-aligned projection keeps textures continuous)
      const source = across !== undefined ? b.faces[across] : f
      faces.push(cloneFace(source, next++, [a, c, dup.get(c)!, dup.get(a)!]))
    }
  }
  const nb = { ...b, vertices, faces }
  const r = finish(nb, selFromFaces(nb, sel.faces))
  return { brush: r.brush, sel: r.sel }
}

// ---- inset -------------------------------------------------------------------------------------
/** Each selected face gets an inner face `amount` metres inside its border, ringed by new quads. */
export function insetFaces(b: Brush, faceIds: number[], amount: number): OpResult {
  const sel = selFromFaces(b, faceIds)
  if (sel.faces.length === 0) return fail('Select at least one face to inset')
  if (Math.abs(amount) < 1e-9) return same(b, sel)
  if (amount < 0) return fail('Inset amount must be positive')
  const vertices = [...b.vertices]
  const faces: Face[] = []
  let next = nextFaceId(b)
  const inner: number[] = []
  const selected = new Set(sel.faces)
  for (const f of b.faces) {
    if (!selected.has(f.id)) { faces.push(f); continue }
    const n = f.loop.length, N = faceNormal(b, f)
    const ring: number[] = []
    for (let i = 0; i < n; i++) {
      const p = b.vertices[f.loop[(i + n - 1) % n]], v = b.vertices[f.loop[i]], q = b.vertices[f.loop[(i + 1) % n]]
      const n1 = cross(N, normalize(sub(v, p))), n2 = cross(N, normalize(sub(q, v)))
      const k = 1 + dot(n1, n2)
      if (k < 1e-6) return fail('Inset failed: a corner is too sharp')
      ring.push(vertices.length)
      vertices.push(add(v, scale(add(n1, n2), amount / k)))
    }
    // the inner polygon must keep the orientation and edge directions of the border, or the inset overshot
    for (let i = 0; i < n; i++) {
      const j = (i + 1) % n
      const e0 = sub(b.vertices[f.loop[j]], b.vertices[f.loop[i]]), e1 = sub(vertices[ring[j]], vertices[ring[i]])
      if (dot(e0, e1) <= 1e-9) return fail('Inset is larger than the face')
    }
    faces.push({ ...f, loop: ring })
    inner.push(f.id)
    for (let i = 0; i < n; i++) {
      const j = (i + 1) % n
      faces.push(cloneFace(f, next++, [f.loop[i], f.loop[j], ring[j], ring[i]]))
    }
  }
  const nb = { ...b, vertices, faces }
  const r = finish(nb, selFromFaces(nb, inner))
  return { brush: r.brush, sel: r.sel }
}

// ---- bevel (chamfer) -----------------------------------------------------------------------------
interface FanEntry { face: number; inV: number; outV: number }

/** Faces around vertex v in rotational order; consecutive entries share the edge (v, outV). Null if not a proper fan. */
function fanOf(b: Brush, t: Topology, v: number): FanEntry[] | null {
  const start = t.vertexFaces.get(v)?.[0]
  if (start === undefined) return null
  const out: FanEntry[] = []
  let fi = start
  for (let guard = 0; guard < 1000; guard++) {
    const f = b.faces[fi], i = f.loop.indexOf(v), n = f.loop.length
    const inV = f.loop[(i + n - 1) % n], outV = f.loop[(i + 1) % n]
    out.push({ face: fi, inV, outV })
    const g = t.half.get(`${outV}>${v}`)
    if (g === undefined) return null
    if (g === start) return out.length === (t.vertexFaces.get(v) ?? []).length ? out : null
    fi = g
  }
  return null
}

/**
 * Chamfers the selected edges by `width` (each side of the edge, measured in the adjacent faces). Vertices
 * where two or more selected edges meet, or a single one ends, are handled; a corner whose beveled edges
 * separate more than two faces on one side is refused. New faces get new ids; existing faces keep theirs.
 */
export function bevelEdges(b: Brush, edgeKeys: string[], width: number, segments = 1): OpResult {
  const segs = Math.max(1, Math.min(MAX_BEVEL_SEGMENTS, Math.round(segments) || 1))
  const sel = selFromEdges(b, edgeKeys)
  if (sel.edges.length === 0) return fail('Select at least one edge to bevel')
  if (Math.abs(width) < 1e-9) return same(b, sel)
  if (width < 0) return fail('Bevel width must be positive')
  if (!isClosedManifold(b)) return fail('Bevel needs a closed brush')
  const t = buildTopology(b)
  const beveled = new Set(sel.edges)
  const bevVerts = new Set<number>()
  for (const k of beveled) parseEdge(k).forEach((v) => bevVerts.add(v))

  const vertices = [...b.vertices]
  // (face index, original vertex) -> the vertices replacing that corner, in loop order
  const corner = new Map<string, number[]>()
  const setCorner = (fi: number, v: number, list: number[]) => corner.set(`${fi}|${v}`, list)
  const dirTo = (from: number, to: number): Vec3 => normalize(sub(b.vertices[to], b.vertices[from]))
  const edgeLen = (x: number, y: number): number => length(sub(b.vertices[x], b.vertices[y]))
  const newVerts = new Set<number>()
  const addVert = (p: Vec3): number => { newVerts.add(vertices.length); vertices.push(p); return vertices.length - 1 }
  // distance along the unbeveled edge `along` (leaving v) at which the offset line of edge `bev` (also leaving v) is `width` away
  const tAlong = (v: number, bevTo: number, alongTo: number): number | null => {
    const s = length(cross(dirTo(v, bevTo), dirTo(v, alongTo)))
    return s < 1e-6 ? null : width / s
  }

  for (const v of bevVerts) {
    const fan = fanOf(b, t, v)
    if (!fan) return fail('Bevel: the mesh is not a clean closed surface around a vertex')
    const n = fan.length
    const marks: number[] = [] // k such that edge (v, fan[k].outV) is beveled; it lies between fan[k] and fan[k+1]
    fan.forEach((e, k) => { if (beveled.has(edgeKey(v, e.outV))) marks.push(k) })
    const m = marks.length
    if (m === 1) {
      const a = marks[0]
      const Fa = fan[a], Fb = fan[(a + 1) % n]
      const ta = tAlong(v, Fa.outV, Fa.inV), tb = tAlong(v, Fb.inV, Fb.outV)
      if (ta === null || tb === null) return fail('Bevel failed at a degenerate corner')
      if (ta >= edgeLen(v, Fa.inV) || tb >= edgeLen(v, Fb.outV)) return fail('Bevel is wider than the geometry')
      const pa = addVert(add(b.vertices[v], scale(dirTo(v, Fa.inV), ta)))
      const pb = addVert(add(b.vertices[v], scale(dirTo(v, Fb.outV), tb)))
      setCorner(Fa.face, v, [pa])
      setCorner(Fb.face, v, [pb])
      if (n === 3) setCorner(fan[(a + 2) % n].face, v, [pb, pa])
      else {
        setCorner(fan[(a + 2) % n].face, v, [pb, v])
        setCorner(fan[(a + n - 1) % n].face, v, [v, pa])
      }
      continue
    }
    // m >= 2: one new vertex per sector between consecutive beveled edges
    for (let j = 0; j < m; j++) {
      const a = marks[j], z = marks[(j + 1) % m]
      const count = ((z - a + n) % n) || n
      const first = fan[(a + 1) % n], last = fan[z]
      let p: Vec3
      if (count === 1) {
        const N = faceNormal(b, b.faces[first.face])
        const n1 = cross(N, dirTo(first.inV, v)), n2 = cross(N, dirTo(v, first.outV))
        const k = 1 + dot(n1, n2)
        if (k < 1e-6) return fail('Bevel failed at a sharp corner')
        p = add(b.vertices[v], scale(add(n1, n2), width / k))
      } else if (count === 2) {
        const shared = first.outV // edge between the two faces of the sector
        const t1 = tAlong(v, first.inV, shared), t2 = tAlong(v, last.outV, shared)
        if (t1 === null || t2 === null) return fail('Bevel failed at a degenerate corner')
        if ((t1 + t2) / 2 >= edgeLen(v, shared)) return fail('Bevel is wider than the geometry')
        p = add(b.vertices[v], scale(dirTo(v, shared), (t1 + t2) / 2))
      } else return fail('Bevel: select the edges around this corner so that at most two faces lie between beveled edges')
      const nv = addVert(p)
      for (let c = 1; c <= count; c++) setCorner(fan[(a + c) % n].face, v, [nv])
    }
  }

  // an offset that overshoots turns an edge around; catch that before building anything
  for (let fi = 0; fi < b.faces.length; fi++) {
    const loop = b.faces[fi].loop
    for (let i = 0; i < loop.length; i++) {
      const v = loop[i], w = loop[(i + 1) % loop.length]
      const A = (corner.get(`${fi}|${v}`) ?? [v]).slice(-1)[0], C = (corner.get(`${fi}|${w}`) ?? [w])[0]
      if (dot(sub(vertices[C], vertices[A]), sub(b.vertices[w], b.vertices[v])) < 1e-12) return fail('Bevel is wider than the geometry')
    }
  }

  // rebuild the existing faces with their corners replaced
  const faces: Face[] = b.faces.map((f, fi) => {
    const loop: number[] = []
    for (const v of f.loop) loop.push(...(corner.get(`${fi}|${v}`) ?? [v]))
    return loop.length === f.loop.length && loop.every((v, i) => v === f.loop[i]) ? f : { ...f, loop }
  })

  // the strip that replaces each beveled edge
  let next = nextFaceId(b)
  const strips: number[] = []
  const stripInfo: { id: number; x: number; y: number }[] = [] // the beveled edge each strip replaced (for the profile)
  for (const k of beveled) {
    const [x, y] = parseEdge(k)
    const f1 = t.half.get(`${x}>${y}`), f2 = t.half.get(`${y}>${x}`)
    if (f1 === undefined || f2 === undefined) return fail('Bevel needs an edge between two faces')
    const pick = (fi: number, v: number): number => corner.get(`${fi}|${v}`)?.[0] ?? v // faces beside a beveled edge always have a single replacement
    // orient so the strip contains (a>b as walked by f2) : [a_f2, b_f2, b_f1, a_f1] for edge a>b in f1
    const [a, bb] = [x, y]
    const loop = [pick(f2, a), pick(f2, bb), pick(f1, bb), pick(f1, a)]
    const id = next++
    strips.push(id)
    stripInfo.push({ id, x: a, y: bb })
    faces.push(cloneFace(b.faces[f1], id, loop))
  }

  // close the little holes left at corners where three or more beveled edges meet
  const nb: Brush = { ...b, vertices, faces }
  const filled = closeOpenLoops(nb, (v) => newVerts.has(v) || bevVerts.has(v), next)
  if ('error' in filled) return filled
  // no surviving face may have flipped or collapsed (the bevel is wider than the geometry allows)
  for (const f of b.faces) {
    const nf = faceById(filled.brush, f.id)
    if (nf && dot(faceNormal(b, f), faceNormal(filled.brush, nf)) <= 0.05) return fail('Bevel is wider than the geometry')
  }
  let result = filled.brush, stripIds = strips
  if (segs > 1) { const round = roundStrips(result, b, stripInfo, segs); result = round.brush; stripIds = round.strips }
  const r = finish(result, selFromFaces(result, stripIds))
  return { brush: r.brush, sel: r.sel }
}

export const MAX_BEVEL_SEGMENTS = 32

/**
 * Turns each chamfer strip into `segs` strips following a circular-arc profile. The arc is a rational quadratic
 * Bezier from one end of the strip's rail to the other with the original corner as control point (weight
 * sin(angle/2), the exact circle for the angle between the two rail legs). The new rail vertices are inserted into
 * the one other face that shares the rail edge (a corner cap, or the face beside a bevel that ends), so the mesh
 * stays closed; such faces may stop being planar and are split by `finish`.
 */
function roundStrips(b: Brush, orig: Brush, info: { id: number; x: number; y: number }[], segs: number): { brush: Brush; strips: number[] } {
  const vertices = [...b.vertices]
  const rails = new Map<string, { from: number; pts: number[]; corner: Vec3; w: number }>() // rail (undirected) -> inserted vertices, ordered from `from`, and its profile
  const arc = (from: number, to: number, vertex: Vec3, e0: Vec3, e1: Vec3): number[] => {
    const key = from < to ? `${from}|${to}` : `${to}|${from}`
    const have = rails.get(key)
    const dirOf = (r: { from: number; pts: number[] }) => (r.from === from ? r.pts : [...r.pts].reverse())
    if (have) return dirOf(have)
    const A = vertices[from], C = vertices[to]
    // control point: where the two tangents meet, i.e. the original edge at the height of the rail (the rail's ends projected onto the edge line)
    const ed = normalize(sub(e1, e0)), proj = (p: Vec3): Vec3 => add(e0, scale(ed, dot(sub(p, e0), ed)))
    const pa = proj(A), pc = proj(C)
    // where the rail's two ends sit at different heights along the edge (a chain corner), the tangents do not meet on the edge; fall back to the original corner
    const aligned = length(sub(pa, pc)) < 1e-6 * Math.max(1, length(sub(A, C)))
    const corner: Vec3 = aligned ? [(pa[0] + pc[0]) / 2, (pa[1] + pc[1]) / 2, (pa[2] + pc[2]) / 2] : vertex
    const l1 = normalize(sub(A, corner)), l2 = normalize(sub(C, corner))
    const w = Math.sqrt(Math.max(0, (1 - dot(l1, l2)) / 2)) // sin(phi/2), phi = angle between the legs at the corner
    const pts: number[] = []
    for (let i = 1; i < segs; i++) {
      const t = i / segs, k0 = (1 - t) * (1 - t), k1 = 2 * w * t * (1 - t), k2 = t * t, den = k0 + k1 + k2
      pts.push(vertices.length)
      vertices.push([0, 1, 2].map((c) => (k0 * A[c] + k1 * corner[c] + k2 * C[c]) / den) as Vec3)
    }
    rails.set(key, { from, pts, corner: vertex, w })
    return pts
  }
  const strips = new Set(info.map((x) => x.id))
  let next = nextFaceId(b)
  const out: Face[] = []
  const ids: number[] = []
  for (const f of b.faces) {
    const inf = info.find((x) => x.id === f.id)
    if (!inf) { out.push(f); continue }
    const [p0, p1, p2, p3] = f.loop
    const ex = orig.vertices[inf.x], ey = orig.vertices[inf.y]
    const A = [p0, ...arc(p0, p3, ex, ex, ey), p3] // rail at x
    const B = [p1, ...arc(p1, p2, ey, ex, ey), p2] // rail at y
    for (let i = 0; i < segs; i++) {
      const id = i === 0 ? f.id : next++
      ids.push(id)
      out.push(i === 0 ? { ...f, loop: [A[i], B[i], B[i + 1], A[i + 1]] } : cloneFace(f, id, [A[i], B[i], B[i + 1], A[i + 1]]))
    }
  }
  // the face on the other side of every rail gets the new vertices along that edge; a corner cap where three
  // or more arcs meet becomes a dome (a fan around a centre pushed out toward the original corner) so it stays convex
  const faces: Face[] = []
  for (const f of out) {
    if (strips.has(f.id) || ids.includes(f.id)) { faces.push(f); continue }
    const loop: number[] = [], touched: { corner: Vec3; w: number }[] = []
    for (let i = 0; i < f.loop.length; i++) {
      const u = f.loop[i], w = f.loop[(i + 1) % f.loop.length]
      loop.push(u)
      const r = rails.get(u < w ? `${u}|${w}` : `${w}|${u}`)
      if (r && r.pts.length) { loop.push(...(r.from === u ? r.pts : [...r.pts].reverse())); touched.push(r) }
    }
    if (touched.length < 3) { faces.push(loop.length === f.loop.length ? f : { ...f, loop }); continue }
    const mid: Vec3 = [0, 0, 0]
    for (const v of loop) for (let c = 0; c < 3; c++) mid[c] += vertices[v][c] / loop.length
    const corner = touched[0].corner, w = touched.reduce((acc, r) => acc + r.w, 0) / touched.length, k = w / (1 + w)
    const centre = vertices.length
    vertices.push([0, 1, 2].map((c) => mid[c] + (corner[c] - mid[c]) * k) as Vec3)
    loop.forEach((u, i) => {
      const tri = [u, loop[(i + 1) % loop.length], centre]
      if (i === 0) faces.push({ ...f, loop: tri })
      else faces.push(cloneFace(f, next++, tri))
    })
  }
  return { brush: { ...b, vertices, faces }, strips: ids }
}

/**
 * Adds a face for every closed chain of open edges (edges walked by one face only) whose vertices all
 * satisfy `include`. Used to cap corners after a bevel and by Fill.
 */
function closeOpenLoops(b: Brush, include: (v: number) => boolean, firstId: number, template?: Face): { brush: Brush; created: number[] } | { error: string } {
  const t = buildTopology(b)
  const open = new Map<number, number[]>() // reverse-walk graph: vertex -> next vertices along the missing face
  for (const key of t.half.keys()) {
    const [a, c] = key.split('>').map(Number)
    if (t.half.has(`${c}>${a}`) || !include(a) || !include(c)) continue
    // the missing face walks c -> a
    const list = open.get(c) ?? []
    list.push(a)
    open.set(c, list)
  }
  const faces = [...b.faces]
  const created: number[] = []
  let id = firstId
  while (open.size) {
    const [start] = open.keys()
    const loop = [start]
    let cur = start
    for (let guard = 0; guard < 10000; guard++) {
      const nxts = open.get(cur)
      if (!nxts || nxts.length === 0) return { error: 'Could not close a hole: the open edges do not form a loop' }
      const nx = nxts.pop()!
      if (nxts.length === 0) open.delete(cur)
      if (nx === start) break
      loop.push(nx)
      cur = nx
    }
    if (loop.length < 3) return { error: 'Could not close a hole: it has fewer than 3 corners' }
    // texture from a neighbour of the hole
    const neighbour = template ?? b.faces[t.half.get(`${loop[1]}>${loop[0]}`) ?? t.half.get(`${loop[0]}>${loop[loop.length - 1]}`) ?? 0]
    faces.push(cloneFace(neighbour, id, loop))
    created.push(id++)
  }
  return { brush: { ...b, faces }, created }
}

// ---- loop cut -----------------------------------------------------------------------------------
export interface LoopCutInfo { ring: string[]; closed: boolean }

/**
 * The ring of parallel edges the cut through `edge` would cross: from the edge, step across each quad to
 * its opposite edge, both ways, stopping at a non-quad face or when the ring closes.
 */
export function edgeRing(b: Brush, edge: string): LoopCutInfo | null {
  const t = buildTopology(b)
  if (!t.edges.has(edge)) return null
  // oriented pairs (p,q): the rail vertices, kept consistent along the ring
  const [a0, b0] = parseEdge(edge)
  const oriented: [number, number][] = [[a0, b0]]
  const seen = new Set<string>([edge])
  const step = (p: number, q: number, fi: number): [number, number] | null => {
    const f = b.faces[fi]
    if (f.loop.length !== 4) return null
    const i = f.loop.indexOf(p), j = f.loop.indexOf(q)
    const n = 4
    // p,q adjacent in the loop; the opposite edge joins the other two vertices, matched p<->p', q<->q'
    if ((i + 1) % n === j) return [f.loop[(i + 3) % n], f.loop[(j + 1) % n]]
    if ((j + 1) % n === i) return [f.loop[(i + 1) % n], f.loop[(j + 3) % n]]
    return null
  }
  let closed = false
  for (const dir of [0, 1]) {
    let [p, q] = [a0, b0]
    let fi = t.edges.get(edge)!.faces[dir]
    for (let guard = 0; guard < 1000 && fi !== undefined; guard++) {
      const nx = step(p, q, fi)
      if (!nx) break
      const k = edgeKey(nx[0], nx[1])
      if (k === edge) { closed = true; break }
      if (seen.has(k)) break
      seen.add(k)
      if (dir === 0) oriented.push(nx)
      else oriented.unshift(nx)
      ;[p, q] = nx
      fi = t.edges.get(k)!.faces.find((x) => x !== fi) as number // continue on the far side of the new edge
    }
    if (closed) break
  }
  return { ring: oriented.map(([p, q]) => `${p}>${q}`), closed }
}

/** Inserts an edge loop across the ring of `edge` at parameter t (0..1) measured along the ring's rails. */
export function loopCut(b: Brush, edge: string, param: number): OpResult {
  const info = edgeRing(b, edge)
  if (!info) return fail('Loop cut: pick an edge')
  if (!(param > 0.001 && param < 0.999)) return fail('Loop cut position must be strictly between the rails')
  const t = buildTopology(b)
  const vertices = [...b.vertices]
  const mid = new Map<string, number>() // undirected ring edge -> new vertex
  const railOf = new Map<string, [number, number]>()
  for (const pq of info.ring) {
    const [p, q] = pq.split('>').map(Number)
    railOf.set(edgeKey(p, q), [p, q])
    mid.set(edgeKey(p, q), vertices.length)
    vertices.push(add(b.vertices[p], scale(sub(b.vertices[q], b.vertices[p]), param)))
  }
  const faces: Face[] = []
  let next = nextFaceId(b)
  const created: string[] = []
  const cutEdges = new Set<string>()
  b.faces.forEach((f, fi) => {
    const ringEdgesIn = f.loop.map((v, i) => ({ i, k: edgeKey(v, f.loop[(i + 1) % f.loop.length]) })).filter((e) => mid.has(e.k))
    if (ringEdgesIn.length === 0) { faces.push(f); return }
    if (f.loop.length === 4 && ringEdgesIn.length === 2 && (ringEdgesIn[0].i + 2) % 4 === ringEdgesIn[1].i) {
      // a quad crossed by the cut: split it into two quads along the new vertices
      const [e0, e1] = ringEdgesIn
      const n = 4
      const m0 = mid.get(e0.k)!, m1 = mid.get(e1.k)!
      const l = f.loop
      const A = [l[e0.i], m0, m1, l[(e1.i + 1) % n]] // from the first corner of e0 across to the end of e1
      const B = [m0, l[(e0.i + 1) % n], l[e1.i], m1]
      // A: l[e0.i] -> m0 -> m1 -> l[e1.i+1] (loop order continues l[e1.i+1] -> l[e0.i] since e1.i+2 == e0.i)
      faces.push({ ...f, loop: A }, cloneFace(f, next++, B))
      created.push(edgeKey(m0, m1))
      cutEdges.add(edgeKey(m0, m1))
      void fi
      return
    }
    // any other face that touches the ring just receives the new vertices on its edges
    const loop: number[] = []
    f.loop.forEach((v, i) => {
      loop.push(v)
      const w = f.loop[(i + 1) % f.loop.length], k = edgeKey(v, w), m = mid.get(k)
      if (m !== undefined) loop.push(m)
    })
    faces.push({ ...f, loop })
  })
  void t
  const nb = { ...b, vertices, faces }
  const r = finish(nb, selFromEdges(nb, [...cutEdges]))
  return { brush: r.brush, sel: r.sel }
}

// ---- merge --------------------------------------------------------------------------------------
/** Welds each group of vertices into one (at the group's centre, or `at` for a single group). */
function weld(b: Brush, groups: number[][], at?: Vec3): OpResult {
  const target = new Map<number, number>()
  const vertices = [...b.vertices]
  const reps: number[] = []
  for (const g of groups) {
    const rep = g[0]
    reps.push(rep)
    vertices[rep] = at ?? centroid(b, g)
    for (const v of g) target.set(v, rep)
  }
  const faces: Face[] = []
  for (const f of b.faces) {
    const loop: number[] = []
    const uvs: [number, number][] = []
    f.loop.forEach((v, i) => {
      const nv = target.get(v) ?? v
      if (loop[loop.length - 1] === nv) return // consecutive duplicate collapses
      loop.push(nv)
      if (f.uvs) uvs.push(f.uvs[i])
    })
    if (loop.length > 1 && loop[0] === loop[loop.length - 1]) { loop.pop(); uvs.pop() }
    if (loop.length < 3) continue // the face collapsed
    if (new Set(loop).size !== loop.length) return fail('Merge would pinch a face; merge neighbouring vertices only')
    faces.push({ ...f, loop, ...(f.uvs ? { uvs } : {}) })
  }
  const nb = { ...b, vertices, faces }
  if (isProperSolid(b) && !isProperSolid(nb)) return fail('Merge would leave holes or pinch the surface')
  const r = finish(nb, selFromVerts(nb, reps))
  return { brush: r.brush, sel: r.sel }
}

/** Welds the selected vertices into one (at their centre, or `at`). */
export function mergeVerts(b: Brush, verts: number[], at?: Vec3): OpResult {
  const vs = [...new Set(verts)]
  if (vs.length < 2) return fail('Select at least two vertices to merge')
  return weld(b, [vs], at)
}

/** Merges vertices closer than `distance` (chains included), each cluster at its centre. */
export function mergeByDistance(b: Brush, verts: number[], distance: number): OpResult {
  const vs = [...new Set(verts)]
  const parent = new Map(vs.map((v) => [v, v]))
  const find = (v: number): number => { while (parent.get(v) !== v) v = parent.get(v)!; return v }
  for (let i = 0; i < vs.length; i++)
    for (let j = i + 1; j < vs.length; j++)
      if (length(sub(b.vertices[vs[i]], b.vertices[vs[j]])) <= distance) parent.set(find(vs[j]), find(vs[i]))
  const groups = new Map<number, number[]>()
  for (const v of vs) groups.set(find(v), [...(groups.get(find(v)) ?? []), v])
  const clusters = [...groups.values()].filter((g) => g.length > 1)
  if (clusters.length === 0) return fail('No vertices closer than that distance')
  return weld(b, clusters)
}

// ---- dissolve -----------------------------------------------------------------------------------
/** Merges the faces on both sides of each selected edge into one polygon. */
export function dissolveEdges(b: Brush, edgeKeys: string[]): OpResult {
  const es = new Set(edgeKeys)
  if (es.size === 0) return fail('Select at least one edge to dissolve')
  const t = buildTopology(b)
  const parent = b.faces.map((_, i) => i)
  const find = (i: number): number => { while (parent[i] !== i) i = parent[i] = parent[parent[i]]; return i }
  for (const k of es) {
    const e = t.edges.get(k)
    if (!e || e.faces.length !== 2) return fail('Dissolve: an edge must lie between two faces')
    parent[find(e.faces[0])] = find(e.faces[1])
  }
  const groups = new Map<number, number[]>()
  b.faces.forEach((_, i) => groups.set(find(i), [...(groups.get(find(i)) ?? []), i]))
  const faces: Face[] = []
  const survivors: number[] = []
  for (const g of groups.values()) {
    if (g.length === 1) { faces.push(b.faces[g[0]]); continue }
    // boundary of the group: directed edges whose reverse is not in the group
    const inGroup = new Set(g)
    const next = new Map<number, number>()
    for (const fi of g) {
      const f = b.faces[fi]
      f.loop.forEach((a, i) => {
        const c = f.loop[(i + 1) % f.loop.length]
        const other = t.half.get(`${c}>${a}`)
        if (other !== undefined && inGroup.has(other)) return // interior edge of the merged region
        next.set(a, c)
      })
    }
    const loop: number[] = []
    const start = next.keys().next().value as number
    let cur = start
    for (let guard = 0; guard < 100000; guard++) {
      loop.push(cur)
      const nx = next.get(cur)
      if (nx === undefined) return fail('Dissolve would leave a hole in the face')
      cur = nx
      if (cur === start) break
    }
    if (loop.length !== next.size) return fail('Dissolve would create a face with a hole or two separate pieces')
    if (loop.length < 3) return fail('Dissolve would remove the whole surface')
    const keep = b.faces[g.sort((x, y) => b.faces[x].id - b.faces[y].id)[0]]
    const { uvs, ...rest } = keep
    faces.push({ ...rest, loop, ...(uvs && uvs.length ? { uvs: loop.map(() => [0, 0] as [number, number]) } : {}) })
    survivors.push(keep.id)
  }
  const nb = { ...b, faces }
  const r = finish(nb, selFromFaces(nb, survivors))
  return { brush: r.brush, sel: r.sel }
}

/**
 * Removes vertices. A vertex with only two edges (e.g. left over by a loop cut) is simply taken out of its
 * faces; any other vertex has the faces around it merged into one polygon.
 */
export function dissolveVerts(b: Brush, verts: number[]): OpResult {
  const vs = [...new Set(verts)]
  if (vs.length === 0) return fail('Select at least one vertex to dissolve')
  let cur = b
  const merge: string[] = []
  for (const v of vs) {
    const t = buildTopology(b)
    const edgesAt = [...t.edges.entries()].filter(([, e]) => e.a === v || e.b === v).map(([k]) => k)
    if (edgesAt.length === 2) {
      cur = { ...cur, faces: cur.faces.map((f) => (f.loop.includes(v)
        ? { ...f, loop: f.loop.filter((x) => x !== v), ...(f.uvs ? { uvs: f.uvs.filter((_, i) => f.loop[i] !== v) } : {}) } : f)) }
    } else merge.push(...edgesAt)
  }
  if (merge.length) {
    const r = dissolveEdges(cur, merge)
    if (isError(r)) return r
    cur = r.brush
    return { brush: cur, sel: { verts: [], edges: [], faces: r.sel.faces } }
  }
  const r = finish(cur, { verts: [], edges: [], faces: [] })
  return { brush: r.brush, sel: r.sel }
}

/** Dissolves the edges between the selected faces, merging them into one polygon per connected group. */
export function dissolveFaces(b: Brush, faceIds: number[]): OpResult {
  const ids = new Set(faceIds)
  if (ids.size < 2) return fail('Select at least two faces to dissolve into one')
  const t = buildTopology(b)
  const inner: string[] = []
  for (const [k, e] of t.edges) if (e.faces.length === 2 && e.faces.every((fi) => ids.has(b.faces[fi].id))) inner.push(k)
  if (inner.length === 0) return fail('The selected faces do not touch')
  return dissolveEdges(b, inner)
}

// ---- delete / fill ------------------------------------------------------------------------------
/** Removes faces (leaving the mesh open); use Fill to close the hole again. */
export function deleteFaces(b: Brush, faceIds: number[]): OpResult {
  const ids = new Set(faceIds)
  if (ids.size === 0) return fail('Nothing selected')
  const faces = b.faces.filter((f) => !ids.has(f.id))
  if (faces.length === 0) return fail('That would delete every face of the brush; delete the brush instead')
  const r = finish({ ...b, faces }, { verts: [], edges: [], faces: [] })
  return { brush: r.brush, sel: r.sel }
}

/** Makes faces from open edges among the selected vertices (the counterpart of deleting faces). */
export function fillHole(b: Brush, verts: number[]): OpResult {
  const vs = new Set(verts)
  if (vs.size < 3) return fail('Select the vertices around a hole')
  const r = closeOpenLoops(b, (v) => vs.has(v), nextFaceId(b))
  if ('error' in r) return fail(r.error)
  if (r.created.length === 0) return fail('No open edges among the selected vertices')
  const f = finish(r.brush, selFromFaces(r.brush, r.created))
  return { brush: f.brush, sel: f.sel }
}

// ---- normals ------------------------------------------------------------------------------------
export function flipFaces(b: Brush, faceIds: number[]): OpResult {
  const ids = new Set(faceIds)
  if (ids.size === 0) return fail('Select faces to flip')
  const faces = b.faces.map((f) => (ids.has(f.id) ? { ...f, loop: [...f.loop].reverse(), ...(f.uvs ? { uvs: [...f.uvs].reverse() } : {}) } : f))
  const nb = { ...b, faces }
  return { brush: nb, sel: selFromFaces(nb, faceIds) }
}

/** Makes the winding consistent across the surface, then points it outward (or inward). */
export function recalcNormals(b: Brush, outward = true): OpResult {
  const t = buildTopology(b)
  const flip = new Array<boolean>(b.faces.length).fill(false)
  const seen = new Array<boolean>(b.faces.length).fill(false)
  for (let s = 0; s < b.faces.length; s++) {
    if (seen[s]) continue
    seen[s] = true
    const queue = [s]
    while (queue.length) {
      const fi = queue.pop()!
      const loop = flip[fi] ? [...b.faces[fi].loop].reverse() : b.faces[fi].loop
      loop.forEach((a, i) => {
        const c = loop[(i + 1) % loop.length]
        const e = t.edges.get(edgeKey(a, c))
        for (const gi of e?.faces ?? []) {
          if (gi === fi || seen[gi]) continue
          // consistent means the neighbour walks c -> a; if it walks a -> c it must be flipped
          const walked = b.faces[gi].loop
          const j = walked.indexOf(a)
          const forward = walked[(j + 1) % walked.length] === c
          flip[gi] = forward
          seen[gi] = true
          queue.push(gi)
        }
      })
    }
  }
  let faces = b.faces.map((f, i) => (flip[i] ? { ...f, loop: [...f.loop].reverse(), ...(f.uvs ? { uvs: [...f.uvs].reverse() } : {}) } : f))
  let nb: Brush = { ...b, faces }
  let vol = 0
  for (const f of nb.faces) {
    const a = nb.vertices[f.loop[0]]
    for (let i = 1; i + 1 < f.loop.length; i++) vol += dot(a, cross(nb.vertices[f.loop[i]], nb.vertices[f.loop[i + 1]])) / 6
  }
  if ((vol < 0) === outward) {
    faces = nb.faces.map((f) => ({ ...f, loop: [...f.loop].reverse(), ...(f.uvs ? { uvs: [...f.uvs].reverse() } : {}) }))
    nb = { ...nb, faces }
  }
  return { brush: nb, sel: selFromFaces(nb, nb.faces.map((f) => f.id)) }
}

// ---- mirror (REQUIREMENTS §21 E3) ---------------------------------------------------------------
/**
 * Mirrors the selected vertices across the plane through their centre with normal `axis` (0 x, 1 y, 2 z). Every vertex selected = a true
 * mirror of the whole brush (faces re-wound so they still face out, UVs mirrored with the geometry); otherwise the vertices just move, and a
 * result that would turn the brush inside out is refused.
 */
export function mirrorVerts(b: Brush, verts: number[], axis: 0 | 1 | 2, lockUv = true): OpResult {
  const vs = [...new Set(verts)]
  if (!vs.length) return fail('Mirror: select something first')
  const c = centroid(b, vs)
  const map = (p: Vec3): Vec3 => { const q: Vec3 = [p[0], p[1], p[2]]; q[axis] = 2 * c[axis] - p[axis]; return q }
  if (vs.length === b.vertices.length) {
    const nb = mapBrushPoints(b, map, true, lockUv)
    return { brush: nb, sel: selFromVerts(nb, vs) }
  }
  const r = transformVerts(b, vs, map, lockUv)
  if (isError(r)) return r
  if (isProperSolid(b) && (!isProperSolid(r.brush) || signedVolume(r.brush) <= 0)) return fail('Mirror would turn the brush inside out; mirror the whole brush or fewer vertices')
  return r
}

// ---- proportional editing (REQUIREMENTS §21 E4) -------------------------------------------------
/** Smooth falloff: 1 at distance 0, 0 at `radius` and beyond (smoothstep, Blender's default "Smooth"). */
export function falloff(d: number, radius: number): number {
  if (!(radius > 0) || d >= radius) return 0
  const t = 1 - d / radius
  return t * t * (3 - 2 * t)
}

/**
 * Like transformVerts, but every unselected vertex within `radius` of the selection (straight-line distance to the nearest selected vertex,
 * before the move) follows by `falloff`: p + w (fn(p) - p). The selection stays what it was.
 */
export function proportionalVerts(b: Brush, verts: number[], fn: (p: Vec3, i: number) => Vec3, radius: number, lockUv = false): OpResult {
  const sel = new Set(verts)
  if (!sel.size) return fail('Nothing selected')
  const w = new Map<number, number>()
  b.vertices.forEach((p, i) => {
    if (sel.has(i)) { w.set(i, 1); return }
    let d = Infinity
    for (const j of sel) d = Math.min(d, length(sub(p, b.vertices[j])))
    const k = falloff(d, radius)
    if (k > 0) w.set(i, k)
  })
  const r = transformVerts(b, [...w.keys()], (p, i) => { const k = w.get(i)!, q = fn(p, i); return k === 1 ? q : add(p, scale(sub(q, p), k)) }, lockUv)
  if (isError(r)) return r
  return { brush: r.brush, sel: selFromVerts(r.brush, verts) }
}
