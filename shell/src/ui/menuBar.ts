import { h } from './dom'

/** One entry of a drop-down menu; `'-'` is a separator. An entry with `submenu` opens it (to the right) instead of acting. */
export type MenuItem = '-' | { label: string; shortcut?: string; title?: string; checked?: boolean; disabled?: boolean; action?: () => void; submenu?: () => MenuItem[] }
/** A top-level menu; `items` is called each time it opens, so labels and check marks are always current. */
export interface Menu { label: string; items: () => MenuItem[] }

/**
 * Desktop-style menu bar (File / Edit / ...): click a title to open its menu, move over the other titles to switch,
 * click an item, Esc or a click elsewhere closes it. Arrow keys move between items, into / out of submenus and between menus.
 */
export function menuBar(menus: Menu[]): HTMLElement {
  const bar = h('div', { class: 'menubar', role: 'menubar' })
  let open = -1
  const popups: HTMLElement[] = []  // [the menu, its open submenu?]
  const titles = menus.map((m, i) => {
    const b = h('button', { class: 'menu-title', role: 'menuitem', 'aria-haspopup': 'true' }, m.label)
    b.addEventListener('mousedown', (e) => { e.preventDefault(); open === i ? close() : show(i) })
    b.addEventListener('mouseenter', () => { if (open >= 0 && open !== i) show(i) })
    bar.append(b)
    return b
  })
  const doc = bar.ownerDocument

  const closeFrom = (level: number) => { for (const p of popups.splice(level)) p.remove() }
  function close(): void {
    closeFrom(0)
    if (open >= 0) titles[open].classList.remove('open')
    open = -1
    doc.removeEventListener('mousedown', outside, true)
    doc.removeEventListener('keydown', keys, true)
  }
  function outside(e: MouseEvent): void { if (!bar.contains(e.target as Node) && !popups.some((p) => p.contains(e.target as Node))) close() }
  const rows = (p: HTMLElement | undefined): HTMLElement[] => (p ? [...p.querySelectorAll<HTMLElement>(':scope > .menu-item:not(.disabled)')] : [])
  function keys(e: KeyboardEvent): void {
    const level = Math.max(0, popups.findIndex((p) => p.contains(doc.activeElement)))
    const list = rows(popups[level]), at = list.indexOf(doc.activeElement as HTMLElement)
    const back = (to: number) => { const owner = popups[to].querySelector<HTMLElement>('.sub-open'); owner?.classList.remove('sub-open'); closeFrom(to + 1); owner?.focus() }
    if (e.key === 'Escape') { if (popups.length > 1) back(popups.length - 2); else close() }
    else if (e.key === 'ArrowDown' || e.key === 'ArrowUp') { if (list.length) list[(at + (e.key === 'ArrowDown' ? 1 : list.length - 1)) % list.length].focus() }
    else if (e.key === 'ArrowRight' && at >= 0 && list[at].classList.contains('has-sub')) list[at].click()
    else if (e.key === 'ArrowLeft' && level > 0) back(level - 1)
    else if (e.key === 'ArrowRight' || e.key === 'ArrowLeft') show((open + (e.key === 'ArrowRight' ? 1 : menus.length - 1)) % menus.length, true)
    else if (e.key === 'Enter' || e.key === ' ') { if (at >= 0) list[at].click() }
    else return // everything else (shortcuts) keeps working while a menu is open
    e.preventDefault(); e.stopPropagation()
  }
  /** A popup with `items` at (x, y); `level` 0 = the menu, 1 = a submenu. */
  function popup(items: MenuItem[], x: number, y: number, level: number): HTMLElement {
    const p = h('div', { class: `menu-popup${level ? ' sub' : ''}`, role: 'menu' })
    for (const it of items) {
      if (it === '-') { p.append(h('div', { class: 'menu-sep' })); continue }
      const row = h('button', { class: `menu-item${it.disabled ? ' disabled' : ''}${it.submenu ? ' has-sub' : ''}`, role: 'menuitem', tabindex: '-1', ...(it.title ? { title: it.title } : {}) },
        h('span', { class: 'menu-check' }, it.checked ? '✓' : ''), h('span', { class: 'menu-label' }, it.label), h('span', { class: 'menu-key' }, it.submenu ? '›' : (it.shortcut ?? '')))
      if (it.disabled) { row.setAttribute('disabled', ''); p.append(row); continue }
      const openSub = (focus: boolean) => {
        if (!it.submenu || row.classList.contains('sub-open')) return
        closeFrom(level + 1)
        p.querySelectorAll('.sub-open').forEach((r) => r.classList.remove('sub-open'))
        row.classList.add('sub-open')
        const r = row.getBoundingClientRect()
        const sub = popup(it.submenu(), r.right + 2, r.top - 5, level + 1)
        if (focus) rows(sub)[0]?.focus()
      }
      row.addEventListener('mouseenter', () => {
        if (it.submenu) openSub(false)
        else if (popups.length > level + 1) { closeFrom(level + 1); p.querySelectorAll('.sub-open').forEach((r) => r.classList.remove('sub-open')) }
      })
      row.addEventListener('click', () => { if (it.submenu) openSub(true); else { close(); it.action?.() } })
      p.append(row)
    }
    p.style.left = `${Math.round(x)}px`; p.style.top = `${Math.round(y)}px`
    doc.body.append(p)
    // keep a submenu on screen
    const r = p.getBoundingClientRect(), vw = doc.documentElement.clientWidth
    if (level && r.right > vw) p.style.left = `${Math.max(0, Math.round(x - r.width - (popups[level - 1]?.getBoundingClientRect().width ?? 0) - 4))}px`
    popups[level] = p
    return p
  }
  function show(i: number, focusFirst = false): void {
    const wasOpen = open >= 0
    closeFrom(0)
    if (open >= 0) titles[open].classList.remove('open')
    open = i
    titles[i].classList.add('open')
    const r = titles[i].getBoundingClientRect()
    const p = popup(menus[i].items(), r.left, r.bottom, 0)
    if (focusFirst) rows(p)[0]?.focus()
    if (!wasOpen) { doc.addEventListener('mousedown', outside, true); doc.addEventListener('keydown', keys, true) }
  }
  return bar
}
