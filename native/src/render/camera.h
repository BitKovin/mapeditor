// render/camera -- fly, orbit and orthographic cameras (port of shell/src/render/camera.ts, same maths and constants).
//
// Owns: camera state and the maths that moves it (look, move, pan, zoom, frame, take over another camera's view) plus the
// view-projection matrix in Vulkan clip space. Qt-free, GPU-free, no input handling (see camera_rig). Y-up, metres.
// Spec: 08_MILESTONES.md T1.4, shell/src/render/camera.ts.
#pragma once

#include "render/math3d.h"

namespace native::render {

struct Ray {
    Vec3 origin, dir;
};

struct CameraBasis {
    Vec3 right, up, forward;
};

// Shared perspective-camera maths; subclasses decide how the eye moves.
class Camera {
public:
    virtual ~Camera() = default;

    double fovY = 50.0 * 3.14159265358979323846 / 180.0;
    double nearPlane = 0.05;
    double farPlane = 2000;

    virtual Vec3 eye() const = 0;
    // A point straight ahead of the camera (orbit: the pivot; fly: focusDistance in front of the eye).
    virtual Vec3 target() const = 0;
    virtual void pan(double dxPx, double dyPx, double viewportHeightPx) = 0;
    virtual void zoom(double wheelDelta) = 0;
    virtual void frame(Vec3 boxMin, Vec3 boxMax) = 0;
    // Take over another camera's view (used when the person switches camera mode).
    virtual void setFrom(const Camera& other) = 0;

    virtual CameraBasis basis() const;
    virtual double distanceTo(Vec3 p) const { return length(p - eye()); }
    // Where a perspective camera taking over this view should sit (differs from eye() only for orthographic cameras).
    virtual Vec3 lookEye() const { return eye(); }
    virtual Mat4 viewProj(double aspect);
    // World-space ray through a point in NDC (-1..1, y up as in the Shell; the matrix itself is y-down Vulkan clip space).
    Ray rayFromNdc(double x, double y, double aspect);
};

// Orbit camera: circles a pivot. RMB orbits, MMB pans, the wheel zooms.
class OrbitCamera : public Camera {
public:
    Vec3 pivot{0, 1, 0};
    double yaw = 0.8, pitch = 0.5, distance = 12;

    Vec3 eye() const override;
    Vec3 target() const override { return pivot; }
    void orbit(double dxPx, double dyPx);
    void pan(double dxPx, double dyPx, double viewportHeightPx) override;
    void zoom(double wheelDelta) override;
    void frame(Vec3 boxMin, Vec3 boxMax) override;
    void setFrom(const Camera& other) override;
};

// Free-fly camera (the default): the eye is a position, yaw / pitch say where it looks. RMB-drag looks, RMB + W/A/S/D/Q/E
// flies, MMB pans, the wheel dollies. yaw 0 looks along +Z; increasing yaw turns left. Pitch > 0 looks up.
class FlyCamera : public Camera {
public:
    static constexpr double kMaxPitch = 1.55;

    Vec3 position{8, 6, 8};
    double yaw = 0, pitch = 0;
    double focusDistance = 10;   // how far ahead target() is; also scales panning
    double speed = 8;            // metres per second; the wheel dolly uses it

    FlyCamera();
    Vec3 forward() const;
    Vec3 eye() const override { return position; }
    Vec3 target() const override { return position + forward() * focusDistance; }
    void lookAt(Vec3 from, Vec3 to);
    // Mouse look: dragging right turns right, dragging down looks down.
    void look(double dxPx, double dyPx);
    // Fly by metres along the view direction, sideways, and straight up (world Y).
    void move(double ahead, double right, double up);
    void pan(double dxPx, double dyPx, double viewportHeightPx) override;
    // Wheel dolly: scrolling up (negative delta) moves forward.
    void zoom(double wheelDelta) override { move(-wheelDelta * 0.002 * speed, 0, 0); }
    void frame(Vec3 boxMin, Vec3 boxMax) override;
    void setFrom(const Camera& other) override { lookAt(other.lookEye(), other.target()); }
};

enum class OrthoAxis { Top, Front, Side };

// Orthographic axis-aligned camera (top / front / side) for 2D editing. halfHeight is half the visible height in metres.
// Top: +X right, +Z down. Front (from +Z): +X right. Side (from +X): -Z right.
class OrthoCamera : public Camera {
public:
    static constexpr double kOffset = 2000;

    explicit OrthoCamera(OrthoAxis axis);
    OrthoAxis axis;
    Vec3 center{0, 0, 0};
    double halfHeight = 8;
    double aspect = 1.5;

    Vec3 forward() const;
    Vec3 eye() const override { return center - forward() * kOffset; }
    Vec3 target() const override { return center; }
    // Perspective distance with the same on-screen scale as this view.
    double equivDistance() const { return halfHeight / std::tan(fovY / 2); }
    Vec3 lookEye() const override { return center - forward() * equivDistance(); }
    double distanceTo(Vec3) const override { return equivDistance(); }
    CameraBasis basis() const override;
    Mat4 viewProj(double aspectRatio) override;
    void pan(double dxPx, double dyPx, double viewportHeightPx) override;
    void zoom(double wheelDelta) override { halfHeight = clampd(halfHeight * std::exp(wheelDelta * 0.0015), 0.05, 5000); }
    // World-space size of one screen pixel.
    double metresPerPixel(double viewportHeightPx) const { return 2 * halfHeight / std::max(1.0, viewportHeightPx); }
    void frame(Vec3 boxMin, Vec3 boxMax) override;
    void setFrom(const Camera& other) override;
};

} // namespace native::render
