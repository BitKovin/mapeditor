// Texture groups (REQUIREMENTS §20): the reader and the solver.
import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { describe, expect, it } from 'vitest'
import { Cell, hash32, rand01, solveLayout } from '../src/model/layout'
import { TexGroup, faceLayoutToJson, parseFaceLayout, parseGroup, parseGroupText, resolveParams } from '../src/model/texgroup'

const apartment = readFileSync(new URL('../../core/assets/texgroups/apartment_block.texgroup.json', import.meta.url), 'utf8')
const mk = (root: unknown, extra: Record<string, unknown> = {}): TexGroup => {
  const r = parseGroup({ format: 1, slots: { a: 'dev/a', b: 'dev/b', c: 'dev/c' }, root, ...extra })
  if (!r.group) throw new Error(r.errors.join('; '))
  return r.group
}
const solve = (g: TexGroup, w: number, h: number, o: Partial<Parameters<typeof solveLayout>[0]> = {}) => solveLayout({ group: g, w, h, seed: 1, ...o })
const slots = (cells: Cell[]): string[] => cells.map((c) => c.slot)
const area = (cells: Cell[]): number => cells.reduce((s, c) => s + (c.x1 - c.x0) * (c.y1 - c.y0), 0)

describe('reader', () => {
  it('reads the shipped apartment block without warnings', () => {
    const r = parseGroupText(apartment)
    expect(r.errors).toEqual([]); expect(r.warnings).toEqual([]); expect(r.group!.params.floorHeight.type).toBe('number')
  })
  it('is lenient: bad parts are dropped and reported, never thrown', () => {
    const r = parseGroup({ slots: { a: 'dev/a', bad: '../x' }, root: { split: 'v', parts: [{ size: 1, tex: 'a' }, { size: -3, tex: 'a' }, 7, { size: 1 }] } })
    expect(r.group).not.toBeNull()
    expect(r.warnings.length).toBeGreaterThan(2)
    expect(Object.keys(r.group!.slots)).toEqual(['a'])
  })
  it('errors: not an object, wrong format, no root, bad JSON', () => {
    expect(parseGroup(5).group).toBeNull(); expect(parseGroup({ format: 2, root: 'a' }).errors[0]).toMatch(/format/)
    expect(parseGroup({ slots: {} }).errors[0]).toMatch(/root/); expect(parseGroupText('{').errors[0]).toMatch(/JSON/)
  })
  it('a part with an unusable condition is dropped, never made unconditional', () => {
    const g = mk({ split: 'u', parts: [{ size: 1, tex: 'a', when: { oops: 1 } }, { size: 1, tex: 'b' }] })
    expect(slots(solve(g, 2, 1).cells)).toEqual(['b'])
  })
  it('reports unknown slots, rules, parameters and variables', () => {
    const r = parseGroup({ slots: { a: 'dev/a' }, root: { split: 'u', parts: [{ size: { param: 'nope' }, tex: 'zz', when: { var: 'bogus', '>': 1 } }, { ref: 'gone' }] } })
    const w = r.warnings.join('\n')
    expect(w).toMatch(/slot 'zz'/); expect(w).toMatch(/rule 'gone'/); expect(w).toMatch(/parameter 'nope'/); expect(w).toMatch(/variable 'bogus'/)
  })
  it('size shorthands', () => {
    const g = mk({ split: 'u', parts: [{ size: '25%', tex: 'a' }, { size: '2*', tex: 'b' }, { size: '*', tex: 'c' }] })
    const c = solve(g, 8, 1).cells
    expect(c.map((x) => x.x1 - x.x0)).toEqual([2, 4, 2])
  })
  it('face layout: only differences are stored; hostile values are refused', () => {
    expect(faceLayoutToJson(parseFaceLayout({ group: 'g/x' })!)).toEqual({ group: 'g/x' })
    expect(parseFaceLayout({ group: '../x' })).toBeNull(); expect(parseFaceLayout({ group: 5 })).toBeNull()
    const l = parseFaceLayout({ group: 'g', seed: 7, params: { a: 1, b: { x: 1 }, 'bad name': 2 }, rotate: 90, flipU: true })!
    expect(l.params).toEqual({ a: 1 }); expect(l.rotate).toBe(90); expect(faceLayoutToJson(l).seed).toBe(7)
  })
  it('parameters: defaults, clamping, choice and bool checks', () => {
    const g = mk('a', { params: { n: { type: 'number', default: 3, min: 2, max: 6 }, i: { type: 'int', default: 2, min: 1, max: 9 }, f: { type: 'bool', default: true }, c: { type: 'choice', options: ['x', 'y'] } } })
    expect(resolveParams(g, { n: 99, i: 4.6, f: 'no' as never, c: 'zz' })).toEqual({ n: 6, i: 5, f: true, c: 'x' })
  })
})

