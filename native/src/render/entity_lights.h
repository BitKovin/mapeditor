// render/entity_lights -- the light volume's values for what the game lights from it (T10.1), as the renderer wants them: a table of entries (ambient, directed, dir) and which brush
// entity / model placement uses which entry. Made off the UI thread by EntityLightProducer from light/entity_probes.
//
// Entry 0 means "none" (the old flat look); entries start at 1. Brush entities are found by their entity key {instance, entityId, ""}, models by the index of their placement in the
// ModelScene the table was made for (a table for another scene is ignored by the model renderer until the right one arrives).
// Thread: the producer's thread builds, the render thread reads; tables are immutable once shared.
// Spec: 06_LIGHTING.md section 11, 08_MILESTONES.md T10.1.
#pragma once

#include "scene/snapshot.h"   // first: core headers use `near` as a name

#include <array>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <core/lighting_inputs.h>
#include <core/profile.h>

#include "light/entity_probes.h"
#include "light/light_filter.h"
#include "render/model_scene.h"

namespace native::render {

struct EntityLightTable {
    std::vector<float> data;   // 12 floats an entry: ambient rgb 0, directed rgb 0, dir xyz 0 (entry e starts at 12 (e - 1))
    std::unordered_map<scene::ObjectKey, uint32_t, scene::ObjectKey::Hash> brush;   // {instance, entityId, ""} -> entry
    std::vector<uint32_t> models;                         // per placement of `forScene`: the entry, 0 = none
    std::shared_ptr<const ModelScene> forScene;           // the models the placements belong to
    uint32_t entries() const { return static_cast<uint32_t>(data.size() / 12); }
    bool operator==(const EntityLightTable& o) const { return data == o.data && brush == o.brush && models == o.models && forScene == o.forScene; }
};

// The world position a model is lit at: the middle of its placed box (the Shell samples the middle of the model's local box transformed).
std::vector<std::array<double, 3>> modelLightCentres(const ModelScene& scene);

// The table of a bake: entries for the valid probes only.
std::shared_ptr<const EntityLightTable> makeEntityLightTable(const light::EntityProbes& probes, std::shared_ptr<const ModelScene> models);

// Bakes the entity probes for the newest request after the editor has been quiet for `quietSeconds`, off the UI thread. `done` is called on the producer's thread with a table that
// differs from the last one it reported (an edit that changes no light does not touch the renderer).
class EntityLightProducer {
public:
    struct Request {
        std::shared_ptr<const scene::SceneSnapshot> snapshot;
        std::shared_ptr<const core::GameProfile> profile;
        std::filesystem::path assetsRoot;
        std::shared_ptr<const light::LightFilter> filter;
        std::shared_ptr<const ModelScene> models;   // may be null (no models)
    };
    explicit EntityLightProducer(std::function<void(std::shared_ptr<const EntityLightTable>)> done, double quietSeconds = 0.4);
    ~EntityLightProducer();
    EntityLightProducer(const EntityLightProducer&) = delete;
    EntityLightProducer& operator=(const EntityLightProducer&) = delete;

    void request(Request r);
    void waitIdle();   // tests: until the newest request was built and reported
    struct Stats {
        uint64_t bakes = 0;
        double lastSeconds = 0;
        size_t lastProbes = 0;
        std::string lastError;
    };
    Stats stats() const;

private:
    void run();
    std::function<void(std::shared_ptr<const EntityLightTable>)> done_;
    double quiet_;
    core::SkyTableCache skyCache_;
    std::shared_ptr<core::SolidSet::Cache> solidCache_;
    std::shared_ptr<core::AlbedoLibrary> albedo_;   // decoded textures of the bounce rays: kept between bakes (a library of its own per game folder / profile)
    std::filesystem::path albedoRoot_;
    const core::GameProfile* albedoProfile_ = nullptr;
    mutable std::mutex mutex_;
    std::condition_variable wake_, idle_;
    Request pending_;
    uint64_t serial_ = 0;   // grows with every request: a bake whose serial is stale is dropped
    bool hasPending_ = false, busy_ = false, stop_ = false;
    std::shared_ptr<const EntityLightTable> last_;
    Stats stats_;
    std::thread thread_;
};

} // namespace native::render
