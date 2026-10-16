import { describe, expect, it } from 'vitest'
import { emptyDocument } from '../src/model/document'
import { makeApp } from './helpers'

const docN = (n: number) => ({ ...emptyDocument(), editor: { n } })

describe('CoreConnection', () => {
  it('sends one update at a time and coalesces the rest into the latest', () => {
    const { conn, sock, sync } = makeApp(); sync()
    conn.submitDocument(docN(1))
    conn.submitDocument(docN(2))
    conn.submitDocument(docN(3))
    expect(sock().updates().map((m) => m.doc.editor.n)).toEqual([1]) // 2 and 3 wait for the ack
    sock().receive({ type: 'update_ack', rev: 2 })
    expect(sock().updates().map((m) => m.doc.editor.n)).toEqual([1, 3]) // only the latest goes out
    sock().receive({ type: 'update_ack', rev: 3 })
    expect(conn.updateInFlight).toBe(false)
    expect(sock().updates()).toHaveLength(2)
  })

  it('a rejection drops the pending update and reports it', () => {
    const { conn, sock, store, sync } = makeApp(); sync()
    conn.submitDocument(docN(1)); conn.submitDocument(docN(2))
    sock().receive({ type: 'error', message: 'nope' })
    expect(conn.updateInFlight).toBe(false)
    expect(conn.hasPendingUpdate).toBe(false)
    expect(sock().updates()).toHaveLength(1) // n=2 was discarded, not sent
    expect(store.state.message).toContain('nope')
  })

  it('resolves requests by id and rejects them if the socket drops', async () => {
    const { conn, sock, sync } = makeApp(); sync()
    const p = conn.request((id) => JSON.stringify({ type: 'save_document', requestId: id }))
    const id = sock().sent[0].requestId
    sock().receive({ type: 'response', requestId: id, ok: true, path: 'x.json' })
    await expect(p).resolves.toMatchObject({ ok: true, path: 'x.json' })

    const q = conn.request((rid) => JSON.stringify({ type: 'open_document', requestId: rid, path: 'a' }))
    sock().close()
    await expect(q).rejects.toThrow('disconnected')
  })

  it('reconnects with growing backoff, and not after close()', () => {
    const { conn, sock, sockets, timers, sync } = makeApp(); sync()
    sock().close()
    expect(conn.status).toBe('disconnected')
    expect(timers.tasks.map((t) => t.ms)).toEqual([500])
    timers.runNext()
    expect(sockets).toHaveLength(2)
    sockets[1].close()
    expect(timers.tasks.map((t) => t.ms)).toEqual([1000])
    timers.runNext(); conn.close()
    expect(timers.tasks).toHaveLength(0)
  })

  it('ignores garbage from the server', () => {
    const { sock, store, sync } = makeApp(); sync()
    sock().onmessage!({ data: 'not json' })
    sock().receive({ type: 'mystery' })
    expect(store.state.synced).toBe(true)
  })
})
