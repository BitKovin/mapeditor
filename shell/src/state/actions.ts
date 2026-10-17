// Editing operations. Pure document functions + thin store wrappers, so tools and UI panels
// stay small and everything is testable without a DOM.
import { defaultLightAngles } from '../model/lights'
import {
  Brush, Entity, EntityOverride, MapDocument, PrefabInstance, Vec3, addEntity, addInstance, brushesCentre, classOf, findEntity, findInstance, instanceNameProblem,
  mapBrush, mapEntity, mapInstance, newId, newInstanceName, originOf, removeBrush, removeEntity, removeInstance,
  requiresConvexBrushes, showsCustomOrigin, translateEntity, isBrushEntity,
} from '../model/document'
import { brushInside, brushesTouch } from '../model/selectVolume'
import { Plane, clipBy, hollowBrush, hullBrush, isConvex, snapBrush, unionVolume } from '../model/brushOps'
import { rotateAbout, transformSelections } from '../model/transform'
import { signedVolume, faceNormal } from '../model/geometry'
import { brushBounds, makeBox, translateBrush, validateBrush } from '../model/geometry'
import { bottomLeft } from '../model/surface'
import { chooseHotspot, faceSize, hotspotProjection } from '../model/hotspots'
import { flowFaces } from '../model/flow'
import { UnwrapMode, Xform, transformFaces, unwrapFaces } from '../model/ftt'
import { parseFaceRepeat } from '../model/texprops'
import { FaceLayout, ParamValue, faceLayoutToJson, parseFaceLayout } from '../model/texgroup'
import { alignBrush, resetSurface } from '../model/surface'
import { naturalSize, texEnv } from '../model/texprops'
import { Projection, clampLightmapScale, defaultProjectionFor, fitProjection, lightmapScaleOf, projectFace, projectionOf } from '../model/uv'
import { selectionPivot } from '../model/transform'
import type { Selection, Store } from './store'
import { ShapePatch, regenerateShape, shapeOf } from '../model/parametric'
import { hideKeys } from '../model/visibility'

export function ensureWorldspawn(doc: MapDocument): MapDocument {
  if (doc.entities.some((e) => classOf(e) === 'worldspawn')) return doc
  return addEntity(doc, { id: 'e_worldspawn', classname: 'worldspawn', keyvalues: {}, origin: [0, 0, 0], brushes: [] })
}

export function addBoxTo(doc: MapDocument, center: Vec3, size = 1): { doc: MapDocument; sel: Selection } {
  const withWorld = ensureWorldspawn(doc)
  const world = withWorld.entities.find((e) => classOf(e) === 'worldspawn')!
  const h = size / 2
  const brush = makeBox(newId('b'), [center[0] - h, center[1] - h, center[2] - h], [center[0] + h, center[1] + h, center[2] + h])
  return {
    doc: mapEntity(withWorld, world.id, (e) => ({ ...e, brushes: [...(e.brushes ?? []), alignBrush(brush)] })),
    sel: { entityId: world.id, brushId: brush.id },
  }
}

export function addBox(store: Store, center: Vec3): void {
  let sel: Selection | null = null
  if (store.edit('Add box', (d) => { const r = addBoxTo(d, center); sel = r.sel; return r.doc })) store.select(sel)
}

export function addPointEntity(store: Store, classname: string, origin: Vec3, keyvalues: Record<string, string> = {}, angles?: Vec3): void {
  const entity: Entity = { id: newId('e'), classname, keyvalues: { ...keyvalues }, origin, angles: angles ?? defaultLightAngles(classname) ?? [0, 0, 0] }
  if (store.edit(`Add ${classname}`, (d) => addEntity(d, entity))) store.select({ entityId: entity.id })
}

/** Everything selected, as a list (the primary selection when there is no multi-selection yet). */
export const selectedList = (store: Store): Selection[] => (store.state.multi.length ? store.state.multi : store.state.selection ? [store.state.selection] : [])

export function deleteSelection(store: Store): void {
  const sels = selectedList(store)
  if (!sels.length) return
  const instances = sels.filter((s) => s.instanceId !== undefined).map((s) => s.instanceId!)
  const plain = sels.filter((s) => s.instanceId === undefined)
  const entities = new Set(plain.filter((s) => !s.brushId).map((s) => s.entityId))
  const brushes = plain.filter((s) => s.brushId && !entities.has(s.entityId))
  const label = sels.length > 1 ? `Delete ${sels.length} items` : instances.length ? 'Delete prefab instance' : sels[0].brushId ? 'Delete brush' : 'Delete entity'
  if (store.edit(label, (d) => {
    let doc = d
    for (const b of brushes) doc = removeBrush(doc, b.entityId, b.brushId!)
    for (const id of entities) doc = removeEntity(doc, id)
    for (const id of instances) doc = removeInstance(doc, id)
    return doc
  })) store.select(null)
}

/** Copies the selection (brushes inside their entity, whole entities as new entities) with fresh ids and selects the copies. */
export function duplicateSelection(store: Store): boolean {
  const sels = selectedList(store)
  if (!sels.length || !store.canEdit) return false
  const copies: Selection[] = []
  const ok = store.edit(sels.length > 1 ? `Duplicate ${sels.length} items` : 'Duplicate', (d) => {
    let doc = d
    const whole = new Set(sels.filter((s) => s.instanceId === undefined && !s.brushId).map((s) => s.entityId))
    for (const s of sels) {
      if (s.instanceId !== undefined) {
        const src = findInstance(doc, s.instanceId)
        if (!src) continue
        const id = newId('pi')
        doc = addInstance(doc, { ...src, id, name: newInstanceName(doc.prefabInstances, src.file) })
        copies.push({ entityId: '', instanceId: id })
        continue
      }
      const e = findEntity(doc, s.entityId)
      if (!e) continue
      if (!s.brushId) {
        const id = newId('e')
        doc = { ...doc, entities: [...doc.entities, { ...e, id, brushes: (e.brushes ?? []).map((b) => ({ ...b, id: newId('b') })) }] }
        copies.push({ entityId: id })
      } else if (!whole.has(s.entityId)) {
        const b = e.brushes?.find((x) => x.id === s.brushId)
        if (!b) continue
        const nb = { ...b, id: newId('b') }
        doc = mapEntity(doc, e.id, (en) => ({ ...en, brushes: [...(en.brushes ?? []), nb] }))
        copies.push({ entityId: e.id, brushId: nb.id })
      }
    }
    return doc
  })
  if (ok) store.selectMany(copies)
  return ok
}

/** Adds several brushes (a shape such as stairs is many) to worldspawn as one undo step and selects them. */
export function addDrawnBrushes(store: Store, brushes: Brush[], label = 'Draw shape'): void {
  if (!brushes.length) return
  let entityId = ''
  const ok = store.edit(label, (d) => {
    const world = ensureWorldspawn(d)
    const w = world.entities.find((e) => classOf(e) === 'worldspawn')!
    entityId = w.id
    return mapEntity(world, w.id, (e) => ({ ...e, brushes: [...(e.brushes ?? []), ...brushes.map(alignBrush)] }))
  })
  if (ok) store.selectMany(brushes.map((b) => ({ entityId, brushId: b.id })))
}

