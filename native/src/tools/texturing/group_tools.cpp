// tools/texturing/group_tools -- see group_tools.h.
#include "tools/texturing/group_tools.h"

#include <algorithm>
#include <cmath>
#include <cctype>

namespace native::tools::tex {

using ojson = nlohmann::ordered_json;

namespace {

std::string pretty(const char* text) { return ojson::parse(text).dump(2); }

const char* kFacade = R"json({
  "format": 1, "name": "Building facade",
  "description": "Ground floor, repeating floors of window bays, a top floor and a cornice. Walls, roofs and floors are told apart by the face orientation.",
  "params": {
    "floorHeight": {"type": "number", "default": 3, "min": 2, "max": 6, "step": 0.1, "label": "Floor height (m)"},
    "bayWidth": {"type": "number", "default": 3, "min": 2, "max": 8, "step": 0.1, "label": "Bay width (m)"}
  },
  "slots": {"wall": "dev/wall", "window": "dev/metal", "door": "dev/wood", "shop": "dev/concrete", "cornice": "dev/concrete", "roof": "dev/floor"},
  "fallback": "wall",
  "rules": {"windowBay": {"split": "u", "parts": [{"size": "*", "tex": "wall"}, {"size": 1.2, "tex": "window", "fit": "stretch"}, {"size": "*", "tex": "wall"}]}},
  "root": {"choice": [
    {"when": {"var": "facing", "==": "wall"}, "split": "v", "parts": [
      {"size": {"param": "floorHeight", "mul": 1.3}, "split": "u", "parts": [
        {"size": "*", "repeat": {"size": {"param": "bayWidth"}, "as": "bay"}, "choice": [
          {"when": {"all": ["bay.first", {"var": "faceW", ">=": 6}]}, "tex": "door", "fit": "stretch"},
          {"tex": "shop", "fit": "stretch"}]}]},
      {"size": "*", "repeat": {"size": {"param": "floorHeight"}, "as": "floor"}, "split": "u", "parts": [
        {"size": "*", "repeat": {"size": {"param": "bayWidth"}, "as": "bay"}, "ref": "windowBay"}]},
      {"size": 0.5, "tex": "cornice", "fit": "width"}]},
    {"when": {"var": "facing", "==": "up"}, "tex": "roof"},
    {"tex": "wall"}]}
})json";

const char* kPanels = R"json({
  "format": 1, "name": "Panel wall",
  "description": "Bottom and top trim with panels between. Panels vary by seed; every fourth bay has a vent.",
  "params": {"panelWidth": {"type": "number", "default": 1.5, "min": 0.5, "max": 4, "step": 0.1, "label": "Panel width (m)"}},
  "slots": {"panel": "dev/metal", "panelB": "dev/concrete", "vent": "dev/grey", "trim": "dev/wall"},
  "fallback": "panel",
  "root": {"split": "v", "parts": [
    {"size": 0.3, "tex": "trim", "fit": "height"},
    {"size": "*", "split": "u", "parts": [
      {"size": "*", "repeat": {"size": {"param": "panelWidth"}, "as": "p"}, "choice": [
        {"when": {"var": "p.index", "mod": [4, 3]}, "tex": "vent", "fit": "stretch"},
        {"tex": "panel", "fit": "stretch", "weight": 3},
        {"tex": "panelB", "fit": "stretch", "weight": 1}], "mode": "random"}]},
    {"size": 0.2, "tex": "trim", "fit": "height"}]}
})json";

const char* kFloor = R"json({
  "format": 1, "name": "Bordered floor",
  "description": "A ring of trim around the edge and tiles of three kinds inside. Useful on floors, ceilings and rugs.",
  "params": {
    "border": {"type": "number", "default": 0.3, "min": 0.05, "max": 1, "step": 0.05, "label": "Border (m)"},
    "tile": {"type": "number", "default": 1, "min": 0.25, "max": 3, "step": 0.25, "label": "Tile (m)"}
  },
  "slots": {"tile": "dev/floor", "tileB": "dev/concrete", "tileC": "dev/grey", "border": "dev/wood"},
  "fallback": "tile",
  "rules": {"tiles": {"split": "v", "parts": [
    {"size": "*", "repeat": {"size": {"param": "tile"}, "as": "r"}, "split": "u", "parts": [
      {"size": "*", "repeat": {"size": {"param": "tile"}, "as": "c"}, "choice": ["tile", "tile", "tileB", "tileC"], "mode": "random"}]}]}},
  "root": {"split": "v", "parts": [
    {"size": {"param": "border"}, "tex": "border"},
    {"size": "*", "split": "u", "parts": [{"size": {"param": "border"}, "tex": "border"}, {"size": "*", "ref": "tiles"}, {"size": {"param": "border"}, "tex": "border"}]},
    {"size": {"param": "border"}, "tex": "border"}]}
})json";

bool isNum(const ojson& j, const char* k) { return j.contains(k) && j[k].is_number() && std::isfinite(j[k].get<double>()); }

} // namespace

const std::vector<GroupTemplate>& groupTemplates() {
    static const std::vector<GroupTemplate> t = {
        {"facade", "Building facade (floors, bays, top floor)", pretty(kFacade)},
        {"panels", "Panel wall (trim, random panels, vents)", pretty(kPanels)},
        {"floor", "Bordered floor (trim ring, random tiles)", pretty(kFloor)},
    };
    return t;
}

