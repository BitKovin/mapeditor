// tools/light_list -- see light_list.h.
#include "tools/light_list.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace native::tools {

namespace {

bool number(const core::Entity& e, const char* key, double& out) {
    const auto it = e.keyvalues.find(key);
    if (it == e.keyvalues.end()) return false;
    char* end = nullptr;
    const double v = std::strtod(it->second.c_str(), &end);
    if (end == it->second.c_str() || !std::isfinite(v)) return false;
    out = v;
    return true;
}

std::string colorOf(const core::Entity& e) {
    double c[3] = {1, 1, 1};
    const auto it = e.keyvalues.find("color");
    if (it != e.keyvalues.end()) {
        std::istringstream in(it->second);
        double r, g, b;
        if (in >> r >> g >> b) {
            c[0] = r;
            c[1] = g;
            c[2] = b;
        }
    }
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", static_cast<int>(std::lround(std::clamp(c[0], 0.0, 1.0) * 255)), static_cast<int>(std::lround(std::clamp(c[1], 0.0, 1.0) * 255)),
                  static_cast<int>(std::lround(std::clamp(c[2], 0.0, 1.0) * 255)));
    return buf;
}

} // namespace

std::vector<LightRow> listLights(const doc::DocumentVersion& doc) {
    std::vector<LightRow> out;
    for (const auto& n : doc.entities) {
        const core::Entity& e = n->typed;
        if (!n->brushes.empty() || e.classname.rfind("light_", 0) != 0) continue;
        LightRow r;
        r.id = e.id;
        r.classname = e.classname;
        const auto tn = e.keyvalues.find("targetname");
        r.name = tn != e.keyvalues.end() && !tn->second.empty() ? tn->second : e.id;
        number(e, "brightness", r.brightness);
        r.color = colorOf(e);
        double style = 0;
        if (number(e, "style", style)) r.style = std::clamp(static_cast<int>(style), 0, 63);
        out.push_back(std::move(r));
    }
    return out;
}

} // namespace native::tools