/** Where the gizmo sits for a selection list. */
export function gizmoPivot(doc: MapDocument, sels: Selection[]): Vec3 | null {
  if (sels.length === 0) return null
  return sels.length === 1 ? pivotOf(doc, sels[0]) : selectionPivot(doc, sels)
}

/** Moves what is selected: one brush, or a whole entity (origin + brushes). */
export function translateSelection(doc: MapDocument, sel: Selection, delta: Vec3): MapDocument {
  if (sel.instanceId !== undefined)
    return mapInstance(doc, sel.instanceId, (p) => ({ ...p, origin: [(p.origin?.[0] ?? 0) + delta[0], (p.origin?.[1] ?? 0) + delta[1], (p.origin?.[2] ?? 0) + delta[2]] }))
  return sel.brushId
    ? mapBrush(doc, sel.entityId, sel.brushId, (b) => translateBrush(b, delta))
    : translateEntity(doc, sel.entityId, delta)
}

/** Where the gizmo sits: the entity origin for point/dynamic entities, otherwise the bounds centre. */
export function pivotOf(doc: MapDocument, sel: Selection): Vec3 | null {
  if (sel.instanceId !== undefined) return findInstance(doc, sel.instanceId)?.origin ?? (findInstance(doc, sel.instanceId) ? [0, 0, 0] : null)
  const e = findEntity(doc, sel.entityId)
  if (!e) return null
  if (!sel.brushId && (!isBrushEntity(e) || showsCustomOrigin(e))) return originOf(e)
  const brushes = sel.brushId ? (e.brushes ?? []).filter((b) => b.id === sel.brushId) : (e.brushes ?? [])
  const bounds = brushes.map(brushBounds).filter((b) => b !== null)
  if (bounds.length === 0) return originOf(e)
  const min: Vec3 = [Infinity, Infinity, Infinity], max: Vec3 = [-Infinity, -Infinity, -Infinity]
  for (const b of bounds) for (let i = 0; i < 3; i++) { min[i] = Math.min(min[i], b!.min[i]); max[i] = Math.max(max[i], b!.max[i]) }
  return [(min[0] + max[0]) / 2, (min[1] + max[1]) / 2, (min[2] + max[2]) / 2]
}

// ---- inspector edits ---------------------------------------------------------------
/** Returns a problem if giving `e` this classname would break the convexity rule. */
export function classnameProblem(e: Entity, classname: string): string | null {
  if (!requiresConvexBrushes(classname)) return null
  for (const b of e.brushes ?? []) {
    const p = validateBrush(b, true)
    if (p) return `${classname} needs convex closed solids, but brush '${b.id}': ${p}`
  }
  return null
}

export function setClassname(store: Store, entityId: string, classname: string): void {
  const e = findEntity(store.state.doc, entityId)
  if (!e || classOf(e) === classname || !classname) return
  const problem = classnameProblem(e, classname)
  if (problem) return store.setMessage(problem)
  store.edit('Change classname', (d) => mapEntity(d, entityId, (x) => ({ ...x, classname })))
}

export function setOrigin(store: Store, entityId: string, axis: 0 | 1 | 2, value: number): void {
  if (!Number.isFinite(value)) return
  store.edit('Set origin', (d) => mapEntity(d, entityId, (e) => {
    const o = [...originOf(e)] as Vec3
    if (o[axis] === value) return e
    o[axis] = value
    return { ...e, origin: o }
  }))
}

export function setKeyvalue(store: Store, entityId: string, key: string, value: string | null): void {
  if (!key) return
  store.edit(value === null ? `Remove '${key}'` : `Set '${key}'`, (d) => mapEntity(d, entityId, (e) => {
    const kv = { ...(e.keyvalues ?? {}) }
    if (value === null) { if (!(key in kv)) return e; delete kv[key] } else { if (kv[key] === value) return e; kv[key] = value }
    return { ...e, keyvalues: kv }
  }))
}

export function setBrushOp(store: Store, entityId: string, brushId: string, op: 'add' | 'subtract'): void {
  store.edit(`Brush -> ${op}`, (d) => mapBrush(d, entityId, brushId, (b) => ((b.op ?? 'add') === op ? b : { ...b, op })))
}

export function setCarve(store: Store, entityId: string, brushId: string, key: 'self' | 'structural' | 'nonStructural', value: boolean): void {
  store.edit(`Carve ${key}`, (d) => mapBrush(d, entityId, brushId, (b: Brush) => ({ ...b, carve: { ...(b.carve ?? {}), [key]: value } })))
}

// ---- drawing ------------------------------------------------------------------------
/** Commits a brush drawn with the box tool into worldspawn and selects it. */
export function addDrawnBrush(store: Store, brush: Brush): void {
  let sel: Selection | null = null
  const ok = store.edit('Draw box', (d) => {
    const world = ensureWorldspawn(d)
    const w = world.entities.find((e) => classOf(e) === 'worldspawn')!
    sel = { entityId: w.id, brushId: brush.id }
    return mapEntity(world, w.id, (e) => ({ ...e, brushes: [...(e.brushes ?? []), alignBrush(brush)] }))
  })
  if (ok) store.select(sel)
}

// ---- texturing ----------------------------------------------------------------------
export interface FaceRef { entityId: string; brushId: string; faceId: number }

/** What texture edits apply to: the selected face; else every face of the selected brush / entity. */
export function faceTargets(doc: MapDocument, sel: Selection | Selection[] | null): FaceRef[] {
  const list = Array.isArray(sel) ? sel : sel ? [sel] : []
  const out: FaceRef[] = []
  const seen = new Set<string>()
  for (const s of list) {
    const e = findEntity(doc, s.entityId)
    if (!e) continue
    for (const b of e.brushes ?? []) {
      if (s.brushId && b.id !== s.brushId) continue
      for (const f of b.faces) {
        if (s.faceId !== undefined && f.id !== s.faceId) continue
        const k = `${e.id}/${b.id}/${f.id}`
        if (!seen.has(k)) { seen.add(k); out.push({ entityId: e.id, brushId: b.id, faceId: f.id }) }
      }
    }
  }
  return out
}

function mapFaces(doc: MapDocument, targets: FaceRef[], fn: (b: Brush, f: Brush['faces'][number]) => Brush['faces'][number]): MapDocument {
  let out = doc
  const byBrush = new Map<string, FaceRef[]>()
  for (const t of targets) byBrush.set(`${t.entityId}/${t.brushId}`, [...(byBrush.get(`${t.entityId}/${t.brushId}`) ?? []), t])
  for (const refs of byBrush.values()) {
    const ids = new Set(refs.map((r) => r.faceId))
    out = mapBrush(out, refs[0].entityId, refs[0].brushId, (b) => {
      const faces = b.faces.map((f) => (ids.has(f.id) ? fn(b, f) : f))
      return faces.every((f, i) => f === b.faces[i]) ? b : { ...b, faces } // unchanged -> same object -> no history entry
    })
  }
  return out
}

