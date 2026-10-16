import { h } from './dom'

/** A small horizontal tab strip that swaps the content below it (used inside a single dock panel). */
export function tabbed(tabs: { label: string; content: () => HTMLElement }[]): HTMLElement {
  const bar = h('div', { class: 'subtabs' })
  const body = h('div', { class: 'subbody' })
  const buttons = tabs.map((t, i) => {
    const b = h('button', { class: 'subtab' }, t.label)
    b.addEventListener('click', () => show(i))
    bar.append(b)
    return b
  })
  const show = (i: number) => { buttons.forEach((b, j) => b.classList.toggle('on', i === j)); body.replaceChildren(tabs[i].content()) }
  show(0)
  return h('div', { class: 'tabbed' }, bar, body)
}
