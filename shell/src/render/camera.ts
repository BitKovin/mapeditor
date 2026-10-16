import { mat4, vec3 } from 'gl-matrix'
import type { Vec3 } from '../model/math'
import { cross, length, normalize, sub } from '../model/math'

export interface Ray { origin: Vec3; dir: Vec3 }

const clamp = (v: number, lo: number, hi: number): number => Math.max(lo, Math.min(hi, v))

/** Shared perspective-camera maths, Y-up, metric. Subclasses decide how the eye moves. */
export abstract class Camera {
  fovY = (50 * Math.PI) / 180
  near = 0.05
  far = 2000

  abstract get eye(): Vec3
  /** A point straight ahead of the camera (orbit: the pivot; fly: `focusDistance` in front of the eye). */
  abstract get target(): Vec3
  abstract pan(dxPx: number, dyPx: number, viewportHeightPx: number): void
  abstract zoom(wheelDelta: number): void
  abstract frame(min: Vec3, max: Vec3): void
  /** Take over another camera's view (used when the person switches camera mode). */
  abstract setFrom(other: Camera): void

  /** Unit vectors of the camera frame. */
  get basis(): { right: Vec3; up: Vec3; forward: Vec3 } {
    const forward = normalize(sub(this.target, this.eye))
    const right = normalize(cross(forward, [0, 1, 0]))
    return { right, up: cross(right, forward), forward }
  }

  distanceTo(p: Vec3): number { return length(sub(p, this.eye)) }
  /** Where a perspective camera taking over this view should sit (differs from `eye` only for orthographic cameras). */
  get lookEye(): Vec3 { return this.eye }

  viewProj(aspect: number): mat4 {
    const view = mat4.lookAt(mat4.create(), this.eye, this.target, [0, 1, 0])
    const proj = mat4.perspective(mat4.create(), this.fovY, aspect, this.near, this.far)
    return mat4.multiply(mat4.create(), proj, view)
  }

  /** World-space ray through a point given in NDC (-1..1, y up). */
  rayFromNdc(x: number, y: number, aspect: number): Ray {
    const inv = mat4.invert(mat4.create(), this.viewProj(aspect))!
    const p0 = vec3.transformMat4(vec3.create(), [x, y, -1], inv)
    const p1 = vec3.transformMat4(vec3.create(), [x, y, 1], inv)
    return { origin: [p0[0], p0[1], p0[2]], dir: normalize([p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]]) }
  }
}

/** Orbit camera: circles a target point. RMB orbits, MMB pans, wheel zooms. */
export class OrbitCamera extends Camera {
  target: Vec3 = [0, 1, 0]
  yaw = 0.8
  pitch = 0.5
  distance = 12

  get eye(): Vec3 {
    const cp = Math.cos(this.pitch)
    return [
      this.target[0] + this.distance * cp * Math.sin(this.yaw),
      this.target[1] + this.distance * Math.sin(this.pitch),
      this.target[2] + this.distance * cp * Math.cos(this.yaw),
    ]
  }

  orbit(dxPx: number, dyPx: number): void {
    this.yaw -= dxPx * 0.008
    this.pitch = clamp(this.pitch + dyPx * 0.008, -1.5, 1.5)
  }
  pan(dxPx: number, dyPx: number, viewportHeightPx: number): void {
    const k = (2 * this.distance * Math.tan(this.fovY / 2)) / Math.max(1, viewportHeightPx)
    const { right, up } = this.basis
    for (let i = 0; i < 3; i++) this.target[i] += (-right[i] * dxPx + up[i] * dyPx) * k
  }
  zoom(wheelDelta: number): void {
    this.distance = clamp(this.distance * Math.exp(wheelDelta * 0.0015), 0.2, 1000)
  }
  frame(min: Vec3, max: Vec3): void {
    this.target = [(min[0] + max[0]) / 2, (min[1] + max[1]) / 2, (min[2] + max[2]) / 2]
    const r = Math.hypot(max[0] - min[0], max[1] - min[1], max[2] - min[2]) / 2
    this.distance = Math.max(1, (r * 1.2) / Math.sin(this.fovY / 2))
  }
  setFrom(other: Camera): void {
    const eye = other.lookEye, t = other.target
    const d = Math.max(0.2, length(sub(eye, t)))
    this.target = [t[0], t[1], t[2]]
    this.distance = clamp(d, 0.2, 1000)
    this.pitch = clamp(Math.asin(clamp((eye[1] - t[1]) / d, -1, 1)), -1.5, 1.5)
    this.yaw = Math.atan2(eye[0] - t[0], eye[2] - t[2])
  }
}

