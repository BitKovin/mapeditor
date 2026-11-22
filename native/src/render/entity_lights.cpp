// render/entity_lights -- see entity_lights.h.
#include "render/entity_lights.h"

#include <chrono>

#include "base/log.h"

namespace native::render {

std::vector<std::array<double, 3>> modelLightCentres(const ModelScene& scene) {
    std::vector<std::array<double, 3>> out;
    out.reserve(scene.placements.size());
    for (const ModelPlacement& p : scene.placements)
        out.push_back({(p.bounds.min[0] + p.bounds.max[0]) * 0.5, (p.bounds.min[1] + p.bounds.max[1]) * 0.5, (p.bounds.min[2] + p.bounds.max[2]) * 0.5});
    return out;
}

std::shared_ptr<const EntityLightTable> makeEntityLightTable(const light::EntityProbes& probes, std::shared_ptr<const ModelScene> models) {
    auto t = std::make_shared<EntityLightTable>();
    const auto add = [&](const light::EntityLightValue& v) -> uint32_t {
        if (!v.valid) return 0;
        if (t->entries() >= 8192) return 0;   // TextureTable::kEntityLightMax: the rest keeps the flat look
        for (int c = 0; c < 3; ++c) t->data.push_back(v.ambient[c]);
        t->data.push_back(0.f);
        for (int c = 0; c < 3; ++c) t->data.push_back(v.directed[c]);
        t->data.push_back(0.f);
        for (int c = 0; c < 3; ++c) t->data.push_back(v.dir[c]);
        t->data.push_back(0.f);
        return t->entries();
    };
    for (const light::BrushEntityLight& e : probes.brushEntities) {
        const uint32_t entry = add(e.light);
        if (entry != 0) t->brush[e.key] = entry;
    }
    t->forScene = std::move(models);
    t->models.reserve(probes.models.size());
    for (const light::EntityLightValue& v : probes.models) t->models.push_back(add(v));
    return t;
}

EntityLightProducer::EntityLightProducer(std::function<void(std::shared_ptr<const EntityLightTable>)> done, double quietSeconds)
    : done_(std::move(done)), quiet_(quietSeconds), solidCache_(core::SolidSet::makeCache()) {
    thread_ = std::thread([this] { run(); });
}

EntityLightProducer::~EntityLightProducer() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void EntityLightProducer::request(Request r) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = std::move(r);
        hasPending_ = true;
        ++serial_;
    }
    wake_.notify_all();
}

void EntityLightProducer::waitIdle() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [&] { return !hasPending_ && !busy_; });
}

EntityLightProducer::Stats EntityLightProducer::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void EntityLightProducer::run() {
    for (;;) {
        Request r;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return stop_ || hasPending_; });
            if (stop_) return;
            // quiet: a request that arrives while we wait replaces this one and the wait starts again
            for (;;) {
                const uint64_t seen = serial_;
                if (wake_.wait_for(lock, std::chrono::duration<double>(quiet_), [&] { return stop_ || serial_ != seen; })) {
                    if (stop_) return;
                    continue;
                }
                break;
            }
            r = std::move(pending_);
            hasPending_ = false;
            busy_ = true;
        }
        std::shared_ptr<const EntityLightTable> table;
        if (r.snapshot && r.snapshot->doc && r.profile) {
            try {
                light::EntityProbeInputs in;
                in.snapshot = r.snapshot;
                in.profile = r.profile;
                in.assetsRoot = r.assetsRoot;
                in.filter = r.filter;
                if (r.models) in.modelCentres = modelLightCentres(*r.models);
                const light::EntityProbes probes = light::bakeEntityProbes(in, &skyCache_, solidCache_.get());
                if (!probes.error.empty()) base::logWarning("light volume: " + probes.error);
                table = makeEntityLightTable(probes, r.models);
                std::lock_guard<std::mutex> lock(mutex_);
                ++stats_.bakes;
                stats_.lastSeconds = probes.seconds;
                stats_.lastProbes = probes.probes;
                stats_.lastError = probes.error;
            } catch (const std::exception& e) {
                base::logError(std::string("light volume: the entity probes could not be baked: ") + e.what());
            }
        }
        if (table && !(last_ && *last_ == *table)) {
            last_ = table;
            if (done_) done_(table);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            busy_ = false;
        }
        idle_.notify_all();
    }
}

} // namespace native::render
