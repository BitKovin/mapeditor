// The editing state of the relief editor, without any DOM or GL: the mesh (in metres, see model/relief.ts), the element selection and an undo history. Every
// modeling tool is one of meshOps.ts' functions; this class runs them, keeps the mesh inside its tile and records history. A modal tool (G / R / S / E / I /
// bevel / loop cut) re-runs its function from the mesh as it was when the modal began, so a whole drag is one undo step.
import type { Brush } from './document'
import { MeshSel, SelMode, allSel, cleanSel, convertSel, emptySel, finish, growSel, invertSel, selFromEdges, selFromFaces, selFromVerts, shrinkSel } from './meshTopology'
import { OpResult, deleteFaces, dissolveEdges, dissolveFaces, dissolveVerts, fillHole, flipFaces, isError, mergeVerts, transformVerts } from './meshOps'
import { Relief, brushToRelief, confineBrush, ensureBrush, flatRelief, reliefToBrush } from './relief'
import { centroid } from './meshTopology'
import type { Vec3 } from './math'

export type ReliefOp = (b: Brush, sel: MeshSel) => OpResult
interface Snapshot { brush: Brush; sel: MeshSel }
export const HISTORY_LIMIT = 200

export class ReliefSession {
  brush: Brush
  sel: MeshSel = emptySel()
  mode: SelMode = 'face'
  private past: Snapshot[] = []
  private future: Snapshot[] = []
  private modalBase: Snapshot | null = null
  /** Last error of a tool (shown in the status line); cleared by the next success. */
  error: string | null = null

  constructor(relief: Relief | null, public width: number, public height: number) {
    this.brush = reliefToBrush(relief ?? flatRelief(), width, height)
  }

  get canUndo(): boolean { return this.past.length > 0 }
  get canRedo(): boolean { return this.future.length > 0 }
  get inModal(): boolean { return this.modalBase !== null }
  /** The stored form of the current mesh. */
  toRelief(): Relief { return brushToRelief(this.brush, this.width, this.height) }

  /** The texture's size in the world changed (pixels per meter): the same relief in the new size, selection and history reset. */
  resize(width: number, height: number): void {
    if (width === this.width && height === this.height) return
    const r = this.toRelief()
    this.width = width; this.height = height
    this.brush = reliefToBrush(r, width, height)
    this.sel = emptySel(); this.past = []; this.future = []
  }

  private push(): void {
    this.past.push({ brush: this.brush, sel: this.sel })
    if (this.past.length > HISTORY_LIMIT) this.past.shift()
    this.future = []
  }

  private settle(r: { brush: Brush; sel: MeshSel }): { brush: Brush; sel: MeshSel } {
    const c = confineBrush(r.brush, this.width, this.height)
    const f = finish(ensureBrush(c, this.width, this.height), c === r.brush ? r.sel : cleanSel(c, r.sel))
    return { brush: f.brush, sel: cleanSel(f.brush, f.sel) }
  }

  /** Runs a tool on the current mesh as one undo step. Returns an error text or null. */
  apply(op: ReliefOp): string | null {
    if (this.modalBase) return 'A tool is active'
    const r = op(this.brush, this.sel)
    if (isError(r)) { this.error = r.error; return r.error }
    this.error = null
    this.push()
    const s = this.settle(r)
    this.brush = s.brush; this.sel = s.sel
    return null
  }

  // ---- modal tools
  beginModal(): void { if (!this.modalBase) this.modalBase = { brush: this.brush, sel: this.sel } }
  /** Shows the tool's result for the current parameter; the base mesh is untouched until `endModal(true)`. Errors leave the last good result on screen. */
  modalUpdate(op: ReliefOp): string | null {
    const base = this.modalBase
    if (!base) return 'No tool is active'
    const r = op(base.brush, base.sel)
    if (isError(r)) { this.error = r.error; return r.error }
    this.error = null
    const s = this.settle(r)
    this.brush = s.brush; this.sel = s.sel
    return null
  }
  endModal(confirm: boolean): void {
    const base = this.modalBase
    if (!base) return
    this.modalBase = null
    if (confirm && (this.brush !== base.brush)) { this.past.push(base); if (this.past.length > HISTORY_LIMIT) this.past.shift(); this.future = [] }
    else { this.brush = base.brush; this.sel = base.sel }
  }

