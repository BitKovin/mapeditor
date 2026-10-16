import { describe, expect, it } from 'vitest'
import { FlyCamera, OrbitCamera } from '../src/render/camera'
import { FLY_SPEED_MAX, FLY_SPEED_MIN, WHEEL_SPEED_FACTOR, clampSpeed, defaultViewportSettings } from '../src/render/viewportSettings'
import { pick, rayTriangle } from '../src/render/picking'
import { axisDistance, dragDelta, hitAxis } from '../src/render/gizmo'
import { makeBox } from '../src/model/geometry'
import type { Entity } from '../src/model/document'

describe('camera', () => {
  it('the centre of the screen looks at the target', () => {
    const c = new OrbitCamera()
    const r = c.rayFromNdc(0, 0, 1.5)
    // closest approach of the ray to the target must be ~0
    const t = (c.target[0] - r.origin[0]) * r.dir[0] + (c.target[1] - r.origin[1]) * r.dir[1] + (c.target[2] - r.origin[2]) * r.dir[2]
    const p = [0, 1, 2].map((i) => r.origin[i] + r.dir[i] * t)
    expect(Math.hypot(p[0] - c.target[0], p[1] - c.target[1], p[2] - c.target[2])).toBeLessThan(1e-3)
  })
  it('is Y-up: looking straight ahead from pitch 0 keeps the horizon level', () => {
    const c = new OrbitCamera(); c.pitch = 0
    expect(c.basis.up[1]).toBeCloseTo(1); expect(c.eye[1]).toBeCloseTo(c.target[1])
  })
  it('panning moves the target in the camera plane; zoom is clamped', () => {
    const c = new OrbitCamera(); const before = [...c.target]
    c.pan(100, 0, 800)
    const moved = Math.hypot(c.target[0] - before[0], c.target[1] - before[1], c.target[2] - before[2])
    expect(moved).toBeGreaterThan(0)
    expect(c.target[1]).toBeCloseTo(before[1]) // horizontal pan stays level
    for (let i = 0; i < 200; i++) c.zoom(-500)
    expect(c.distance).toBeGreaterThanOrEqual(0.2)
  })
})

describe('fly camera', () => {
  const near = (a: number[], b: number[], eps = 1e-6) => a.forEach((v, i) => expect(Math.abs(v - b[i])).toBeLessThan(eps))
  it('is the default camera mode, with a sensible speed', () => {
    const s = defaultViewportSettings()
    expect(s.cameraMode).toBe('fly')
    expect(s.flySpeed).toBeGreaterThanOrEqual(FLY_SPEED_MIN)
    expect(s.flySpeed).toBeLessThanOrEqual(FLY_SPEED_MAX)
    expect(s.showOutlines).toBe(false)            // outlines are optional and OFF by default
    expect(s.backfaceCulling).toBe(true)          // backface culling is ON by default
    expect(s.showSubtractive).toBe(true)
    expect(WHEEL_SPEED_FACTOR).toBe(1.2)
    expect(clampSpeed(1e9)).toBe(FLY_SPEED_MAX)
    expect(clampSpeed(-3)).toBe(FLY_SPEED_MIN)
    expect(clampSpeed(NaN)).toBe(8)
  })
  it('lookAt aims at the target; the screen centre ray passes through it', () => {
    const c = new FlyCamera(); c.lookAt([3, 2, -4], [10, 1, 5])
    const r = c.rayFromNdc(0, 0, 1.6)
    const t = [10, 1, 5], o = r.origin
    const k = (t[0] - o[0]) * r.dir[0] + (t[1] - o[1]) * r.dir[1] + (t[2] - o[2]) * r.dir[2]
    near([0, 1, 2].map((i) => o[i] + r.dir[i] * k), t, 1e-3)
    near(c.eye, [3, 2, -4])
  })
  it('mouse look: right turns right, down looks down, pitch is clamped', () => {
    const c = new FlyCamera(); c.yaw = 0; c.pitch = 0
    const rightBefore = c.basis.right
    c.look(100, 0)
    const f = c.forward
    expect(f[0] * rightBefore[0] + f[1] * rightBefore[1] + f[2] * rightBefore[2]).toBeGreaterThan(0.2) // forward swung toward old 'right'
    c.look(0, 100000); expect(c.forward[1]).toBeLessThan(-0.99)
    c.look(0, -1000000); expect(c.forward[1]).toBeGreaterThan(0.99)
    expect(Math.abs(c.pitch)).toBeLessThan(Math.PI / 2)
  })
  it('W/S move along the view, A/D sideways, E/Q straight up/down in world space', () => {
    const c = new FlyCamera(); c.position = [0, 0, 0]; c.yaw = 0; c.pitch = 0.5
    const f = c.forward
    c.move(2, 0, 0); near(c.position, [f[0] * 2, f[1] * 2, f[2] * 2])
    c.position = [0, 0, 0]; c.move(0, 1, 0)
    near(c.position, c.basis.right); expect(c.position[1]).toBeCloseTo(0)     // strafing stays level even when looking up
    c.position = [0, 0, 0]; c.move(0, 0, 3); near(c.position, [0, 3, 0])
  })
  it('switching cameras keeps the same view in both directions', () => {
    const o = new OrbitCamera(); o.yaw = 2.1; o.pitch = -0.4; o.distance = 20; o.target = [4, 1, -3]
    const f = new FlyCamera(); f.setFrom(o)
    near(f.eye, o.eye, 1e-5); near(f.target, o.target, 1e-5)
    const o2 = new OrbitCamera(); o2.setFrom(f)
    near(o2.eye, o.eye, 1e-5); near(o2.target, o.target, 1e-5)
    f.move(5, 2, 1); f.look(30, 12) // fly around, then go back to orbit around what we are looking at
    const o3 = new OrbitCamera(); o3.setFrom(f)
    near(o3.eye, f.eye, 1e-5); near(o3.target, f.target, 1e-5)
  })
  it('frame() keeps the look direction and lands the content in front of the camera', () => {
    const c = new FlyCamera(); c.yaw = 1; c.pitch = -0.3
    const dir = c.forward
    c.frame([-5, 0, -5], [5, 4, 5])
    near(c.forward, dir)
    near(c.target, [0, 2, 0], 1e-5)
    expect(c.distanceTo([0, 2, 0])).toBeGreaterThan(5)
  })
  it('wheel dolly moves forward on scroll-up and scales with speed; pan moves in the view plane', () => {
    const c = new FlyCamera(); c.position = [0, 0, 0]; c.yaw = 0; c.pitch = 0; c.speed = 10
    c.zoom(-100); expect(c.position[2]).toBeGreaterThan(1)
    c.position = [0, 0, 0]; c.pan(50, 0, 800)
    expect(c.position[1]).toBeCloseTo(0); expect(Math.abs(c.position[0])).toBeGreaterThan(0)
  })
})

