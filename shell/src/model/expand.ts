// Prefab-instance expansion (mirrors core/src/prefab.cpp). Core loads the prefab files
// (filesystem work) and sends them as the "library"; expanding instances is pure math,
// so it happens here, instantly -- moving an instance never waits on Core.
import type { Brush, Entity, EntityOverride, MapDocument, PrefabInstance } from './document'
import { translateBrush } from './geometry'
import { Vec3 } from './math'
import { mapBrushPoints } from './transform'
import type { Mat3 } from './rotation'
import { I3, anglesFromRotation, apply3, det3, mul3 as mul, rotationFromAngles } from './rotation'
export { anglesFromRotation, rotationFromAngles }

export type Library = Record<string, MapDocument | { error: string }>
/** `refScopes[key]` = how many leading ids of `instancePath.slice(1)` the ref `entity.refs[key]` is relative to (mirrors Core's RefSlot.scopeLen):
 *  a ref written in the prefab file is relative to the entity's own instance; one overridden in instance j, to j's parent scope. */
export interface ResolvedEntity { entity: Entity; instancePath: string[]; sourceEntityId: string; refScopes?: Record<string, number> }

export function normalizePath(p: string): string {
  const out: string[] = []
  for (const part of p.replace(/\\/g, '/').split('/')) {
    if (part === '' || part === '.') continue
    if (part === '..' && out.length > 0 && out[out.length - 1] !== '..') out.pop()
    else out.push(part)
  }
  return out.join('/')
}

// ---- affine algebra (mirrors core/src/prefab.cpp; DESIGN.md 18.1b) ---------------------------------------
interface Xf { lin: Mat3; rot: Mat3; scale: Vec3; t: Vec3; rotated: boolean; linear: boolean }
const IDENTITY: Xf = { lin: I3, rot: I3, scale: [1, 1, 1], t: [0, 0, 0], rotated: false, linear: false }
const applyXf = (x: Xf, p: Vec3): Vec3 => { const q = apply3(x.lin, p); return [q[0] + x.t[0], q[1] + x.t[1], q[2] + x.t[2]] }

function compose(parent: Xf, inst: PrefabInstance): Xf {
  const ang = inst.angles ?? [0, 0, 0]
  const s = inst.scale ?? [1, 1, 1]
  const R = rotationFromAngles(ang)
  const S: Mat3 = [s[0], 0, 0, 0, s[1], 0, 0, 0, s[2]]
  const rotated = parent.rotated || ang.some((v) => v !== 0)
  const linear = parent.linear || rotated || s.some((v) => v !== 1)
  return {
    lin: mul(parent.lin, mul(R, S)), rot: mul(parent.rot, R),
    scale: [parent.scale[0] * s[0], parent.scale[1] * s[1], parent.scale[2] * s[2]],
    t: applyXf(parent, inst.origin ?? [0, 0, 0]), rotated, linear,
  }
}

const fmtVec = (v: Vec3): string => `${v[0]} ${v[1]} ${v[2]}`

function placeBrush(b: Brush, x: Xf): Brush {
  if (!x.linear) return translateBrush(b, x.t)
  return mapBrushPoints(b, (p) => applyXf(x, p), det3(x.lin) < 0, true)
}