  undo(): boolean {
    const s = this.past.pop()
    if (!s || this.modalBase) { if (s) this.past.push(s); return false }
    this.future.push({ brush: this.brush, sel: this.sel })
    this.brush = s.brush; this.sel = s.sel
    return true
  }
  redo(): boolean {
    const s = this.future.pop()
    if (!s || this.modalBase) { if (s) this.future.push(s); return false }
    this.past.push({ brush: this.brush, sel: this.sel })
    this.brush = s.brush; this.sel = s.sel
    return true
  }

  // ---- selection (not part of the history, as in the Mesh editor)
  setMode(mode: SelMode): void {
    if (mode === this.mode) return
    this.sel = convertSel(this.brush, this.sel, this.mode, mode)
    this.mode = mode
  }
  setSel(s: MeshSel): void { this.sel = cleanSel(this.brush, s) }
  selectAll(): void { this.sel = allSel(this.brush) }
  selectNone(): void { this.sel = emptySel() }
  invert(): void { this.sel = invertSel(this.brush, this.sel, this.mode) }
  grow(): void { this.sel = growSel(this.brush, this.sel, this.mode) }
  shrink(): void { this.sel = shrinkSel(this.brush, this.sel, this.mode) }
  /** Selection from picked elements of the current mode. */
  pick(ids: (number | string)[]): MeshSel {
    return this.mode === 'vertex' ? selFromVerts(this.brush, ids as number[]) : this.mode === 'edge' ? selFromEdges(this.brush, ids as string[]) : selFromFaces(this.brush, ids as number[])
  }
  get hasSelection(): boolean { return this.sel.verts.length > 0 }
  /** Centre of the selected vertices, or of the whole mesh when nothing is selected. */
  pivot(): Vec3 { return centroid(this.brush, this.sel.verts.length ? this.sel.verts : this.brush.vertices.map((_, i) => i)) }

  // ---- one-shot tools
  dissolve(): string | null {
    return this.apply((b, s) => (this.mode === 'vertex' ? dissolveVerts(b, s.verts) : this.mode === 'edge' ? dissolveEdges(b, s.edges) : dissolveFaces(b, s.faces)))
  }
  deleteSelectedFaces(): string | null { return this.apply((b, s) => deleteFaces(b, s.faces.length ? s.faces : faceIdsTouching(b, s))) }
  merge(): string | null { return this.apply((b, s) => mergeVerts(b, s.verts)) }
  fill(): string | null { return this.apply((b, s) => fillHole(b, s.verts)) }
  flip(): string | null { return this.apply((b, s) => flipFaces(b, s.faces.length ? s.faces : faceIdsTouching(b, s))) }
  /** Puts every selected vertex at depth z (metres). */
  setDepth(z: number): string | null { return this.apply((b, s) => transformVerts(b, s.verts, (p) => [p[0], p[1], z])) }
  /** Back to the flat quad. */
  reset(): void {
    this.push()
    this.brush = reliefToBrush(flatRelief(), this.width, this.height)
    this.sel = emptySel(); this.error = null
  }
  /** Replaces the mesh (loading a preset or another relief) as one undo step. */
  load(r: Relief): void {
    this.push()
    this.brush = reliefToBrush(r, this.width, this.height)
    this.sel = emptySel(); this.error = null
  }
}

/** Faces whose every corner is selected (so vertex / edge selections can drive face tools). */
export function faceIdsTouching(b: Brush, s: MeshSel): number[] {
  const v = new Set(s.verts)
  return b.faces.filter((f) => f.loop.every((i) => v.has(i))).map((f) => f.id)
}
