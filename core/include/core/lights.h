#pragma once
#include "core/document.h"
#include "core/math3.h"
#include "core/prefab.h"
#include "core/sky.h"
#include <memory>
#include <string>
#include <vector>

namespace core {

// Modern light entities (REQUIREMENTS §12, DESIGN §6.5): Unreal-like parameters, no physical units. A white surface facing
// a point light of brightness B at 1 m (inside the attenuation radius) stores exactly B.
//
//   classname:     light_point | light_spot | light_directional | light_rect | light_sky
//   color          "r g b", 0..1                              (default 1 1 1)
//   brightness     >= 0                                       (default 1)
//   radius         attenuation radius in metres               (default 10; point, spot, rect)
//   bounce         bounce power, >= 0                         (default 1)
//   cast_shadows   0 | 1                                      (default 1)
//   style          light style 0..63 (0 = always on; see light_styles.h)   (default 0)
//   source_size    sphere radius in m (point, spot) or angular diameter in degrees (directional)   (default 0)
//   inner_angle / outer_angle   spot cone half-angles, degrees (default 20 / 35)
//   width / height rect size in metres                        (default 1 / 1)
// Direction: forward = R * (0,0,1), right = R * (1,0,0), up = R * (0,1,0), R = Ry(yaw) Rx(pitch) Rz(roll) from the entity's
// `angles` (the Shell's direction arrow axis). A rect light emits towards `forward`.
// worldspawn's `_ambient` ("r g b") is a constant added to every texel.
enum class LightType : int { Point = 0, Spot = 1, Directional = 2, Rect = 3, Sky = 4 };

struct Light {
    std::string id;        // entity id
    std::string instance;  // prefab instance path ("" = the document)
    LightType type = LightType::Point;
    Vec3 position;
    Vec3 forward{0, 0, 1}, right{1, 0, 0}, up{0, 1, 0};
    Vec3 color{1, 1, 1};
    double brightness = 1;
    double radius = 10;
    double bounce = 1;
    bool castShadows = true;
    int style = 0;
    double sourceSize = 0;
    double innerAngle = 20, outerAngle = 35;
    double width = 1, height = 1;
    // Sky light only: the incoming light per direction from the level's skybox (REQUIREMENTS §28 K12), times colour x brightness; null = uniform.
    std::shared_ptr<const SkyRadiance> sky;
};

// Level settings (REQUIREMENTS §22 P2), stored as worldspawn keys so they travel with the map:
//   _light_scale     multiplies the brightness of every light entity (not the ambient)          (default 1)
//   _ao_radius       baked ambient occlusion: how far (m) rays look for nearby geometry; 0 = off (default 0)
//   _ao_power        contrast of the occlusion (exponent on the unoccluded fraction)            (default 1)
//   _ao_intensity    0..1: how much the occlusion darkens the light                            (default 1)
//   _ao_rays         occlusion rays per texel (hardware ray traced when a GPU has ray queries)  (default 32)
//   _cc_exposure     display multiplier of the Lit view (colour correction); 0 = lighting.json's exposure (default 0)
//   _cc_brightness   display offset added after the exposure (linear)                           (default 0)
//   _rays_per_texel  gather rays per lightmap texel over the whole live bake; 0 = lighting.json  (default 0)
//   _supersample     sub-texel positions per texel for the direct light; 0 = lighting.json      (default 0)
//   _lightvol_grid   light volume grid spacing in m; 0 = lighting.json                          (default 0)
//   _rays_per_probe  gather rays per light volume probe; 0 = lighting.json                      (default 0)
//   _bake_quality    multiplier of the ray counts for the final bake (M6 export); the live bake uses x1 (default 2)
struct AoSettings {
    double radius = 0, power = 1, intensity = 1;
    int rays = 32;
    bool active() const { return radius > 0 && intensity > 0; }
    bool operator==(const AoSettings& o) const { return radius == o.radius && power == o.power && intensity == o.intensity && rays == o.rays; }
    bool operator!=(const AoSettings& o) const { return !(*this == o); }
};
struct LevelSettings {
    double lightScale = 1;
    AoSettings ao;
    double exposure = 0, brightness = 0;
    int raysPerTexel = 0, supersample = 0, raysPerProbe = 0;
    double lightvolGrid = 0, bakeQuality = 2;
};
// The level settings of a document's worldspawn (missing / bad keys keep the defaults; values are clamped; `warnings` gets what was wrong).
LevelSettings levelSettingsOf(const Entity& worldspawn, std::vector<std::string>* warnings = nullptr);

struct LightScene {
    std::vector<Light> lights;
    Vec3 ambient;  // black unless worldspawn has `_ambient`
    LevelSettings level;  // from worldspawn; every light's brightness already includes level.lightScale
    std::vector<std::string> warnings;
};

bool isLightEntity(const Entity& e);
LightScene collectLights(const Document& doc, const std::vector<ResolvedEntity>& prefabs);

}  // namespace core

namespace core {
// The lights of one style layer: only the lights with that style; the ambient belongs to style 0.
LightScene lightsOfStyle(const LightScene& all, int style);
// The distinct styles of the lights, ascending; always contains 0.
std::vector<int> lightStyles(const LightScene& scene);
}  // namespace core
