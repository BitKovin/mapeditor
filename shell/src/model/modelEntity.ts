// Entities that show a model (keys model_path, model_scale, model_animation; REQUIREMENTS: model preview). Pure maths and a small registry through which the
// loaded models tell the box code (pointBox) how big they are, so the model replaces the class box for drawing and picking.
import type { Entity } from './document'
import { classOf, isBrushEntity, originOf } from './document'
import type { Box } from './entityBox'
import type { Vec3 } from './math'
import { rotationFromAngles } from './rotation'

/** The model file of an entity (relative to the game folder, forward slashes); '' when it has none or is a brush entity. */
export function modelPathOf(e: Entity): string {
  if (isBrushEntity(e) || classOf(e) === 'worldspawn') return ''
  return (e.keyvalues?.model_path ?? '').trim().replace(/\\/g, '/').replace(/^\/+/, '')
}
/** The animation to play: a name, or an index; '' = the rest pose. */
export const modelAnimationOf = (e: Entity): string => (e.keyvalues?.model_animation ?? '').trim()

const numbers = (s: string | undefined): number[] | null => {
  if (s === undefined) return null
  const v = s.trim().split(/\s+/).map(Number)
  return v.length > 0 && v.every(Number.isFinite) ? v : null
}
/** model_scale (one number or x y z) times modelscale and modelscale_vec, which the scale tool and prefab instances write (core obj_model.h modelScaleOf). */
export function modelScaleOf(e: Entity): Vec3 {
  const s: Vec3 = [1, 1, 1]
  for (const key of ['model_scale', 'modelscale', 'modelscale_vec']) {
    const v = numbers(e.keyvalues?.[key])
    if (v?.length === 3) { s[0] *= v[0]; s[1] *= v[1]; s[2] *= v[2] }
    else if (v?.length === 1 && v[0] !== 0) { s[0] *= v[0]; s[1] *= v[0]; s[2] *= v[0] }
  }
  return s
}

/** model -> world, column major (WebGL): translate(origin) * rotate(angles) * scale. */
export function modelMatrix(e: Entity): Float32Array {
  const r = rotationFromAngles(e.angles ?? [0, 0, 0]), s = modelScaleOf(e), o = originOf(e)
  return Float32Array.from([
    r[0] * s[0], r[3] * s[0], r[6] * s[0], 0,
    r[1] * s[1], r[4] * s[1], r[7] * s[1], 0,
    r[2] * s[2], r[5] * s[2], r[8] * s[2], 0,
    o[0], o[1], o[2], 1,
  ])
}

// ---- the registry -----------------------------------------------------------------------------------------------------------------------------
let boundsOf: (path: string) => Box | null = () => null
/** The model store installs itself here; `fn` gives the rest pose bounds (model space) of a loaded model, null while it is loading or unreadable. */
export const setModelBoundsProvider = (fn: (path: string) => Box | null): void => { boundsOf = fn }

/** The box of the entity's model relative to its origin (scaled, before the entity's rotation), or null when it shows no (loaded) model. */
export function entityModelBox(e: Entity): Box | null {
  const path = modelPathOf(e)
  if (!path) return null
  const b = boundsOf(path)
  if (!b) return null
  const s = modelScaleOf(e)
  const min: Vec3 = [0, 0, 0], max: Vec3 = [0, 0, 0]
  for (let i = 0; i < 3; i++) { const x = b.min[i] * s[i], y = b.max[i] * s[i]; min[i] = Math.min(x, y); max[i] = Math.max(x, y) }
  return { min, max }
}
