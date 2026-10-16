import { h } from './dom'

/** One entry of a drop-down menu; `'-'` is a separator. */
export type MenuItem = '-' | { label: string; shortcut?: string; title?: string; checked?: boolean; disabled?: boolean; action: () => void }
/** A top-level menu; `items` is called each time it opens, so labels and check marks are always current. */
export interface Menu { label: string; items: () => MenuItem[] }

/**
 * Desktop-style menu bar (File / Edit / ...): click a title to open its menu, move over the other titles to switch,
 * click an item, Esc or a click elsewhere closes it. Arrow keys move between items and menus.
 */
export function menuBar(menus: Menu[]): HTMLElement {
  const bar = h('div', { class: 'menubar', role: 'menubar' })
  let open = -1, popup: HTMLElement | null = null
  const titles = menus.map((m, i) => {
    const b = h('button', { class: 'menu-title', role: 'menuitem', 'aria-haspopup': 'true' }, m.label)
    b.addEventListener('mousedown', (e) => { e.preventDefault(); open === i ? close() : show(i) })
    b.addEventListener('mouseenter', () => { if (open >= 0 && open !== i) show(i) })
    bar.append(b)
    return b
  })
  const doc = bar.ownerDocument

  function close(): void {
    popup?.remove(); popup = null
    if (open >= 0) titles[open].classList.remove('open')
    open = -1
    doc.removeEventListener('mousedown', outside, true)
    doc.removeEventListener('keydown', keys, true)
  }
  function outside(e: MouseEvent): void { if (!bar.contains(e.target as Node) && !popup?.contains(e.target as Node)) close() }
  function rows(): HTMLElement[] { return popup ? [...popup.querySelectorAll<HTMLElement>('.menu-item:not(.disabled)')] : [] }
  function keys(e: KeyboardEvent): void {
    const list = rows(), at = list.indexOf(doc.activeElement as HTMLElement)
    if (e.key === 'Escape') close()
    else if (e.key === 'ArrowDown' || e.key === 'ArrowUp') { if (list.length) list[(at + (e.key === 'ArrowDown' ? 1 : list.length - 1)) % list.length].focus() }
    else if (e.key === 'ArrowRight' || e.key === 'ArrowLeft') show((open + (e.key === 'ArrowRight' ? 1 : menus.length - 1)) % menus.length, true)
    else if (e.key === 'Enter' || e.key === ' ') { if (at >= 0) list[at].click() }
    else return // everything else (shortcuts) keeps working while a menu is open
    e.preventDefault(); e.stopPropagation()
  }
  function show(i: number, focusFirst = false): void {
    const wasOpen = open >= 0
    popup?.remove()
    if (open >= 0) titles[open].classList.remove('open')
    open = i
    titles[i].classList.add('open')
    popup = h('div', { class: 'menu-popup', role: 'menu' })
    for (const it of menus[i].items()) {
      if (it === '-') { popup.append(h('div', { class: 'menu-sep' })); continue }
      const row = h('button', { class: `menu-item${it.disabled ? ' disabled' : ''}`, role: 'menuitem', tabindex: '-1', ...(it.title ? { title: it.title } : {}) },
        h('span', { class: 'menu-check' }, it.checked ? '✓' : ''), h('span', { class: 'menu-label' }, it.label), h('span', { class: 'menu-key' }, it.shortcut ?? ''))
      if (it.disabled) row.setAttribute('disabled', '')
      row.addEventListener('click', () => { close(); it.action() })
      popup.append(row)
    }
    const r = titles[i].getBoundingClientRect()
    popup.style.left = `${Math.round(r.left)}px`; popup.style.top = `${Math.round(r.bottom)}px`
    doc.body.append(popup)
    if (focusFirst) rows()[0]?.focus()
    if (!wasOpen) { doc.addEventListener('mousedown', outside, true); doc.addEventListener('keydown', keys, true) }
  }
  return bar
}
