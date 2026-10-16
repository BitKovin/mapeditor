// Brush geometry. validateBrush() is the Shell's copy of Core's rules
// (core/src/brush.cpp); both are run against testdata/brush_vectors.json so they can't drift.
import type { Brush, Face } from './document'
import { Vec3, sub, dot, cross, normalize, length } from './math'

export function makeBox(id: string, min: Vec3, max: Vec3, texture = 'dev/grey'): Brush {
  const [x0, y0, z0] = min
  const [x1, y1, z1] = max
  const vertices: Vec3[] = [
    [x0, y0, z0], [x1, y0, z0], [x1, y1, z0], [x0, y1, z0],
    [x0, y0, z1], [x1, y0, z1], [x1, y1, z1], [x0, y1, z1],
  ]
  // -Z, +Z, -X, +X, -Y, +Y; every loop is CCW seen from outside.
  const loops = [[0, 3, 2, 1], [4, 5, 6, 7], [0, 4, 7, 3], [1, 2, 6, 5], [0, 1, 5, 4], [3, 7, 6, 2]]
  return { id, op: 'add', vertices, faces: loops.map((loop, i) => ({ id: i, loop, texture })) }
}

export function faceNormal(b: Brush, f: Face): Vec3 {
  const p0 = b.vertices[f.loop[0]]
  let sum: Vec3 = [0, 0, 0]
  for (let i = 1; i + 1 < f.loop.length; i++) {
    const c = cross(sub(b.vertices[f.loop[i]], p0), sub(b.vertices[f.loop[i + 1]], p0))
    sum = [sum[0] + c[0], sum[1] + c[1], sum[2] + c[2]]
  }
  return normalize(sum)
}

export function signedVolume(b: Brush): number {
  let vol = 0
  for (const f of b.faces) {
    const a = b.vertices[f.loop[0]]
    for (let i = 1; i + 1 < f.loop.length; i++) vol += dot(a, cross(b.vertices[f.loop[i]], b.vertices[f.loop[i + 1]])) / 6
  }
  return vol
}

/** Problem description, or null if the brush is fine. `requireConvex` = worldspawn/func_group. */
export function validateBrush(b: Brush, requireConvex: boolean, eps = 1e-4): string | null {
  const n = b.vertices.length
  for (let i = 0; i < n; i++)
    if (!b.vertices[i].every(Number.isFinite)) return `vertex ${i} has a non-finite coordinate`
  if (b.faces.length === 0) return 'brush has no faces'

  const ids = new Set<number>()
  for (const f of b.faces) {
    const where = `face ${f.id}`
    if (ids.has(f.id)) return `duplicate face id ${f.id}`
    ids.add(f.id)
    if (f.loop.length < 3) return `${where} has fewer than 3 vertices`
    const seen = new Set<number>()
    for (const idx of f.loop) {
      if (!Number.isInteger(idx) || idx < 0 || idx >= n)
        return `${where} references vertex ${idx} but the brush only has ${n}`
      if (seen.has(idx)) return `${where} uses vertex ${idx} twice`
      seen.add(idx)
    }
    if (f.uvs && f.uvs.length > 0 && f.uvs.length !== f.loop.length)
      return `${where} has ${f.uvs.length} uvs for ${f.loop.length} corners`
  }
  for (const f of b.faces) {
    const nrm = faceNormal(b, f)
    if (length(nrm) < 0.5) return `face ${f.id} is degenerate (zero area)`
    const p0 = b.vertices[f.loop[0]]
    for (const idx of f.loop) {
      const off = Math.abs(dot(nrm, sub(b.vertices[idx], p0)))
      if (off > eps) return `face ${f.id} is not planar (vertex ${idx} is ${off.toFixed(6)} m off the plane); split it into planar pieces`
    }
  }
  return requireConvex ? checkConvexSolid(b, eps) : null
}

