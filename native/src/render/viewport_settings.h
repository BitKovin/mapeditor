// render/viewport_settings -- the per-viewport settings struct (port of shell/src/render/viewportSettings.ts).
//
// Owns: every viewport has its own settings (DESIGN.md §6.1 / §6.2). The render mode enum and the flags exist from the start so
// adding a mode later is a new enum case plus a shader path. Qt-free; saved as JSON.
// Spec: 07_EDITOR_PORT.md (viewport settings), 08_MILESTONES.md T1.4.
#pragma once

#include <string>
#include <vector>

namespace native::render {

enum class CameraMode { Fly, Orbit, Top, Front, Side };
enum class RenderMode { Unlit, Lit, Wireframe, LightingOnly };
enum class GizmoMode { Move, Rotate, Scale };

constexpr double kFlySpeedMin = 0.05, kFlySpeedMax = 500, kWheelSpeedFactor = 1.2;

inline bool isOrthoMode(CameraMode m) { return m == CameraMode::Top || m == CameraMode::Front || m == CameraMode::Side; }
// Non-finite values fall back to the default speed (8 m/s), as the Shell's clampSpeed.
double clampFlySpeed(double v);

// Grid sizes in metres, rotation snaps in degrees, scale snaps as factor steps (Shell: GRID_SIZES / ROT_SNAPS / SCALE_SNAPS).
const std::vector<double>& gridSizes();
const std::vector<double>& rotSnaps();
const std::vector<double>& scaleSnaps();

struct ViewportSettings {
    CameraMode cameraMode = CameraMode::Fly;
    double flySpeed = 8;
    bool showOutlines = false, showSubtractive = true, showVolumes = true, backfaceCulling = true;
    RenderMode renderMode = RenderMode::Unlit;
    bool showGrid = true, showOrigins = true, showDirections = true, showLinks = true, showLightGizmos = true;
    bool showLightvol = false, showVisibility = false, showLightmapDensity = false, showTexelHeat = false, showSizes = true;
    bool snapEnabled = true;
    double gridSize = 0.5, rotSnap = 15, scaleSnap = 0.1;
    GizmoMode gizmo = GizmoMode::Move;

    bool operator==(const ViewportSettings&) const = default;
};

inline ViewportSettings defaultViewportSettings() { return {}; }
// Position snap step to use, 0 = free movement.
inline double snapStep(const ViewportSettings& s) { return s.snapEnabled ? s.gridSize : 0; }
inline double rotSnapRadians(const ViewportSettings& s) { return s.snapEnabled ? s.rotSnap * 3.14159265358979323846 / 180.0 : 0; }
inline double scaleSnapStep(const ViewportSettings& s) { return s.snapEnabled ? s.scaleSnap : 0; }

const char* cameraModeName(CameraMode m);
const char* renderModeName(RenderMode m);

// JSON text; unknown / missing fields keep their defaults, bad values are clamped or ignored.
std::string settingsToJson(const ViewportSettings& s);
ViewportSettings settingsFromJson(const std::string& text);

} // namespace native::render
