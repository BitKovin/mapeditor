import { emptyDocument, MapDocument } from '../src/model/document'
import { ConnectionEvents, CoreConnection, SocketLike, Timers } from '../src/net/connection'
import { Store } from '../src/state/store'

export class FakeSocket implements SocketLike {
  sent: any[] = []
  closed = false
  onopen: (() => void) | null = null
  onclose: (() => void) | null = null
  onmessage: ((e: { data: unknown }) => void) | null = null
  onerror: (() => void) | null = null
  send(d: string) { this.sent.push(JSON.parse(d)) }
  close() { this.closed = true; this.onclose?.() }
  open() { this.onopen?.() }
  receive(m: object) { this.onmessage?.({ data: JSON.stringify(m) }) }
  updates() { return this.sent.filter((m) => m.type === 'document_update') }
}

export class FakeTimers implements Timers {
  tasks: { fn: () => void; ms: number }[] = []
  set(fn: () => void, ms: number) { const t = { fn, ms }; this.tasks.push(t); return t }
  clear(h: unknown) { this.tasks = this.tasks.filter((t) => t !== h) }
  runNext() { this.tasks.shift()?.fn() }
}

/** A Store wired to a CoreConnection over a FakeSocket, exactly as app.ts wires the real thing. */
export function makeApp() {
  const sockets: FakeSocket[] = []
  const timers = new FakeTimers()
  let store!: Store
  const events: ConnectionEvents = {
    onStatus: (s) => store.setStatus(s),
    onMessage: (m) => store.onMessage(m),
    onAck: (rev, doc) => store.onAck(rev, doc),
    onRejected: (msg) => store.onRejected(msg),
  }
  const conn = new CoreConnection('ws://test', events, () => { const s = new FakeSocket(); sockets.push(s); return s }, timers)
  store = new Store(conn)
  conn.connect()
  const sock = () => sockets[sockets.length - 1]
  /** Connect and receive the initial document from Core. */
  const sync = (doc: MapDocument = emptyDocument(), rev = 1) => {
    sock().open()
    sock().receive({ type: 'document_sync', rev, path: 'maps/t.json', doc })
  }
  return { conn, store, sock, sockets, timers, sync }
}
