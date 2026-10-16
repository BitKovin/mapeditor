import { describe, expect, it } from 'vitest'
import { instanceNameProblem, instanceNames, newInstanceName, prefabBaseName, PrefabInstance } from '../src/model/document'

const inst = (id: string, file: string, name?: string): PrefabInstance => ({ id, file, ...(name ? { name } : {}) })

describe('prefab instance names (N1 a)', () => {
  it('base name strips the folder and .map.json / .json', () => {
    expect(prefabBaseName('prefabs/sub/door_prefab.map.json')).toBe('door_prefab')
    expect(prefabBaseName('a\\b\\x.json')).toBe('x')
    expect(prefabBaseName('plain')).toBe('plain')
  })
  // Same case as core/tests/test_document.cpp "effectiveInstanceNames" -- both sides must agree.
  it('derives unique default names in order, skipping explicit names (matches Core)', () => {
    const list = [
      inst('a', 'prefabs/door_prefab.map.json'), inst('b', 'prefabs/sub/door_prefab.map.json'),
      inst('c', 'prefabs/door_prefab.map.json'), inst('n', 'prefabs/hinge.map.json', 'door_prefab3'),
    ]
    expect(instanceNames(list)).toEqual(['door_prefab', 'door_prefab2', 'door_prefab4', 'door_prefab3'])
  })
  it('newInstanceName picks the first free generated name', () => {
    expect(newInstanceName([], 'p/door.map.json')).toBe('door')
    expect(newInstanceName([inst('a', 'p/door.map.json')], 'p/door.map.json')).toBe('door2')
    expect(newInstanceName([inst('a', 'p/x.map.json', 'door'), inst('b', 'p/x.map.json', 'door2')], 'p/door.map.json')).toBe('door3')
  })
  it('a rename may keep its own name but never take a sibling\'s', () => {
    const list = [inst('a', 'p/door.map.json'), inst('b', 'p/door.map.json')]
    expect(instanceNameProblem(list, 'a', 'door')).toBeNull()
    expect(instanceNameProblem(list, 'a', 'door2')).toMatch(/already called/)
    expect(instanceNameProblem(list, 'a', 'gate')).toBeNull()
    expect(instanceNameProblem(list, 'a', '')).toMatch(/empty/)
    expect(instanceNameProblem(list, 'a', 'x|y')).toMatch(/may not contain/)
  })
})
