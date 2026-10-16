// Where a dropped prefab or entity lands. Pure, so the live preview and the real drop cannot disagree.
import type { Box } from './entityBox'
import { originOnSurface } from './entityBox'
import { Vec3, snap } from './math'

export const snapVec = (p: Vec3, step: number): Vec3 => [snap(p[0], step), snap(p[1], step), snap(p[2], step)]

/** A prefab instance goes to the grid-snapped point under the cursor. */
export const prefabOrigin = (point: Vec3, step: number): Vec3 => snapVec(point, step)

/** A point entity goes to the snapped point under the cursor and is then pushed out along the surface normal so its box rests on the surface. */
export const entityOrigin = (point: Vec3, normal: Vec3, box: Box, step: number): Vec3 => originOnSurface(snapVec(point, step), normal, box)
