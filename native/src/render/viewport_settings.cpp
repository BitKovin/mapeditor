// render/viewport_settings -- see viewport_settings.h.
#include "render/viewport_settings.h"

#include <nlohmann/json.hpp>

#include <cmath>

namespace native::render {

namespace {

const char* const kCameraNames[] = {"fly", "orbit", "top", "front", "side"};
const char* const kRenderNames[] = {"unlit", "lit", "wireframe", "lightingOnly"};
const char* const kGizmoNames[] = {"move", "rotate", "scale"};

template <size_t N>
int indexOf(const char* const (&names)[N], const std::string& s, int fallback) {
    for (size_t i = 0; i < N; ++i)
        if (s == names[i]) return static_cast<int>(i);
    return fallback;
}

void readBool(const nlohmann::json& j, const char* key, bool& out) {
    if (j.contains(key) && j[key].is_boolean()) out = j[key].get<bool>();
}
void readNum(const nlohmann::json& j, const char* key, double& out) {
    if (j.contains(key) && j[key].is_number()) {
        const double v = j[key].get<double>();
        if (std::isfinite(v)) out = v;
    }
}

} // namespace

double clampFlySpeed(double v) { return std::min(kFlySpeedMax, std::max(kFlySpeedMin, std::isfinite(v) ? v : 8.0)); }

const std::vector<double>& gridSizes() {
    static const std::vector<double> v = {0.1, 0.25, 0.5, 1, 2, 4, 5, 10, 15, 20, 50, 100, 250, 500};
    return v;
}
const std::vector<double>& rotSnaps() {
    static const std::vector<double> v = {1, 5, 10, 15, 22.5, 30, 45, 90};
    return v;
}
const std::vector<double>& scaleSnaps() {
    static const std::vector<double> v = {0.01, 0.05, 0.1, 0.25, 0.5, 1};
    return v;
}

const char* cameraModeName(CameraMode m) { return kCameraNames[static_cast<int>(m)]; }
const char* renderModeName(RenderMode m) { return kRenderNames[static_cast<int>(m)]; }

std::string settingsToJson(const ViewportSettings& s) {
    nlohmann::json j;
    j["cameraMode"] = cameraModeName(s.cameraMode);
    j["flySpeed"] = s.flySpeed;
    j["showOutlines"] = s.showOutlines;
    j["showSubtractive"] = s.showSubtractive;
    j["showVolumes"] = s.showVolumes;
    j["backfaceCulling"] = s.backfaceCulling;
    j["renderMode"] = renderModeName(s.renderMode);
    j["showGrid"] = s.showGrid;
    j["showOrigins"] = s.showOrigins;
    j["showDirections"] = s.showDirections;
    j["showLinks"] = s.showLinks;
    j["showLightGizmos"] = s.showLightGizmos;
    j["showLightvol"] = s.showLightvol;
    j["showVisibility"] = s.showVisibility;
    j["showLightmapDensity"] = s.showLightmapDensity;
    j["showTexelHeat"] = s.showTexelHeat;
    j["showSizes"] = s.showSizes;
    j["snapEnabled"] = s.snapEnabled;
    j["gridSize"] = s.gridSize;
    j["rotSnap"] = s.rotSnap;
    j["scaleSnap"] = s.scaleSnap;
    j["gizmo"] = kGizmoNames[static_cast<int>(s.gizmo)];
    return j.dump();
}

ViewportSettings settingsFromJson(const std::string& text) {
    ViewportSettings s;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) return s;
    if (j.contains("cameraMode") && j["cameraMode"].is_string())
        s.cameraMode = static_cast<CameraMode>(indexOf(kCameraNames, j["cameraMode"].get<std::string>(), 0));
    if (j.contains("renderMode") && j["renderMode"].is_string())
        s.renderMode = static_cast<RenderMode>(indexOf(kRenderNames, j["renderMode"].get<std::string>(), 0));
    if (j.contains("gizmo") && j["gizmo"].is_string())
        s.gizmo = static_cast<GizmoMode>(indexOf(kGizmoNames, j["gizmo"].get<std::string>(), 0));
    readNum(j, "flySpeed", s.flySpeed);
    s.flySpeed = clampFlySpeed(s.flySpeed);
    readBool(j, "showOutlines", s.showOutlines);
    readBool(j, "showSubtractive", s.showSubtractive);
    readBool(j, "showVolumes", s.showVolumes);
    readBool(j, "backfaceCulling", s.backfaceCulling);
    readBool(j, "showGrid", s.showGrid);
    readBool(j, "showOrigins", s.showOrigins);
    readBool(j, "showDirections", s.showDirections);
    readBool(j, "showLinks", s.showLinks);
    readBool(j, "showLightGizmos", s.showLightGizmos);
    readBool(j, "showLightvol", s.showLightvol);
    readBool(j, "showVisibility", s.showVisibility);
    readBool(j, "showLightmapDensity", s.showLightmapDensity);
    readBool(j, "showTexelHeat", s.showTexelHeat);
    readBool(j, "showSizes", s.showSizes);
    readBool(j, "snapEnabled", s.snapEnabled);
    readNum(j, "gridSize", s.gridSize);
    readNum(j, "rotSnap", s.rotSnap);
    readNum(j, "scaleSnap", s.scaleSnap);
    if (!(s.gridSize > 0)) s.gridSize = 0.5;
    if (!(s.rotSnap > 0)) s.rotSnap = 15;
    if (!(s.scaleSnap > 0)) s.scaleSnap = 0.1;
    return s;
}

} // namespace native::render