describe('solver: splits and sizes', () => {
  it('v runs bottom to top, u left to right; the cells cover the face exactly', () => {
    const g = mk({ split: 'v', parts: [{ size: 1, tex: 'a' }, { size: '*', tex: 'b' }, { size: 0.5, tex: 'c' }] })
    const c = solve(g, 4, 5).cells
    expect(slots(c)).toEqual(['a', 'b', 'c']); expect(c[0].y0).toBe(0); expect(c[2].y1).toBeCloseTo(5)
    expect(area(c)).toBeCloseTo(20)
  })
  it('flex min / max are honoured and the rest is shared again', () => {
    const g = mk({ split: 'u', parts: [{ size: { flex: 1, max: 1 }, tex: 'a' }, { size: '*', tex: 'b' }, { size: { flex: 1, min: 4 }, tex: 'c' }] })
    const c = solve(g, 9, 1).cells
    expect(c.map((x) => +(x.x1 - x.x0).toFixed(6))).toEqual([1, 4, 4])
  })
  it('natural size uses the texture size; fraction and parameter sizes', () => {
    const g = mk({ split: 'u', parts: [{ size: { natural: 'a' }, tex: 'a' }, { size: { param: 'k', mul: 2 }, tex: 'b' }, { size: '*', tex: 'c' }] }, { params: { k: { type: 'number', default: 1, min: 0, max: 5 } } })
    const info = (p: string) => (p === 'dev/a' ? { path: p, width: 128, height: 64, ppm: 64, props: undefined as never } : undefined)
    const c = solve(g, 10, 1, { info, params: { k: 1.5 } }).cells
    expect(c.map((x) => x.x1 - x.x0)).toEqual([2, 3, 5])
  })
  it('too little room: optional parts go first, lowest priority first, then everything shrinks', () => {
    const g = mk({ split: 'u', parts: [{ size: 2, tex: 'a' }, { size: 2, tex: 'b', optional: true, priority: 1 }, { size: 2, tex: 'c', optional: true, priority: 0 }] })
    expect(slots(solve(g, 4, 1).cells)).toEqual(['a', 'b']) // c (priority 0) dropped, then it fits
    const r = solve(g, 1, 1)
    expect(slots(r.cells)).toEqual(['a']); expect(r.cells[0].x1).toBeCloseTo(1) // a alone, shrunk? 2 m part in 1 m: stretched/shrunk to the face
    const g2 = mk({ split: 'u', parts: [{ size: 3, tex: 'a' }, { size: 3, tex: 'b' }] })
    const s = solve(g2, 3, 1)
    expect(s.cells.map((x) => x.x1 - x.x0)).toEqual([1.5, 1.5]); expect(s.warnings.join()).toMatch(/shrunk/)
  })
  it('leftover: stretch scales, start / center / end leave a gap filled by the fallback or `fill`', () => {
    const parts = [{ size: 1, tex: 'a' }, { size: 1, tex: 'b' }]
    expect(solve(mk({ split: 'u', parts }), 4, 1).cells.map((x) => x.x1 - x.x0)).toEqual([2, 2])
    const c = solve(mk({ split: 'u', parts, leftover: 'center', fill: 'c' }), 4, 1).cells
    expect(slots(c)).toEqual(['c', 'a', 'b', 'c']); expect(c[1].x0).toBeCloseTo(1); expect(c[3].x1).toBeCloseTo(4)
    expect(slots(solve(mk({ split: 'u', parts, leftover: 'end' }), 4, 1).cells)).toEqual(['', 'a', 'b']) // no fill: fallback cell
  })
  it('repeat: whole equal copies (round / floor / ceil), min and max', () => {
    const rp = (o: object) => mk({ split: 'v', parts: [{ size: '*', tex: 'a', repeat: { size: 3, ...o } }] })
    expect(solve(rp({}), 1, 10).cells.length).toBe(3); expect(solve(rp({ fit: 'floor' }), 1, 10).cells.length).toBe(3); expect(solve(rp({ fit: 'ceil' }), 1, 10).cells.length).toBe(4)
    expect(solve(rp({}), 1, 1).cells.length).toBe(1); expect(solve(rp({ max: 2 }), 1, 30).cells.length).toBe(2)
    const c = solve(rp({}), 1, 10).cells
    expect(c.map((x) => +(x.y1 - x.y0).toFixed(6))).toEqual([3.333333, 3.333333, 3.333333])
  })
  it('conditions on parts: a door only on a wide wall', () => {
    const g = mk({ split: 'u', parts: [{ size: 1, tex: 'a', when: { var: 'faceW', '>=': 5 } }, { size: '*', tex: 'b' }] })
    expect(slots(solve(g, 4, 1).cells)).toEqual(['b']); expect(slots(solve(g, 6, 1).cells)).toEqual(['a', 'b'])
  })
})

