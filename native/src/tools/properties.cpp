// tools/properties -- see properties.h.
#include "tools/properties.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>

namespace native::tools {

namespace {

bool inSet(const std::set<std::string>& s, const std::string& v) { return s.count(v) > 0; }

double clamp01(double x) { return std::min(1.0, std::max(0.0, x)); }

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// Splits on whitespace (and commas when `commas`).
std::vector<std::string> words(const std::string& s, bool commas) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : s) {
        if (std::isspace(static_cast<unsigned char>(c)) || (commas && c == ',')) {
            if (!cur.empty()) out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

std::optional<double> toNumber(const std::string& s) {
    if (s.empty()) return std::nullopt;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || !std::isfinite(v)) return std::nullopt;
    return v;
}

} // namespace

bool isRefType(const std::string& type) {
    static const std::set<std::string> t = {"target", "target_destination", "killtarget"};
    return inSet(t, type);
}

FieldKind fieldKind(const std::string& type, bool hasItems) {
    static const std::set<std::string> asset = {"asset", "model"}, real = {"real", "float", "angle", "direction"}, integer = {"integer", "int"},
                                       vec = {"real3", "vector", "vec3", "angles", "origin"}, color01 = {"color1", "color"};
    if (isRefType(type)) return FieldKind::Ref;
    if (inSet(asset, type)) return FieldKind::Asset;
    if (hasItems) return FieldKind::List;
    if (type == "boolean" || type == "bool") return FieldKind::Boolean;
    if (inSet(color01, type)) return FieldKind::Color01;
    if (type == "color255") return FieldKind::Color255;
    if (inSet(integer, type)) return FieldKind::Integer;
    if (inSet(real, type)) return FieldKind::Real;
    if (inSet(vec, type)) return FieldKind::Vec3;
    return FieldKind::Text;
}

std::vector<const core::EntKey*> propertyKeys(const core::EntClass* def, const std::vector<std::string>& hidden) {
    std::vector<const core::EntKey*> out;
    if (!def) return out;
    for (const core::EntKey& k : def->keys)
        if (std::find(hidden.begin(), hidden.end(), k.key) == hidden.end()) out.push_back(&k);
    return out;
}

std::optional<std::array<double, 3>> parseColor1(const std::string& v) {
    const std::vector<std::string> w = words(v, false);
    if (w.size() != 3) return std::nullopt;
    std::array<double, 3> c{};
    for (size_t i = 0; i < 3; ++i) {
        const auto n = toNumber(w[i]);
        if (!n) return std::nullopt;
        c[i] = *n;
    }
    return c;
}

std::string colorToHex(const std::array<double, 3>& c) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", static_cast<int>(std::lround(clamp01(c[0]) * 255)), static_cast<int>(std::lround(clamp01(c[1]) * 255)),
                  static_cast<int>(std::lround(clamp01(c[2]) * 255)));
    return buf;
}

std::optional<std::array<double, 3>> hexToColor(const std::string& hex) {
    std::string h = trim(hex);
    if (!h.empty() && h[0] == '#') h.erase(0, 1);
    if (h.size() != 6) return std::nullopt;
    for (const char c : h)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return std::nullopt;
    std::array<double, 3> out{};
    for (size_t i = 0; i < 3; ++i) out[i] = std::strtol(h.substr(i * 2, 2).c_str(), nullptr, 16) / 255.0;
    return out;
}

std::string formatNumber(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6f", v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

std::string formatColor(const std::array<double, 3>& c) {
    std::string out;
    for (size_t i = 0; i < 3; ++i) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.4f", clamp01(c[i]));
        std::string s = buf;
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        if (s.empty()) s = "0";
        out += (i ? " " : "") + s;
    }
    return out;
}

std::string formatColor255(const std::array<double, 3>& c) {
    std::string out;
    for (size_t i = 0; i < 3; ++i) out += (i ? " " : "") + std::to_string(static_cast<int>(std::lround(clamp01(c[i]) * 255)));
    return out;
}

std::optional<double> clampValue(const core::EntKey& k, double raw) {
    if (!std::isfinite(raw)) return std::nullopt;
    double v = fieldKind(k) == FieldKind::Integer ? std::round(raw) : raw;
    if (k.min) v = std::max(*k.min, v);
    if (k.max) v = std::min(*k.max, v);
    return v;
}

std::array<std::optional<double>, 3> parseVec3(const std::string& v) {
    std::array<std::optional<double>, 3> out;
    const std::vector<std::string> w = words(v, true);
    for (size_t i = 0; i < 3 && i < w.size(); ++i) out[i] = toNumber(w[i]);
    return out;
}

std::string currentValue(const core::Entity& e, const core::EntKey& k) {
    const auto it = e.keyvalues.find(k.key);
    if (it != e.keyvalues.end()) return it->second;
    return k.defaultValue.value_or("");
}

bool isDefault(const core::Entity& e, const core::EntKey& k) {
    const auto it = e.keyvalues.find(k.key);
    if (it == e.keyvalues.end()) return true;
    return k.defaultValue && it->second == *k.defaultValue;
}

bool isChecked(const std::string& v) {
    if (const auto n = toNumber(v)) return *n != 0;
    std::string t = trim(v);
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return !t.empty() && t != "false";
}

std::array<double, 3> parseScaleVec(const std::string& kv) {
    const std::vector<std::string> w = words(kv, false);
    if (w.size() != 3) return {1, 1, 1};
    std::array<double, 3> out{};
    for (size_t i = 0; i < 3; ++i) {
        const auto n = toNumber(w[i]);
        if (!n || *n == 0) return {1, 1, 1};
        out[i] = *n;
    }
    return out;
}

} // namespace native::tools
