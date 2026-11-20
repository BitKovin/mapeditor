// tools/prefs -- see prefs.h.
#include "tools/prefs.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace native::tools {

namespace {
double positive(double v, double fallback) { return std::isfinite(v) && v > 0 ? v : fallback; }
void readBool(const nlohmann::json& j, const char* key, bool& out) {
    if (j.contains(key) && j[key].is_boolean()) out = j[key].get<bool>();
}
void readNum(const nlohmann::json& j, const char* key, double& out) {
    if (j.contains(key) && j[key].is_number()) out = j[key].get<double>();
}
} // namespace

EditorPrefs sanitize(const EditorPrefs& p) {
    const EditorPrefs d;
    EditorPrefs r = p;
    r.gridSize = std::clamp(positive(p.gridSize, d.gridSize), 0.001, 10000.0);
    r.rotSnap = std::clamp(positive(p.rotSnap, d.rotSnap), 0.01, 360.0);
    r.scaleSnap = std::clamp(positive(p.scaleSnap, d.scaleSnap), 0.001, 100.0);
    r.flySpeed = render::clampFlySpeed(p.flySpeed);
    return r;
}

std::string prefsToJson(const EditorPrefs& p) {
    const EditorPrefs s = sanitize(p);
    nlohmann::json j;
    j["gridSize"] = s.gridSize;
    j["snapEnabled"] = s.snapEnabled;
    j["rotSnap"] = s.rotSnap;
    j["scaleSnap"] = s.scaleSnap;
    j["flySpeed"] = s.flySpeed;
    j["showGrid"] = s.showGrid;
    return j.dump();
}

EditorPrefs prefsFromJson(const std::string& text) {
    EditorPrefs p;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_object()) {
        readNum(j, "gridSize", p.gridSize);
        readBool(j, "snapEnabled", p.snapEnabled);
        readNum(j, "rotSnap", p.rotSnap);
        readNum(j, "scaleSnap", p.scaleSnap);
        readNum(j, "flySpeed", p.flySpeed);
        readBool(j, "showGrid", p.showGrid);
    }
    return sanitize(p);
}

render::ViewportSettings applyPrefs(const EditorPrefs& p, render::ViewportSettings s) {
    const EditorPrefs c = sanitize(p);
    s.gridSize = c.gridSize;
    s.snapEnabled = c.snapEnabled;
    s.rotSnap = c.rotSnap;
    s.scaleSnap = c.scaleSnap;
    s.flySpeed = c.flySpeed;
    s.showGrid = c.showGrid;
    return s;
}

EditorPrefs prefsOf(const render::ViewportSettings& s) {
    EditorPrefs p;
    p.gridSize = s.gridSize;
    p.snapEnabled = s.snapEnabled;
    p.rotSnap = s.rotSnap;
    p.scaleSnap = s.scaleSnap;
    p.flySpeed = s.flySpeed;
    p.showGrid = s.showGrid;
    return sanitize(p);
}

} // namespace native::tools