export function applyTexture(store: Store, texture: string): void {
  store.setActiveTexture(texture)
  const sels = selectedList(store)
  // a selected decal takes the texture as its image
  const decals = sels.filter((s) => !s.brushId && findEntity(store.state.doc, s.entityId)?.classname === 'decal')
  if (decals.length) store.edit('Decal texture', (d) => decals.reduce((doc, s) => mapEntity(doc, s.entityId, (e) => ({ ...e, keyvalues: { ...e.keyvalues, texture } })), d))
  const targets = faceTargets(store.state.doc, sels)
  if (!targets.length) return
  store.edit('Apply texture', (d) => mapFaces(d, targets, (b, f) => {
    if (f.texture === texture) return f
    // a face without any UVs gets the texture at its natural size from its bottom-left corner
    if (!f.projection && !f.uvs?.length && texEnv.info(texture)) {
      const p = bottomLeft(b, { ...f, texture }, defaultProjectionFor(texture))
      return { ...f, texture, projection: p, uvs: projectFace(b, f, p) }
    }
    // otherwise the face keeps its Texture panel values: stretch (scale / the texture's natural size), shift in texels, rotation, shear
    const was = texEnv.info(f.texture ?? 'dev/grey'), now = texEnv.info(texture)
    if (was && now && was.width > 0 && was.height > 0 && now.width > 0 && now.height > 0) {
      const [aw, ah] = naturalSize(was), [nw, nh] = naturalSize(now), kx = nw / aw, ky = nh / ah
      if (f.projection) {
        const p = projectionOf(f)
        const q: Projection = { ...p, scale: [p.scale[0] * kx, p.scale[1] * ky], shift: [p.shift[0] * was.width / now.width, p.shift[1] * was.height / now.height] }
        return { ...f, texture, projection: q, uvs: projectFace(b, { ...f, texture }, q) }
      }
      if (f.uvs?.length) return { ...f, texture, uvs: f.uvs.map(([u, v]) => [u / kx, v / ky] as [number, number]) }
    }
    return { ...f, texture }
  }))
}

/** Applies `change` to each target face's projection and rewrites its UVs. */
export function editProjection(store: Store, label: string, change: (p: Projection, b: Brush, f: Brush['faces'][number]) => Projection): void {
  const targets = faceTargets(store.state.doc, selectedList(store))
  if (!targets.length) return
  store.edit(label, (d) => mapFaces(d, targets, (b, f) => {
    const p = change(projectionOf(f), b, f)
    return { ...f, projection: p, uvs: projectFace(b, f, p) }
  }))
}

/** Sets the lightmap texel density of the target faces to `change(current)` (1 is stored as "absent", so default documents stay unchanged). */
export function editLightmapScale(store: Store, label: string, change: (current: number) => number): void {
  const targets = faceTargets(store.state.doc, selectedList(store))
  if (!targets.length) return
  store.edit(label, (d) => mapFaces(d, targets, (_b, f) => {
    const v = clampLightmapScale(change(lightmapScaleOf(f)))
    if (v === lightmapScaleOf(f)) return f
    const { lightmapScale: _drop, ...rest } = f
    return v === 1 ? (rest as typeof f) : { ...f, lightmapScale: v }
  }))
}

export const resetProjection = (store: Store): void =>
  editProjection(store, 'Reset texture', (_p, b, f) => resetSurface(b, f))
/**
 * Copies the texture and the texturing parameters (scale, shift, rotate) of face `from` onto face `to` and recomputes the target's UVs
 * (REQUIREMENTS §6 B3, right-click a second face while a face is selected). Returns false when either face is missing or they are the same.
 */
export function copySurface(store: Store, from: FaceRef, to: FaceRef): boolean {
  if (from.entityId === to.entityId && from.brushId === to.brushId && from.faceId === to.faceId) return false
  const src = findEntity(store.state.doc, from.entityId)?.brushes?.find((b) => b.id === from.brushId)?.faces.find((f) => f.id === from.faceId)
  if (!src || !findEntity(store.state.doc, to.entityId)?.brushes?.find((b) => b.id === to.brushId)?.faces.some((f) => f.id === to.faceId)) return false
  const p = projectionOf(src), texture = src.texture
  store.edit('Copy UV', (d) => mapFaces(d, [to], (b, f) => ({ ...f, texture, projection: p, uvs: projectFace(b, f, p) })))
  return true
}

/**
 * Flow texture (REQUIREMENTS §6 B5): the UVs of face `from` continue onto face `to` (and the faces between) across their shared edges, unfolded flat, nothing
 * stretched (`model/flow.ts`). Works inside one brush; returns a message for the user ('' = done).
 */
export function flowTexture(store: Store, from: FaceRef, to: FaceRef): string {
  if (from.entityId !== to.entityId || from.brushId !== to.brushId) return 'Flow works between faces of the same brush'
  if (from.faceId === to.faceId) return 'Pick a different face to flow onto'
  const brush = findEntity(store.state.doc, from.entityId)?.brushes?.find((b) => b.id === from.brushId)
  if (!brush) return 'Face not found'
  const r = flowFaces(brush, from.faceId, [to.faceId])
  if (!r) return from.faceId === to.faceId ? 'Pick a different face to flow onto' : 'The faces are not connected through shared edges'
  store.edit('Flow texture', (d) => mapBrush(d, from.entityId, from.brushId, () => r.brush))
  return ''
}

/** The texture of a face (Lift Material, Shift + right-click); null when the face does not exist. Pure so it can be tested without a viewport. */
export function liftMaterial(doc: MapDocument, entityId: string, brushId: string, faceId: number): string | null {
  const f = findEntity(doc, entityId)?.brushes?.find((b) => b.id === brushId)?.faces.find((x) => x.id === faceId)
  return f ? (f.texture ?? 'dev/grey') : null
}

/**
 * Hotspot (REQUIREMENTS §6 B5): every target face shows the hotspot of its texture that fits its size best (`model/hotspots.ts`). Returns a message for the
 * user ('' = done); faces whose texture has no hotspots (or is not loaded) are left alone.
 */
export function applyHotspot(store: Store): string {
  const targets = faceTargets(store.state.doc, selectedList(store))
  if (!targets.length) return 'Select a face or a brush first'
  let hit = 0
  const spotsOf = (texture: string | undefined) => { const i = texEnv.info(texture ?? 'dev/grey'); return i && i.props.hotspots.length ? i : null }
  store.edit('Apply hotspot', (d) => mapFaces(d, targets, (b, f) => {
    const i = spotsOf(f.texture)
    if (!i) return f
    const k = chooseHotspot(i.props.hotspots, i, faceSize(b, f))
    if (k < 0) return f
    hit++
    const p = hotspotProjection(b, f, i.props.hotspots[k], i)
    return { ...f, projection: p, uvs: projectFace(b, f, p) }
  }))
  return hit ? '' : 'The texture has no hotspots yet: draw them in the texture properties (double-click the texture in the browser)'
}

