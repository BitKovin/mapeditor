// light/lighting_service -- see lighting_service.h.
#include "light/lighting_service.h"

#include <core/bsp_compile.h>
#include <core/lighting_classes.h>
#include <core/lighting_inputs.h>
#include <core/lights.h>
#include <core/parallel.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>

#include "base/log.h"
#include "base/trace.h"
#include "gpu/upload_ring.h"
#include "light/accel.h"
#include "light/layout.h"
#include "light/light_pack.h"
#include "light/lit_mesh.h"
#include "light/page_images.h"
#include "light/results.h"
#include "light/samples.h"

namespace native::light {

namespace {

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

} // namespace

// A command pool and one-shot submits on the compute queue, waited for.
class ComputeShots {
public:
    explicit ComputeShots(gpu::Device& d) : d_(d) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = d.computeFamily();
        vkCreateCommandPool(d.device(), &pi, nullptr, &pool_);
    }
    ~ComputeShots() {
        if (pool_) vkDestroyCommandPool(d_.device(), pool_, nullptr);
    }
    ComputeShots(const ComputeShots&) = delete;
    ComputeShots& operator=(const ComputeShots&) = delete;
    // `record` returns false to cancel (nothing is submitted). `signal` / `signalValue`: a timeline semaphore the submit signals (null = none).
    bool run(const std::function<bool(VkCommandBuffer)>& record, VkSemaphore signal = VK_NULL_HANDLE, uint64_t signalValue = 0) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(d_.device(), &ai, &cmd) != VK_SUCCESS) return false;
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        const bool keep = record(cmd);
        vkEndCommandBuffer(cmd);
        bool ok = false;
        if (keep) {
            VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
            cbi.commandBuffer = cmd;
            VkSemaphoreSubmitInfo sig{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
            sig.semaphore = signal;
            sig.value = signalValue;
            sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
            si.commandBufferInfoCount = 1;
            si.pCommandBufferInfos = &cbi;
            if (signal) {
                si.signalSemaphoreInfoCount = 1;
                si.pSignalSemaphoreInfos = &sig;
            }
            ok = d_.submitAndWait(d_.computeQueue(), si, "bake submit");
        }
        vkFreeCommandBuffers(d_.device(), pool_, 1, &cmd);
        return ok;
    }

private:
    gpu::Device& d_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
};

namespace {

void transferToCompute(VkCommandBuffer cmd) {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &di);
}

LightClass classOf(const core::Light& l) {
    return l.type == core::LightType::Directional ? LightClass::Sun : l.type == core::LightType::Sky ? LightClass::Sky : LightClass::Local;
}

} // namespace

struct LightingService::Epoch {
    struct Chunk {
        uint32_t slotBase = 0, count = 0;
        uint32_t passes = 0;
        bool ao = false;
        uint64_t version = 0, sent = 0, ack = 0;   // versions: bumped by a pass; the newest in a mailbox; the newest the renderer consumed
        ComposeJob rect;                           // the chunk's rectangle with its gutter
    };
    struct InFlight {
        uint64_t value = 0;
        std::vector<std::pair<uint32_t, uint64_t>> items;   // chunk, version
    };

    LightingInputs in;
    uint64_t inputsSerial = 0;
    core::LightScene lights;
    core::LightingClasses classes;
    std::vector<core::LightSurface> surfaces;
    std::shared_ptr<core::SolidSet> solids;
    std::unique_ptr<LiveLayout> live;
    std::unique_ptr<SampleStore> store;
    std::unique_ptr<Results> results;
    std::unique_ptr<Accel> accel;
    std::unique_ptr<LightBuffer> lightBuffer;
    std::unique_ptr<PageImages> slotMap;
    std::unique_ptr<JobList> aoJobs, directJobs, composeJobs;
    std::vector<Chunk> chunks;
    std::deque<InFlight> inflight;
    size_t cursor = 0, composeCursor = 0;
    uint32_t maxPasses = 32;
    bool hasClass[kLightClasses] = {false, false, false};
    ComposeParams compose;
    bool aoOn = false;
    uint32_t aoRays = 0;
    float aoRadius = 0, aoFalloff = 0;
    uint32_t convergedChunks = 0;
    bool failed = false;
    Clock::time_point refineStart = Clock::now();
    double buildSeconds = 0;
};