/**
 * Free-fly camera (the default): the eye is a position, `yaw`/`pitch` say where it looks.
 * RMB-drag looks, RMB + W/A/S/D/Q/E flies (see ViewportPanel), MMB pans, the wheel dollies.
 * yaw 0 looks along +Z; increasing yaw turns left. Pitch > 0 looks up.
 */
export class FlyCamera extends Camera {
  position: Vec3 = [8, 6, 8]
  yaw = 0
  pitch = 0
  /** How far ahead `target` is; also scales panning. Follows the framed content. */
  focusDistance = 10
  /** Metres per second; the viewport keeps this equal to its fly-speed setting (wheel dolly uses it). */
  speed = 8

  constructor() {
    super()
    this.lookAt([8, 6, 8], [0, 1, 0])
  }

  get forward(): Vec3 {
    const cp = Math.cos(this.pitch)
    return [cp * Math.sin(this.yaw), Math.sin(this.pitch), cp * Math.cos(this.yaw)]
  }
  get eye(): Vec3 { return this.position }
  get target(): Vec3 {
    const f = this.forward
    return [this.position[0] + f[0] * this.focusDistance, this.position[1] + f[1] * this.focusDistance, this.position[2] + f[2] * this.focusDistance]
  }

  lookAt(from: Vec3, to: Vec3): void {
    const d = normalize(sub(to, from))
    this.position = [from[0], from[1], from[2]]
    this.pitch = clamp(Math.asin(d[1]), -MAX_PITCH, MAX_PITCH)
    // looking straight up/down leaves yaw undefined: keep the old one
    if (Math.hypot(d[0], d[2]) > 1e-6) this.yaw = Math.atan2(d[0], d[2])
    this.focusDistance = Math.max(0.5, length(sub(to, from)))
  }

  /** Mouse look. Dragging right turns right, dragging down looks down. */
  look(dxPx: number, dyPx: number): void {
    this.yaw -= dxPx * 0.003
    this.pitch = clamp(this.pitch - dyPx * 0.003, -MAX_PITCH, MAX_PITCH)
  }

  /** Fly by metres along the view direction, sideways, and straight up (world Y). */
  move(ahead: number, right: number, up: number): void {
    const f = this.forward
    const r = normalize(cross(f, [0, 1, 0]))
    for (let i = 0; i < 3; i++) this.position[i] += f[i] * ahead + r[i] * right
    this.position[1] += up
  }

  pan(dxPx: number, dyPx: number, viewportHeightPx: number): void {
    const k = (2 * this.focusDistance * Math.tan(this.fovY / 2)) / Math.max(1, viewportHeightPx)
    const { right, up } = this.basis
    for (let i = 0; i < 3; i++) this.position[i] += (-right[i] * dxPx + up[i] * dyPx) * k
  }
  /** Wheel dolly: scrolling up (negative delta) moves forward. One notch (~100) is ~0.2 s of flying. */
  zoom(wheelDelta: number): void { this.move(-wheelDelta * 0.002 * this.speed, 0, 0) }

  frame(min: Vec3, max: Vec3): void {
    const c: Vec3 = [(min[0] + max[0]) / 2, (min[1] + max[1]) / 2, (min[2] + max[2]) / 2]
    const r = Math.hypot(max[0] - min[0], max[1] - min[1], max[2] - min[2]) / 2
    const d = Math.max(1, (r * 1.2) / Math.sin(this.fovY / 2))
    const f = this.forward
    this.position = [c[0] - f[0] * d, c[1] - f[1] * d, c[2] - f[2] * d]
    this.focusDistance = d
  }

  setFrom(other: Camera): void { this.lookAt(other.lookEye, other.target) }
}

const MAX_PITCH = 1.55