/** Sets (or clears, `undefined`) one axis' repeat mode / edge override on every target face (`face.repeat`, REQUIREMENTS §6 B4). */
export function setFaceRepeat(store: Store, axis: 'u' | 'v', patch: { mode?: 'repeat' | 'mirror' | null; edge?: 'wrap' | 'clamp' | null }): void {
  const targets = faceTargets(store.state.doc, selectedList(store))
  if (!targets.length) return
  store.edit('Face repetition', (d) => mapFaces(d, targets, (_b, f) => {
    const cur = parseFaceRepeat(f.repeat) ?? {}
    const a = { ...(cur[axis] ?? {}) } as { mode?: string; edge?: string }
    for (const k of ['mode', 'edge'] as const) { if (patch[k] === null) delete a[k]; else if (patch[k]) a[k] = patch[k]! }
    const next: Record<string, unknown> = { ...cur }
    if (a.mode || a.edge) next[axis] = a; else delete next[axis]
    const { repeat: _old, ...rest } = f
    return Object.keys(next).length ? { ...rest, repeat: next } : rest
  }))
}

// ---- texture groups (REQUIREMENTS §20 T8) ---------------------------------------------------------------------------------
/**
 * Lays the target faces out by a texture group (`face.layout`). Faces keep their own texture as the fallback (drawn while the group or one of its textures
 * loads); `fallbackTexture` (the group's fallback slot) replaces it so the Surface Inspector shows something that belongs to the group.
 * Each face takes the branch of the rules that fits its orientation, so a whole brush (walls, roof, floor) can be done in one go. Returns the number of faces.
 */
export function applyTexGroup(store: Store, group: string, fallbackTexture?: string, opts: { wallsOnly?: boolean } = {}): number {
  const sels = selectedList(store)
  let targets = faceTargets(store.state.doc, sels)
  if (opts.wallsOnly) {
    // a wall set on a whole brush or entity leaves its roof and floor faces alone; a face picked by hand always gets it
    const explicit = new Set(sels.filter((s) => s.faceId !== undefined).map((s) => `${s.entityId}/${s.brushId}/${s.faceId}`))
    targets = targets.filter((t) => {
      if (explicit.has(`${t.entityId}/${t.brushId}/${t.faceId}`)) return true
      const b = findEntity(store.state.doc, t.entityId)?.brushes?.find((x) => x.id === t.brushId)
      const f = b?.faces.find((x) => x.id === t.faceId)
      return !!b && !!f && Math.abs(faceNormal(b, f)[1]) <= 0.5
    })
  }
  if (!targets.length) return 0
  store.edit('Apply texture group', (d) => mapFaces(d, targets, (_b, f) => ({ ...f, ...(fallbackTexture ? { texture: fallbackTexture } : {}), layout: { group } })))
  return targets.length
}

/** Changes the layout settings of every target face that has one. `params`: a value sets, `null` returns that parameter to the group's default. */
export function setFaceLayout(store: Store, patch: { params?: Record<string, ParamValue | null>; seed?: number | null; rotate?: 0 | 90 | 180 | 270; flipU?: boolean }): void {
  const targets = faceTargets(store.state.doc, selectedList(store))
  if (!targets.length) return
  store.edit('Face layout', (d) => mapFaces(d, targets, (_b, f) => {
    const cur = parseFaceLayout(f.layout)
    if (!cur) return f
    const next: FaceLayout = { ...cur, params: { ...cur.params } }
    for (const [k, v] of Object.entries(patch.params ?? {})) { if (v === null) delete next.params[k]; else next.params[k] = v }
    if (patch.seed !== undefined) next.seed = patch.seed
    if (patch.rotate !== undefined) next.rotate = patch.rotate
    if (patch.flipU !== undefined) next.flipU = patch.flipU
    return { ...f, layout: faceLayoutToJson(next) }
  }))
}

/** A new random seed for every target face that has a layout (each gets its own, so neighbouring walls differ). */
export function rerollLayoutSeed(store: Store): void {
  const targets = faceTargets(store.state.doc, selectedList(store))
  if (!targets.length) return
  store.edit('Re-roll layout', (d) => mapFaces(d, targets, (_b, f) => {
    const cur = parseFaceLayout(f.layout)
    return cur ? { ...f, layout: faceLayoutToJson({ ...cur, seed: Math.floor(Math.random() * 4294967296) }) } : f
  }))
}

/** Back to a plain face (the face keeps its texture). */
export function clearFaceLayout(store: Store): void {
  const targets = faceTargets(store.state.doc, selectedList(store))
  if (!targets.length) return
  store.edit('Clear layout', (d) => mapFaces(d, targets, (_b, f) => { const { layout: _l, ...rest } = f; return rest }))
}

// ---- Fast Texture Tool (REQUIREMENTS §6 B5; model/ftt.ts) ------------------------------------------------------------------
/** What the tool edits: the selected faces of ONE brush (the first selected one; with a whole brush selected, all its faces). */
export interface FttTarget { entityId: string; brushId: string; faceIds: number[] }

export function fttTarget(store: Store): FttTarget | null {
  const targets = faceTargets(store.state.doc, selectedList(store))
  if (!targets.length) return null
  const first = targets[0]
  return { entityId: first.entityId, brushId: first.brushId, faceIds: targets.filter((t) => t.entityId === first.entityId && t.brushId === first.brushId).map((t) => t.faceId) }
}

/** The target's brush in a document (null when it is gone). */
export const fttBrush = (doc: MapDocument, t: FttTarget): Brush | null => findEntity(doc, t.entityId)?.brushes?.find((b) => b.id === t.brushId) ?? null

/** One undoable step: `xform` applied to the island. Returns the number of faces that changed. */
export function fttApply(store: Store, t: FttTarget, label: string, xform: Xform): number {
  let n = 0
  store.edit(label, (d) => mapBrush(d, t.entityId, t.brushId, (b) => { const r = transformFaces(b, t.faceIds, xform); n = r.changed; return r.brush }))
  return n
}

/** Continuous drag: begin, then `fttDrag` for every pointer move (relative to the document at the start), then `store.endDrag()` (one history entry). */
export function fttDrag(store: Store, t: FttTarget, xform: Xform): void {
  store.dragUpdate((start) => mapBrush(start, t.entityId, t.brushId, (b) => transformFaces(b, t.faceIds, xform).brush))
}

/** Unwrap buttons: Follow Quads (flow from the first face), Ortho, Axial. */
export function fttUnwrap(store: Store, t: FttTarget, mode: UnwrapMode): number {
  let n = 0
  const label = mode === 'follow' ? 'Follow quads' : mode === 'ortho' ? 'Project ortho' : 'Project axial'
  store.edit(label, (d) => mapBrush(d, t.entityId, t.brushId, (b) => { const r = unwrapFaces(b, t.faceIds, mode); n = r.changed; return r.brush }))
  return n
}

/** The layout of the first selected face that has one (what the Layout section shows), with how many of the targets have one. */
export function selectedLayout(store: Store): { layout: FaceLayout; count: number; total: number } | null {
  const doc = store.state.doc
  const targets = faceTargets(doc, selectedList(store))
  let first: FaceLayout | null = null, count = 0
  for (const t of targets) {
    const f = findEntity(doc, t.entityId)?.brushes?.find((b) => b.id === t.brushId)?.faces.find((x) => x.id === t.faceId)
    const l = f ? parseFaceLayout(f.layout) : null
    if (l) { count++; first ??= l }
  }
  return first ? { layout: first, count, total: targets.length } : null
}

