// The relief of textures as geometry for the Core's lightmap bake (core/relief_geometry.h).
//
// The Core bakes light on the FLAT surface pieces; a relief laid over a face (model/reliefApply.ts) is drawn on top of them and reuses their lightmap
// coordinates. For that light to be right, the bake has to see the relief: it must cast shadows (be an occluder, instead of the flat face it replaces)
// and the texel under a relief vertex has to be lit where the relief really is (displaced position, relief normal), so pits and bumps get shadowed and
// shaded like geometry. The Core knows nothing of texture projections, repeat modes or relief tiles (they live in the document and in the texture's
// properties), so the Shell sends the geometry it draws: for every authored face (and side) whose texture has a relief, the displaced triangles of all
// its surface pieces. The Core keys them like its own surface pieces (instance, entity, brush instance, brush, face, side) and checks that the face's
// plane still matches before it uses them, so a message that is out of date by one edit is ignored instead of wrong.
//
// The triangles are exactly what the viewport draws: the same functions (`reliefTriangle`, the repetition plan of uvmesh.ts) over the same pieces
// (the Core's carved fragments, or the authored faces of a brush nothing cuts). A triangle that cannot be expanded (see reliefTriangle) is flat in the
// drawing, and so it is here.
import type { Brush, Face } from './document'
import type { DerivedEntry, World } from './derived'
import { brushIndex, carvedKey, instanceOf } from './derived'
import { faceNormal } from './geometry'
import type { Vec3 } from './math'
import { fanBorder, neighbourFn, newBudget, planHasRelief, pieceBorder, polygonBorders, reliefOf, reliefTriangle } from './reliefApply'
import { faceUvAt, lerpVec3, planForFace, retexture } from './uvmesh'

/** Most triangles of relief one send holds (beyond it the remaining faces stay flat in the bake, like in the drawing's own budget). */
export const RELIEF_BAKE_BUDGET = 400000

export interface ReliefFace {
  /** Identity of the surface pieces this replaces: the instance and entity that own the surface, the brush (a cut wall: the subtractor) and its instance, the face. */
  instance: string; entityId: string; brushInstance: string; brushId: string; faceId: number
  /** A cut wall (a subtractor's face seen from the carved room): its normal is the authored one reversed. */
  back: boolean
  /** The flat face's plane: normal . x = offset. The relief stands out of it along `normal`. */
  normal: Vec3; offset: number
  /** Displaced world triangles, 9 floats each. */
  tris: number[]
}

const sub = (a: Vec3, b: Vec3): Vec3 => [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
const dot = (a: Vec3, b: Vec3): number => a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
const cross = (a: Vec3, b: Vec3): Vec3 => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]

/** Newell normal of a polygon, unit (the Core's piece normal). */
function polygonNormal(v: readonly Vec3[]): Vec3 {
  let s: Vec3 = [0, 0, 0]
  const p0 = v[0]
  for (let i = 1; i + 1 < v.length; i++) { const c = cross(sub(v[i], p0), sub(v[i + 1], p0)); s = [s[0] + c[0], s[1] + c[1], s[2] + c[2]] }
  const l = Math.hypot(s[0], s[1], s[2])
  return l < 1e-12 ? [0, 0, 0] : [s[0] / l, s[1] / l, s[2] / l]
}

/** `border()[k]`: edge k of `verts` (corner k to k + 1) is the outline of the authored face (the relief is closed there by connecting walls); computed when needed. */
interface Piece { brushInstance: string; brushId: string; faceId: number; verts: Vec3[]; border: () => boolean[] }

/** Does the face show any relief: its own texture's, or (repeat modes) an alternate texture's of the plan? */
function faceHasRelief(brush: Brush, face: Face): boolean {
  const plan = planForFace(brush, face)
  return plan ? planHasRelief(plan) : reliefOf(face.texture) !== null
}

/** The displayed triangles of one polygon piece of a face with relief (9 floats each; a triangle that stays flat in the drawing stays flat). */
function pieceTriangles(brush: Brush, face: Face, verts: readonly Vec3[], border: readonly boolean[], normal: Vec3, budget: { left: number }, out: number[]): void {
  const nb = neighbourFn(brush, face)
  const plan = planForFace(brush, face)
  const relief = reliefOf(face.texture)
  const uv = plan ? [] : (verts as Vec3[]).map(faceUvAt(brush, face)) // the face's own UVs (stored before projection), see faceUvAt
  const flat = (p: readonly [Vec3, Vec3, Vec3]): void => { for (const q of p) out.push(q[0], q[1], q[2]) }
  const take = (got: number[]): void => { for (let i = 0; i + 7 < got.length; i += 8) out.push(got[i], got[i + 1], got[i + 2]) }
  for (let i = 1; i + 1 < verts.length; i++) {
    const tri: [Vec3, Vec3, Vec3] = [verts[0], verts[i], verts[i + 1]]
    const edges = fanBorder(i, verts.length, border)
    if (plan) {
      // every piece of the repetition plan takes the relief of the texture it samples, placed by its own uv (reliefApply.ts planWithRelief)
      for (const o of retexture(plan, [tri])) {
        const pos = o.verts.map((v) => lerpVec3(tri, v.w)) as [Vec3, Vec3, Vec3]
        const r = reliefOf(o.texture)
        const got = r ? reliefTriangle(pos, o.verts.map((v) => v.uv) as [[number, number], [number, number], [number, number]], normal, r, budget, pieceBorder(o.verts, edges), nb) : null
        if (got) take(got); else flat(pos)
      }
      continue
    }
    const got = relief ? reliefTriangle(tri, [uv[0], uv[i], uv[i + 1]], normal, relief, budget, edges, nb) : null
    if (got) take(got); else flat(tri)
  }
}

