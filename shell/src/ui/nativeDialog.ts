// The operating system's own file and folder dialogs, opened by Core (`native_dialog`): a browser page cannot get real paths from
// its own file inputs. While one is open the page is covered, as an application window is while its file dialog is up.
import type { CoreConnection } from '../net/connection'
import { msg } from '../net/protocol'
import type { DialogFilter } from '../net/protocol'
import type { Store } from '../state/store'
import { h } from './dom'

export interface NativePick {
  kind: 'folder' | 'open' | 'save'
  title: string
  /** relative: inside the assets root, else absolute; '' = the system's choice */
  startDir?: string
  fileName?: string
  filters?: DialogFilter[]
  defaultExt?: string
  /** start folder and result relative to the assets (game) folder; Core refuses a choice outside it */
  relative?: boolean
}

/**
 * The chosen path; null = cancelled (or refused, with the reason on the status bar); undefined = Core cannot show native dialogs here,
 * so the caller uses its in-page browser instead.
 */
export async function nativePick(conn: CoreConnection, store: Store, o: NativePick): Promise<string | null | undefined> {
  if (!store.state.coreSettings?.nativeDialogs) return undefined
  const cover = h('div', { class: 'native-wait' }, h('div', { class: 'native-wait-box' }, `${o.title}…`, h('div', { class: 'dim' }, 'Finish in the dialog window')))
  document.body.append(cover)
  try {
    const r = await conn.request((id) => msg.nativeDialog(id, o))
    if (r.unsupported) return undefined
    if (r.cancelled) return null
    if (!r.ok || typeof r.path !== 'string') { store.setMessage(r.error ?? 'The dialog failed'); return null }
    return r.path
  } catch (e) {
    store.setMessage((e as Error).message)
    return null
  } finally {
    cover.remove()
  }
}

export const MAP_FILTERS: DialogFilter[] = [{ name: 'Maps (*.map.json)', pattern: '*.map.json' }]
