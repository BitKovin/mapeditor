// Mesh-editing foundation (DESIGN.md §5.1, §8). The stored brush is a plain indexed polygon mesh; this
// builds the adjacency the modeling tools need (a half-edge view: every directed edge `a>b` belongs to the
// face that walks it) and provides the shared clean-up every tool runs afterwards.
//
// Conventions: vertex indices are stable through an operation (tools append new vertices); `finish`
// then drops unused ones and remaps the selection. Face ids are stable: a face that survives an
// operation keeps its id, pieces of a split face and newly created faces get fresh ids (lighting is keyed
// on them). Selections name faces by id, vertices by index, edges by `a-b` with a < b.
import type { Brush, Face } from './document'
import { Vec3, cross, dot, length, normalize, sub } from './math'
import { faceNormal, triangulateFace } from './geometry'
import { projectFace } from './uv'
import { faceAffine } from './uvmesh'

export const edgeKey = (a: number, b: number): string => (a < b ? `${a}-${b}` : `${b}-${a}`)
export const parseEdge = (k: string): [number, number] => {
  const [a, b] = k.split('-').map(Number)
  return [a, b]
}

export interface MeshSel { verts: number[]; edges: string[]; faces: number[] }
export const emptySel = (): MeshSel => ({ verts: [], edges: [], faces: [] })
export type SelMode = 'vertex' | 'edge' | 'face'

export interface Topology {
  /** directed edge "a>b" -> index into brush.faces of the face that walks a then b */
  half: Map<string, number>
  /** undirected edge -> indices of the faces touching it (2 for a closed manifold) */
  edges: Map<string, { a: number; b: number; faces: number[] }>
  vertexFaces: Map<number, number[]>
}

export function buildTopology(b: Brush): Topology {
  const half = new Map<string, number>()
  const edges = new Map<string, { a: number; b: number; faces: number[] }>()
  const vertexFaces = new Map<number, number[]>()
  b.faces.forEach((f, fi) => {
    const n = f.loop.length
    for (let i = 0; i < n; i++) {
      const a = f.loop[i], c = f.loop[(i + 1) % n]
      half.set(`${a}>${c}`, fi)
      const k = edgeKey(a, c)
      const e = edges.get(k)
      if (e) e.faces.push(fi)
      else edges.set(k, { a: Math.min(a, c), b: Math.max(a, c), faces: [fi] })
      const vf = vertexFaces.get(a)
      if (vf) vf.push(fi)
      else vertexFaces.set(a, [fi])
    }
  })
  return { half, edges, vertexFaces }
}

/** Every edge shared by exactly two faces that walk it in opposite directions. */
export function isClosedManifold(b: Brush): boolean {
  const t = buildTopology(b)
  for (const [k, e] of t.edges) {
    if (e.faces.length !== 2) return false
    if (!t.half.has(`${e.a}>${e.b}`) || !t.half.has(`${e.b}>${e.a}`)) return false
    void k
  }
  return true
}

export const faceById = (b: Brush, id: number): Face | undefined => b.faces.find((f) => f.id === id)
export const nextFaceId = (b: Brush): number => b.faces.reduce((m, f) => Math.max(m, f.id), -1) + 1

/** A copy of `from` (texture, projection, anything else) with a new id and loop; UVs are recomputed later. */
export function cloneFace(from: Face, id: number, loop: number[]): Face {
  const { uvs, ...rest } = from
  return { ...rest, id, loop, ...(uvs && uvs.length ? { uvs: loop.map(() => [0, 0] as [number, number]) } : {}) }
}

export const centroid = (b: Brush, verts: number[]): Vec3 => {
  const c: Vec3 = [0, 0, 0]
  for (const v of verts) for (let i = 0; i < 3; i++) c[i] += b.vertices[v][i]
  return verts.length ? [c[0] / verts.length, c[1] / verts.length, c[2] / verts.length] : c
}

export const faceCentre = (b: Brush, f: Face): Vec3 => centroid(b, f.loop)

