import { h } from './dom'

export interface MenuItem { label: string; run?: () => void; disabled?: boolean; sub?: MenuItem[]; sep?: boolean }

let open: HTMLElement | null = null
export function closeMenu(): void { open?.remove(); open = null }

/** A small popup menu at the pointer; closes on any outside click, Escape, or after an item ran. */
/** `doc` = the document (window) the pointer is in: a viewport in a popout window must open its menu there, not in the main window. */
export function showMenu(x: number, y: number, items: MenuItem[], doc: Document = document): void {
  closeMenu()
  const build = (list: MenuItem[], cls = 'ctx-menu'): HTMLElement => {
    const el = h('div', { class: cls })
    for (const it of list) {
      if (it.sep) { el.append(h('div', { class: 'ctx-sep' })); continue }
      const row = h('div', { class: `ctx-item${it.disabled ? ' disabled' : ''}` }, it.label, it.sub ? h('span', { class: 'ctx-arrow' }, '▸') : '')
      if (it.sub && !it.disabled) {
        const sub = build(it.sub, 'ctx-menu ctx-sub')
        row.append(sub)
        row.addEventListener('pointerenter', () => { sub.style.display = 'block' })
        row.addEventListener('pointerleave', () => { sub.style.display = 'none' })
      } else if (!it.disabled) row.addEventListener('click', (e) => { e.stopPropagation(); closeMenu(); it.run?.() })
      el.append(row)
    }
    return el
  }
  const menu = build(items)
  menu.style.left = `${x}px`; menu.style.top = `${y}px`
  const win = doc.defaultView ?? window
  doc.body.append(menu)
  const r = menu.getBoundingClientRect()
  if (r.right > win.innerWidth) menu.style.left = `${Math.max(0, win.innerWidth - r.width - 4)}px`
  if (r.bottom > win.innerHeight) menu.style.top = `${Math.max(0, win.innerHeight - r.height - 4)}px`
  open = menu
  const away = (e: Event) => { if (open && !open.contains(e.target as Node)) { closeMenu(); cleanup() } }
  const esc = (e: KeyboardEvent) => { if (e.key === 'Escape') { e.stopPropagation(); closeMenu(); cleanup() } }
  const cleanup = () => { win.removeEventListener('pointerdown', away, true); win.removeEventListener('keydown', esc, true) }
  win.setTimeout(() => { win.addEventListener('pointerdown', away, true); win.addEventListener('keydown', esc, true) })
}
