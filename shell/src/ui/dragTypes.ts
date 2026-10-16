/** dataTransfer types of things dragged from the Place tab into a viewport. */
export const PREFAB_DRAG = 'application/x-mapeditor-prefab'
export const ENTITY_DRAG = 'application/x-mapeditor-entity'

/**
 * What is being dragged. The browser hides dataTransfer contents until the drop, but the viewport needs to know
 * during the drag to show the half-transparent preview, so the source also publishes it here (every window of the
 * editor, popouts included, runs in this one JavaScript context).
 */
export type DragItem =
  | { kind: 'prefab'; file: string }
  | { kind: 'entity'; classname: string; keyvalues?: Record<string, string> }

let current: DragItem | null = null
const listeners = new Set<() => void>()

export const dragItem = (): DragItem | null => current
export function beginDrag(item: DragItem): void { current = item; listeners.forEach((f) => f()) }
export function endDrag(): void { if (current) { current = null; listeners.forEach((f) => f()) } }
export function onDragItemChange(fn: () => void): () => void { listeners.add(fn); return () => void listeners.delete(fn) }

/** The dragged item from a drop event's dataTransfer, for drops whose source did not publish it. */
export function itemFromTransfer(dt: DataTransfer | null): DragItem | null {
  if (!dt) return null
  const file = dt.getData(PREFAB_DRAG)
  if (file) return { kind: 'prefab', file }
  const classname = dt.getData(ENTITY_DRAG)
  if (classname) return { kind: 'entity', classname }
  return null
}
export const isDragTransfer = (dt: DataTransfer | null): boolean => !!dt && (dt.types.includes(PREFAB_DRAG) || dt.types.includes(ENTITY_DRAG))
