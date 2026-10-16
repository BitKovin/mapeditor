#pragma once
#include "core/profile.h"
#include <nlohmann/json.hpp>
#include <set>
#include <string>

namespace core {

// Which classnames receive baked light (lightmaps) and which block light (cast shadows, bounce light). Both come from the game profile
// (GameProfile::receiveBakedLight / blockLight); worldspawn, func_group and func_detail are always in both. Brush entities outside
// `lightmapped` are lit from light volumes instead.
struct LightingClasses {
    std::set<std::string> lightmapped;
    std::set<std::string> castShadows;

    static LightingClasses defaults();
    static LightingClasses fromProfile(const GameProfile& profile);
    bool isLightmapped(const std::string& classname) const { return lightmapped.count(classname) != 0; }
    bool castsShadows(const std::string& classname) const { return castShadows.count(classname) != 0; }
    nlohmann::json toJson() const;
    bool operator==(const LightingClasses& o) const { return lightmapped == o.lightmapped && castShadows == o.castShadows; }
};

}  // namespace core
