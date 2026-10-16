import { h } from './dom'

/**
 * A small tab strip that swaps the content next to it (used inside a single dock panel).
 * `vertical`: the strip runs down the left side of the panel, for panels with more tabs than fit across.
 */
export function tabbed(tabs: { label: string; content: () => HTMLElement }[], opts: { vertical?: boolean } = {}): HTMLElement {
  const bar = h('div', { class: 'subtabs', role: 'tablist', ...(opts.vertical ? { 'aria-orientation': 'vertical' } : {}) })
  const body = h('div', { class: 'subbody' })
  const buttons = tabs.map((t, i) => {
    const b = h('button', { class: 'subtab', role: 'tab', title: t.label }, t.label)
    b.addEventListener('click', () => show(i))
    bar.append(b)
    return b
  })
  const show = (i: number) => { buttons.forEach((b, j) => { b.classList.toggle('on', i === j); b.setAttribute('aria-selected', String(i === j)) }); body.replaceChildren(tabs[i].content()) }
  show(0)
  return h('div', { class: `tabbed${opts.vertical ? ' vertical' : ''}` }, bar, body)
}
