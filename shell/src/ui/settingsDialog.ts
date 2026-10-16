// Settings: which assets folder Core works in. The folder picker asks Core (`browse_filesystem`) to list
// folders (folders only), because a browser page cannot see the disk. Choosing one persists it (Core keeps
// settings.json) and reloads the game profile, entity definitions and the current map from there.
import type { CoreConnection } from '../net/connection'
import { msg } from '../net/protocol'
import type { FolderEntry } from '../net/protocol'
import type { Store } from '../state/store'
import { confirmDialog, openDialog } from './dialog'
import { h } from './dom'

export function openSettings(store: Store, conn: CoreConnection): void {
  let unsub: () => void = () => {}
  const d = openDialog('Settings', { width: 620, onClose: () => unsub() })
  const st = () => store.state

  const current = h('div', { class: 'modal-text' })
  const info = h('div', { class: 'dim modal-text' })
  const refreshInfo = () => {
    current.replaceChildren(h('b', {}, 'Assets folder: '), st().assetsRoot || '(unknown)')
    const s = st()
    const lines = [
      s.profile ? `Game profile: ${s.profile.name}` : 'Game profile: none',
      s.entityDefs.length ? `Entity definitions: ${s.entityDefs.length} classes (${s.profile?.entityDefinitions ?? ''})` : `Entity definitions: none found (${s.profile?.entityDefinitions ?? 'entities.ent'} is missing) - built-in list is used`,
      ...s.entityWarnings.slice(0, 5).map((w) => `⚠ ${w}`),
      ...(s.entityWarnings.length > 5 ? [`… and ${s.entityWarnings.length - 5} more warnings`] : []),
    ]
    info.replaceChildren(...lines.map((l) => h('div', {}, l)))
  }
  refreshInfo()
  unsub = store.subscribe(refreshInfo)

  const path = h('input', { class: 'fd-name', placeholder: 'Absolute folder path, e.g. C:\\Games\\MyGame\\assets' })
  const list = h('div', { class: 'fd-list' }), err = h('div', { class: 'fd-error' }), badge = h('div', { class: 'dim' })
  const up = h('button', {}, '↑ Up'), go = h('button', {}, 'Go'), use = h('button', { class: 'primary' }, 'Use this folder'), close = h('button', {}, 'Close')
  let here = '', parent: string | null = null

  async function browse(to: string): Promise<void> {
    err.textContent = ''
    try {
      const r = await conn.request((id) => msg.browseFilesystem(id, to))
      if (!r.ok) { err.textContent = r.error ?? 'Cannot open that folder'; return }
      here = r.path ?? ''; parent = r.parent ?? null
      path.value = here
      up.toggleAttribute('disabled', parent === null)
      use.toggleAttribute('disabled', here === '')
      badge.textContent = here === '' ? 'Pick a drive' : r.hasProfile ? '✓ game_profile.json found here' : 'No game_profile.json here - built-in defaults would be used'
      list.replaceChildren()
      const row = (label: string, target: string, icon: string) => {
        const el = h('div', { class: 'fd-row' }, h('span', { class: 'fd-icon' }, icon), label)
        el.addEventListener('click', () => void browse(target))
        list.append(el)
      }
      for (const drv of r.drives ?? []) row(drv, drv, '💽')
      for (const f of (r.dirs ?? []) as FolderEntry[]) row(f.name, f.path, '📁')
      if (!list.children.length) list.append(h('div', { class: 'dim pad' }, 'No sub-folders.'))
    } catch (e) { err.textContent = (e as Error).message }
  }

  async function apply(): Promise<void> {
    if (!here) return
    if (st().dirty && !(await confirmDialog('Unsaved changes', 'The current map has unsaved changes. Switching the assets folder re-opens the map from the new folder and discards them. Continue?', 'Switch folder'))) return
    try {
      await conn.whenIdle() // a document_sync is ignored while our own edit is still in flight
      const r = await conn.request((id) => msg.setAssetsRoot(id, here))
      if (!r.ok) { err.textContent = r.error ?? 'Could not switch'; return }
      store.setMessage(r.persisted === false ? 'Assets folder changed (could not be saved to settings.json)' : `Assets folder: ${r.assetsRoot}`)
      d.close()
    } catch (e) { err.textContent = (e as Error).message }
  }

  up.addEventListener('click', () => void browse(parent ?? ''))
  go.addEventListener('click', () => void browse(path.value.trim()))
  path.addEventListener('keydown', (e) => { if (e.key === 'Enter') void browse(path.value.trim()) })
  use.addEventListener('click', () => void apply())
  close.addEventListener('click', () => d.close())

  d.body.append(current, info, h('div', { class: 'section' }, 'Change assets folder'),
    h('div', { class: 'fd-bar' }, up, path, go), list, badge, err, h('div', { class: 'modal-buttons' }, close, use))
  void browse(st().assetsRoot)
}