/** Area-weighted average normal of some faces. */
export function averageNormal(b: Brush, faces: Face[]): Vec3 {
  let s: Vec3 = [0, 0, 0]
  for (const f of faces) {
    const p0 = b.vertices[f.loop[0]]
    for (let i = 1; i + 1 < f.loop.length; i++) {
      const c = cross(sub(b.vertices[f.loop[i]], p0), sub(b.vertices[f.loop[i + 1]], p0))
      s = [s[0] + c[0], s[1] + c[1], s[2] + c[2]]
    }
  }
  return normalize(s)
}

// ---- selection ------------------------------------------------------------------------------
export function selFromFaces(b: Brush, faceIds: number[]): MeshSel {
  const ids = new Set(faceIds), verts = new Set<number>(), edges = new Set<string>(), faces: number[] = []
  for (const f of b.faces) {
    if (!ids.has(f.id)) continue
    faces.push(f.id)
    f.loop.forEach((v, i) => { verts.add(v); edges.add(edgeKey(v, f.loop[(i + 1) % f.loop.length])) })
  }
  return { verts: [...verts], edges: [...edges], faces }
}

export function selFromVerts(b: Brush, verts: number[]): MeshSel {
  const vs = new Set(verts), edges = new Set<string>(), faces: number[] = []
  for (const f of b.faces) {
    if (f.loop.every((v) => vs.has(v))) {
      faces.push(f.id)
      f.loop.forEach((v, i) => edges.add(edgeKey(v, f.loop[(i + 1) % f.loop.length])))
    } else f.loop.forEach((v, i) => { const w = f.loop[(i + 1) % f.loop.length]; if (vs.has(v) && vs.has(w)) edges.add(edgeKey(v, w)) })
  }
  return { verts: [...vs], edges: [...edges], faces }
}

export function selFromEdges(b: Brush, edgeKeys: string[]): MeshSel {
  const es = new Set(edgeKeys), verts = new Set<number>(), faces: number[] = []
  for (const k of es) { const [a, c] = parseEdge(k); verts.add(a); verts.add(c) }
  for (const f of b.faces) if (f.loop.every((v, i) => es.has(edgeKey(v, f.loop[(i + 1) % f.loop.length])))) faces.push(f.id)
  return { verts: [...verts], edges: [...es], faces }
}

/** Blender's rule when switching select mode: keep what is still fully selected in the new mode. */
export function convertSel(b: Brush, sel: MeshSel, from: SelMode, to: SelMode): MeshSel {
  if (from === to) return sel
  if (to === 'vertex') {
    const verts = from === 'edge' ? sel.edges.flatMap(parseEdge) : sel.faces.flatMap((id) => faceById(b, id)?.loop ?? [])
    return selFromVerts(b, [...new Set(verts)])
  }
  if (to === 'edge') return from === 'vertex' ? selEdgesOnly(b, selFromVerts(b, sel.verts).edges) : selEdgesOnly(b, sel.faces.length ? selFromFaces(b, sel.faces).edges : [])
  // to face: faces fully inside the selection (vertex mode: all corners selected; edge mode: all edges selected)
  return from === 'vertex' ? selFromVerts(b, sel.verts) : selFromEdges(b, sel.edges)
}
const selEdgesOnly = (b: Brush, edges: string[]): MeshSel => selFromEdges(b, edges)

export function allSel(b: Brush): MeshSel { return selFromFaces(b, b.faces.map((f) => f.id)) }

export function invertSel(b: Brush, sel: MeshSel, mode: SelMode): MeshSel {
  const all = allSel(b)
  if (mode === 'vertex') return selFromVerts(b, all.verts.filter((v) => !sel.verts.includes(v)))
  if (mode === 'edge') return selFromEdges(b, all.edges.filter((e) => !sel.edges.includes(e)))
  return selFromFaces(b, all.faces.filter((f) => !sel.faces.includes(f)))
}

