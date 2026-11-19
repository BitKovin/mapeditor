// render/camera -- see camera.h. A line-by-line port of shell/src/render/camera.ts.
#include "render/camera.h"

namespace native::render {

namespace {

Vec3 boxCenter(Vec3 a, Vec3 b) { return {(a.x + b.x) / 2, (a.y + b.y) / 2, (a.z + b.z) / 2}; }

struct OrthoView {
    Vec3 forward, up;
};
OrthoView orthoView(OrthoAxis a) {
    switch (a) {
        case OrthoAxis::Top: return {{0, -1, 0}, {0, 0, -1}};
        case OrthoAxis::Front: return {{0, 0, -1}, {0, 1, 0}};
        case OrthoAxis::Side: return {{-1, 0, 0}, {0, 1, 0}};
    }
    return {{0, 0, -1}, {0, 1, 0}};
}

} // namespace

// ---- Camera ---------------------------------------------------------------------------------------------------------------

CameraBasis Camera::basis() const {
    const Vec3 fwd = normalize(target() - eye());
    const Vec3 right = normalize(cross(fwd, {0, 1, 0}));
    return {right, cross(right, fwd), fwd};
}

Mat4 Camera::viewProj(double aspect) {
    const Mat4 view = mat4LookAt(eye(), target(), {0, 1, 0});
    const Mat4 proj = mat4Perspective(fovY, aspect, nearPlane, farPlane);
    return mat4Multiply(proj, view);
}

Ray Camera::rayFromNdc(double x, double y, double aspect) {
    // The matrix is Vulkan clip space (y down); the Shell's NDC has y up.
    const Mat4 inv = mat4Invert(viewProj(aspect));
    const Vec3 p0 = mat4Transform(inv, {x, -y, 0});
    const Vec3 p1 = mat4Transform(inv, {x, -y, 1});
    return {p0, normalize(p1 - p0)};
}

// ---- OrbitCamera ----------------------------------------------------------------------------------------------------------

Vec3 OrbitCamera::eye() const {
    const double cp = std::cos(pitch);
    return {pivot.x + distance * cp * std::sin(yaw), pivot.y + distance * std::sin(pitch), pivot.z + distance * cp * std::cos(yaw)};
}

void OrbitCamera::orbit(double dxPx, double dyPx) {
    yaw -= dxPx * 0.008;
    pitch = clampd(pitch + dyPx * 0.008, -1.5, 1.5);
}

void OrbitCamera::pan(double dxPx, double dyPx, double viewportHeightPx) {
    const double k = (2 * distance * std::tan(fovY / 2)) / std::max(1.0, viewportHeightPx);
    const CameraBasis b = basis();
    pivot = pivot + (b.right * -dxPx + b.up * dyPx) * k;
}

void OrbitCamera::zoom(double wheelDelta) { distance = clampd(distance * std::exp(wheelDelta * 0.0015), 0.2, 1000); }

void OrbitCamera::frame(Vec3 boxMin, Vec3 boxMax) {
    pivot = boxCenter(boxMin, boxMax);
    const double r = length(boxMax - boxMin) / 2;
    distance = std::max(1.0, (r * 1.2) / std::sin(fovY / 2));
}

void OrbitCamera::setFrom(const Camera& other) {
    const Vec3 e = other.lookEye(), t = other.target();
    const double d = std::max(0.2, length(e - t));
    pivot = t;
    distance = clampd(d, 0.2, 1000);
    pitch = clampd(std::asin(clampd((e.y - t.y) / d, -1, 1)), -1.5, 1.5);
    yaw = std::atan2(e.x - t.x, e.z - t.z);
}

// ---- FlyCamera ------------------------------------------------------------------------------------------------------------

FlyCamera::FlyCamera() { lookAt({8, 6, 8}, {0, 1, 0}); }

Vec3 FlyCamera::forward() const {
    const double cp = std::cos(pitch);
    return {cp * std::sin(yaw), std::sin(pitch), cp * std::cos(yaw)};
}

void FlyCamera::lookAt(Vec3 from, Vec3 to) {
    const Vec3 d = normalize(to - from);
    position = from;
    pitch = clampd(std::asin(d.y), -kMaxPitch, kMaxPitch);
    // Looking straight up / down leaves yaw undefined: keep the old one.
    if (std::hypot(d.x, d.z) > 1e-6) yaw = std::atan2(d.x, d.z);
    focusDistance = std::max(0.5, length(to - from));
}

void FlyCamera::look(double dxPx, double dyPx) {
    yaw -= dxPx * 0.003;
    pitch = clampd(pitch - dyPx * 0.003, -kMaxPitch, kMaxPitch);
}

void FlyCamera::move(double ahead, double right, double up) {
    const Vec3 f = forward();
    const Vec3 r = normalize(cross(f, {0, 1, 0}));
    position = position + f * ahead + r * right;
    position.y += up;
}

void FlyCamera::pan(double dxPx, double dyPx, double viewportHeightPx) {
    const double k = (2 * focusDistance * std::tan(fovY / 2)) / std::max(1.0, viewportHeightPx);
    const CameraBasis b = basis();
    position = position + (b.right * -dxPx + b.up * dyPx) * k;
}

void FlyCamera::frame(Vec3 boxMin, Vec3 boxMax) {
    const Vec3 c = boxCenter(boxMin, boxMax);
    const double r = length(boxMax - boxMin) / 2;
    const double d = std::max(1.0, (r * 1.2) / std::sin(fovY / 2));
    position = c - forward() * d;
    focusDistance = d;
}

// ---- OrthoCamera ----------------------------------------------------------------------------------------------------------

OrthoCamera::OrthoCamera(OrthoAxis a) : axis(a) {
    nearPlane = 1;
    farPlane = 2 * kOffset;
}

Vec3 OrthoCamera::forward() const { return orthoView(axis).forward; }

CameraBasis OrthoCamera::basis() const {
    const OrthoView v = orthoView(axis);
    return {normalize(cross(v.forward, v.up)), v.up, v.forward};
}

Mat4 OrthoCamera::viewProj(double aspectRatio) {
    aspect = aspectRatio;
    const Mat4 view = mat4LookAt(eye(), center, orthoView(axis).up);
    const Mat4 proj = mat4Ortho(halfHeight * aspectRatio, halfHeight, nearPlane, farPlane);
    return mat4Multiply(proj, view);
}

void OrthoCamera::pan(double dxPx, double dyPx, double viewportHeightPx) {
    const double k = (2 * halfHeight) / std::max(1.0, viewportHeightPx);
    const CameraBasis b = basis();
    center = center + (b.right * -dxPx + b.up * dyPx) * k;
}

void OrthoCamera::frame(Vec3 boxMin, Vec3 boxMax) {
    center = boxCenter(boxMin, boxMax);
    const CameraBasis b = basis();
    const auto ext = [&](Vec3 v) {
        return std::fabs(v.x) * (boxMax.x - boxMin.x) + std::fabs(v.y) * (boxMax.y - boxMin.y) + std::fabs(v.z) * (boxMax.z - boxMin.z);
    };
    halfHeight = clampd(std::max({ext(b.up) / 2, ext(b.right) / 2 / aspect, 0.5}) * 1.2, 0.05, 5000);
}

void OrthoCamera::setFrom(const Camera& other) {
    center = other.target();
    if (const auto* o = dynamic_cast<const OrthoCamera*>(&other)) halfHeight = o->halfHeight;
    else halfHeight = clampd(length(other.eye() - other.target()) * std::tan(other.fovY / 2), 0.05, 5000);
}

} // namespace native::render