LightingService::LightingService(gpu::Device& device, LightLink& link, const LightingOptions& options) : device_(device), link_(link), options_(options) {
    if (!device.caps().lit()) {
        base::logInfo("LightingService: no ray tracing device, the lit modes are off");
        return;
    }
    if (device.computeQueue() == device.graphicsQueue()) {
        base::logWarning("LightingService: the device has no second queue for the bake, the lit modes are off");
        return;
    }
    if (!link.valid()) return;
    valid_ = true;
    thread_ = std::thread([this] { loop(); });
}

LightingService::~LightingService() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void LightingService::setInputs(LightingInputs inputs) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = std::move(inputs);
        hasPending_ = true;
        ++submittedInputs_;
        status_.converged = false;
    }
    wake_.notify_all();
}

LightingService::Status LightingService::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

bool LightingService::waitConverged(int timeoutMs) {
    std::unique_lock<std::mutex> lock(mutex_);
    return progress_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return finishedInputs_ >= submittedInputs_ && (status_.converged || !status_.error.empty()); });
}

bool LightingService::interrupted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hasPending_ || stop_;
}

void LightingService::loop() {
    std::unique_ptr<Epoch> epoch;
    passes_ = std::make_unique<Passes>(device_);
    if (!passes_->valid()) {
        base::logWarning("LightingService: the compute passes could not be made");
        std::lock_guard<std::mutex> lock(mutex_);
        status_.error = "the compute passes could not be made";
        finishedInputs_ = submittedInputs_;
        progress_.notify_all();
        return;
    }
    shots_ = std::make_unique<ComputeShots>(device_);
    for (;;) {
        LightingInputs inputs;
        bool have = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stop_) break;
            if (device_.lost()) {   // nothing can be baked any more: the inputs are answered with the reason, the GPU resources go
                hasPending_ = false;
                status_.building = false;
                status_.converged = false;
                status_.error = "the GPU device was lost (" + device_.lostReason() + ")";
                finishedInputs_ = submittedInputs_;
                progress_.notify_all();
                epoch.reset();
                wake_.wait_for(lock, std::chrono::milliseconds(100));
                continue;
            }
            const bool work = epoch && !epoch->failed && link_.wanted() && needsWork(*epoch);
            if (!hasPending_ && !work) {
                wake_.wait_for(lock, std::chrono::milliseconds(30));
                if (stop_) break;
            }
            if (hasPending_ && link_.wanted()) {
                inputs = std::move(pending_);
                hasPending_ = false;
                have = true;
            }
        }
        if (have) {
            uint64_t serial;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                serial = submittedInputs_;   // the inputs taken are the newest submitted (a newer one would have replaced them)
                status_.building = true;
                status_.converged = false;
                status_.error.clear();
            }
            link_.progress().building = true;
            epoch.reset();   // the old epoch's GPU resources go before the new ones are made
            std::string error;
            std::unique_ptr<Epoch> next;
            const bool ok = build(inputs, next, error);
            link_.progress().building = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                status_.building = false;
                if (ok && next) {
                    next->inputsSerial = serial;
                    status_.epoch = link_.progress().epoch.load();
                    status_.chunks = static_cast<uint32_t>(next->chunks.size());
                    status_.pages = static_cast<uint32_t>(next->live->layout().pageCount);
                    status_.samples = next->store->usedSlots();
                    status_.buildSeconds = next->buildSeconds;
                    status_.convergedChunks = 0;
                } else if (!ok && !error.empty()) {
                    status_.error = error;
                    finishedInputs_ = std::max(finishedInputs_, serial);
                    progress_.notify_all();
                }
            }
            if (ok && next) {
                epoch = std::move(next);
                epoch->refineStart = Clock::now();
                if (epoch->chunks.empty()) {   // nothing to light
                    std::lock_guard<std::mutex> lock(mutex_);
                    status_.converged = true;
                    finishedInputs_ = std::max(finishedInputs_, epoch->inputsSerial);
                    progress_.notify_all();
                }
            }
            continue;
        }
        if (epoch && !epoch->failed && link_.wanted() && needsWork(*epoch)) {
            if (!step(*epoch)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            bool done = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                status_.convergedChunks = epoch->convergedChunks;
                status_.refineSeconds = secondsSince(epoch->refineStart);
                done = !hasPending_ && !needsWork(*epoch) && epoch->inflight.empty();
                if (done) {
                    status_.converged = true;
                    finishedInputs_ = std::max(finishedInputs_, epoch->inputsSerial);
                    progress_.notify_all();
                }
            }
        }
    }
    vkQueueWaitIdle(device_.computeQueue());
}

