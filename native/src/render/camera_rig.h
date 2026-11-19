// render/camera_rig -- the cameras of one viewport plus the Shell's input semantics (what a drag, the wheel and the fly keys do).
//
// Owns: one fly, one orbit and three orthographic cameras, the viewport's ViewportSettings and the held fly keys. Switching the
// camera mode makes the new camera take over the old view (Shell: ViewportPanel.setCameraMode). Qt-free: the UI translates
// mouse / key events into these calls (ViewportWindow), tests call them directly.
// Spec: 08_MILESTONES.md T1.4, REQUIREMENTS §2 (RMB look, RMB+WASDQE, Shift x3, wheel while RMB changes the speed by 1.2).
#pragma once

#include <string>

#include "render/camera.h"
#include "render/view_state.h"
#include "render/viewport_settings.h"

namespace native::render {

enum class FlyKey { Forward, Back, Right, Left, Up, Down };

class CameraRig {
public:
    explicit CameraRig(const ViewportSettings& settings = defaultViewportSettings());

    const ViewportSettings& settings() const { return settings_; }
    // Replaces the settings (mode, speed...) without moving the cameras.
    void applySettings(const ViewportSettings& s);

    CameraMode mode() const { return settings_.cameraMode; }
    // The new camera takes over the view of the old one.
    void setMode(CameraMode mode);
    Camera& active();
    const Camera& active() const;
    FlyCamera& fly() { return fly_; }
    OrbitCamera& orbit() { return orbit_; }
    OrthoCamera& ortho(OrthoAxis a) { return ortho_[static_cast<int>(a)]; }

    // Right-button drag: look (fly), orbit (orbit) or pan (orthographic).
    void drag(double dxPx, double dyPx, double viewportHeightPx);
    // Middle-button drag: pan in every mode.
    void pan(double dxPx, double dyPx, double viewportHeightPx);
    // Wheel (Shell units: ~100 per notch, positive = scroll down). With the right button held in fly mode it changes the fly
    // speed by x1.2 / ÷1.2 per notch instead of dollying. Returns true when the speed changed.
    bool wheel(double deltaY, bool rightButtonHeld);
    void setFlySpeed(double metresPerSecond);

    // Held fly keys; Shift = x3. Keys only act while flying is allowed (right button held) - the caller decides, see clearFlyKeys.
    void setFlyKey(FlyKey key, bool down);
    void setBoost(bool on) { boost_ = on; }
    bool boosting() const { return boost_; }
    void clearFlyKeys();
    bool flying() const { return keys_ != 0; }
    // Moves the fly camera for dt seconds along the held keys; true when it moved.
    bool advance(double dt);

    void frame(Vec3 boxMin, Vec3 boxMax);

    // The matrices and grid of the active camera for a viewport of this size in pixels.
    ViewState viewState(double widthPx, double heightPx);

    // Poses (all five cameras) as JSON text, for layout persistence.
    std::string poseToJson() const;
    void poseFromJson(const std::string& text);

private:
    ViewportSettings settings_;
    FlyCamera fly_;
    OrbitCamera orbit_;
    OrthoCamera ortho_[3] = {OrthoCamera(OrthoAxis::Top), OrthoCamera(OrthoAxis::Front), OrthoCamera(OrthoAxis::Side)};
    unsigned keys_ = 0;
    bool boost_ = false;
};

// Spacing (metres) of the minor grid lines: gridSize doubled until lines are at least minPixels apart on screen.
double adaptiveGridSpacing(double gridSize, double metresPerPixel, double minPixels = 14);

} // namespace native::render