/** One entity of a prefab file, with the overrides of every instance in `chain` applied (outer wins), moved into root space. */
function placeEntity(src: Entity, x: Xf, chain: PrefabInstance[]): { entity: Entity; refScopes: Record<string, number> } | null {
  let e: Entity = src
  const refScopes: Record<string, number> = {}
  for (const k of Object.keys(src.refs ?? {})) refScopes[k] = chain.length
  for (let j = chain.length - 1; j >= 0; j--) {
    const path = [...chain.slice(j + 1).map((p) => p.id), src.id].join('/')
    const ov: EntityOverride | undefined = chain[j].overrides?.[path]
    if (!ov) continue
    if (ov.deleted) return null
    e = { ...e }
    if (ov.keyvalues) e.keyvalues = { ...(e.keyvalues ?? {}), ...ov.keyvalues }
    if (ov.refs) { e.refs = { ...(e.refs ?? {}), ...ov.refs }; for (const k of Object.keys(ov.refs)) refScopes[k] = j }
    if (ov.origin) e.origin = ov.origin
    if (ov.angles) e.angles = ov.angles
    if (ov.scale) e.keyvalues = { ...(e.keyvalues ?? {}), modelscale_vec: fmtVec(ov.scale) }
  }
  const localRot = rotationFromAngles(e.angles ?? [0, 0, 0])
  const out: Entity = { ...e, origin: applyXf(x, e.origin ?? [0, 0, 0]) }
  if (x.rotated || e.angles) out.angles = x.rotated ? anglesFromRotation(mul(x.rot, rotationFromAngles(e.angles ?? [0, 0, 0]))) : e.angles
  if (e.brushes) out.brushes = e.brushes.map((b) => placeBrush(b, x))
  else if (e.classname === 'decal' && x.scale.some((v) => Math.abs(v - 1) > 1e-12)) {
    // a decal's size is width / height / depth along its own axes (x / z / y before `angles`); see core/src/prefab.cpp
    const kv = { ...(e.keyvalues ?? {}) }
    for (const [key, axis, dflt] of [['width', [1, 0, 0], 1], ['height', [0, 0, 1], 1], ['depth', [0, 1, 0], 0.5]] as [string, Vec3, number][]) {
      const v = apply3(x.lin, apply3(localRot, axis))
      const cur = Number(kv[key])
      kv[key] = String((Number.isFinite(cur) && cur > 1e-6 ? cur : dflt) * Math.hypot(v[0], v[1], v[2]))
    }
    out.keyvalues = kv
  } else if (x.scale.some((v) => Math.abs(v - 1) > 1e-12)) {
    const cur = (e.keyvalues?.modelscale_vec ?? '1 1 1').split(/\s+/).map(Number)
    const base: Vec3 = cur.length === 3 && cur.every(Number.isFinite) ? [cur[0], cur[1], cur[2]] : [1, 1, 1]
    out.keyvalues = { ...(e.keyvalues ?? {}), modelscale_vec: fmtVec([base[0] * x.scale[0], base[1] * x.scale[1], base[2] * x.scale[2]]) }
  }
  return { entity: out, refScopes }
}

/** Entities that come from prefab instances only (the root document's own entities are drawn directly). */
export function expandPrefabs(
  doc: MapDocument, library: Library, rootPath = '', maxDepth = 16,
): { entities: ResolvedEntity[]; problems: string[] } {
  const entities: ResolvedEntity[] = []
  const problems: string[] = []
  const stack: string[] = rootPath ? [normalizePath(rootPath)] : []

  const walk = (d: MapDocument, xf: Xf, chain: PrefabInstance[], path: string[]) => {
    if (path.length > maxDepth) return void problems.push(`prefab nesting deeper than ${maxDepth} at ${path.join('/')}`)
    for (const inst of d.prefabInstances ?? []) {
      const key = normalizePath(inst.file)
      if (stack.includes(key)) { problems.push(`prefab cycle: ${[...stack, key].join(' -> ')}`); continue }
      const child = library[key]
      if (!child) { problems.push(`prefab '${key}' is not loaded`); continue }
      if ('error' in child) { problems.push(`prefab '${key}': ${child.error}`); continue }
      const cx = compose(xf, inst)
      const cchain = [...chain, inst]
      const childPath = [...path, inst.id]
      for (const e of child.entities) {
        const placed = placeEntity(e, cx, cchain)
        if (placed) entities.push({ entity: placed.entity, instancePath: childPath, sourceEntityId: e.id, refScopes: placed.refScopes })
      }
      stack.push(key)
      walk(child, cx, cchain, childPath)
      stack.pop()
    }
  }
  walk(doc, IDENTITY, [], ['root'])
  return { entities, problems }
}