describe('solver: logic', () => {
  it('named repeats: nested floors x bays are told apart; mod / first / last work', () => {
    const g = mk({ split: 'v', parts: [{ size: '*', repeat: { size: 1, as: 'floor' }, split: 'u', parts: [{ size: '*', repeat: { size: 1, as: 'bay' }, choice: [
      { when: { all: ['floor.first', 'bay.first'] }, tex: 'a' }, { when: { var: 'floor.index', mod: [2, 1] }, tex: 'b' }, { tex: 'c' }] }] }] })
    const c = solve(g, 3, 4).cells
    const at = (x: number, y: number) => c.find((q) => q.x0 <= x && x < q.x1 && q.y0 <= y && y < q.y1)!.slot
    expect(at(0.5, 0.5)).toBe('a'); expect(at(1.5, 0.5)).toBe('c'); expect(at(1.5, 1.5)).toBe('b'); expect(at(1.5, 2.5)).toBe('c'); expect(at(1.5, 3.5)).toBe('b')
  })
  it('choice modes: first, cycle by index, random is seeded and weighted', () => {
    const cyc = mk({ split: 'u', parts: [{ size: '*', repeat: { size: 1 }, choice: ['a', 'b', 'c'], mode: 'cycle' }] })
    expect(slots(solve(cyc, 7, 1).cells)).toEqual(['a', 'b', 'c', 'a', 'b', 'c', 'a'])
    const rnd = mk({ split: 'u', parts: [{ size: '*', repeat: { size: 1 }, choice: [{ tex: 'a', weight: 1 }, { tex: 'b', weight: 1 }], mode: 'random' }] })
    const a = slots(solve(rnd, 40, 1, { seed: 5 }).cells), b = slots(solve(rnd, 40, 1, { seed: 5 }).cells), d = slots(solve(rnd, 40, 1, { seed: 6 }).cells)
    expect(a).toEqual(b); expect(a).not.toEqual(d)
    expect(a.filter((s) => s === 'a').length).toBeGreaterThan(8); expect(a.filter((s) => s === 'b').length).toBeGreaterThan(8)
    const heavy = mk({ split: 'u', parts: [{ size: '*', repeat: { size: 1 }, choice: [{ tex: 'a', weight: 99 }, { tex: 'b', weight: 1 }], mode: 'random' }] })
    expect(slots(solve(heavy, 100, 1).cells).filter((s) => s === 'a').length).toBeGreaterThan(85)
  })
  it('rand in a condition: deterministic per place (10% broken windows)', () => {
    const g = mk({ split: 'u', parts: [{ size: '*', repeat: { size: 1 }, choice: [{ when: { var: 'rand', '<': 0.2 }, tex: 'b' }, { tex: 'a' }] }] })
    const x = slots(solve(g, 200, 1).cells), y = slots(solve(g, 200, 1).cells)
    expect(x).toEqual(y); const n = x.filter((s) => s === 'b').length; expect(n).toBeGreaterThan(15); expect(n).toBeLessThan(70)
  })
  it('facing picks the branch (roof / floor / wall), tilt and worldY are readable', () => {
    const g = mk({ choice: [{ when: { var: 'facing', '==': 'up' }, tex: 'a' }, { when: { var: 'tilt', '>': 120 }, tex: 'b' }, { tex: 'c' }] })
    expect(slots(solve(g, 2, 2, { facing: 'up', tilt: 0 }).cells)).toEqual(['a']); expect(slots(solve(g, 2, 2, { facing: 'down', tilt: 180 }).cells)).toEqual(['b']); expect(slots(solve(g, 2, 2).cells)).toEqual(['c'])
    const w = mk({ split: 'v', parts: [{ size: 5, tex: 'a', when: { var: 'worldY', '<': 10 } }, { size: '*', tex: 'b' }] })
    expect(slots(solve(w, 1, 20, { worldY: (_x, y) => y + 12 }).cells)).toEqual(['b'])
  })
  it('refs reuse rules; a rule calling itself stops with a warning, not a hang', () => {
    const g = mk({ split: 'u', parts: [{ size: 1, ref: 'r' }, { size: 1, ref: 'r' }] }, { rules: { r: { tex: 'a' } } })
    expect(slots(solve(g, 2, 1).cells)).toEqual(['a', 'a'])
    const loop = mk({ ref: 'x' }, { rules: { x: { split: 'u', parts: [{ size: '*', ref: 'x' }] } } })
    const r = solve(loop, 2, 2); expect(r.warnings.join()).toMatch(/nested too deep|too many/)
  })
  it('caps: too many cells is reported and gives no cells', () => {
    const g = mk({ split: 'u', parts: [{ size: '*', repeat: { size: 0.01, max: 4096 }, tex: 'a' }] })
    const r = solve(g, 100, 1); expect(r.truncated).toBe(true); expect(r.cells).toEqual([])
  })
  it('a missing slot fills with the fallback cell and says so', () => {
    const g = mk({ split: 'u', parts: [{ size: 1, tex: 'a' }] }); g.slots = {}
    const r = solve(g, 1, 1, { fallbackTexture: 'dev/f' }); expect(r.cells[0].texture).toBe('dev/f'); expect(r.warnings.join()).toMatch(/slot/)
  })
  it('hash / rand01 are stable', () => {
    expect(hash32('abc')).toBe(hash32('abc')); expect(rand01(1, 'x')).toBeGreaterThanOrEqual(0); expect(rand01(1, 'x')).toBeLessThan(1); expect(rand01(1, 'x')).not.toBe(rand01(2, 'x'))
  })
})

