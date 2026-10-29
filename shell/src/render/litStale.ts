// Lit / Lighting-only views while the Core's lit mesh is out of date. The lit mesh is baked from the document as the Core saw it; after a move or
// an edit the Shell's brush differs from the mesh pieces until the Core sends a new mesh. Instead of showing the brush at its old position, the
// pieces of every changed brush are hidden and the brush is drawn from its current faces with the light it had: each face keeps its lightmap
// mapping (page, style mask, affine uv) evaluated at the position the face had in the mesh (current position minus the estimated translation).
// A new mesh replaces all of this, because then the pieces match the brushes again.
import type { LitFace, LitFaceMap } from '../net/protocol'
import type { World } from '../model/derived'
import { brushIndex } from '../model/derived'
import type { Brush } from '../model/document'
import { faceNormal, triangulateFace } from '../model/geometry'
import type { Vec3 } from '../model/math'
import { faceUvAt, lerpVec3, planForFace, retexture } from '../model/uvmesh'

/** Floats per vertex of the lit mesh as the Core sends it: position (3), normal (3), lightmap u, v, page, style mask (3). */
const CORE_FLOATS = 12
/** Distance (metres) a mesh piece may be off its face's plane or the brush bounds and still count as unchanged. */
const EPS = 2e-3
const COS = 0.999

export const staleKey = (brushInstance: string, brushId: string): string => `${brushInstance}|${brushId}`
const mapKey = (f: LitFace): string => `${f.i}|${f.e}|${f.b}|${f.f}`

/** One face of a changed brush, in the Core's vertex layout, plus the albedo coordinates of the current projection. */
export interface OverlayFace { texture: string; vertexCount: number; core: number[]; uv: number[]; /** the brush it belongs to (`staleKey`) */ brush: string }
export interface LitAnalysis {
  /** Brushes (`brushInstance|brushId`) whose mesh pieces no longer match: hide their pieces. */
  stale: Set<string>
  /** The changed brushes' current faces with their old light. */
  overlay: OverlayFace[]
}

const dot3 = (a: Vec3, b: Vec3): number => a[0] * b[0] + a[1] * b[1] + a[2] * b[2]

/** The piece lies on the face's plane facing the same way (a subtractor's cut walls face the other way: either side counts there). */
const sameOrientation = (b: Brush, faceNormal: Vec3, pieceNormal: Vec3): boolean => {
  const d = dot3(faceNormal, pieceNormal)
  return b.op === 'subtract' ? Math.abs(d) >= COS : d >= COS
}

/** Least squares translation t with n . t = r for every constraint (directions no face constrains stay 0). */
function solveShift(rows: { n: Vec3; r: number }[]): Vec3 {
  const A = [[1e-6, 0, 0], [0, 1e-6, 0], [0, 0, 1e-6]], b: Vec3 = [0, 0, 0]
  for (const { n, r } of rows) for (let i = 0; i < 3; i++) { for (let j = 0; j < 3; j++) A[i][j] += n[i] * n[j]; b[i] += n[i] * r }
  const det = (m: number[][]) => m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0])
  const D = det(A)
  if (Math.abs(D) < 1e-12) return [0, 0, 0]
  const t: Vec3 = [0, 0, 0]
  for (let c = 0; c < 3; c++) t[c] = det(A.map((row, i) => row.map((v, j) => (j === c ? b[i] : v)))) / D
  return t
}

interface Piece { face: LitFace; pts: Vec3[]; normal: Vec3 }
interface BrushPieces { brush: Brush | undefined; pieces: Piece[]; first: LitFace }

/**
 * Compares the mesh pieces with the current brushes. `fresh` remembers brush objects already found unchanged for this mesh (the document keeps
 * unchanged brushes as the same objects), so a drag only re-checks what it touches. Returns null when nothing differs.
 */
