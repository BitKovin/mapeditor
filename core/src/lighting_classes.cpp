#include "core/lighting_classes.h"
#include "core/entity_class.h"

using nlohmann::json;

namespace core {

LightingClasses LightingClasses::defaults() {
    LightingClasses c;
    for (const char* name : {"worldspawn", "func_detail", "func_group"}) {
        // Keep this list honest: it must agree with the registry's definition of "structural".
        if (EntityClassRegistry::isStructural(name)) {
            c.lightmapped.insert(name);
            c.castShadows.insert(name);
        }
    }
    // misc_model (obj_model.h) is baked into the world: lightmapped, and an occluder unless the entity says _cs 0 (LightSurface::castsShadow)
    c.lightmapped.insert("misc_model");
    c.castShadows.insert("misc_model");
    return c;
}

LightingClasses LightingClasses::fromProfile(const GameProfile& profile) {
    LightingClasses c = defaults();
    for (const auto& n : profile.receiveBakedLight) c.lightmapped.insert(n);
    for (const auto& n : profile.blockLight) c.castShadows.insert(n);
    return c;
}

json LightingClasses::toJson() const { return json{{"receiveBakedLight", lightmapped}, {"blockLight", castShadows}}; }

}  // namespace core