// Something to do now: a chunk still refines, a chunk's newest version is in no mailbox, or the renderer consumed a mailbox the bookkeeping has not seen yet.
// Mailboxes waiting for the renderer are not work (the bake sleeps until it takes one).
bool LightingService::needsWork(const Epoch& e) const {
    for (const Epoch::Chunk& c : e.chunks)
        if (c.passes < e.maxPasses || c.version > c.sent) return true;
    return !e.inflight.empty() && e.inflight.front().value <= link_.mailbox().consumedBakeValue();
}

bool LightingService::build(const LightingInputs& in, std::unique_ptr<Epoch>& out, std::string& error) {
    const auto t0 = Clock::now();
    if (!in.snapshot || !in.snapshot->doc || !in.profile) {
        error = "no scene to light";
        return false;
    }
    const scene::SceneSnapshot& snap = *in.snapshot;
    auto e = std::make_unique<Epoch>();
    e->in = in;

    // ---- the lighting inputs of the snapshot
    e->classes = core::LightingClasses::fromProfile(*in.profile);
    core::Document coreDoc = doc::toCoreDocument(*snap.doc);
    const std::vector<core::ResolvedEntity> resolved = snap.prefabs ? snap.prefabs->flat() : std::vector<core::ResolvedEntity>();
    const core::LightingSource source{coreDoc, resolved, *in.profile, in.assetsRoot};
    core::LightScene allLights = core::lightSceneOf(source, skyCache_);
    if (in.filter) in.filter->apply(allLights.lights);
    e->lights = core::lightsOfStyle(allLights, 0);
    e->lights.playable = allLights.playable;
    for (core::Light& l : e->lights.lights) {
        if (l.sourceSize > 0) continue;
        if ((l.type == core::LightType::Point || l.type == core::LightType::Spot) && options_.defaultSourceSize > 0) l.sourceSize = options_.defaultSourceSize;
        else if (l.type == core::LightType::Directional && options_.defaultSunSize > 0) l.sourceSize = options_.defaultSunSize;
    }
    if (!solidCache_) solidCache_ = core::SolidSet::makeCache();
    e->solids = core::lightSolidsOf(source, solidCache_.get());
    {
        // the surfaces of the objects, in key order (the layout does not depend on it, the reproducibility of the sample numbers does)
        std::vector<const scene::ObjectKey*> keys;
        keys.reserve(snap.objects.size());
        for (const auto& [k, p] : snap.objects)
            if (p.surfaces && !p.surfaces->empty()) keys.push_back(&k);
        std::sort(keys.begin(), keys.end(), [](const scene::ObjectKey* a, const scene::ObjectKey* b) { return *a < *b; });
        const core::BspContentRules rules = core::BspContentRules::fromJson(in.profile->bsp);
        for (const scene::ObjectKey* k : keys)
            for (const core::LightSurface& s : *snap.objects.at(*k).surfaces)
                if (!rules.isSky(s.texture)) e->surfaces.push_back(s);
    }
    if (interrupted()) return false;

    // ---- level settings
    const core::LevelSettings& level = e->lights.level;
    {
        const int giPerPass = std::max(1, static_cast<int>(options_.direct.perPass));
        const int levelPasses = level.raysPerTexel > 0 ? std::clamp((level.raysPerTexel + giPerPass - 1) / giPerPass, 1, 4096) : options_.maxPasses;
        e->maxPasses = static_cast<uint32_t>(levelPasses);
    }
    e->compose.ambient[0] = static_cast<float>(e->lights.ambient.x);
    e->compose.ambient[1] = static_cast<float>(e->lights.ambient.y);
    e->compose.ambient[2] = static_cast<float>(e->lights.ambient.z);
    e->compose.aoActive = level.ao.active();
    e->compose.aoStrength = static_cast<float>(level.ao.strength);
    e->compose.aoPower = static_cast<float>(level.ao.power);
    e->compose.aoIntensity = static_cast<float>(level.ao.intensity);
    e->aoOn = level.ao.active() && level.ao.radius > 0;
    e->aoRays = static_cast<uint32_t>(std::max(1, level.ao.rays));
    e->aoRadius = static_cast<float>(level.ao.radius);
    e->aoFalloff = static_cast<float>(level.ao.falloff);
    for (const core::Light& l : e->lights.lights) e->hasClass[static_cast<size_t>(classOf(l))] = true;

    // ---- the layout over the whole level
    core::LightmapSettings ls;
    ls.playable = e->lights.playable;
    ls.lod = level.lod;
    e->live = std::make_unique<LiveLayout>(ls);
    e->live->buildAll(e->surfaces, e->classes);
    const core::LightmapLayout& layout = e->live->layout();
    e->compose.atlas = static_cast<uint32_t>(layout.settings.atlasSize);
    if (interrupted()) return false;

    // ---- samples
    const size_t chartCount = e->live->charts().size();
    const auto pieces = piecesByChart(layout, e->surfaces);
    std::vector<PreparedChart> prepared(chartCount);
    core::parallelFor(chartCount, 1, [&](size_t begin, size_t end) {
        for (size_t c = begin; c < end; ++c) prepared[c] = prepareChart(*e->live, static_cast<int>(c), pieces[c], e->solids.get(), options_.giScale);
    });
    uint64_t sampleCount = 0, gridCount = 0, chunkRows = 0;
    for (const PreparedChart& p : prepared) {
        sampleCount += p.count;
        gridCount += p.gridCount;
        chunkRows += p.chunks.size();
    }
    if (interrupted()) return false;
    const uint32_t slots = static_cast<uint32_t>(sampleCount + sampleCount / 8 + 4096);
    e->store = std::make_unique<SampleStore>(device_, slots, static_cast<uint32_t>(gridCount + 16), static_cast<uint32_t>(chunkRows + 16));
    e->results = std::make_unique<Results>(device_, slots);
    if (!e->store->valid() || !e->results->valid()) {
        error = "the sample buffers could not be allocated";
        return false;
    }
    if (!ring_) ring_ = std::make_unique<gpu::UploadRing>(device_, gpu::UploadRingOptions{128ull << 20, 2});
    {
        size_t next = 0;
        while (next < prepared.size()) {
            if (interrupted()) return false;
            if (!ring_->beginFrame(ringFrame_)) {
                error = "the upload ring is busy";
                return false;
            }
            const size_t before = next;
            const bool ok = shots_->run([&](VkCommandBuffer cmd) {
                while (next < prepared.size()) {
                    const PreparedChart& p = prepared[next];
                    const uint64_t need = p.samples.size() * 32ull + p.sub.size() * 4ull + p.grid.size() * 32ull + p.chunks.size() * 64ull + 4096;
                    if (ring_->usedBytes() + need > ring_->bytesPerFrame()) break;
                    if (p.count > 0 && !e->store->upload(cmd, *ring_, p, p.page)) {
                        e->failed = true;
                        break;
                    }
                    ++next;
                }
                transferToCompute(cmd);
                return true;
            });
            ring_->endFrame(++ringFrame_);
            if (!ok || e->failed || next == before) {
                error = "the samples did not fit the GPU buffers";
                return false;
            }
        }
    }

    // ---- acceleration structures
    e->accel = std::make_unique<Accel>(device_);
    if (!e->accel->valid() || !e->accel->update(buildAccelObjects(e->surfaces, e->classes, &layout))) {
        error = "the acceleration structures could not be built";
        return false;
    }
    const std::vector<float> packedLights = packLights(e->lights.lights);   // the records, then the sky tables (they are far more than the lights)
    e->lightBuffer = std::make_unique<LightBuffer>(device_, static_cast<uint32_t>(packedLights.size() * sizeof(float) / 128) + 16);
    if (!e->lightBuffer->valid() || !e->lightBuffer->set(packedLights, static_cast<uint32_t>(e->lights.lights.size()))) {
        error = "the lights did not fit";
        return false;
    }

    // ---- chunks, slot map
    const int pad = layout.settings.padding, atlas = layout.settings.atlasSize;
    for (size_t c = 0; c < chartCount; ++c) {
        const SampleStore::ChartEntry* ce = e->store->chart(static_cast<int>(c));
        if (!ce) continue;
        for (uint32_t k = 0; k < ce->chunkCount; ++k) {
            const GpuChunk& g = e->store->chunk(ce->firstChunk + k);
            Epoch::Chunk ch;
            ch.slotBase = g.slotBase;
            ch.count = g.slotCount;
            const int x = static_cast<int>(g.rectXY & 0xffffu), y = static_cast<int>(g.rectXY >> 16), w = static_cast<int>(g.rectWH & 0xffffu), h = static_cast<int>(g.rectWH >> 16);
            const int x0 = std::max(0, x - pad), y0 = std::max(0, y - pad), x1 = std::min(atlas, x + w + pad), y1 = std::min(atlas, y + h + pad);
            ch.rect = {g.page, static_cast<uint32_t>(x0), static_cast<uint32_t>(y0), static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)};
            if (ch.count > 0) e->chunks.push_back(ch);
        }
    }
    const uint32_t jobCapacity = static_cast<uint32_t>(e->chunks.size()) + 1;
    e->aoJobs = std::make_unique<JobList>(device_, jobCapacity);
    e->directJobs = std::make_unique<JobList>(device_, jobCapacity);
    e->composeJobs = std::make_unique<JobList>(device_, jobCapacity);
    JobList slotJobs(device_, jobCapacity);
    e->slotMap = std::make_unique<PageImages>(device_, atlas, VK_FORMAT_R32_UINT);
    if (!e->aoJobs->valid() || !e->directJobs->valid() || !e->composeJobs->valid() || !slotJobs.valid() || !e->slotMap->ensureLayers(std::max(1, layout.pageCount))) {
        error = "the job buffers could not be made";
        return false;
    }
    {
        std::vector<PassJob> all;
        all.reserve(e->chunks.size());
        for (const Epoch::Chunk& c : e->chunks) all.push_back({c.slotBase, c.count, 0});
        if (!slotJobs.set(all)) {
            error = "too many chunks";
            return false;
        }
        const bool ok = shots_->run([&](VkCommandBuffer cmd) { return passes_->recordSlotMap(cmd, *e->store, slotJobs, e->slotMap->view()); });
        passes_->resetDescriptors();
        if (!ok) {
            error = "the slot map could not be made";
            return false;
        }
    }
    if (interrupted()) return false;

    // ---- lit meshes for the renderer (published before the first mailbox of this epoch)
    auto set = std::make_shared<LitSet>();
    set->epoch = link_.progress().epoch.load() + 1;
    set->firstBakeValue = nextBakeValue_;
    set->pageCount = std::max(1, layout.pageCount);
    set->atlasSize = atlas;
    set->layout = std::make_shared<core::LightmapLayout>(layout);
    set->exposure = level.exposure > 0 ? static_cast<float>(level.exposure) : 1.f;
    set->brightness = static_cast<float>(level.brightness);
    {
        std::vector<const std::pair<const scene::ObjectKey, scene::ObjectProducts>*> items;
        for (const auto& kv : snap.objects)
            if (kv.second.render && kv.second.surfaces && !kv.second.surfaces->empty()) items.push_back(&kv);
        std::vector<std::shared_ptr<const scene::RenderMesh>> lit(items.size());
        core::parallelFor(items.size(), 8, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) lit[i] = makeLitMesh(*items[i]->second.render, *items[i]->second.surfaces, layout);
        });
        for (size_t i = 0; i < items.size(); ++i)
            if (lit[i]) set->objects[items[i]->first] = LitObject{items[i]->second.render, lit[i]};
    }
    e->buildSeconds = secondsSince(t0);
    link_.progress().epoch = set->epoch;
    link_.progress().chunks = static_cast<uint32_t>(e->chunks.size());
    link_.progress().pages = static_cast<uint32_t>(set->pageCount);
    link_.progress().samples = e->store->usedSlots();
    link_.progress().convergedChunks = 0;
    link_.publish(std::move(set));
    link_.notify();
    base::logInfo("LightingService: epoch " + std::to_string(link_.progress().epoch.load()) + ": " + std::to_string(e->surfaces.size()) + " surfaces, " + std::to_string(chartCount) +
                  " charts, " + std::to_string(e->chunks.size()) + " chunks, " + std::to_string(e->store->usedSlots()) + " samples, " + std::to_string(layout.pageCount) + " pages, " +
                  std::to_string(e->buildSeconds) + " s");
    out = std::move(e);
    return true;
}

