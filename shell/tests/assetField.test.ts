// @vitest-environment jsdom
import { describe, expect, it } from 'vitest'
import { fieldKind, propertyField, setAssetPicker } from '../src/ui/properties'
import type { EntityKeyDef } from '../src/net/protocol'
import type { Entity } from '../src/model/document'

const key = (type: string): EntityKeyDef => ({ key: 'model_path', type, name: 'Model Path', description: '' })
const ent = (kv: Record<string, string> = {}): Entity => ({ id: 'e', classname: 'x', keyvalues: kv })

describe('asset keys', () => {
  it('asset and the classic model type are edited with the file picker', () => {
    expect(fieldKind(key('asset'))).toBe('asset')
    expect(fieldKind(key('model'))).toBe('asset')
    expect(fieldKind(key('string'))).toBe('text')
  })
  it('the button opens the picker and stores the chosen path', async () => {
    const got: (string | null)[] = []
    setAssetPicker(async (_k, current) => (current === 'old.glb' ? 'models/new.glb' : null))
    const f = propertyField(key('asset'), ent({ model_path: 'old.glb' }), false, (v) => got.push(v))
    const btn = f.querySelector('button.browse') as HTMLButtonElement
    expect(btn.disabled).toBe(false)
    btn.click()
    await new Promise((r) => setTimeout(r, 0))
    expect(got).toEqual(['models/new.glb'])
    expect((f.querySelector('.asset input') as HTMLInputElement).value).toBe('models/new.glb')
    setAssetPicker(null)
  })
  it('typing a path still works, and a cancelled picker changes nothing', async () => {
    const got: (string | null)[] = []
    setAssetPicker(async () => null)
    const f = propertyField(key('asset'), ent(), false, (v) => got.push(v))
    const inp = f.querySelector('.asset input') as HTMLInputElement
    inp.value = 'a/b.obj'; inp.dispatchEvent(new Event('change'))
    ;(f.querySelector('button.browse') as HTMLButtonElement).click()
    await new Promise((r) => setTimeout(r, 0))
    expect(got).toEqual(['a/b.obj'])
    setAssetPicker(null)
  })
})
