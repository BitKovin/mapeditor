// WebSocket client for Core (DESIGN.md §3/§10).
//  * At most ONE document_update in flight; while waiting for the ack, newer edits replace a
//    single pending slot, so a drag sends the latest state, never a backlog.
//  * A rejected update (`error`) drops the pending slot and is reported so the store can roll back.
//  * Reconnects with backoff. While disconnected the Shell is read-only, so nothing is lost.
import type { MapDocument } from '../model/document'
import { Inbound, msg, parseInbound } from './protocol'

export interface SocketLike {
  send(data: string): void
  close(): void
  onopen: (() => void) | null
  onclose: (() => void) | null
  onmessage: ((e: { data: unknown }) => void) | null
  onerror: (() => void) | null
}
export type Status = 'connecting' | 'connected' | 'disconnected'
export interface ConnectionEvents {
  onStatus(s: Status): void
  onMessage(m: Inbound): void
  onAck(rev: number, doc: MapDocument | null): void
  onRejected(message: string): void
}
export interface Timers { set(fn: () => void, ms: number): unknown; clear(h: unknown): void }

const defaultTimers: Timers = { set: (fn, ms) => setTimeout(fn, ms), clear: (h) => clearTimeout(h as any) }

export class CoreConnection {
  private sock: SocketLike | null = null
  private inFlight: MapDocument | null = null
  private pending: MapDocument | null = null
  private nextRequest = 1
  private requests = new Map<number, { resolve: (m: Extract<Inbound, { type: 'response' }>) => void; reject: (e: Error) => void }>()
  private idleWaiters: (() => void)[] = []
  private retry: unknown = null
  private backoff = 500
  private closedByUser = false
  status: Status = 'disconnected'

  constructor(
    private url: string,
    private events: ConnectionEvents,
    private socketFactory: (url: string) => SocketLike = (u) => new WebSocket(u) as unknown as SocketLike,
    private timers: Timers = defaultTimers,
  ) {}

  get updateInFlight(): boolean { return this.inFlight !== null }
  get hasPendingUpdate(): boolean { return this.pending !== null }

  connect(): void {
    this.closedByUser = false
    this.setStatus('connecting')
    const s = this.socketFactory(this.url)
    this.sock = s
    s.onopen = () => { this.backoff = 500; this.setStatus('connected') }
    s.onmessage = (e) => this.receive(String(e.data))
    s.onerror = () => {}
    s.onclose = () => {
      this.sock = null
      this.inFlight = this.pending = null
      this.notifyIdle()
      for (const r of this.requests.values()) r.reject(new Error('disconnected'))
      this.requests.clear()
      this.setStatus('disconnected')
      if (!this.closedByUser) {
        this.retry = this.timers.set(() => this.connect(), this.backoff)
        this.backoff = Math.min(this.backoff * 2, 5000)
      }
    }
  }

  close(): void {
    this.closedByUser = true
    if (this.retry) this.timers.clear(this.retry)
    this.sock?.close()
  }

  /** Queue the latest document for Core. Coalesces while an update is in flight. */
  submitDocument(doc: MapDocument): void {
    this.pending = doc
    this.flush()
  }

  /** Resolves once no document update is in flight or waiting (immediately if none is). Save waits for this. */
  whenIdle(): Promise<void> {
    if (!this.inFlight && !this.pending) return Promise.resolve()
    return new Promise((resolve) => this.idleWaiters.push(resolve))
  }
  private notifyIdle(): void {
    if (this.inFlight || this.pending) return
    const w = this.idleWaiters; this.idleWaiters = []
    w.forEach((f) => f())
  }

  /** Fire-and-forget message (live lighting requests); dropped while disconnected, the store re-sends its wishes on reconnect. */
  sendLighting(text: string): void {
    if (this.status === 'connected' && this.sock) this.sock.send(text)
  }

  request(build: (requestId: number) => string): Promise<Extract<Inbound, { type: 'response' }>> {
    const id = this.nextRequest++
    return new Promise((resolve, reject) => {
      if (this.status !== 'connected' || !this.sock) return reject(new Error('not connected to Core'))
      this.requests.set(id, { resolve, reject })
      this.sock.send(build(id))
    })
  }

  private flush(): void {
    if (this.status !== 'connected' || !this.sock || this.inFlight || !this.pending) return
    this.inFlight = this.pending
    this.pending = null
    this.sock.send(msg.documentUpdate(this.inFlight))
  }

  private receive(text: string): void {
    const m = parseInbound(text)
    if (!m) return
    switch (m.type) {
      case 'update_ack': {
        const doc = this.inFlight
        this.inFlight = null
        this.events.onAck(m.rev, doc)
        this.flush()
        this.notifyIdle()
        return
      }
      case 'error':
        if (this.inFlight) {
          this.inFlight = this.pending = null
          this.events.onRejected(m.message)
          this.notifyIdle()
          return
        }
        break
      case 'response': {
        const r = this.requests.get(m.requestId as number)
        if (r) { this.requests.delete(m.requestId as number); r.resolve(m) }
        break
      }
    }
    this.events.onMessage(m)
  }

  private setStatus(s: Status): void {
    this.status = s
    this.events.onStatus(s)
  }
}
