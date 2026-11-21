// tools/texturing/props_edit -- see props_edit.h.
#include "tools/texturing/props_edit.h"

#include <algorithm>
#include <cmath>

#include "core/textures.h"

namespace native::tools::tex {

using json = nlohmann::json;

namespace {

bool isNum(const json& j) { return j.is_number() && std::isfinite(j.get<double>()); }
std::string str(const json& j, const char* key, const std::string& fallback) { return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : fallback; }
double num(const json& j, const char* key, double fallback) { return j.is_object() && j.contains(key) && isNum(j[key]) ? j[key].get<double>() : fallback; }
const char* axisKey(int axis) { return axis == 0 ? "u" : "v"; }

} // namespace

AxisProps defaultAxis(int axis) {
    AxisProps a;
    a.anchor = axis == 0 ? "start" : "end";
    return a;
}

json normalizedProps(const json& props) { return core::sanitizeTextureProps(props); }

std::optional<double> ppmOf(const json& props) {
    if (props.is_object() && props.contains("pixelsPerMeter") && isNum(props["pixelsPerMeter"]) && props["pixelsPerMeter"].get<double>() > 0) return props["pixelsPerMeter"].get<double>();
    return std::nullopt;
}

json withPpm(const json& props, std::optional<double> ppm) {
    json out = props.is_object() ? props : json::object();
    if (ppm && std::isfinite(*ppm) && *ppm > 0) out["pixelsPerMeter"] = std::clamp(*ppm, kPpmMin, kPpmMax);
    else out.erase("pixelsPerMeter");
    return out;
}

AxisProps axisOf(const json& props, int axis) {
    AxisProps a = defaultAxis(axis);
    if (!props.is_object() || !props.contains("repeat") || !props["repeat"].is_object()) return a;
    const json& r = props["repeat"];
    if (!r.contains(axisKey(axis)) || !r[axisKey(axis)].is_object()) return a;
    const json& j = r[axisKey(axis)];
    a.mode = str(j, "mode", a.mode);
    a.anchor = str(j, "anchor", a.anchor);
    a.fill = str(j, "fill", a.fill);
    a.edge = str(j, "edge", a.edge);
    a.split = std::clamp(num(j, "split", a.split), 0.02, 0.98);
    if (j.contains("alt") && j["alt"].is_object()) {
        a.altTexture = str(j["alt"], "texture", "");
        if (j["alt"].contains("offset") && j["alt"]["offset"].is_array() && j["alt"]["offset"].size() == 2 && isNum(j["alt"]["offset"][0]) && isNum(j["alt"]["offset"][1]))
            a.altOffset = {j["alt"]["offset"][0].get<double>(), j["alt"]["offset"][1].get<double>()};
    }
    return a;
}

json withAxis(const json& props, int axis, const AxisProps& a) {
    json out = props.is_object() ? props : json::object();
    if (!out.contains("repeat") || !out["repeat"].is_object()) out["repeat"] = json::object();
    json& r = out["repeat"];
    if (a == defaultAxis(axis)) {
        r.erase(axisKey(axis));
        return out;
    }
    json j = {{"mode", a.mode}, {"anchor", a.anchor}, {"split", a.split}, {"fill", a.fill}, {"edge", a.edge}};
    if (!a.altTexture.empty()) j["alt"] = {{"texture", a.altTexture}, {"offset", json::array({a.altOffset[0], a.altOffset[1]})}};
    r[axisKey(axis)] = j;
    return out;
}

std::vector<Hotspot> hotspotsOf(const json& props) {
    if (!props.is_object() || !props.contains("hotspots")) return {};
    return parseHotspots(props["hotspots"]);
}

json withHotspots(const json& props, const std::vector<Hotspot>& hotspots) {
    json out = props.is_object() ? props : json::object();
    out["hotspots"] = hotspotsJson(hotspots);
    return out;
}

TransparencyProps transparencyOf(const json& props) {
    TransparencyProps t;
    if (!props.is_object() || !props.contains("transparency") || !props["transparency"].is_object()) return t;
    const json& j = props["transparency"];
    t.mode = str(j, "mode", t.mode);
    t.cutoff = std::clamp(num(j, "cutoff", t.cutoff), 0.0, 1.0);
    t.opacity = std::clamp(num(j, "opacity", t.opacity), 0.0, 1.0);
    return t;
}

json withTransparency(const json& props, const TransparencyProps& t) {
    json out = props.is_object() ? props : json::object();
    json j = out.contains("transparency") && out["transparency"].is_object() ? out["transparency"] : json::object();   // a tint stays
    if (t == TransparencyProps{}) {
        j.erase("mode");
        j.erase("cutoff");
        j.erase("opacity");
    } else {
        j["mode"] = t.mode;
        j["cutoff"] = t.cutoff;
        j["opacity"] = t.opacity;
    }
    if (j.empty()) out.erase("transparency");
    else out["transparency"] = j;
    return out;
}

bool isPlain(const json& props) {
    const json n = normalizedProps(props);
    if (n.contains("pixelsPerMeter")) return false;
    if (n.contains("hotspots") && !n["hotspots"].empty()) return false;
    if (n.contains("relief") && !n["relief"].is_null()) return false;
    for (int a = 0; a < 2; ++a)
        if (!(axisOf(n, a) == defaultAxis(a))) return false;
    return transparencyOf(n) == TransparencyProps{} && !(n.contains("transparency") && n["transparency"].contains("tint"));
}

json forWrite(const json& props) {
    json out = normalizedProps(props);
    if (!out.contains("hotspots")) out["hotspots"] = json::array();
    if (!out.contains("repeat")) out["repeat"] = json::object();
    if (!out.contains("transparency")) out["transparency"] = {{"mode", "auto"}};
    return out;
}

} // namespace native::tools::tex