bool LightingService::step(Epoch& e) {
    Mailbox& mailbox = link_.mailbox();

    // acknowledgements: the renderer consumed the mailbox with this bake value
    const uint64_t consumed = mailbox.consumedBakeValue();
    while (!e.inflight.empty() && e.inflight.front().value <= consumed) {
        const Epoch::InFlight& f = e.inflight.front();
        for (const auto& [chunk, version] : f.items) {
            Epoch::Chunk& c = e.chunks[chunk];
            if (f.value == consumed) c.ack = std::max(c.ack, version);
            else if (c.ack < version) c.sent = 0;   // dropped for a newer mailbox that did not carry it: send it again
        }
        e.inflight.pop_front();
    }

    // the batch: chunks that still refine, up to batchSamples
    std::vector<uint32_t> batch;
    uint64_t samples = 0;
    for (size_t n = 0; n < e.chunks.size() && samples < options_.batchSamples; ++n) {
        const size_t i = (e.cursor + n) % e.chunks.size();
        if (e.chunks[i].passes >= e.maxPasses) continue;
        batch.push_back(static_cast<uint32_t>(i));
        samples += e.chunks[i].count;
        e.cursor = (i + 1) % e.chunks.size();
    }
    const bool anyClass = e.hasClass[0] || e.hasClass[1] || e.hasClass[2];

    std::vector<PassJob> aoList, directList;
    for (uint32_t i : batch) {
        const Epoch::Chunk& c = e.chunks[i];
        if (e.aoOn && !c.ao) aoList.push_back({c.slotBase, c.count, 0});
        directList.push_back({c.slotBase, c.count, c.passes});
    }
    if (!aoList.empty() && !e.aoJobs->set(aoList)) return false;
    if (!directList.empty() && !e.directJobs->set(directList)) return false;

    // what goes into a mailbox: chunks whose newest version is not in one yet (a batch's chunks included)
    std::vector<uint32_t> send;
    std::vector<ComposeJob> rects;
    uint64_t entries = 0;
    {
        std::vector<char> inBatch(e.chunks.size(), 0);
        for (uint32_t i : batch) inBatch[i] = 1;
        for (size_t n = 0; n < e.chunks.size(); ++n) {
            const size_t i = (e.composeCursor + n) % e.chunks.size();
            const Epoch::Chunk& c = e.chunks[i];
            if (!(inBatch[i] || c.version > c.sent)) continue;
            const uint64_t area = static_cast<uint64_t>(c.rect.w) * c.rect.h;
            if (entries + area > mailbox.entriesPerSlot()) break;
            entries += area;
            send.push_back(static_cast<uint32_t>(i));
            rects.push_back(c.rect);
        }
    }
    if (batch.empty() && send.empty()) return false;

    int slot = -1;
    if (!send.empty()) {
        slot = mailbox.acquire();
        if (slot < 0 && batch.empty()) return false;   // nothing to refine and no mailbox free: wait for the renderer
    }
    const bool compose = slot >= 0;
    if (compose && !e.composeJobs->setCompose(rects)) {
        mailbox.abandon(slot);
        return false;
    }
    const uint64_t bakeValue = compose ? nextBakeValue_ : 0;
    const DirectParams& params = options_.direct;
    bool recorded = true;
    const bool ok = shots_->run(
        [&](VkCommandBuffer cmd) {
            if (!aoList.empty()) recorded = passes_->recordOcclusion(cmd, *e.store, *e.results, *e.accel, *e.aoJobs, e.aoRays, e.aoRadius, e.aoFalloff) && recorded;
            if (!directList.empty())
                for (uint32_t cls = 0; cls < kLightClasses; ++cls)
                    if (e.hasClass[cls]) recorded = passes_->recordDirect(cmd, *e.store, *e.results, *e.accel, *e.lightBuffer, *e.directJobs, static_cast<LightClass>(cls), params) && recorded;
            if (compose) recorded = passes_->recordCompose(cmd, *e.results, e.slotMap->view(), *e.composeJobs, mailbox.buffer(slot), e.compose) && recorded;
            return recorded;
        },
        compose ? mailbox.bakeTimeline() : VK_NULL_HANDLE, bakeValue);
    passes_->resetDescriptors();
    if (!ok) {
        if (slot >= 0) mailbox.abandon(slot);
        if (!recorded && !directList.empty() && !e.accel->tlas()) {
            // an empty scene (no occluders): there is nothing to trace, ambient only
            for (uint32_t i : batch) {
                e.chunks[i].passes = e.maxPasses;
                ++e.chunks[i].version;
                e.chunks[i].ao = true;
            }
            e.convergedChunks += static_cast<uint32_t>(batch.size());
            return true;
        }
        base::logWarning("LightingService: a batch failed, the lighting of this epoch stops");
        e.failed = true;
        std::lock_guard<std::mutex> lock(mutex_);
        status_.error = "a bake batch failed";
        finishedInputs_ = std::max(finishedInputs_, e.inputsSerial);
        progress_.notify_all();
        return false;
    }

    // bookkeeping
    for (uint32_t i : batch) {
        Epoch::Chunk& c = e.chunks[i];
        c.ao = true;
        ++c.version;
        c.passes = anyClass ? c.passes + 1 : e.maxPasses;   // without lights a chunk is done after its first compose
        if (c.passes >= e.maxPasses) ++e.convergedChunks;
    }
    link_.progress().convergedChunks = e.convergedChunks;
    link_.progress().passesDone += batch.size();
    if (compose) {
        Epoch::InFlight f;
        f.value = bakeValue;
        for (uint32_t i : send) {
            e.chunks[i].sent = e.chunks[i].version;
            f.items.emplace_back(i, e.chunks[i].version);
        }
        e.inflight.push_back(std::move(f));
        e.composeCursor = (static_cast<size_t>(send.back()) + 1) % e.chunks.size();
        ++nextBakeValue_;
        mailbox.commit(slot, static_cast<uint32_t>(entries), bakeValue);
        link_.notify();
    }
    return true;
}

} // namespace native::light
