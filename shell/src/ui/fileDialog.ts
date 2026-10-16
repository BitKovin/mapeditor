// File browser for opening and saving maps. Everything is relative to Core's assets folder; Core
// does the listing (`list_directory`), so the browser never sees a path outside it.
import type { DirEntry } from '../net/protocol'
import { confirmDialog, openDialog } from './dialog'
import { h } from './dom'

export interface FileDialogOptions {
  mode: 'open' | 'save'
  title: string
  /** Extension of the files shown / appended when saving, e.g. ".map.json". */
  ext: string
  /** Folder to start in (assets-root relative, '' = the root). Falls back to the root when it does not exist. */
  startDir: string
  /** Suggested file name (save mode) or file to pre-select (open mode). */
  startName?: string
  list(dir: string): Promise<{ path: string; entries: DirEntry[] }>
}

export const joinPath = (dir: string, name: string): string => (dir ? `${dir.replace(/\/+$/, '')}/${name}` : name)
export const parentDir = (dir: string): string => dir.replace(/\/+$/, '').split('/').slice(0, -1).join('/')

/** Adds `ext` unless the name already has it; rejects names that would leave the folder. */
export function fileNameWithExt(raw: string, ext: string): string | null {
  const name = raw.trim()
  if (!name || /[\\/:*?"<>|]/.test(name) || name === '.' || name === '..' || name.startsWith('.')) return null
  return name.toLowerCase().endsWith(ext.toLowerCase()) ? name : name + ext
}

/** Resolves to the chosen assets-root-relative path, or null when cancelled. */
export function chooseFile(o: FileDialogOptions): Promise<string | null> {
  return new Promise((resolve) => {
    let result: string | null = null
    const d = openDialog(o.title, { width: 560, onClose: () => resolve(result) })
    let dir = o.startDir
    let entries: DirEntry[] = []
    let selected = o.mode === 'open' ? (o.startName ?? '') : ''

    const crumbs = h('div', { class: 'fd-crumbs' })
    const list = h('div', { class: 'fd-list', tabindex: '0' })
    const err = h('div', { class: 'fd-error' })
    const name = h('input', { class: 'fd-name', placeholder: o.mode === 'save' ? `file name (${o.ext})` : `pick a ${o.ext} file`, value: o.startName ?? '' })
    if (o.mode === 'open') name.setAttribute('readonly', '')
    const ok = h('button', { class: 'primary' }, o.mode === 'open' ? 'Open' : 'Save'), cancel = h('button', {}, 'Cancel')
    const up = h('button', { title: 'Parent folder' }, '↑ Up')

    const matches = (e: DirEntry) => e.dir || e.name.toLowerCase().endsWith(o.ext.toLowerCase())

    async function go(to: string): Promise<void> {
      err.textContent = ''
      try {
        const r = await o.list(to)
        dir = r.path; entries = r.entries.filter(matches)
      } catch (e) {
        if (to !== '') return go('') // the start folder does not exist (yet): show the root instead
        err.textContent = (e as Error).message
        entries = []
      }
      draw()
    }

    function draw(): void {
      crumbs.replaceChildren()
      const parts = dir ? dir.split('/') : []
      const seg = (label: string, target: string) => {
        const b = h('button', { class: 'crumb' }, label)
        b.addEventListener('click', () => void go(target))
        return b
      }
      crumbs.append(seg('assets', ''))
      parts.forEach((p, i) => { crumbs.append(h('span', { class: 'dim' }, '›'), seg(p, parts.slice(0, i + 1).join('/'))) })
      up.toggleAttribute('disabled', !dir)

      list.replaceChildren()
      if (!entries.length) list.append(h('div', { class: 'dim pad' }, `No folders or ${o.ext} files here.`))
      for (const e of entries) {
        const row = h('div', { class: `fd-row${!e.dir && e.name === selected ? ' sel' : ''}` }, h('span', { class: 'fd-icon' }, e.dir ? '📁' : '📄'), e.name)
        row.addEventListener('click', () => {
          if (e.dir) return void go(joinPath(dir, e.name))
          selected = e.name; name.value = e.name; draw()
        })
        row.addEventListener('dblclick', () => { if (!e.dir) void accept() })
        list.append(row)
      }
    }

    async function accept(): Promise<void> {
      err.textContent = ''
      const file = o.mode === 'open' ? selected : fileNameWithExt(name.value, o.ext)
      if (!file) { err.textContent = o.mode === 'open' ? 'Pick a file first.' : 'Enter a valid file name (no slashes or special characters).'; return }
      if (o.mode === 'save' && entries.some((e) => !e.dir && e.name.toLowerCase() === file.toLowerCase()) &&
          !(await confirmDialog('Replace file?', `${joinPath(dir, file)} already exists. Replace it?`, 'Replace'))) return
      result = joinPath(dir, file)
      d.close()
    }

    up.addEventListener('click', () => void go(parentDir(dir)))
    ok.addEventListener('click', () => void accept())
    cancel.addEventListener('click', () => d.close())
    name.addEventListener('keydown', (e) => { if (e.key === 'Enter') void accept() })
    name.addEventListener('input', () => { selected = name.value })
    d.body.append(h('div', { class: 'fd-bar' }, up, crumbs), list, err,
      h('div', { class: 'fd-foot' }, name, cancel, ok))
    void go(dir)
    if (o.mode === 'save') { name.focus(); name.select() }
  })
}