GroupTab groupTabOf(const nlohmann::json& raw) { return raw.is_object() && raw.contains("kit") ? GroupTab::Simple : GroupTab::Advanced; }

std::vector<std::string> filterGroups(const std::vector<std::string>& names, const std::string& query) {
    std::string q = query;
    std::transform(q.begin(), q.end(), q.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::vector<std::string> out;
    for (const std::string& n : names) {
        std::string l = n;
        std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (l.find(q) != std::string::npos) out.push_back(n);
    }
    return out;
}

std::string copyName(const std::string& name, const std::vector<std::string>& existing) {
    const auto taken = [&](const std::string& n) { return std::find(existing.begin(), existing.end(), n) != existing.end(); };
    std::string c = name + "_copy";
    for (int i = 2; taken(c) && i < 10000; ++i) c = name + "_copy" + std::to_string(i);
    return c;
}

bool validGroupName(const std::string& name) {
    if (name.empty() || name.size() > 120) return false;
    bool segmentStart = true;
    for (const char c : name) {
        if (c == '/') {
            if (segmentStart) return false;   // an empty segment: leading or doubled slash
            segmentStart = true;
            continue;
        }
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
        segmentStart = false;
    }
    return !segmentStart;
}

std::optional<std::string> setSlotTexture(const std::string& text, const std::string& slot, const std::string& path) {
    const ojson j = ojson::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("slots") || !j["slots"].is_object() || !j["slots"].contains(slot)) return std::nullopt;
    ojson out = j;
    ojson& cur = out["slots"][slot];
    if (cur.is_object()) cur["texture"] = path;
    else cur = path;
    return out.dump(2);
}

std::vector<ParamDef> paramDefsOf(const ojson& group) {
    std::vector<ParamDef> out;
    if (!group.is_object() || !group.contains("params") || !group["params"].is_object()) return out;
    for (const auto& [name, d] : group["params"].items()) {
        if (!d.is_object() || !d.contains("type") || !d["type"].is_string()) continue;
        ParamDef p;
        p.name = name;
        p.label = d.contains("label") && d["label"].is_string() && !d["label"].get<std::string>().empty() ? d["label"].get<std::string>() : name;
        const std::string type = d["type"].get<std::string>();
        if (type == "bool") {
            p.type = "bool";
            p.defBool = d.contains("default") && d["default"].is_boolean() && d["default"].get<bool>();
        } else if (type == "choice") {
            p.type = "choice";
            if (d.contains("options") && d["options"].is_array())
                for (const auto& o : d["options"])
                    if (o.is_string() && !o.get<std::string>().empty()) p.options.push_back(o.get<std::string>());
            if (p.options.empty()) continue;
            p.defChoice = d.contains("default") && d["default"].is_string() && std::find(p.options.begin(), p.options.end(), d["default"].get<std::string>()) != p.options.end() ? d["default"].get<std::string>() : p.options[0];
        } else if (type == "number" || type == "int") {
            p.type = type;
            p.min = isNum(d, "min") ? d["min"].get<double>() : 0;
            p.max = isNum(d, "max") && d["max"].get<double>() >= p.min ? d["max"].get<double>() : std::max(p.min, 1000.0);
            p.step = isNum(d, "step") && d["step"].get<double>() > 0 ? d["step"].get<double>() : type == "int" ? 1 : 0.1;
            double def = isNum(d, "default") ? d["default"].get<double>() : p.min;
            def = std::clamp(def, p.min, p.max);
            p.defNumber = type == "int" ? std::round(def) : def;
        } else {
            continue;
        }
        out.push_back(std::move(p));
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> slotsOf(const ojson& group) {
    std::vector<std::pair<std::string, std::string>> out;
    if (!group.is_object() || !group.contains("slots") || !group["slots"].is_object()) return out;
    for (const auto& [name, v] : group["slots"].items()) {
        if (v.is_string()) out.emplace_back(name, v.get<std::string>());
        else if (v.is_object() && v.contains("texture") && v["texture"].is_string()) out.emplace_back(name, v["texture"].get<std::string>());
    }
    return out;
}

double previewScale(double canvasW, double canvasH, double w, double h, double margin) { return std::min((canvasW - 2 * margin) / w, (canvasH - 2 * margin) / h); }

int cellAt(const std::vector<core::texplan::Cell>& cells, double x, double y) {
    for (int i = static_cast<int>(cells.size()) - 1; i >= 0; --i) {
        const auto& c = cells[static_cast<size_t>(i)];
        if (x >= c.x0 && x <= c.x1 && y >= c.y0 && y <= c.y1) return i;
    }
    return -1;
}

std::array<double, 6> previewPatternMatrix(const core::texplan::CellMap& m, double imgW, double imgH, double scale, double faceH) {
    const double pu = m.u[0], qu = m.u[1], tu = m.u[2], pv = m.v[0], qv = m.v[1], tv = m.v[2];
    const double det = pu * qv - qu * pv;
    const double k11 = qv / imgW / det, k12 = -qu / imgH / det, k13 = (-qv * tu + qu * tv) / det;
    const double k21 = -pv / imgW / det, k22 = pu / imgH / det, k23 = (pv * tu - pu * tv) / det;
    return {scale * k11, -scale * k21, scale * k12, -scale * k22, scale * k13, scale * faceH - scale * k23};
}

} // namespace native::tools::tex
