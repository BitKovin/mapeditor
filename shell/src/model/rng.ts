// Seeded random numbers shared by the layout solvers: deterministic, no clock, no Math.random.

/** 32 bit FNV-1a with a final avalanche. */
export function hash32(s: string): number {
  let h = 0x811c9dc5
  for (let i = 0; i < s.length; i++) { h ^= s.charCodeAt(i); h = Math.imul(h, 0x01000193) }
  h ^= h >>> 16; h = Math.imul(h, 0x85ebca6b); h ^= h >>> 13; h = Math.imul(h, 0xc2b2ae35); h ^= h >>> 16
  return h >>> 0
}
/** A number in [0, 1) that depends only on the seed and the key. */
export const rand01 = (seed: number, key: string): number => hash32(`${seed >>> 0}|${key}`) / 4294967296
/** The seed a face gets when it has none of its own: stable across reloads, different per face. */
export const faceSeed = (brushId: string, faceId: number): number => hash32(`${brushId}/${faceId}`)