describe('the apartment block', () => {
  const g = parseGroupText(apartment).group!
  const run = (w: number, h: number, o = {}) => solveLayout({ group: g, w, h, seed: 3, ...o })
  it('lays out ground floor, repeating floors, a top floor and a cornice', () => {
    const r = run(9, 3.9 + 12 + 3 + 0.5)
    expect(r.warnings).toEqual([]); expect(area(r.cells)).toBeCloseTo(9 * 19.4, 5)
    const rows = (y0: number, y1: number) => r.cells.filter((c) => c.y0 >= y0 - 1e-6 && c.y1 <= y1 + 1e-6)
    expect(rows(0, 3.9).some((c) => c.slot === 'door')).toBe(true) // door in the first bay of a wide wall
    expect(rows(0, 3.9).filter((c) => c.slot === 'shop').length).toBe(2)
    for (let f = 0; f < 4; f++) expect(rows(3.9 + 3 * f, 3.9 + 3 * f + 3).some((c) => c.slot === 'window')).toBe(true)
    expect(r.cells.filter((c) => c.slot === 'stair').length).toBe(4) // one stair window per middle floor
    expect(r.cells.filter((c) => c.slot === 'balcony').length).toBeGreaterThan(0)
    expect(r.cells.filter((c) => c.slot === 'cornice')).toHaveLength(1)
  })
  it('balconies can be switched off; a narrow wall has no door; roofs take the roof texture', () => {
    expect(run(9, 19.4, { params: { balconies: false } }).cells.some((c) => c.slot === 'balcony')).toBe(false)
    expect(run(4, 19.4).cells.some((c) => c.slot === 'door')).toBe(false)
    expect(slots(run(9, 9, { facing: 'up', tilt: 0 }).cells)).toEqual(['roof'])
  })
  it('the same wall with another seed varies only the top floor', () => {
    const a = run(30, 19.4, { seed: 1 }).cells, b = run(30, 19.4, { seed: 2 }).cells
    const low = (c: Cell[]) => JSON.stringify(c.filter((x) => x.y1 <= 15.9 + 1e-6)), top = (c: Cell[]) => JSON.stringify(c.filter((x) => x.y0 >= 15.9 - 1e-6 && x.y1 <= 18.9 + 1e-6))
    expect(low(a)).toBe(low(b)); expect(top(a)).not.toBe(top(b))
  })
})