function checkConvexSolid(b: Brush, eps: number): string | null {
  if (b.faces.length < 4) return 'a solid needs at least 4 faces'
  const edges = new Map<string, number>()
  for (const f of b.faces)
    for (let i = 0; i < f.loop.length; i++) {
      const k = `${f.loop[i]}>${f.loop[(i + 1) % f.loop.length]}`
      edges.set(k, (edges.get(k) ?? 0) + 1)
    }
  for (const [k, count] of edges) {
    const [a, c] = k.split('>')
    if (count !== 1) return `edge ${a}->${c} is used twice in the same direction (inconsistent winding or non-manifold)`
    if (!edges.has(`${c}>${a}`)) return `mesh is not closed (open edge between vertices ${a} and ${c})`
  }
  const vol = signedVolume(b)
  if (vol < 0) return 'faces are wound inward (negative volume)'
  if (vol < 1e-12) return 'brush is degenerate (zero volume)'

  const used = new Set<number>()
  for (const f of b.faces) f.loop.forEach((i) => used.add(i))
  for (const f of b.faces) {
    const nrm = faceNormal(b, f)
    const d = dot(nrm, b.vertices[f.loop[0]])
    for (const idx of used)
      if (dot(nrm, b.vertices[idx]) - d > eps) return `brush is not convex (vertex ${idx} lies in front of face ${f.id})`
  }
  return null
}

/** Ear-clipping triangulation in the face's plane; handles concave faces. Winding is preserved. */
export function triangulateFace(b: Brush, f: Face): [number, number, number][] {
  const nrm = faceNormal(b, f)
  const [ax, ay, az] = [Math.abs(nrm[0]), Math.abs(nrm[1]), Math.abs(nrm[2])]
  const k = ax >= ay && ax >= az ? 0 : ay >= az ? 1 : 2
  const [u, v] = k === 0 ? [1, 2] : k === 1 ? [2, 0] : [0, 1] // (u, v, k) is right-handed
  const p = (i: number): [number, number] => [b.vertices[i][u], b.vertices[i][v]]
  const cr = (o: [number, number], a: [number, number], c: [number, number]) =>
    (a[0] - o[0]) * (c[1] - o[1]) - (a[1] - o[1]) * (c[0] - o[0])

  let idx = [...f.loop]
  let area = 0
  for (let i = 0; i < idx.length; i++) {
    const a = p(idx[i]), c = p(idx[(i + 1) % idx.length])
    area += a[0] * c[1] - c[0] * a[1]
  }
  const flipped = area < 0
  if (flipped) idx = idx.reverse()

  const tris: [number, number, number][] = []
  const inside = (q: [number, number], a: [number, number], c: [number, number], d: [number, number]) =>
    cr(a, c, q) >= -1e-12 && cr(c, d, q) >= -1e-12 && cr(d, a, q) >= -1e-12
  let guard = idx.length * idx.length
  while (idx.length > 3 && guard-- > 0) {
    let clipped = false
    for (let i = 0; i < idx.length; i++) {
      const a = idx[(i + idx.length - 1) % idx.length], c = idx[i], d = idx[(i + 1) % idx.length]
      if (cr(p(a), p(c), p(d)) <= 1e-12) continue // reflex or degenerate corner
      if (idx.some((q) => q !== a && q !== c && q !== d && inside(p(q), p(a), p(c), p(d)))) continue
      tris.push([a, c, d])
      idx.splice(i, 1)
      clipped = true
      break
    }
    if (!clipped) break
  }
  for (let i = 1; i + 1 < idx.length; i++) tris.push([idx[0], idx[i], idx[i + 1]]) // last triangle (or fan fallback)
  return flipped ? tris.map(([a, c, d]) => [a, d, c]) : tris
}

export function brushBounds(b: Brush): { min: Vec3; max: Vec3 } | null {
  let min: Vec3 = [Infinity, Infinity, Infinity], max: Vec3 = [-Infinity, -Infinity, -Infinity]
  let any = false
  for (const f of b.faces)
    for (const i of f.loop) {
      const p = b.vertices[i]
      if (!p) continue
      any = true
      min = [Math.min(min[0], p[0]), Math.min(min[1], p[1]), Math.min(min[2], p[2])]
      max = [Math.max(max[0], p[0]), Math.max(max[1], p[1]), Math.max(max[2], p[2])]
    }
  return any ? { min, max } : null
}

export function translateBrush(b: Brush, d: Vec3): Brush {
  return { ...b, vertices: b.vertices.map((p) => [p[0] + d[0], p[1] + d[1], p[2] + d[2]] as Vec3) }
}