export function analyzeLit(mesh: Float32Array, faces: readonly LitFace[], world: World, maps: ReadonlyMap<string, LitFaceMap>, fresh: WeakSet<Brush>): LitAnalysis | null {
  const indexes = new Map<string, ReturnType<typeof brushIndex>>()
  const owner = (instance: string) => { let m = indexes.get(instance); if (!m) { m = brushIndex(world, instance); indexes.set(instance, m) } return m }
  const total = Math.floor(mesh.length / CORE_FLOATS)
  const byBrush = new Map<string, BrushPieces>()
  let at = 0
  for (const f of faces) {
    const verts = f.n * 3
    if (at + verts > total) break
    const key = staleKey(f.bi, f.b)
    let g = byBrush.get(key)
    if (!g) { g = { brush: owner(f.bi).get(f.b)?.brush, pieces: [], first: f }; byBrush.set(key, g) }
    if (!g.brush || !fresh.has(g.brush)) {
      const pts: Vec3[] = []
      for (let k = 0; k < verts; k++) { const o = (at + k) * CORE_FLOATS; pts.push([mesh[o], mesh[o + 1], mesh[o + 2]]) }
      const o0 = at * CORE_FLOATS
      g.pieces.push({ face: f, pts, normal: [mesh[o0 + 3], mesh[o0 + 4], mesh[o0 + 5]] })
    }
    at += verts
  }

  const stale = new Set<string>()
  const overlay: OverlayFace[] = []
  for (const [key, g] of byBrush) {
    const b = g.brush
    if (!b) { stale.add(key); continue } // the brush is gone: its pieces go with it
    if (g.pieces.length === 0) continue // already known unchanged
    const cur = new Map<number, { n: Vec3; d: number }>()
    for (const f of b.faces) { const n = faceNormal(b, f); cur.set(f.id, { n, d: dot3(n, b.vertices[f.loop[0]]) }) }
    const lo: Vec3 = [Infinity, Infinity, Infinity], hi: Vec3 = [-Infinity, -Infinity, -Infinity]
    for (const v of b.vertices) for (let c = 0; c < 3; c++) { lo[c] = Math.min(lo[c], v[c]); hi[c] = Math.max(hi[c], v[c]) }
    let changed = false
    for (const p of g.pieces) {
      const c = cur.get(p.face.f)
      // A subtractor's pieces are the walls it cut (FaceSide::Back in the Core): their normal is the authored face normal REVERSED, so for a subtractor
      // either orientation of the same plane is unchanged. Comparing signed made every cut wall look changed, and they were dropped from the Lit view.
      if (!c || !sameOrientation(b, c.n, p.normal)) { changed = true; break }
      if (p.pts.some((q) => Math.abs(dot3(c.n, q) - c.d) > EPS || q.some((x, i) => x < lo[i] - EPS || x > hi[i] + EPS))) { changed = true; break }
    }
    if (!changed) { fresh.add(b); continue }
    stale.add(key)
    // A subtractive brush is tooling: it never receives light and has no surfaces of its own. Its pieces in the mesh are the walls it cut into other brushes;
    // while it is out of date those go (the carved world is redrawn by the next mesh), but its authored volume must not be drawn lit.
    if (b.op === 'subtract') continue

    // translation between the mesh and the brush, from the planes of the faces that kept their orientation
    const rows: { n: Vec3; r: number }[] = []
    const seen = new Set<number>()
    for (const p of g.pieces) {
      const c = cur.get(p.face.f)
      if (!c || seen.has(p.face.f) || dot3(c.n, p.normal) < COS) continue
      seen.add(p.face.f)
      rows.push({ n: c.n, r: c.d - dot3(p.normal, p.pts[0]) })
    }
    const t = solveShift(rows)
    // the lightmap mapping of every face of the brush (a new face takes the one of the face most like it)
    const own = g.pieces.filter((p) => maps.has(mapKey(p.face)))
    if (own.length === 0) { stale.delete(key); continue } // no light data to carry over: leave the old pieces
    for (const f of b.faces) {
      const c = cur.get(f.id)!
      let src = own.find((p) => p.face.f === f.id)
      if (!src) { let best = -2; for (const p of own) { const s = dot3(p.normal, c.n); if (s > best) { best = s; src = p } } }
      const m = maps.get(mapKey(src!.face))!
      const texture = f.texture ?? 'dev/grey'
      const tris = triangulateFace(b, f)
      const coreAt = (v: Vec3): number[] => {
        const x = v[0] - t[0], y = v[1] - t[1], z = v[2] - t[2]
        return [v[0], v[1], v[2], c.n[0], c.n[1], c.n[2], x * m.u[0] + y * m.u[1] + z * m.u[2] + m.u[3], x * m.v[0] + y * m.v[1] + z * m.v[2] + m.v[3], m.pg, m.k[0], m.k[1], m.k[2]]
      }
      const plan = planForFace(b, f)
      if (plan) {
        const triPts = tris.map(([a, bb, cc]) => [b.vertices[a], b.vertices[bb], b.vertices[cc]] as [Vec3, Vec3, Vec3])
        const per = new Map<string, OverlayFace>()
        for (const o of retexture(plan, triPts)) {
          let of = per.get(o.texture)
          if (!of) { of = { texture: o.texture, vertexCount: 0, core: [], uv: [], brush: key }; per.set(o.texture, of); overlay.push(of) }
          for (const v of o.verts) { of.core.push(...coreAt(lerpVec3(triPts[o.src], v.w))); of.uv.push(v.uv[0], v.uv[1]); of.vertexCount++ }
        }
        continue
      }
      const pts: Vec3[] = [], core: number[] = []
      for (const tri of tris) for (const i of tri) { pts.push(b.vertices[i]); core.push(...coreAt(b.vertices[i])) }
      if (pts.length === 0) continue
      const uvAt = faceUvAt(b, f)
      const uvs = pts.map(uvAt)
      overlay.push({ texture, vertexCount: pts.length, core, uv: uvs.flatMap((u) => [u[0], u[1]]), brush: key })
    }
  }
  return stale.size === 0 ? null : { stale, overlay }
}

/** The mesh without the pieces of stale brushes, followed by the overlay (Core vertex layout; the Lighting-only view draws it directly). */
export function buildPlainMesh(mesh: Float32Array, faces: readonly LitFace[], a: LitAnalysis): Float32Array {
  const total = Math.floor(mesh.length / CORE_FLOATS)
  const out: number[] = []
  let at = 0
  for (const f of faces) {
    const verts = f.n * 3
    if (at + verts > total) break
    if (!a.stale.has(staleKey(f.bi, f.b))) for (let i = at * CORE_FLOATS; i < (at + verts) * CORE_FLOATS; i++) out.push(mesh[i])
    at += verts
  }
  for (const o of a.overlay) for (const v of o.core) out.push(v)
  return Float32Array.from(out)
}