describe('shipped example groups', () => {
  it('every file in core/assets/texgroups reads cleanly and lays out a wall, a floor and a tiny face without gaps', async () => {
    const { readdirSync } = await import('node:fs')
    const dir = new URL('../../core/assets/texgroups/', import.meta.url)
    const files = readdirSync(dir).filter((f) => f.endsWith('.texgroup.json'))
    expect(files.length).toBeGreaterThanOrEqual(3)
    for (const f of files) {
      const r = parseGroupText(readFileSync(new URL(f, dir), 'utf8'))
      expect(r.errors, f).toEqual([]); expect(r.warnings, f).toEqual([])
      for (const [w, h, facing] of [[9, 12, 'wall'], [6, 6, 'up'], [0.8, 0.8, 'wall']] as const) {
        const s = solveLayout({ group: r.group!, w, h, facing, seed: 2 })
        expect(s.truncated, f).toBe(false)
        expect(area(s.cells), `${f} ${w}x${h}`).toBeCloseTo(w * h, 4)
      }
    }
  })
})

describe('shipped example groups: textures exist', () => {
  it('every texture a shipped group uses is a file in core/assets/textures (any depth of group folder)', async () => {
    const { readdirSync, existsSync, statSync } = await import('node:fs')
    const root = new URL('../../core/assets/', import.meta.url)
    const walk = (dir: URL, acc: string[] = []): string[] => { for (const f of readdirSync(dir)) { const u = new URL(f + (statSync(new URL(f, dir)).isDirectory() ? '/' : ''), dir); if (f.endsWith('/')) continue; if (statSync(u).isDirectory()) walk(u, acc); else if (f.endsWith('.texgroup.json')) acc.push(fileURLToPath(u)) } return acc }
    const files = walk(new URL('texgroups/', root))
    expect(files.length).toBeGreaterThanOrEqual(6)
    for (const f of files) {
      const r = parseGroupText(readFileSync(f, 'utf8'))
      expect(r.errors, f).toEqual([]); expect(r.warnings, f).toEqual([])
      for (const t of Object.values(r.group!.slots)) {
        const hit = ['.png', '.jpg', '.jpeg', '.tga'].some((e) => existsSync(new URL(`textures/${t}${e}`, root)))
        expect(hit, `${f}: texture ${t}`).toBe(true)
      }
    }
  })
})