/** Surface Inspector operations on every target face (model/surface.ts). */
export const surfaceEdit = (store: Store, label: string, change: (p: Projection, b: Brush, f: Brush['faces'][number]) => Projection): void => editProjection(store, label, change)

export const fitTexture = (store: Store): void => editProjection(store, 'Fit texture', (p, b, f) => fitProjection(b, f, p))

// ---- prefab instances (N1 c/d) --------------------------------------------------------
/** Adds an instance of `file` at `origin` with a generated unique name, and selects it. Returns its id (or null when refused). */
export function spawnInstance(store: Store, file: string, origin: Vec3): string | null {
  const id = newId('pi')
  const ok = store.edit('Add prefab', (d) => addInstance(d, { id, name: newInstanceName(d.prefabInstances, file), file, origin, angles: [0, 0, 0] }))
  if (ok) store.select({ entityId: '', instanceId: id })
  return ok ? id : null
}

/** Renames an instance; refuses (with a status message) a name that is empty, illegal or already used by a sibling. */
export function renameInstance(store: Store, id: string, name: string): boolean {
  const n = name.trim()
  const problem = instanceNameProblem(store.state.doc.prefabInstances, id, n)
  if (problem) { store.setMessage(`Rename: ${problem}`); return false }
  return store.edit('Rename prefab instance', (d) => mapInstance(d, id, (p) => (p.name === n ? p : { ...p, name: n })))
}

export type InstanceVec = 'origin' | 'angles' | 'scale'
export function setInstanceVec(store: Store, id: string, field: InstanceVec, axis: 0 | 1 | 2, value: number): boolean {
  if (!Number.isFinite(value)) return false
  if (field === 'scale' && value === 0) { store.setMessage('Scale cannot be 0'); return false }
  return store.edit(`Set instance ${field}`, (d) => mapInstance(d, id, (p: PrefabInstance) => {
    const cur: Vec3 = field === 'scale' ? p.scale ?? [1, 1, 1] : p[field] ?? [0, 0, 0]
    if (cur[axis] === value) return p
    const next = [...cur] as Vec3
    next[axis] = value
    return { ...p, [field]: next }
  }))
}

// ---- per-instance overrides (N1 c) -------------------------------------------------------
const emptyOverride = (o: EntityOverride): boolean =>
  !o.deleted && !o.origin && !o.angles && !o.scale && !Object.keys(o.keyvalues ?? {}).length && !Object.keys(o.refs ?? {}).length

/** Pure: edits (or creates/removes) the override of `path` on instance `instId`; empty overrides disappear. */
export function editOverride(doc: MapDocument, instId: string, path: string, fn: (o: EntityOverride) => EntityOverride): MapDocument {
  return mapInstance(doc, instId, (p) => {
    const cur = p.overrides?.[path] ?? {}
    const next = fn(cur)
    if (next === cur) return p
    const overrides = { ...(p.overrides ?? {}) }
    if (emptyOverride(next)) delete overrides[path]
    else overrides[path] = next
    const { overrides: _old, ...rest } = p
    void _old
    return Object.keys(overrides).length ? { ...rest, overrides } : (rest as PrefabInstance)
  })
}

/** Sets one keyvalue override; `null` reverts that key to the prefab's value. */
export function setOverrideKeyvalue(store: Store, instId: string, path: string, key: string, value: string | null): boolean {
  return store.edit(value === null ? 'Revert override' : 'Override keyvalue', (d) => editOverride(d, instId, path, (o) => {
    const kv = { ...(o.keyvalues ?? {}) }
    if (value === null) { if (!(key in kv)) return o; delete kv[key] }
    else { if (kv[key] === value) return o; kv[key] = value }
    const { keyvalues: _k, ...rest } = o
    void _k
    return Object.keys(kv).length ? { ...rest, keyvalues: kv } : rest
  }))
}

export type OverrideField = 'origin' | 'angles' | 'scale' | 'deleted'
/** Sets (or, with `null`, reverts) a transform / deleted override of one entity of the instance. */
export function setOverrideField(store: Store, instId: string, path: string, field: OverrideField, value: Vec3 | boolean | null): boolean {
  return store.edit(value === null || value === false ? 'Revert override' : 'Override entity', (d) => editOverride(d, instId, path, (o) => {
    const { [field]: _f, ...rest } = o
    void _f
    if (value === null || value === false) return field in o ? rest : o
    return { ...rest, [field]: value } as EntityOverride
  }))
}

export function revertOverride(store: Store, instId: string, path: string): boolean {
  return store.edit('Revert overrides', (d) => mapInstance(d, instId, (p) => {
    if (!p.overrides || !(path in p.overrides)) return p
    const overrides = { ...p.overrides }
    delete overrides[path]
    const { overrides: _o, ...rest } = p
    void _o
    return Object.keys(overrides).length ? { ...rest, overrides } : (rest as PrefabInstance)
  }))
}

// ---- references (N1 e) -------------------------------------------------------------------
/** Points reference `key` of a document entity at `path` (a root-relative target path); null removes it. */
export function setEntityRef(store: Store, entityId: string, key: string, path: string | null): boolean {
  return store.edit(path === null ? `Clear ${key}` : `Set ${key}`, (d) => mapEntity(d, entityId, (e) => {
    const refs = { ...(e.refs ?? {}) }
    if (path === null) { if (!(key in refs)) return e; delete refs[key] } else { if (refs[key] === path) return e; refs[key] = path }
    const { refs: _r, ...rest } = e
    void _r
    return Object.keys(refs).length ? { ...rest, refs } : rest
  }))
}

/** Overrides a reference of one prefab entity for THIS instance. The path is relative to the instance's parent scope
 *  (for a top-level instance: the document root), which is exactly what the picker lists. null reverts to the prefab's own. */
export function setOverrideRef(store: Store, instId: string, entityPath: string, key: string, path: string | null): boolean {
  return store.edit(path === null ? 'Revert reference' : `Override ${key}`, (d) => editOverride(d, instId, entityPath, (o) => {
    const refs = { ...(o.refs ?? {}) }
    if (path === null) { if (!(key in refs)) return o; delete refs[key] } else { if (refs[key] === path) return o; refs[key] = path }
    const { refs: _r, ...rest } = o
    void _r
    return Object.keys(refs).length ? { ...rest, refs } : rest
  }))
}

