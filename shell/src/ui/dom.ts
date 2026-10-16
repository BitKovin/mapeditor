// Tiny DOM helpers so panels stay short.
export function h<K extends keyof HTMLElementTagNameMap>(
  tag: K, attrs: Record<string, string> = {}, ...kids: (Node | string)[]
): HTMLElementTagNameMap[K] {
  const el = document.createElement(tag)
  for (const [k, v] of Object.entries(attrs)) k === 'class' ? (el.className = v) : el.setAttribute(k, v)
  for (const kid of kids) el.append(kid)
  return el
}
export const fmt = (n: number): string => String(Math.round(n * 10000) / 10000)
