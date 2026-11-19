// T1.4: camera maths (port of shell/src/render/camera.ts), viewport settings and the rig's input semantics. Pure maths, no GPU.
#include <doctest.h>

#include <cmath>

#include "render/camera.h"
#include "render/camera_rig.h"
#include "render/viewport_settings.h"

using namespace native::render;

namespace {
constexpr double kPi = 3.14159265358979323846;
bool near3(Vec3 a, Vec3 b, double tol = 1e-6) { return length(a - b) <= tol; }
// World point -> NDC (x right, y DOWN as Vulkan) and depth.
Vec3 project(Camera& c, Vec3 p, double aspect) { return mat4Transform(c.viewProj(aspect), p); }
} // namespace

TEST_CASE("math3d: lookAt, perspective and invert agree") {
    const Mat4 view = mat4LookAt({0, 0, 5}, {0, 0, 0}, {0, 1, 0});
    // The point the camera looks at is straight ahead, 5 m away: view-space (0, 0, -5).
    CHECK(near3(mat4Transform(view, {0, 0, 0}), {0, 0, -5}));
    const Mat4 vp = mat4Multiply(mat4Perspective(kPi / 2, 1.0, 1.0, 100.0), view);
    // 90 degree fov, aspect 1: a point at x = distance appears at the right edge. Vulkan: y down.
    CHECK(near3(mat4Transform(vp, {5, 0, 0}), {1, 0, mat4Transform(vp, {5, 0, 0}).z}, 1e-9));
    CHECK(mat4Transform(vp, {0, 5, 0}).y == doctest::Approx(-1.0));   // up in the world = negative NDC y
    // Depth 0..1: near plane -> 0, far plane -> 1.
    CHECK(mat4Transform(vp, {0, 0, 4}).z == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(mat4Transform(vp, {0, 0, -95}).z == doctest::Approx(1.0).epsilon(1e-9));
    const Mat4 inv = mat4Invert(vp);
    const Mat4 id = mat4Multiply(inv, vp);
    for (int i = 0; i < 16; ++i) CHECK(id[i] == doctest::Approx(i % 5 == 0 ? 1.0 : 0.0).epsilon(1e-9));
}

TEST_CASE("fly camera: default view, look, move, pan, zoom") {
    FlyCamera c;
    // Constructed looking from (8,6,8) at (0,1,0).
    CHECK(near3(c.position, {8, 6, 8}));
    CHECK(near3(c.target(), {0, 1, 0}, 1e-9));
    CHECK(c.focusDistance == doctest::Approx(length(Vec3{8, 5, 8})));

    FlyCamera f;
    f.position = {0, 0, 0};
    f.yaw = 0;
    f.pitch = 0;
    // yaw 0 looks along +Z; increasing yaw turns left (towards +X is right of +Z? yaw 0.5 looks to +X).
    CHECK(near3(f.forward(), {0, 0, 1}));
    f.yaw = kPi / 2;
    CHECK(near3(f.forward(), {1, 0, 0}, 1e-9));
    f.yaw = 0;
    // Dragging right turns right: yaw decreases (a camera looking +Z has -X on its... right-hand side in this convention).
    f.look(100, 0);
    CHECK(f.yaw == doctest::Approx(-0.3));
    f.look(0, 100);                       // dragging down looks down
    CHECK(f.pitch == doctest::Approx(-0.3));
    f.look(0, -100000);                   // pitch is clamped
    CHECK(f.pitch == doctest::Approx(FlyCamera::kMaxPitch));

    // move(): ahead along the view, right = forward x up, up = world Y.
    FlyCamera m;
    m.position = {0, 0, 0};
    m.yaw = 0;
    m.pitch = 0;
    m.move(2, 3, 5);
    CHECK(near3(m.position, {-3, 5, 2}, 1e-9));   // right of +Z is -X in this handedness (cross((0,0,1),(0,1,0)) = (-1,0,0))

    // zoom: wheel up (negative) moves forward by |delta| * 0.002 * speed.
    FlyCamera z;
    z.position = {0, 0, 0};
    z.yaw = 0;
    z.pitch = 0;
    z.speed = 10;
    z.zoom(-100);
    CHECK(near3(z.position, {0, 0, 2}, 1e-9));

    // pan: one viewport height of pixels moves by the view height at the focus distance.
    FlyCamera p;
    p.position = {0, 0, 0};
    p.yaw = 0;
    p.pitch = 0;
    p.focusDistance = 10;
    p.pan(0, 600, 600);
    CHECK(near3(p.position, {0, 2 * 10 * std::tan(p.fovY / 2), 0}, 1e-9));
}

TEST_CASE("fly camera: lookAt straight down keeps yaw and clamps pitch") {
    FlyCamera c;
    c.yaw = 1.0;
    c.lookAt({0, 10, 0}, {0, 0, 0});
    CHECK(c.pitch == doctest::Approx(-FlyCamera::kMaxPitch));
    CHECK(c.yaw == doctest::Approx(1.0));
    CHECK(c.focusDistance == doctest::Approx(10.0));
}

TEST_CASE("orbit camera: eye on the sphere, orbit, zoom, pan, frame, setFrom round trip") {
    OrbitCamera o;
    CHECK(length(o.eye() - o.pivot) == doctest::Approx(12.0));
    o.orbit(100, 0);
    CHECK(o.yaw == doctest::Approx(0.8 - 0.8));
    o.orbit(0, 100000);
    CHECK(o.pitch == doctest::Approx(1.5));
    o.zoom(1e6);
    CHECK(o.distance == doctest::Approx(1000.0));
    o.zoom(-1e6);
    CHECK(o.distance == doctest::Approx(0.2));

    OrbitCamera f;
    f.frame({-5, 0, -5}, {5, 10, 5});
    CHECK(near3(f.pivot, {0, 5, 0}));
    CHECK(f.distance == doctest::Approx((length(Vec3{10, 10, 10}) / 2 * 1.2) / std::sin(f.fovY / 2)));

    // The fly camera's view is reproduced by an orbit camera taking over.
    FlyCamera fly;
    fly.lookAt({3, 4, 5}, {-1, 0.5, 2});
    OrbitCamera t;
    t.setFrom(fly);
    CHECK(near3(t.eye(), fly.eye(), 1e-9));
    CHECK(near3(t.target(), fly.target(), 1e-9));
}

TEST_CASE("ortho camera: axis bases, projection, pan, zoom, frame") {
    OrthoCamera top(OrthoAxis::Top), front(OrthoAxis::Front), side(OrthoAxis::Side);
    // Top: +X right, +Z down on screen. Front (from +Z): +X right, +Y up. Side (from +X): -Z right.
    CHECK(near3(top.basis().right, {1, 0, 0}));
    CHECK(near3(front.basis().right, {1, 0, 0}));
    CHECK(near3(side.basis().right, {0, 0, -1}));
    CHECK(near3(top.basis().up, {0, 0, -1}));

    // Vulkan NDC: screen right = +x, screen down = +y. top.halfHeight 8, aspect 2.
    const Vec3 a = project(top, {4, 0, 0}, 2.0);     // 4 m right of the centre, hh*aspect = 16 -> 0.25
    CHECK(a.x == doctest::Approx(0.25));
    CHECK(a.y == doctest::Approx(0.0).epsilon(1e-9));
    const Vec3 b = project(top, {0, 0, 4}, 2.0);     // +Z is down on screen: 4 / 8 = 0.5 down
    CHECK(b.y == doctest::Approx(0.5));
    const Vec3 c = project(front, {0, 4, 0}, 2.0);   // +Y is up: negative NDC y
    CHECK(c.y == doctest::Approx(-0.5));
    const Vec3 s = project(side, {0, 0, -4}, 2.0);   // -Z is to the right in the side view
    CHECK(s.x == doctest::Approx(0.25));
    // Depth is inside 0..1 for geometry around the centre, whatever its height.
    const double zt = project(top, {0, 500, 0}, 1.0).z;
    CHECK(zt > 0.0);
    CHECK(zt < 1.0);

    // pan: dragging right moves the view centre left (the world follows the cursor); one viewport height = 2*halfHeight.
    OrthoCamera p(OrthoAxis::Top);
    p.halfHeight = 8;
    p.pan(100, 0, 800);
    CHECK(near3(p.center, {-2, 0, 0}, 1e-9));
    p.pan(0, 100, 800);                   // dragging down moves the centre against the screen-up direction (-Z): up means +... see basis
    CHECK(near3(p.center, {-2, 0, -2}, 1e-9));

    OrthoCamera z(OrthoAxis::Front);
    z.zoom(-1e6);
    CHECK(z.halfHeight == doctest::Approx(0.05));
    z.zoom(1e7);
    CHECK(z.halfHeight == doctest::Approx(5000.0));
    CHECK(z.metresPerPixel(1000) == doctest::Approx(10.0));

    OrthoCamera fr(OrthoAxis::Front);
    fr.aspect = 2;
    fr.frame({-10, 0, -1}, {10, 4, 1});
    CHECK(near3(fr.center, {0, 2, 0}));
    CHECK(fr.halfHeight == doctest::Approx(std::max(4.0 / 2, 20.0 / 2 / 2) * 1.2));   // width is the limiting extent
    // equivDistance gives perspective the same scale; distanceTo reports it for gizmo sizing.
    CHECK(fr.distanceTo({1, 2, 3}) == doctest::Approx(fr.halfHeight / std::tan(fr.fovY / 2)));
}

TEST_CASE("rayFromNdc goes through the picked point") {
    FlyCamera c;
    c.lookAt({0, 2, 10}, {0, 2, 0});
    // The centre of the screen looks straight at the target.
    Ray r = c.rayFromNdc(0, 0, 1.5);
    CHECK(near3(r.dir, {0, 0, -1}, 1e-9));
    // A point in the world projects to NDC; the ray through that NDC (y up) passes through the point.
    const Vec3 world{1.5, 3, 0};
    const Vec3 ndc = project(c, world, 1.5);
    r = c.rayFromNdc(ndc.x, -ndc.y, 1.5);
    const double t = dot(world - r.origin, r.dir);
    CHECK(near3(r.origin + r.dir * t, world, 1e-6));
    // The same holds for an orthographic camera.
    OrthoCamera o(OrthoAxis::Front);
    const Vec3 ow{2, 1, -3};
    const Vec3 ondc = project(o, ow, 1.5);
    r = o.rayFromNdc(ondc.x, -ondc.y, 1.5);
    CHECK(near3(r.dir, {0, 0, -1}, 1e-9));
    CHECK(near3(r.origin + r.dir * dot(ow - r.origin, r.dir), ow, 1e-6));
}

TEST_CASE("viewport settings: defaults match the Shell, JSON round trip, bad input is repaired") {
    const ViewportSettings d = defaultViewportSettings();
    CHECK(d.cameraMode == CameraMode::Fly);
    CHECK(d.flySpeed == 8);
    CHECK(d.gridSize == 0.5);
    CHECK(d.rotSnap == 15);
    CHECK(d.scaleSnap == 0.1);
    CHECK(d.renderMode == RenderMode::Unlit);
    CHECK(d.showGrid);
    CHECK(d.backfaceCulling);
    CHECK_FALSE(d.showOutlines);
    CHECK(snapStep(d) == 0.5);
    ViewportSettings off = d;
    off.snapEnabled = false;
    CHECK(snapStep(off) == 0);
    CHECK(rotSnapRadians(d) == doctest::Approx(15 * kPi / 180));
    CHECK(gridSizes().size() == 14);
    CHECK(rotSnaps().size() == 8);
    CHECK(scaleSnaps().size() == 6);

    ViewportSettings s = d;
    s.cameraMode = CameraMode::Front;
    s.renderMode = RenderMode::Lit;
    s.flySpeed = 33.5;
    s.showGrid = false;
    s.gridSize = 4;
    s.gizmo = GizmoMode::Rotate;
    s.showLightvol = true;
    CHECK(settingsFromJson(settingsToJson(s)) == s);

    CHECK(settingsFromJson("not json") == d);
    CHECK(settingsFromJson("{}") == d);
    ViewportSettings bad = settingsFromJson(R"({"cameraMode":"warp","flySpeed":1e9,"gridSize":-3,"showGrid":"yes"})");
    CHECK(bad.cameraMode == CameraMode::Fly);
    CHECK(bad.flySpeed == kFlySpeedMax);
    CHECK(bad.gridSize == 0.5);
    CHECK(bad.showGrid);
    CHECK(clampFlySpeed(0.0) == kFlySpeedMin);
    CHECK(clampFlySpeed(std::nan("")) == 8);
}

TEST_CASE("camera rig: drag, wheel, fly keys follow the Shell's rules") {
    CameraRig rig;
    rig.fly().position = {0, 0, 0};
    rig.fly().yaw = 0;
    rig.fly().pitch = 0;
    CHECK(rig.mode() == CameraMode::Fly);

    // RMB drag looks.
    rig.drag(100, 0, 600);
    CHECK(rig.fly().yaw == doctest::Approx(-0.3));
    rig.fly().yaw = 0;

    // Wheel without RMB dollies; with RMB it multiplies the speed by 1.2 per notch and does not move.
    rig.fly().speed = 10;
    CHECK_FALSE(rig.wheel(-100, false));
    CHECK(near3(rig.fly().position, {0, 0, 2}, 1e-9));
    CHECK(rig.wheel(-100, true));
    CHECK(rig.settings().flySpeed == doctest::Approx(9.6));
    CHECK(rig.fly().speed == doctest::Approx(9.6));
    CHECK(near3(rig.fly().position, {0, 0, 2}, 1e-9));
    rig.wheel(100, true);
    CHECK(rig.settings().flySpeed == doctest::Approx(8.0));
    for (int i = 0; i < 100; ++i) rig.wheel(100, true);
    CHECK(rig.settings().flySpeed == kFlySpeedMin);

    // Fly keys: W + D for half a second at 8 m/s; Shift = x3; E/Q move along world Y; opposite keys cancel.
    rig.setFlySpeed(8);
    rig.fly().position = {0, 0, 0};
    rig.setFlyKey(FlyKey::Forward, true);
    rig.setFlyKey(FlyKey::Right, true);
    CHECK(rig.flying());
    CHECK(rig.advance(0.5));
    CHECK(near3(rig.fly().position, {-4, 0, 4}, 1e-9));
    rig.setBoost(true);
    rig.fly().position = {0, 0, 0};
    rig.setFlyKey(FlyKey::Right, false);
    rig.advance(0.5);
    CHECK(near3(rig.fly().position, {0, 0, 12}, 1e-9));
    rig.setBoost(false);
    rig.setFlyKey(FlyKey::Back, true);                 // W and S cancel
    CHECK_FALSE(rig.advance(1.0));
    rig.setFlyKey(FlyKey::Forward, false);
    rig.setFlyKey(FlyKey::Back, false);
    rig.setFlyKey(FlyKey::Up, true);
    rig.fly().position = {0, 0, 0};
    rig.advance(1.0);
    CHECK(near3(rig.fly().position, {0, 8, 0}, 1e-9));
    rig.clearFlyKeys();
    CHECK_FALSE(rig.flying());
    CHECK_FALSE(rig.advance(1.0));
}

TEST_CASE("camera rig: switching the mode keeps the view; ortho drag pans; MMB pans everywhere") {
    CameraRig rig;
    rig.fly().lookAt({10, 5, 10}, {0, 0, 0});
    const Vec3 eyeBefore = rig.fly().eye();
    rig.setMode(CameraMode::Orbit);
    CHECK(near3(rig.orbit().eye(), eyeBefore, 1e-6));
    CHECK(near3(rig.orbit().target(), {0, 0, 0}, 1e-6));
    rig.setMode(CameraMode::Top);
    CHECK(near3(rig.ortho(OrthoAxis::Top).center, {0, 0, 0}, 1e-6));
    // Same scale as the perspective view of the orbit camera: halfHeight = distance * tan(fov / 2).
    CHECK(rig.ortho(OrthoAxis::Top).halfHeight ==
          doctest::Approx(length(eyeBefore) * std::tan(rig.orbit().fovY / 2)).epsilon(1e-6));
    // RMB drag pans in an orthographic view.
    const Vec3 c0 = rig.ortho(OrthoAxis::Top).center;
    rig.drag(50, 0, 600);
    CHECK(rig.ortho(OrthoAxis::Top).center.x < c0.x);
    // Back to fly: the camera takes the ortho view's equivalent perspective eye.
    rig.setMode(CameraMode::Fly);
    CHECK(rig.fly().eye().y > 0);
    // Pan with the middle button in fly mode moves the position, not the angles.
    const double yaw = rig.fly().yaw, pitch = rig.fly().pitch;
    rig.pan(30, 30, 600);
    CHECK(rig.fly().yaw == yaw);
    CHECK(rig.fly().pitch == pitch);
}

TEST_CASE("camera rig: view state, adaptive grid, pose JSON") {
    CameraRig rig;
    rig.setMode(CameraMode::Top);
    ViewState v = rig.viewState(800, 600);
    CHECK(v.gridPlane == 0);
    CHECK(v.showGrid);
    // 16 m view height over 600 px = 0.027 m/px: minor lines at least 14 px apart -> 0.5 doubled to 0.5 (0.5 m = 18.8 px).
    CHECK(v.gridSpacing == doctest::Approx(0.5));
    rig.ortho(OrthoAxis::Top).halfHeight = 100;      // 0.33 m/px -> needs >= 4.7 m: 0.5 -> 8
    v = rig.viewState(800, 600);
    CHECK(v.gridSpacing == doctest::Approx(8.0));
    rig.setMode(CameraMode::Front);
    CHECK(rig.viewState(800, 600).gridPlane == 1);
    rig.setMode(CameraMode::Side);
    CHECK(rig.viewState(800, 600).gridPlane == 2);
    CHECK(adaptiveGridSpacing(1, 0.0001) == 1);
    CHECK(adaptiveGridSpacing(-1, 0.0001) == 0.5);

    CameraRig a;
    a.fly().lookAt({1, 2, 3}, {4, 5, 6});
    a.orbit().yaw = 2.5;
    a.orbit().distance = 33;
    a.ortho(OrthoAxis::Front).center = {7, 8, 9};
    a.ortho(OrthoAxis::Front).halfHeight = 21;
    CameraRig b;
    b.poseFromJson(a.poseToJson());
    CHECK(near3(b.fly().position, a.fly().position, 1e-12));
    CHECK(b.fly().yaw == a.fly().yaw);
    CHECK(b.fly().pitch == a.fly().pitch);
    CHECK(b.fly().focusDistance == a.fly().focusDistance);
    CHECK(b.orbit().yaw == 2.5);
    CHECK(b.orbit().distance == 33);
    CHECK(near3(b.ortho(OrthoAxis::Front).center, {7, 8, 9}));
    CHECK(b.ortho(OrthoAxis::Front).halfHeight == 21);
    CameraRig c;
    c.poseFromJson("garbage");
    c.poseFromJson(R"({"fly":{"position":[1,"x",3],"pitch":99},"orbit":{"distance":-5}})");
    CHECK(near3(c.fly().position, CameraRig().fly().position));   // a malformed vector is ignored
    CHECK(c.fly().pitch == FlyCamera::kMaxPitch);                 // clamped
    CHECK(c.orbit().distance == 0.2);
}