// ---- Radiant brush operations on the selected brushes (clip, hollow, merge, flip, rotate, snap) ----------------------
interface SelBrush { entityId: string; brush: Brush }
/** The brushes a selection covers: picked brushes, or every brush of a selected entity. Prefab instances are skipped. */
export function selectedBrushes(store: Store): SelBrush[] {
  const doc = store.state.doc, out: SelBrush[] = [], seen = new Set<string>()
  for (const s of selectedList(store)) {
    if (s.instanceId !== undefined) continue
    const e = findEntity(doc, s.entityId)
    for (const b of e?.brushes ?? []) if ((!s.brushId || s.brushId === b.id) && !seen.has(b.id)) { seen.add(b.id); out.push({ entityId: e!.id, brush: b }) }
  }
  return out
}
const replaceBrush = (doc: MapDocument, entityId: string, brushId: string, list: Brush[]): MapDocument =>
  mapEntity(doc, entityId, (e) => ({ ...e, brushes: (e.brushes ?? []).flatMap((b) => (b.id === brushId ? list : [b])) }))

/** Clipper: keep the back side of `pl` ('back'), the front ('front'), or both as two brushes ('both'). */
export function clipSelection(store: Store, pl: Plane, keep: 'back' | 'front' | 'both'): boolean {
  const targets = selectedBrushes(store)
  if (!targets.length) { store.setMessage('Clipper: select a brush first'); return false }
  const skipped = targets.filter((t) => !isConvex(t.brush)).length
  let cut = 0
  const ok = store.edit(keep === 'both' ? 'Split brushes' : 'Clip brushes', (d) => {
    let doc = d
    for (const { entityId, brush } of targets) {
      if (!isConvex(brush)) continue
      const r = clipBy(brush, pl, keep)
      const parts = [r.back, r.front].filter((x): x is Brush => !!x)
      if (!parts.length || (parts.length === 1 && parts[0] === brush)) continue // the plane misses this brush
      cut++
      doc = replaceBrush(doc, entityId, brush.id, parts.map((p, i) => ({ ...p, id: i === 0 ? brush.id : newId('b') })))
    }
    return cut ? doc : d
  })
  if (!cut) store.setMessage(skipped ? 'Clipper: concave brushes cannot be clipped' : 'Clipper: the plane does not cut the selected brushes')
  else if (skipped) store.setMessage(`Clipped ${cut}; ${skipped} concave brush(es) skipped`)
  return ok && cut > 0
}

/** Make Hollow: each selected convex brush becomes wall brushes `t` thick. */
export function hollowSelection(store: Store, t: number): boolean {
  const targets = selectedBrushes(store)
  if (!targets.length) { store.setMessage('Hollow: select a brush first'); return false }
  let done = 0
  const ok = store.edit('Make hollow', (d) => {
    let doc = d
    for (const { entityId, brush } of targets) {
      const walls = isConvex(brush) ? hollowBrush(brush, t) : null
      if (!walls?.length) continue
      done++
      doc = replaceBrush(doc, entityId, brush.id, walls.map((w, i) => ({ ...w, id: i === 0 ? brush.id : newId('b') })))
    }
    return done ? doc : d
  })
  if (!done) store.setMessage(`Hollow: no brush can take a wall of ${t} (too thick, or not convex)`)
  return ok && done > 0
}

/** Merge: the selected brushes become one convex brush - only when that adds no space the brushes did not fill. */
export function mergeSelection(store: Store): boolean {
  const targets = selectedBrushes(store)
  if (targets.length < 2) { store.setMessage('Merge: select two or more brushes'); return false }
  const hull = hullBrush(targets.flatMap((t) => t.brush.vertices), targets[0].brush.faces.find((f) => f.texture)?.texture)
  const filled = unionVolume(targets.map((t) => t.brush)) // overlapping brushes count once
  if (!hull || signedVolume(hull) > filled * (1 + 1e-6)) { store.setMessage('Merge: the brushes do not form a convex shape together'); return false }
  const [first, ...rest] = targets
  const ok = store.edit('Merge brushes', (d) => {
    let doc = replaceBrush(d, first.entityId, first.brush.id, [{ ...hull, id: first.brush.id, op: first.brush.op ?? 'add' }])
    for (const t of rest) doc = removeBrush(doc, t.entityId, t.brush.id)
    return doc
  })
  if (ok) store.select({ entityId: first.entityId, brushId: first.brush.id })
  return ok
}

const AXES: Vec3[] = [[1, 0, 0], [0, 1, 0], [0, 0, 1]]
/** Mirrors the selected brushes about the centre of the selection along one axis. */
export function flipSelection(store: Store, axis: 0 | 1 | 2): boolean {
  const sels = selectedList(store), c = gizmoPivot(store.state.doc, sels)
  if (!c || !selectedBrushes(store).length) { store.setMessage('Flip: select a brush first'); return false }
  const map = (p: Vec3): Vec3 => { const q: Vec3 = [...p]; q[axis] = 2 * c[axis] - p[axis]; return q }
  return store.edit(`Flip ${'XYZ'[axis]}`, (d) => transformSelections(d, sels, map, true, 0, store.state.uvLock))
}

/** Rotates the selection by 90 degrees (or `-90`) about an axis through its centre. */
export function rotateSelection90(store: Store, axis: 0 | 1 | 2, dir: 1 | -1 = 1): boolean {
  const sels = selectedList(store), c = gizmoPivot(store.state.doc, sels)
  if (!c || !sels.length) { store.setMessage('Rotate: select something first'); return false }
  const map = rotateAbout(c, AXES[axis], dir * Math.PI / 2)
  return store.edit(`Rotate ${dir * 90}° ${'XYZ'[axis]}`, (d) => transformSelections(d, sels, map, false, axis === 1 ? dir * 90 : 0, store.state.uvLock))
}

/** Snaps the selected brushes' vertices to the grid; brushes that would break are left alone (and reported). */
export function snapSelection(store: Store, step: number): boolean {
  const targets = selectedBrushes(store)
  if (!targets.length) { store.setMessage('Snap: select a brush first'); return false }
  let snapped = 0, failed = 0
  const ok = store.edit('Snap to grid', (d) => {
    let doc = d
    for (const { entityId, brush } of targets) {
      const r = snapBrush(brush, step)
      if (!r) { failed++; continue }
      if (r.vertices.every((v, i) => v.every((x, k) => x === brush.vertices[i][k]))) continue
      snapped++
      doc = replaceBrush(doc, entityId, brush.id, [r])
    }
    return snapped ? doc : d
  })
  store.setMessage(failed ? `Snapped ${snapped}; ${failed} brush(es) would break and were left alone` : snapped ? `Snapped ${snapped} brush(es)` : 'Already on the grid')
  return ok && snapped > 0
}

/** Radiant arrow-key nudge: moves everything selected by `delta` as ONE undo step. */
export function nudgeSelection(store: Store, delta: Vec3): boolean {
  const sels = selectedList(store)
  if (!sels.length) return false
  const map = (p: Vec3): Vec3 => [p[0] + delta[0], p[1] + delta[1], p[2] + delta[2]]
  return store.edit('Nudge', (d) => transformSelections(d, sels, map, false, 0, store.state.uvLock))
}

// ---- entity angles / scale (DESIGN 18.3) ------------------------------------------------
export const parseScaleVec = (kv?: string): Vec3 => {
  const n = (kv ?? '').trim().split(/\s+/).map(Number)
  return n.length === 3 && n.every((x) => Number.isFinite(x) && x !== 0) ? [n[0], n[1], n[2]] : [1, 1, 1]
}

