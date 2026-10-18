// Copy / Cut / Paste of the selection. The buffer lives in the OS clipboard as JSON text, so it survives
// between editor windows and can be inspected or pasted into a text editor. Everything that touches the
// document is a pure function (testable without a DOM); the async wrappers at the bottom do the clipboard I/O.
import {
  Brush, Entity, MapDocument, PrefabInstance, addEntity, addInstance, classOf, findEntity, findInstance, mapEntity, newId, newInstanceName,
} from '../model/document'
import { translateBrush } from '../model/geometry'
import { add } from '../model/math'
import type { Vec3 } from '../model/math'
import { ensureWorldspawn, deleteSelection, selectedList } from './actions'
import type { Selection, Store } from './store'

export const CLIPBOARD_FORMAT = 'level-editor/clipboard'
export const CLIPBOARD_VERSION = 1

/** One copied thing. A brush remembers the entity it came from (so it pastes back into the same kind of entity). */
export type ClipItem =
  | { kind: 'brush'; entity: { id: string; classname?: string }; brush: Brush }
  | { kind: 'entity'; entity: Entity }
  | { kind: 'instance'; instance: PrefabInstance }

export interface ClipboardPayload { format: typeof CLIPBOARD_FORMAT; version: number; items: ClipItem[] }

const clone = <T>(v: T): T => JSON.parse(JSON.stringify(v)) as T

/** The selection as clipboard data; null when nothing copyable is selected. Deep copies, so later edits never leak in. */
export function buildClipboard(doc: MapDocument, sels: readonly Selection[]): ClipboardPayload | null {
  const items: ClipItem[] = []
  const whole = new Set(sels.filter((s) => s.instanceId === undefined && !s.brushId).map((s) => s.entityId))
  const seen = new Set<string>()
  for (const s of sels) {
    if (s.instanceId !== undefined) {
      const p = findInstance(doc, s.instanceId)
      if (p && !seen.has('i:' + p.id)) { seen.add('i:' + p.id); items.push({ kind: 'instance', instance: clone(p) }) }
      continue
    }
    const e = findEntity(doc, s.entityId)
    if (!e) continue
    if (!s.brushId) {
      if (seen.has('e:' + e.id)) continue
      seen.add('e:' + e.id)
      items.push({ kind: 'entity', entity: clone(e) })
    } else if (!whole.has(e.id)) {
      const b = e.brushes?.find((x) => x.id === s.brushId)
      if (!b || seen.has('b:' + b.id)) continue
      seen.add('b:' + b.id)
      items.push({ kind: 'brush', entity: { id: e.id, classname: e.classname }, brush: clone(b) })
    }
  }
  return items.length ? { format: CLIPBOARD_FORMAT, version: CLIPBOARD_VERSION, items } : null
}

export const serializeClipboard = (p: ClipboardPayload): string => JSON.stringify(p)

/** Parses clipboard text; null when it is not (valid) editor clipboard JSON, e.g. ordinary text copied elsewhere. */
export function parseClipboard(text: string): ClipboardPayload | null {
  let v: unknown
  try { v = JSON.parse(text) } catch { return null }
  if (!v || typeof v !== 'object') return null
  const p = v as Partial<ClipboardPayload>
  if (p.format !== CLIPBOARD_FORMAT || typeof p.version !== 'number' || p.version > CLIPBOARD_VERSION || !Array.isArray(p.items)) return null
  const items = p.items.filter((it): it is ClipItem => {
    if (!it || typeof it !== 'object') return false
    const i = it as Record<string, any>
    if (i.kind === 'brush') return !!i.brush && Array.isArray(i.brush.vertices) && Array.isArray(i.brush.faces) && !!i.entity && typeof i.entity.id === 'string'
    if (i.kind === 'entity') return !!i.entity && typeof i.entity.id === 'string'
    if (i.kind === 'instance') return !!i.instance && typeof i.instance.file === 'string'
    return false
  })
  return items.length ? { format: CLIPBOARD_FORMAT, version: p.version, items } : null
}

const shiftEntity = (e: Entity, d: Vec3): Entity => ({
  ...e,
  ...(e.origin ? { origin: add(e.origin, d) } : {}),
  ...(e.brushes ? { brushes: e.brushes.map((b) => translateBrush(b, d)) } : {}),
})