export type OrthoAxis = 'top' | 'front' | 'side'
/** Looking direction and screen-up vector of each axis view (Y-up world). Top: +X right, +Z down. Front (from +Z): +X right. Side (from +X): -Z right. */
const ORTHO_VIEWS: Record<OrthoAxis, { forward: Vec3; up: Vec3 }> = {
  top: { forward: [0, -1, 0], up: [0, 0, -1] },
  front: { forward: [0, 0, -1], up: [0, 1, 0] },
  side: { forward: [-1, 0, 0], up: [0, 1, 0] },
}

/**
 * Orthographic axis-aligned camera (top / front / side) for 2D editing. `halfHeight` is half the visible height in metres;
 * panning moves `target` in the view plane, the wheel zooms. Handles and gizmos keep their screen size because
 * `distanceTo` reports the perspective distance that would give the same scale.
 */
export class OrthoCamera extends Camera {
  target: Vec3 = [0, 0, 0]
  halfHeight = 8
  aspect = 1.5
  private static readonly OFFSET = 2000
  constructor(readonly axis: OrthoAxis) { super(); this.near = 1; this.far = 2 * OrthoCamera.OFFSET }

  get forward(): Vec3 { return ORTHO_VIEWS[this.axis].forward }
  get eye(): Vec3 { const f = this.forward; return [this.target[0] - f[0] * OrthoCamera.OFFSET, this.target[1] - f[1] * OrthoCamera.OFFSET, this.target[2] - f[2] * OrthoCamera.OFFSET] }
  /** Perspective distance with the same on-screen scale as this view. */
  get equivDistance(): number { return this.halfHeight / Math.tan(this.fovY / 2) }
  get lookEye(): Vec3 { const f = this.forward, d = this.equivDistance; return [this.target[0] - f[0] * d, this.target[1] - f[1] * d, this.target[2] - f[2] * d] }
  distanceTo(_p: Vec3): number { return this.equivDistance }

  get basis(): { right: Vec3; up: Vec3; forward: Vec3 } {
    const { forward, up } = ORTHO_VIEWS[this.axis]
    return { right: normalize(cross(forward, up)), up, forward }
  }

  viewProj(aspect: number): mat4 {
    this.aspect = aspect
    const { up } = ORTHO_VIEWS[this.axis]
    const view = mat4.lookAt(mat4.create(), this.eye, this.target, up)
    const hh = this.halfHeight, hw = hh * aspect
    const proj = mat4.ortho(mat4.create(), -hw, hw, -hh, hh, this.near, this.far)
    return mat4.multiply(mat4.create(), proj, view)
  }

  pan(dxPx: number, dyPx: number, viewportHeightPx: number): void {
    const k = (2 * this.halfHeight) / Math.max(1, viewportHeightPx)
    const { right, up } = this.basis
    for (let i = 0; i < 3; i++) this.target[i] += (-right[i] * dxPx + up[i] * dyPx) * k
  }
  zoom(wheelDelta: number): void { this.halfHeight = clamp(this.halfHeight * Math.exp(wheelDelta * 0.0015), 0.05, 5000) }
  /** World-space size of one screen pixel. */
  metresPerPixel(viewportHeightPx: number): number { return (2 * this.halfHeight) / Math.max(1, viewportHeightPx) }

  frame(min: Vec3, max: Vec3): void {
    this.target = [(min[0] + max[0]) / 2, (min[1] + max[1]) / 2, (min[2] + max[2]) / 2]
    const { right, up } = this.basis
    const ext = (v: Vec3) => Math.abs(v[0]) * (max[0] - min[0]) + Math.abs(v[1]) * (max[1] - min[1]) + Math.abs(v[2]) * (max[2] - min[2])
    this.halfHeight = clamp(Math.max(ext(up) / 2, ext(right) / 2 / this.aspect, 0.5) * 1.2, 0.05, 5000)
  }
  setFrom(other: Camera): void {
    this.target = [other.target[0], other.target[1], other.target[2]]
    this.halfHeight = other instanceof OrthoCamera ? other.halfHeight : clamp(length(sub(other.eye, other.target)) * Math.tan(other.fovY / 2), 0.05, 5000)
  }
}