/** Grow the selection by one ring of neighbours (vertex: connected vertices; edge/face: touching ones). */
export function growSel(b: Brush, sel: MeshSel, mode: SelMode): MeshSel {
  const t = buildTopology(b)
  if (mode === 'face') {
    const ids = new Set(sel.faces)
    const verts = new Set(selFromFaces(b, sel.faces).verts)
    for (const f of b.faces) if (f.loop.some((v) => verts.has(v))) ids.add(f.id)
    return selFromFaces(b, [...ids])
  }
  if (mode === 'edge') {
    const verts = new Set(sel.edges.flatMap(parseEdge)), edges = new Set(sel.edges)
    for (const [k, e] of t.edges) if (verts.has(e.a) || verts.has(e.b)) edges.add(k)
    return selFromEdges(b, [...edges])
  }
  const verts = new Set(sel.verts)
  for (const e of t.edges.values()) { if (sel.verts.includes(e.a)) verts.add(e.b); if (sel.verts.includes(e.b)) verts.add(e.a) }
  return selFromVerts(b, [...verts])
}

/** Shrink: drop elements that touch anything unselected. */
export function shrinkSel(b: Brush, sel: MeshSel, mode: SelMode): MeshSel {
  const t = buildTopology(b)
  if (mode === 'face') {
    const ids = new Set(sel.faces)
    const keep = sel.faces.filter((id) => {
      const f = faceById(b, id)!
      return f.loop.every((v) => (t.vertexFaces.get(v) ?? []).every((fi) => ids.has(b.faces[fi].id)))
    })
    return selFromFaces(b, keep)
  }
  const vs = new Set(mode === 'edge' ? sel.edges.flatMap(parseEdge) : sel.verts), es = new Set(sel.edges)
  if (mode === 'edge') {
    const keep = sel.edges.filter((k) => {
      const [a, c] = parseEdge(k)
      return [a, c].every((v) => [...t.edges].filter(([, e]) => e.a === v || e.b === v).every(([kk]) => es.has(kk)))
    })
    return selFromEdges(b, keep)
  }
  const keep = sel.verts.filter((v) => [...t.edges.values()].filter((e) => e.a === v || e.b === v).every((e) => vs.has(e.a) && vs.has(e.b)))
  return selFromVerts(b, keep)
}

// ---- clean-up shared by every tool ------------------------------------------------------------
export const PLANAR_EPS = 1e-4

/** Faces whose vertices are no longer coplanar (same test as validateBrush). */
function nonPlanar(b: Brush, f: Face, eps: number): boolean {
  if (f.loop.length <= 3) return false
  const n = faceNormal(b, f)
  const p0 = b.vertices[f.loop[0]]
  return f.loop.some((i) => Math.abs(dot(n, sub(b.vertices[i], p0))) > eps)
}

/**
 * Splits non-planar faces into triangles (decided in DESIGN.md §14: faces stay planar). The first
 * triangle keeps the original face id. Returns the new brush and a map old-face-id -> ids now covering it.
 */
export function splitNonPlanar(b: Brush, eps = PLANAR_EPS): { brush: Brush; pieces: Map<number, number[]> } {
  const pieces = new Map<number, number[]>()
  if (!b.faces.some((f) => nonPlanar(b, f, eps))) return { brush: b, pieces }
  let next = nextFaceId(b)
  const faces: Face[] = []
  for (const f of b.faces) {
    if (!nonPlanar(b, f, eps)) { faces.push(f); continue }
    const tris = triangulateFace(b, f)
    const ids: number[] = []
    tris.forEach((t, k) => {
      const id = k === 0 ? f.id : next++
      ids.push(id)
      const nf = cloneFace(f, id, [...t])
      if (f.uvs && f.uvs.length === f.loop.length) nf.uvs = t.map((v) => f.uvs![f.loop.indexOf(v)])
      faces.push(nf)
    })
    pieces.set(f.id, ids)
  }
  return { brush: { ...b, faces }, pieces }
}

/**
 * Faces with stored UVs get them re-projected: the projection is world-aligned, so textures stay put in the world.
 *
 * With `before` (UV lock, REQUIREMENTS §6 B4) the face keeps the UV map it had BEFORE the edit instead: the old affine map (from its stored uvs or its
 * projection) is evaluated at the new corner positions. A face that only slides in its plane or whose other corners stay put keeps its texture exactly,
 * and a face that tilts keeps its texture attached instead of jumping to the axes of its new dominant direction. `parentOf` maps the faces created by
 * splitting a non-planar face to the face they came from. Faces without stored UVs whose corners moved get UVs baked this way.
 */