describe('picking', () => {
  const world: Entity = { id: 'w', classname: 'worldspawn', brushes: [makeBox('near', [-1, -1, 4], [1, 1, 5]), makeBox('far', [-1, -1, 8], [1, 1, 9])] }
  const point: Entity = { id: 'p', classname: 'light', origin: [5, 0, 0] }
  it('hits the nearest brush along the ray', () => {
    const h = pick({ origin: [0, 0, 0], dir: [0, 0, 1] }, [world, point])
    expect(h).toMatchObject({ entityId: 'w', brushId: 'near' })
    expect(h!.t).toBeCloseTo(4)
  })
  it('hits point entities through their marker box, and misses empty space', () => {
    expect(pick({ origin: [5, 0, -3], dir: [0, 0, 1] }, [world, point])).toMatchObject({ entityId: 'p' })
    expect(pick({ origin: [20, 0, 0], dir: [0, 0, 1] }, [world, point])).toBeNull()
  })
  it('rayTriangle is two-sided', () => {
    const t: [number, number, number][] = [[0, 0, 1], [1, 0, 1], [0, 1, 1]]
    expect(rayTriangle({ origin: [0.2, 0.2, 0], dir: [0, 0, 1] }, ...(t as [any, any, any]))).toBeCloseTo(1)
    expect(rayTriangle({ origin: [0.2, 0.2, 2], dir: [0, 0, -1] }, ...(t as [any, any, any]))).toBeCloseTo(1)
  })
})

describe('translate gizmo', () => {
  const pivot: [number, number, number] = [0, 0, 0]
  it('finds the axis under the ray, and none elsewhere', () => {
    const rayAtX = { origin: [1, 5, 0] as [number, number, number], dir: [0, -1, 0] as [number, number, number] }
    expect(hitAxis(rayAtX, pivot, 2, 0.1)).toBe(0)
    expect(hitAxis({ origin: [1, 5, 3], dir: [0, -1, 0] }, pivot, 2, 0.1)).toBeNull()
    expect(hitAxis({ origin: [3, 5, 0], dir: [0, -1, 0] }, pivot, 2, 0.1)).toBeNull() // beyond the handle
  })
  it('drag distance along an axis is independent of how the ray approaches', () => {
    const down = { origin: [3, 5, 0] as [number, number, number], dir: [0, -1, 0] as [number, number, number] }
    expect(axisDistance(down, pivot, 0)).toBeCloseTo(3)
    const start = axisDistance({ origin: [1, 5, 0], dir: [0, -1, 0] }, pivot, 0)!
    expect(dragDelta(down, pivot, 0, start, 0)).toEqual([2, 0, 0])
    expect(dragDelta(down, pivot, 0, start, 0.25)[0]).toBeCloseTo(2)
    expect(dragDelta({ origin: [3.13, 5, 0], dir: [0, -1, 0] }, pivot, 0, start, 0.5)[0]).toBeCloseTo(2)
  })
})