/**
 * The relief geometry of the world: one entry per authored face side that shows a relief. `entries` are the Core's carved results that are still valid for
 * this world (`usableEntries`); `known` lists every brush the Core carved (`carvedKey`), so a brush whose result is out of date is left out (its pieces
 * are not the authored faces any more) instead of being covered by a whole-face relief.
 */
export function reliefFaces(world: World, entries: readonly DerivedEntry[], known: ReadonlySet<string> = new Set(entries.map((e) => carvedKey(e.entityId, e.brushId, e.instance)))): ReliefFace[] {
  const byKey = new Map(entries.map((e) => [carvedKey(e.entityId, e.brushId, e.instance), e]))
  const indexes = new Map<string, ReturnType<typeof brushIndex>>()
  const owner = (instance: string) => { let m = indexes.get(instance); if (!m) { m = brushIndex(world, instance); indexes.set(instance, m) } return m }
  const budget = newBudget(RELIEF_BAKE_BUDGET)
  const out = new Map<string, ReliefFace>()

  const add = (entityId: string, instance: string, p: Piece): void => {
    const parentBrush = owner(p.brushInstance).get(p.brushId)?.brush
    const face = parentBrush?.faces.find((f) => f.id === p.faceId)
    if (!parentBrush || !face || p.verts.length < 3 || !faceHasRelief(parentBrush, face)) return
    const normal = polygonNormal(p.verts)
    if (normal[0] === 0 && normal[1] === 0 && normal[2] === 0) return
    const back = dot(normal, faceNormal(parentBrush, face)) < 0
    const key = `${instance}|${entityId}|${p.brushInstance}|${p.brushId}|${p.faceId}|${back ? 'B' : 'F'}`
    let f = out.get(key)
    if (!f) { f = { instance, entityId, brushInstance: p.brushInstance, brushId: p.brushId, faceId: p.faceId, back, normal, offset: dot(normal, p.verts[0]), tris: [] }; out.set(key, f) }
    pieceTriangles(parentBrush, face, p.verts, p.border(), normal, budget, f.tris)
  }

  const visit = (entityId: string, instance: string, brushes: readonly Brush[] | undefined): void => {
    for (const b of brushes ?? []) {
      if (b.op === 'subtract') continue // a subtractor owns surface only as the walls it cuts, which come with the carved brush's fragments
      const key = carvedKey(entityId, b.id, instance)
      const entry = byKey.get(key)
      if (entry) {
        const polys = entry.fragments.map((fr) => {
          const verts: Vec3[] = []
          for (let i = 0; i + 2 < fr.v.length; i += 3) verts.push([fr.v[i], fr.v[i + 1], fr.v[i + 2]])
          return verts
        })
        // the fragments of one authored face close their relief along the outline of their union only
        const groups = new Map<string, number[]>()
        entry.fragments.forEach((fr, k) => { const g = groups.get(`${fr.brush}#${fr.face}`) ?? []; groups.set(`${fr.brush}#${fr.face}`, g); g.push(k) })
        const border: boolean[][] = []
        const borderOf = (k: number): boolean[] => {
          if (!border[k]) { const g = groups.get(`${entry.fragments[k].brush}#${entry.fragments[k].face}`)!; polygonBorders(g.map((m) => polys[m])).forEach((b, m) => { border[g[m]] = b }) }
          return border[k]
        }
        entry.fragments.forEach((fr, k) => add(entityId, instance, { brushInstance: fr.instance ?? entry.instance, brushId: fr.brush, faceId: fr.face, verts: polys[k], border: () => borderOf(k) }))
      } else if (!known.has(key)) {
        for (const f of b.faces) add(entityId, instance, { brushInstance: instance, brushId: b.id, faceId: f.id, verts: f.loop.map((i) => b.vertices[i]), border: () => f.loop.map(() => true) })
      }
    }
  }
  for (const e of world.doc.entities) visit(e.id, '', e.brushes)
  for (const r of world.prefabs) visit(r.entity.id, instanceOf(r), r.entity.brushes)

  return [...out.entries()].sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0)).map(([, f]) => f).filter((f) => f.tris.length >= 9)
}

/** A cheap content signature (FNV-1a over the keys and the float bits): equal geometry gives equal text, so an unchanged relief is not sent again. */
export function reliefSignature(faces: readonly ReliefFace[]): string {
  const buf = new DataView(new ArrayBuffer(8))
  let h = 2166136261 >>> 0
  const mix = (n: number): void => { h = Math.imul(h ^ n, 16777619) >>> 0 }
  for (const f of faces) {
    for (const ch of `${f.instance}|${f.entityId}|${f.brushInstance}|${f.brushId}|${f.faceId}|${f.back}`) mix(ch.charCodeAt(0))
    for (const x of [...f.normal, f.offset, ...f.tris]) { buf.setFloat32(0, x); mix(buf.getUint32(0)) }
    mix(f.tris.length)
  }
  return `${faces.length}:${h.toString(16)}`
}

const toBase64 = (bytes: Uint8Array): string => {
  let s = ''
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode(...bytes.subarray(i, i + 0x8000))
  return btoa(s)
}

/** The `relief_geometry` message (core/protocol.h): vertices are base64 of little endian float32, 9 per triangle. */
export function reliefMessage(faces: readonly ReliefFace[]): string {
  return JSON.stringify({
    type: 'relief_geometry',
    faces: faces.map((f) => ({
      i: f.instance, e: f.entityId, bi: f.brushInstance, b: f.brushId, f: f.faceId, s: f.back ? 1 : 0, n: f.normal, d: f.offset,
      v: toBase64(new Uint8Array(Float32Array.from(f.tris).buffer)),
    })),
  })
}