export function setEntityAngle(store: Store, entityId: string, axis: 0 | 1 | 2, value: number): boolean {
  if (!Number.isFinite(value)) return false
  return store.edit('Set angles', (d) => mapEntity(d, entityId, (e) => {
    const cur: Vec3 = e.angles ?? [0, 0, 0]
    if (cur[axis] === value) return e
    const next: Vec3 = [...cur]
    next[axis] = value
    return { ...e, angles: next }
  }))
}

/** Writes `modelscale_vec` ("x y z"); the key disappears again at 1 1 1. */
export function setEntityScale(store: Store, entityId: string, axis: 0 | 1 | 2, value: number): boolean {
  if (!Number.isFinite(value) || value === 0) { store.setMessage('Scale cannot be 0'); return false }
  return store.edit('Set scale', (d) => mapEntity(d, entityId, (e) => {
    const cur = parseScaleVec(e.keyvalues?.modelscale_vec)
    if (cur[axis] === value) return e
    const next: Vec3 = [...cur]
    next[axis] = value
    const kv = { ...(e.keyvalues ?? {}) }
    if (next.every((x) => x === 1)) delete kv.modelscale_vec
    else kv.modelscale_vec = next.join(' ')
    return { ...e, keyvalues: kv }
  }))
}

// ---- brush <-> entity (right-click menu) -----------------------------------------------------------------------
/** Moves the selected brushes into ONE new entity of `classname` (func_group, func_door...). Refused for convex-only classes
 *  when a solid brush is concave; the selection follows the brushes. */
export function makeEntityFromSelection(store: Store, classname: string): boolean {
  const targets = selectedBrushes(store)
  if (!targets.length) { store.setMessage('Make entity: select brush(es) first'); return false }
  if (requiresConvexBrushes(classname)) {
    const bad = targets.find((t) => t.brush.op !== 'subtract' && !isConvex(t.brush))
    if (bad) { store.setMessage(`Make entity: ${classname} needs convex brushes - use func_detail for the concave one`); return false }
  }
  const id = newId('e')
  const ok = store.edit(`Make ${classname}`, (d) => {
    let doc = d
    for (const t of targets) doc = removeBrush(doc, t.entityId, t.brush.id)
    const brushes = targets.map((t) => t.brush)
    return addEntity(doc, { id, classname, brushes, origin: brushesCentre(brushes) ?? [0, 0, 0] }) // pivot = centre of the selection's bounding box, not the world origin
  })
  if (ok) store.selectMany(targets.map((t) => ({ entityId: id, brushId: t.brush.id })))
  return ok
}

/** Moves the selected brushes back into worldspawn. */
export function moveSelectionToWorld(store: Store): boolean {
  const world = store.state.doc.entities.find((e) => e.classname === 'worldspawn')
  const targets = selectedBrushes(store).filter((t) => t.entityId !== world?.id)
  if (!world) { store.setMessage('There is no worldspawn entity'); return false }
  if (!targets.length) { store.setMessage('Move to world: the selected brushes are already in the world'); return false }
  const bad = targets.find((t) => t.brush.op !== 'subtract' && !isConvex(t.brush))
  if (bad) { store.setMessage('Move to world: a concave brush cannot be solid world geometry - keep it in func_detail'); return false }
  const ok = store.edit('Move to world', (d) => {
    let doc = d
    for (const t of targets) doc = removeBrush(doc, t.entityId, t.brush.id)
    return mapEntity(doc, world.id, (e) => ({ ...e, brushes: [...(e.brushes ?? []), ...targets.map((t) => t.brush)] }))
  })
  if (ok) store.selectMany(targets.map((t) => ({ entityId: world.id, brushId: t.brush.id })))
  return ok
}

// ---- texture keys (Radiant: Shift+arrows shift, Ctrl+Shift+arrows scale, Shift+PageUp/PageDown rotate) ---------------
export type TexKey = 'left' | 'right' | 'up' | 'down' | 'rotcw' | 'rotccw'
/**
 * One key press on the selected faces (a selected brush = all its faces). `shift`: left/right move U, up/down move V (the same
 * directions as the arrow buttons in the Texture panel). `scale`: right/up = larger, left/down = smaller (U for left/right, V for
 * up/down). `rotate`: clockwise adds degrees. `fine` divides the step by 8. Returns false when no face is selected.
 */
export function nudgeTexture(store: Store, mode: 'shift' | 'scale' | 'rotate', key: TexKey, fine = false): boolean {
  if (!faceTargets(store.state.doc, selectedList(store)).length) return false
  const st = store.state.texSteps, f = fine ? 1 / 8 : 1
  const dir = key === 'left' || key === 'up' || key === 'rotcw' ? 1 : -1
  const horizontal = key === 'left' || key === 'right'
  if (mode === 'shift') {
    const d = st.shift * f * dir
    editProjection(store, 'Shift texture', (p) => ({ ...p, shift: horizontal ? [p.shift[0] + d, p.shift[1]] : [p.shift[0], p.shift[1] + d] }))
  } else if (mode === 'scale') {
    const grow = key === 'right' || key === 'up'
    const k = grow ? 1 + st.scale * f : 1 / (1 + st.scale * f)
    editProjection(store, 'Scale texture', (p) => ({ ...p, scale: horizontal ? [p.scale[0] * k, p.scale[1]] : [p.scale[0], p.scale[1] * k] }))
  } else {
    const d = st.rotate * f * (key === 'rotcw' ? 1 : -1)
    editProjection(store, 'Rotate texture', (p) => ({ ...p, rotate: p.rotate + d }))
  }
  return true
}

// ---- select inside / touching (Radiant) ---------------------------------------------------------------------------------
/** Selects the other brushes of the document that lie completely inside ('inside') or touch ('touching') the selected brushes. */
export function selectByVolume(store: Store, mode: 'inside' | 'touching'): boolean {
  const volumes = selectedBrushes(store).filter((v) => isConvex(v.brush))
  if (!volumes.length) { store.setMessage(`Select ${mode}: select a convex brush first`); return false }
  const taken = new Set(volumes.map((v) => v.brush.id))
  const found: Selection[] = []
  for (const e of store.state.doc.entities)
    for (const b of e.brushes ?? []) {
      if (taken.has(b.id) || !isConvex(b)) continue
      if (volumes.some((v) => (mode === 'inside' ? brushInside(v.brush, b) : brushesTouch(v.brush, b)))) found.push({ entityId: e.id, brushId: b.id })
    }
  if (!found.length) { store.setMessage(`Select ${mode}: no other brush is ${mode === 'inside' ? 'inside' : 'touching'}`); return false }
  store.selectMany(found)
  store.setMessage(`Selected ${found.length} brush${found.length === 1 ? '' : 'es'} ${mode}`)
  return true
}