/** Adds the clipboard's contents to `doc` with fresh ids, moved by `offset`. Returns the new document and what to select. */
export function pasteInto(doc: MapDocument, payload: ClipboardPayload, offset: Vec3 = [0, 0, 0]): { doc: MapDocument; sels: Selection[] } {
  let out = doc
  const sels: Selection[] = []
  for (const it of payload.items) {
    if (it.kind === 'instance') {
      const id = newId('pi')
      const src = it.instance
      out = addInstance(out, { ...clone(src), id, name: newInstanceName(out.prefabInstances, src.file), ...(src.origin ? { origin: add(src.origin, offset) } : { origin: offset }) })
      sels.push({ entityId: '', instanceId: id })
    } else if (it.kind === 'entity') {
      const src = it.entity
      if (classOf(src) === 'worldspawn') { // there is only one worldspawn: its brushes join the existing one
        out = ensureWorldspawn(out)
        const w = out.entities.find((e) => classOf(e) === 'worldspawn')!
        const bs = (src.brushes ?? []).map((b) => ({ ...translateBrush(clone(b), offset), id: newId('b') }))
        out = mapEntity(out, w.id, (e) => ({ ...e, brushes: [...(e.brushes ?? []), ...bs] }))
        for (const b of bs) sels.push({ entityId: w.id, brushId: b.id })
        continue
      }
      const id = newId('e')
      const e = shiftEntity(clone(src), offset)
      out = addEntity(out, { ...e, id, ...(e.brushes ? { brushes: e.brushes.map((b) => ({ ...b, id: newId('b') })) } : {}) })
      sels.push({ entityId: id })
    } else {
      // back into the entity it came from when that still exists and is a real brush container, else into worldspawn
      let target = findEntity(out, it.entity.id)
      if (!target || (classOf(target) !== classOf({ id: '', classname: it.entity.classname }))) { out = ensureWorldspawn(out); target = out.entities.find((e) => classOf(e) === 'worldspawn')! }
      const nb: Brush = { ...translateBrush(clone(it.brush), offset), id: newId('b') }
      out = mapEntity(out, target.id, (e) => ({ ...e, brushes: [...(e.brushes ?? []), nb] }))
      sels.push({ entityId: target.id, brushId: nb.id })
    }
  }
  return { doc: out, sels }
}

// ---- store wrappers + OS clipboard I/O ------------------------------------------------------------

/** Used when the browser denies clipboard access (e.g. no permission, insecure context), so copy/paste still works inside the editor. */
let fallbackText: string | null = null

async function writeText(text: string): Promise<void> {
  fallbackText = text
  try { await navigator.clipboard.writeText(text) } catch { /* fallback buffer keeps in-editor paste working */ }
}
async function readText(): Promise<string | null> {
  try { return await navigator.clipboard.readText() } catch { return fallbackText }
}

/** Copies the selection to the OS clipboard as JSON. */
export async function copySelection(store: Store): Promise<boolean> {
  const p = buildClipboard(store.state.doc, selectedList(store))
  if (!p) { store.setMessage('Copy: nothing selected'); return false }
  await writeText(serializeClipboard(p))
  store.setMessage(`Copied ${p.items.length} item${p.items.length > 1 ? 's' : ''}`)
  return true
}

/** Copy, then delete the selection. */
export async function cutSelection(store: Store): Promise<boolean> {
  if (!store.canEdit) return false
  if (!(await copySelection(store))) return false
  deleteSelection(store)
  return true
}

/** Pastes what the OS clipboard holds (editor JSON) as one undo step and selects the pasted items. */
export async function pasteClipboard(store: Store, offset: Vec3 = [0, 0, 0]): Promise<boolean> {
  if (!store.canEdit) return false
  const text = await readText()
  const p = text === null ? null : parseClipboard(text)
  if (!p) { store.setMessage('Paste: the clipboard holds no copied editor items'); return false }
  let sels: Selection[] = []
  const ok = store.edit(p.items.length > 1 ? `Paste ${p.items.length} items` : 'Paste', (d) => { const r = pasteInto(d, p, offset); sels = r.sels; return r.doc })
  if (ok) store.selectMany(sels)
  return ok
}
