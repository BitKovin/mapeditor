// render/rt_lighting -- see rt_lighting.h.
#include "render/rt_lighting.h"

#include <algorithm>
#include <cmath>
#include <sstream>

#include <core/lights.h>

namespace native::render {

bool RtLighting::operator==(const RtLighting& o) const {
    return ambient[0] == o.ambient[0] && ambient[1] == o.ambient[1] && ambient[2] == o.ambient[2] && aoActive == o.aoActive && aoRadius == o.aoRadius &&
           aoIntensity == o.aoIntensity && aoStrength == o.aoStrength && aoPower == o.aoPower && aoFalloff == o.aoFalloff && exposure == o.exposure &&
           brightness == o.brightness;
}

float RtLighting::aoFactor(float occlusion) const {
    if (!aoActive) return 1.0f;
    const double t = std::clamp(1.0 - static_cast<double>(aoStrength) * occlusion, 0.0, 1.0);
    const double p = aoPower == 1.0f ? t : std::pow(t, static_cast<double>(aoPower));
    return static_cast<float>(1.0 - static_cast<double>(aoIntensity) * (1.0 - p));
}

RtLighting rtLightingOf(const doc::DocumentVersion& version) {
    RtLighting out;
    for (const auto& e : version.entities) {
        if (!e || e->typed.classname != "worldspawn") continue;
        const core::LevelSettings level = core::levelSettingsOf(e->typed);
        out.aoActive = level.ao.active();
        out.aoRadius = static_cast<float>(level.ao.radius);
        out.aoIntensity = static_cast<float>(level.ao.intensity);
        out.aoStrength = static_cast<float>(level.ao.strength);
        out.aoPower = static_cast<float>(level.ao.power);
        out.aoFalloff = static_cast<float>(level.ao.falloff);
        out.exposure = level.exposure > 0 ? static_cast<float>(level.exposure) : 1.0f;   // 0 = the default exposure (the bake's rule)
        out.brightness = static_cast<float>(level.brightness);
        const auto it = e->typed.keyvalues.find("_ambient");
        if (it != e->typed.keyvalues.end()) {
            std::istringstream in(it->second);
            double r, g, b;
            if ((in >> r >> g >> b) && std::isfinite(r + g + b)) {
                out.ambient[0] = static_cast<float>(std::max(0.0, r));
                out.ambient[1] = static_cast<float>(std::max(0.0, g));
                out.ambient[2] = static_cast<float>(std::max(0.0, b));
            }
        }
        break;
    }
    return out;
}

} // namespace native::render