// ---- select all / invert / none in object mode ---------------------------------------------------------------------------
/** Everything that can be selected in the document: each brush (of any entity), each point entity, each prefab instance. */
export function selectableItems(doc: MapDocument): Selection[] {
  const out: Selection[] = []
  for (const e of doc.entities) {
    if (e.brushes?.length) for (const b of e.brushes) out.push({ entityId: e.id, brushId: b.id })
    else if (e.classname !== 'worldspawn' && !isBrushEntity(e)) out.push({ entityId: e.id })
  }
  for (const p of doc.prefabInstances) out.push({ entityId: '', instanceId: p.id })
  return out
}
const selKey = (s: Selection): string => `${s.instanceId ?? ''}|${s.entityId}|${s.brushId ?? ''}`

export function selectAllItems(store: Store): boolean {
  const all = selectableItems(store.state.doc)
  if (!all.length) { store.setMessage('Nothing to select'); return false }
  store.selectMany(all)
  store.setMessage(`Selected ${all.length} item${all.length === 1 ? '' : 's'}`)
  return true
}

/** Selects everything that is NOT selected now (a selected whole entity counts as all of its brushes). */
export function invertSelectionItems(store: Store): boolean {
  const have = new Set<string>()
  for (const s of selectedList(store)) {
    if (s.brushId === undefined && s.instanceId === undefined) {
      const e = findEntity(store.state.doc, s.entityId)
      if (e?.brushes?.length) { for (const b of e.brushes) have.add(selKey({ entityId: e.id, brushId: b.id })); continue }
    }
    have.add(selKey(s))
  }
  const rest = selectableItems(store.state.doc).filter((s) => !have.has(selKey(s)))
  store.selectMany(rest)
  store.setMessage(rest.length ? `Selected ${rest.length} item${rest.length === 1 ? '' : 's'}` : 'Everything was selected')
  return rest.length > 0
}

// ---- parametric shapes (REQUIREMENTS §22 P1) ----------------------------------------------------
/** Rebuilds a placed shape with new parameters / size / orientation as one undo step. */
export function editShape(store: Store, entityId: string, group: string, patch: ShapePatch): boolean {
  let error: string | null = null
  const ok = store.edit('Edit shape', (d) => {
    const r = regenerateShape(d, entityId, group, patch, () => newId('b'))
    if ('error' in r) { error = r.error; return d }
    return r.doc
  })
  if (error) store.setMessage(error)
  return ok
}

/** Turns a placed shape into ordinary brushes (drops the shape record from every part). */
export function convertShape(store: Store, entityId: string, group: string): boolean {
  return store.edit('Convert shape to brushes', (d) => mapEntity(d, entityId, (e) => ({
    ...e,
    brushes: (e.brushes ?? []).map((b) => { if (shapeOf(b)?.group !== group) return b; const { shape: _s, ...rest } = b; void _s; return rest as Brush }),
  })))
}

// ---- level settings (REQUIREMENTS §22 P2) -------------------------------------------------------
/** Sets (or with null removes) a level setting: a worldspawn key. Creates the worldspawn when the map has none. */
export function setLevelKey(store: Store, key: string, value: string | null): boolean {
  return store.edit(value === null ? `Level: reset ${key}` : `Level: ${key}`, (d) => {
    const world = value === null ? d : ensureWorldspawn(d)
    const w = world.entities.find((e) => classOf(e) === 'worldspawn')
    if (!w) return d
    const kv = { ...(w.keyvalues ?? {}) }
    if (value === null) { if (!(key in kv)) return d; delete kv[key] } else { if (kv[key] === value) return d; kv[key] = value }
    return mapEntity(world, w.id, (e) => ({ ...e, keyvalues: kv }))
  })
}

/** Several worldspawn keys as ONE undo step (null removes a key), e.g. Auto sky on together with its skybox. False when nothing changed. */
export function setLevelKeys(store: Store, label: string, changes: Record<string, string | null>): boolean {
  return store.edit(label, (d) => {
    const world = Object.values(changes).some((v) => v !== null) ? ensureWorldspawn(d) : d
    const w = world.entities.find((e) => classOf(e) === 'worldspawn')
    if (!w) return d
    const kv = { ...(w.keyvalues ?? {}) }
    let changed = false
    for (const [k, v] of Object.entries(changes)) {
      if (v === null) { if (k in kv) { delete kv[k]; changed = true } } else if (kv[k] !== v) { kv[k] = v; changed = true }
    }
    return changed ? mapEntity(world, w.id, (e) => ({ ...e, keyvalues: kv })) : d
  })
}

// ---- right-click: hide, group, copy / paste UV (REQUIREMENTS §24 C4) -----------------------------
/** Hides what is selected (brushes, whole entities) and clears the selection. Returns how many things were hidden. */
export function hideSelection(store: Store): number {
  const keys = hideKeys(selectedList(store))
  if (!keys.length) return 0
  store.setHidden([...new Set([...store.state.hidden, ...keys])])
  store.select(null)
  store.setMessage(`Hidden: ${keys.length}. Right-click > Unhide all shows everything again.`)
  return keys.length
}
export function unhideAll(store: Store): void { store.setHidden([]) }

/** Group = the selected brushes into a new func_group; Ungroup = the brushes of selected func_groups back into the world. */
export const groupSelection = (store: Store): boolean => makeEntityFromSelection(store, 'func_group')
export function ungroupSelection(store: Store): boolean {
  const doc = store.state.doc
  const groups = new Set(selectedList(store).map((s) => s.entityId).filter((id) => classOf(findEntity(doc, id) ?? { id: '' }) === 'func_group'))
  if (!groups.size) { store.setMessage('Ungroup: select brushes of a func_group'); return false }
  store.selectMany([...groups].flatMap((id) => (findEntity(doc, id)?.brushes ?? []).map((b) => ({ entityId: id, brushId: b.id }))))
  return moveSelectionToWorld(store)
}

/** Copy UV: remembers the one selected face. */
export function copyUv(store: Store): boolean {
  const sel = selectedList(store)
  const f = sel.length === 1 && sel[0].brushId && sel[0].faceId !== undefined ? { entityId: sel[0].entityId, brushId: sel[0].brushId, faceId: sel[0].faceId } : null
  if (!f) { store.setMessage('Copy UV: select one face (Shift + click)'); return false }
  store.setUvClipboard(f)
  store.setMessage('UV copied: select faces or brushes, then right-click > Paste UV')
  return true
}

/** Paste UV: the copied face's texture and mapping onto every selected face (whole brushes: all their faces), as ONE undo step. */
export function pasteUv(store: Store): number {
  const from = store.state.uvClipboard
  const src = from ? findEntity(store.state.doc, from.entityId)?.brushes?.find((b) => b.id === from.brushId)?.faces.find((f) => f.id === from.faceId) : undefined
  if (!from || !src) { store.setMessage('Paste UV: copy a face first (the copied face no longer exists)'); return 0 }
  const targets = faceTargets(store.state.doc, selectedList(store)).filter((t) => !(t.entityId === from.entityId && t.brushId === from.brushId && t.faceId === from.faceId))
  if (!targets.length) return 0
  const p = projectionOf(src), texture = src.texture
  store.edit('Paste UV', (d) => mapFaces(d, targets, (b, f) => ({ ...f, texture, projection: p, uvs: projectFace(b, f, p) })))
  return targets.length
}
