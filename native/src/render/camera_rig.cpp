// render/camera_rig -- see camera_rig.h.
#include "render/camera_rig.h"

#include <nlohmann/json.hpp>

namespace native::render {

namespace {

OrthoAxis orthoAxisOf(CameraMode m) { return m == CameraMode::Top ? OrthoAxis::Top : (m == CameraMode::Front ? OrthoAxis::Front : OrthoAxis::Side); }

nlohmann::json vecJson(Vec3 v) { return nlohmann::json::array({v.x, v.y, v.z}); }
bool readVec(const nlohmann::json& j, const char* key, Vec3& out) {
    if (!j.contains(key) || !j[key].is_array() || j[key].size() != 3) return false;
    Vec3 v;
    for (int i = 0; i < 3; ++i) {
        if (!j[key][i].is_number()) return false;
        v[i] = j[key][i].get<double>();
        if (!std::isfinite(v[i])) return false;
    }
    out = v;
    return true;
}
bool readNum(const nlohmann::json& j, const char* key, double& out) {
    if (!j.contains(key) || !j[key].is_number()) return false;
    const double v = j[key].get<double>();
    if (!std::isfinite(v)) return false;
    out = v;
    return true;
}

} // namespace

double adaptiveGridSpacing(double gridSize, double metresPerPixel, double minPixels) {
    double s = gridSize > 0 ? gridSize : 0.5;
    const double need = std::max(metresPerPixel, 1e-9) * minPixels;
    int guard = 0;
    while (s < need && guard++ < 64) s *= 2;
    return s;
}

CameraRig::CameraRig(const ViewportSettings& settings) : settings_(settings) {
    settings_.flySpeed = clampFlySpeed(settings_.flySpeed);
    fly_.speed = settings_.flySpeed;
}

void CameraRig::applySettings(const ViewportSettings& s) {
    ViewportSettings next = s;
    next.flySpeed = clampFlySpeed(next.flySpeed);
    const CameraMode wanted = next.cameraMode;
    next.cameraMode = settings_.cameraMode;
    settings_ = next;
    fly_.speed = settings_.flySpeed;
    setMode(wanted);
}

Camera& CameraRig::active() {
    switch (settings_.cameraMode) {
        case CameraMode::Fly: return fly_;
        case CameraMode::Orbit: return orbit_;
        default: return ortho_[static_cast<int>(orthoAxisOf(settings_.cameraMode))];
    }
}
const Camera& CameraRig::active() const { return const_cast<CameraRig*>(this)->active(); }

void CameraRig::setMode(CameraMode mode) {
    if (mode == settings_.cameraMode) return;
    const Camera& from = active();
    clearFlyKeys();
    // The old camera must stay unchanged while the new one reads it: take over from a copy of what matters.
    Camera* to = nullptr;
    settings_.cameraMode = mode;
    to = &active();
    to->setFrom(from);
}

void CameraRig::drag(double dxPx, double dyPx, double viewportHeightPx) {
    switch (settings_.cameraMode) {
        case CameraMode::Fly: fly_.look(dxPx, dyPx); break;
        case CameraMode::Orbit: orbit_.orbit(dxPx, dyPx); break;
        default: active().pan(dxPx, dyPx, viewportHeightPx); break;
    }
}

void CameraRig::pan(double dxPx, double dyPx, double viewportHeightPx) { active().pan(dxPx, dyPx, viewportHeightPx); }

bool CameraRig::wheel(double deltaY, bool rightButtonHeld) {
    if (settings_.cameraMode == CameraMode::Fly && rightButtonHeld) {
        setFlySpeed(settings_.flySpeed * (deltaY < 0 ? kWheelSpeedFactor : 1 / kWheelSpeedFactor));
        return true;
    }
    active().zoom(deltaY);
    return false;
}

void CameraRig::setFlySpeed(double metresPerSecond) {
    settings_.flySpeed = clampFlySpeed(metresPerSecond);
    fly_.speed = settings_.flySpeed;
}

void CameraRig::setFlyKey(FlyKey key, bool down) {
    const unsigned bit = 1u << static_cast<int>(key);
    if (down) keys_ |= bit;
    else keys_ &= ~bit;
}

void CameraRig::clearFlyKeys() { keys_ = 0; }

bool CameraRig::advance(double dt) {
    if (keys_ == 0 || settings_.cameraMode != CameraMode::Fly) return false;
    const auto held = [&](FlyKey k) { return (keys_ >> static_cast<int>(k)) & 1u ? 1.0 : 0.0; };
    const double a = held(FlyKey::Forward) - held(FlyKey::Back);
    const double r = held(FlyKey::Right) - held(FlyKey::Left);
    const double u = held(FlyKey::Up) - held(FlyKey::Down);
    if (a == 0 && r == 0 && u == 0) return false;
    const double dist = settings_.flySpeed * (boost_ ? 3 : 1) * dt;
    fly_.move(a * dist, r * dist, u * dist);
    return true;
}

void CameraRig::frame(Vec3 boxMin, Vec3 boxMax) { active().frame(boxMin, boxMax); }

ViewState CameraRig::viewState(double widthPx, double heightPx) {
    ViewState s;
    const double aspect = widthPx / std::max(1.0, heightPx);
    Camera& cam = active();
    s.viewProj = cam.viewProj(aspect);
    s.showGrid = settings_.showGrid;
    s.showAxes = settings_.showOrigins;
    const Vec3 eyePos = cam.eye();
    s.eye[0] = eyePos.x;
    s.eye[1] = eyePos.y;
    s.eye[2] = eyePos.z;
    s.perspective = !isOrthoMode(settings_.cameraMode);
    s.renderMode = settings_.renderMode == RenderMode::Lit ? 1 : settings_.renderMode == RenderMode::LightingOnly ? 2 : 0;
    const Vec3 t = cam.target();
    double mpp;
    switch (settings_.cameraMode) {
        case CameraMode::Top: s.gridPlane = 0; s.gridCenterA = t.x; s.gridCenterB = t.z; break;
        case CameraMode::Front: s.gridPlane = 1; s.gridCenterA = t.x; s.gridCenterB = t.y; break;
        case CameraMode::Side: s.gridPlane = 2; s.gridCenterA = t.z; s.gridCenterB = t.y; break;
        default: s.gridPlane = 0; s.gridCenterA = cam.eye().x; s.gridCenterB = cam.eye().z; break;
    }
    if (isOrthoMode(settings_.cameraMode)) {
        mpp = static_cast<OrthoCamera&>(cam).metresPerPixel(heightPx);
    } else {
        // Scale of the view at the point where the camera looks at the ground (or the focus distance when looking away).
        const Vec3 e = cam.eye();
        const Vec3 f = cam.basis().forward;
        const double d = f.y < -1e-3 ? clampd(-e.y / f.y, 1.0, 5000.0) : cam.distanceTo(t);
        mpp = (2 * d * std::tan(cam.fovY / 2)) / std::max(1.0, heightPx);
    }
    // Orthographic views (shell/src/render/orthoGrid.ts): lines at least 8 px apart, and far enough apart that the grid's 100 steps each side of the centre
    // cover the whole window.
    const double minPx = s.perspective ? 14.0 : std::max(8.0, 0.5 * std::max(widthPx, heightPx) / 98.0);
    s.gridSpacing = adaptiveGridSpacing(settings_.gridSize, mpp, minPx);
    return s;
}

std::string CameraRig::poseToJson() const {
    nlohmann::json j;
    j["fly"] = {{"position", vecJson(fly_.position)}, {"yaw", fly_.yaw}, {"pitch", fly_.pitch}, {"focusDistance", fly_.focusDistance}};
    j["orbit"] = {{"pivot", vecJson(orbit_.pivot)}, {"yaw", orbit_.yaw}, {"pitch", orbit_.pitch}, {"distance", orbit_.distance}};
    nlohmann::json o = nlohmann::json::object();
    const char* names[3] = {"top", "front", "side"};
    for (int i = 0; i < 3; ++i) o[names[i]] = {{"center", vecJson(ortho_[i].center)}, {"halfHeight", ortho_[i].halfHeight}};
    j["ortho"] = o;
    return j.dump();
}

void CameraRig::poseFromJson(const std::string& text) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) return;
    if (j.contains("fly") && j["fly"].is_object()) {
        const auto& f = j["fly"];
        readVec(f, "position", fly_.position);
        readNum(f, "yaw", fly_.yaw);
        if (readNum(f, "pitch", fly_.pitch)) fly_.pitch = clampd(fly_.pitch, -FlyCamera::kMaxPitch, FlyCamera::kMaxPitch);
        if (readNum(f, "focusDistance", fly_.focusDistance)) fly_.focusDistance = std::max(0.5, fly_.focusDistance);
    }
    if (j.contains("orbit") && j["orbit"].is_object()) {
        const auto& o = j["orbit"];
        readVec(o, "pivot", orbit_.pivot);
        readNum(o, "yaw", orbit_.yaw);
        if (readNum(o, "pitch", orbit_.pitch)) orbit_.pitch = clampd(orbit_.pitch, -1.5, 1.5);
        if (readNum(o, "distance", orbit_.distance)) orbit_.distance = clampd(orbit_.distance, 0.2, 1000);
    }
    if (j.contains("ortho") && j["ortho"].is_object()) {
        const char* names[3] = {"top", "front", "side"};
        for (int i = 0; i < 3; ++i) {
            if (!j["ortho"].contains(names[i]) || !j["ortho"][names[i]].is_object()) continue;
            const auto& c = j["ortho"][names[i]];
            readVec(c, "center", ortho_[i].center);
            if (readNum(c, "halfHeight", ortho_[i].halfHeight)) ortho_[i].halfHeight = clampd(ortho_[i].halfHeight, 0.05, 5000);
        }
    }
}

} // namespace native::render