export function refreshUvs(b: Brush, before?: Brush, parentOf?: Map<number, number>): Brush {
  let changed = false
  const faces = b.faces.map((f) => {
    const old = before ? before.faces.find((x) => x.id === (parentOf?.get(f.id) ?? f.id)) : undefined
    if (before && old) {
      const m = faceAffine(before, old)
      const pts = f.loop.map((i) => b.vertices[i])
      const moved = parentOf?.has(f.id) || old.loop.length !== f.loop.length || f.loop.some((vi, k) => { const o = before.vertices[old.loop[k]], n = b.vertices[vi]; return !o || o[0] !== n[0] || o[1] !== n[1] || o[2] !== n[2] })
      if (moved || (f.uvs && f.uvs.length)) {
        changed = true
        const ev = (a: [number, number, number, number], q: Vec3): number => a[0] * q[0] + a[1] * q[1] + a[2] * q[2] + a[3]
        return { ...f, uvs: pts.map((q) => [ev(m.u, q), ev(m.v, q)] as [number, number]) }
      }
      return f
    }
    if (!f.uvs || f.uvs.length === 0) return f
    changed = true
    return { ...f, uvs: projectFace(b, f) }
  })
  return changed ? { ...b, faces } : b
}

/** Drops vertices no face uses; remaps loops and the selection. */
export function compact(b: Brush, sel: MeshSel): { brush: Brush; sel: MeshSel } {
  const used = new Set<number>()
  for (const f of b.faces) for (const v of f.loop) used.add(v)
  if (used.size === b.vertices.length) return { brush: b, sel: cleanSel(b, sel) }
  const map = new Map<number, number>(), vertices: Vec3[] = []
  b.vertices.forEach((p, i) => { if (used.has(i)) { map.set(i, vertices.length); vertices.push(p) } })
  const faces = b.faces.map((f) => ({ ...f, loop: f.loop.map((v) => map.get(v)!) }))
  const remap = (v: number) => map.get(v)
  const nb = { ...b, vertices, faces }
  return {
    brush: nb,
    sel: cleanSel(nb, {
      verts: sel.verts.map(remap).filter((v): v is number => v !== undefined),
      edges: sel.edges.map((k) => { const [x, y] = parseEdge(k).map(remap); return x === undefined || y === undefined ? '' : edgeKey(x, y) }).filter(Boolean),
      faces: sel.faces,
    }),
  }
}

/** Drops selection entries that no longer exist (edges not in the mesh, faces gone). */
export function cleanSel(b: Brush, sel: MeshSel): MeshSel {
  const t = buildTopology(b), ids = new Set(b.faces.map((f) => f.id))
  return {
    verts: [...new Set(sel.verts)].filter((v) => v >= 0 && v < b.vertices.length),
    edges: [...new Set(sel.edges)].filter((k) => t.edges.has(k)),
    faces: [...new Set(sel.faces)].filter((id) => ids.has(id)),
  }
}

/**
 * The standard tail of every tool: split non-planar faces, drop unused vertices, refresh UVs. `splitFrom` (with `before`): faces the tool cut
 * out of an existing face (new id -> the face's id), so they keep that face's UV map instead of being re-projected.
 */
export function finish(b: Brush, sel: MeshSel, before?: Brush, splitFrom?: Map<number, number>): { brush: Brush; sel: MeshSel } {
  const split = splitNonPlanar(b)
  const faces = [...sel.faces]
  for (const [old, ids] of split.pieces) if (faces.includes(old)) faces.push(...ids.filter((i) => i !== old))
  const c = compact(split.brush, { ...sel, faces })
  const parentOf = new Map<number, number>(splitFrom)
  for (const [old, ids] of split.pieces) for (const id of ids) if (id !== old) parentOf.set(id, splitFrom?.get(old) ?? old)
  return { brush: refreshUvs(c.brush, before, parentOf), sel: c.sel }
}

export const edgeLength = (b: Brush, k: string): number => { const [x, y] = parseEdge(k); return length(sub(b.vertices[x], b.vertices[y])) }
