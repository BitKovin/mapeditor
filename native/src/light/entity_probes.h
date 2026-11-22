// light/entity_probes -- the light volume for the things the game lights from it (T10.1): brush entities of classes without lightmaps (doors, lifts, buttons) and models.
//
// The game does not light them per pixel: it samples its light grid once at the centre of the entity (ambient + directed * max(0, n . dir), DESIGN §6.7). The editor does the same, with
// the Core's own probe evaluator and fit (core::bakeProbesCpu, fitProbeDirectFirst, LightvolGrid::sample), but bakes only the probes around those entities instead of the whole grid:
// a 6 x 6 x 6 block of the level's lattice (origin = bounds of the surfaces minus one cell, the level's `_lightvol_grid` or 1 m) around every sample point. Probes inside solids are skipped
// like the export does, a sample falls back to the nearest valid probe within 3 cells (LightvolGrid::sample). Direct light, sky and the level's ambient; no bounce light and one style layer
// (the base layer), see 06_LIGHTING.md section 11.
// Thread: any (pure; the work is for a background thread: it builds the occluders and a BVH of the level). Spec: 08_MILESTONES.md T10.1.
#pragma once

#include "scene/snapshot.h"   // first: core headers use `near` as a name

#include <core/lighting_inputs.h>
#include <core/lightvol.h>
#include <core/profile.h>

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "light/light_filter.h"

namespace native::light {

struct EntityProbeInputs {
    std::shared_ptr<const scene::SceneSnapshot> snapshot;
    std::shared_ptr<const core::GameProfile> profile;
    std::filesystem::path assetsRoot;
    std::shared_ptr<const LightFilter> filter;               // muted / soloed lights (null = all)
    std::vector<std::array<double, 3>> modelCentres;         // world position every model is sampled at (the middle of its box), one probe each
    double defaultSourceSize = 0.25;                         // m: the same defaults the live bake gives lights without a size
    double defaultSunSize = 0.5;                             // degrees
};

// What an entity is lit with: light(n) = ambient + directed * max(0, n . dir), linear (values above 1 are fine).
struct EntityLightValue {
    float ambient[3] = {0, 0, 0};
    float directed[3] = {0, 0, 0};
    float dir[3] = {0, 1, 0};   // unit, towards the light
    bool valid = false;         // false: no probe near it (inside solid rock): the entity keeps the flat look
};

struct BrushEntityLight {
    scene::ObjectKey key;          // {instance, entityId, ""}
    double centre[3] = {0, 0, 0};  // the middle of the bounds of the entity's surfaces: where it is sampled
    EntityLightValue light;
};

struct EntityProbes {
    std::vector<BrushEntityLight> brushEntities;
    std::vector<EntityLightValue> models;   // parallel to EntityProbeInputs::modelCentres
    size_t probes = 0;                      // probes baked
    double seconds = 0;
    std::string error;                      // "" = fine
};

// Bakes the probes around the entities of the inputs. `skyCache` / `solidCache` may be shared between calls (null = a private one).
EntityProbes bakeEntityProbes(const EntityProbeInputs& in, core::SkyTableCache* skyCache = nullptr, core::SolidSet::Cache* solidCache = nullptr);

// The pure maths the renderer repeats: the light an entity receives on a surface with normal n.
void entityLightAt(const EntityLightValue& v, const float n[3], float out[3]);

} // namespace native::light
