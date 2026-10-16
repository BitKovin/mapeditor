// Minimal modal dialogs. They live in document.body (above Dockview), close on Esc / backdrop click,
// and swallow key presses so the app's shortcuts (Del, Q, B, ...) never fire while one is open.
import { h } from './dom'

export interface Dialog { root: HTMLElement; body: HTMLElement; close(): void }

export function openDialog(title: string, opts: { width?: number; onClose?: () => void; doc?: Document } = {}): Dialog {
  const doc = opts.doc ?? document
  const body = h('div', { class: 'modal-body' })
  const box = h('div', { class: 'modal', role: 'dialog', 'aria-label': title }, h('div', { class: 'modal-title' }, title), body)
  if (opts.width) box.style.width = `${opts.width}px`
  const backdrop = h('div', { class: 'modal-backdrop' }, box)
  let closed = false
  const close = () => {
    if (closed) return
    closed = true
    doc.removeEventListener('keydown', onKey, true)
    backdrop.remove()
    opts.onClose?.()
  }
  const onKey = (e: KeyboardEvent) => {
    if (!backdrop.isConnected) return
    if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); close() }
  }
  doc.addEventListener('keydown', onKey, true)
  backdrop.addEventListener('mousedown', (e) => { if (e.target === backdrop) close() })
  doc.body.append(backdrop)
  return { root: backdrop, body, close }
}

/** Resolves true on OK, false on Cancel / Esc. */
export function confirmDialog(title: string, message: string, okLabel = 'OK'): Promise<boolean> {
  return new Promise((resolve) => {
    let answer = false
    const d = openDialog(title, { width: 420, onClose: () => resolve(answer) })
    const ok = h('button', { class: 'primary' }, okLabel), cancel = h('button', {}, 'Cancel')
    ok.addEventListener('click', () => { answer = true; d.close() })
    cancel.addEventListener('click', () => d.close())
    d.body.append(h('div', { class: 'modal-text' }, message), h('div', { class: 'modal-buttons' }, cancel, ok))
    ok.focus()
  })
}
