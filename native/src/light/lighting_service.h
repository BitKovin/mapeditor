// light/lighting_service -- the bake thread of the live lighting: scene snapshot in, lit meshes and display texels out.
//
// Owns: one thread. For every set of inputs (a scene snapshot, the game profile, the assets root) it builds an EPOCH: the lighting inputs from the snapshot
// (surfaces without sky, solids, lights of style 0), the live layout over the whole level, the per-chart samples uploaded to a SampleStore, one BLAS per object,
// the result buffers, and the lit meshes (light/lit_mesh) which it publishes through the LightLink. Then it refines: waves of passes over every chunk in batches
// (ambient occlusion once, direct light per class, accumulation), composing the chunks that changed into a mailbox after each batch; it stops when every chunk
// reached `maxPasses` and starts again on new inputs. A newer set of inputs interrupts the epoch in progress (latest wins).
// Thread: setInputs / status from any thread; the GPU work runs on the compute queue from the bake thread only. The service refuses to run (valid() false) on a
// device without ray queries or without a second queue.
// Spec: 06_LIGHTING.md §2 - §8, 08_MILESTONES.md T6.4. Deviations: the whole level is rebuilt for every snapshot (M8 makes it incremental), batches go in layout
// order (T6.5 adds camera / visibility priority and the budget), bounces and light styles come later.
#pragma once

#include "scene/snapshot.h"   // first: core headers use `near` as a name

#include <core/derived.h>
#include <core/lighting_inputs.h>
#include <core/profile.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

#include "gpu/device.h"
#include "gpu/upload_ring.h"
#include "light/light_link.h"
#include "light/passes.h"

namespace native::light {

class ComputeShots;

struct LightingInputs {
    std::shared_ptr<const scene::SceneSnapshot> snapshot;
    std::shared_ptr<const core::GameProfile> profile;
    std::filesystem::path assetsRoot;
};

struct LightingOptions {
    int maxPasses = 32;                    // passes a chunk refines for (the level's rays per texel setting changes it)
    DirectParams direct;
    uint32_t batchSamples = 1u << 20;      // samples one submission works on
    double defaultSourceSize = 0.25;       // m: point / spot lights authored with source_size 0 (Core: LightingService::Options)
    double defaultSunSize = 0.5;           // degrees: the same for directional lights
    int giScale = 1;                       // fine GI grid (bounces are not computed yet)
};

class LightingService {
public:
    // `link` must outlive the service.
    LightingService(gpu::Device& device, LightLink& link, const LightingOptions& options = {});
    ~LightingService();
    LightingService(const LightingService&) = delete;
    LightingService& operator=(const LightingService&) = delete;

    bool valid() const { return valid_; }
    // The latest inputs win; the epoch in progress stops at its next step.
    void setInputs(LightingInputs inputs);

    struct Status {
        uint64_t epoch = 0;          // epochs built so far
        bool building = false;       // the layout / samples of an epoch are being made
        bool converged = false;      // every chunk of the latest epoch reached maxPasses and everything was composed
        uint32_t chunks = 0, convergedChunks = 0, pages = 0;
        uint64_t samples = 0;
        double buildSeconds = 0;     // the last epoch's inputs + layout + samples + acceleration structures
        double refineSeconds = 0;    // the last epoch's refinement so far
        std::string error;           // why the last epoch failed ("" = fine)
    };
    Status status() const;
    // Tests: waits until the latest inputs' epoch converged (or failed); false on timeout.
    bool waitConverged(int timeoutMs);

private:
    struct Epoch;
    void loop();
    bool build(const LightingInputs& in, std::unique_ptr<Epoch>& out, std::string& error);
    bool step(Epoch& e);
    bool needsWork(const Epoch& e) const;
    bool interrupted() const;

    gpu::Device& device_;
    LightLink& link_;
    LightingOptions options_;
    bool valid_ = false;

    mutable std::mutex mutex_;
    std::condition_variable wake_, progress_;
    LightingInputs pending_;
    bool hasPending_ = false;
    bool stop_ = false;
    Status status_;
    uint64_t submittedInputs_ = 0, finishedInputs_ = 0;
    uint64_t nextBakeValue_ = 1;      // bake thread only: the bake timeline value of the next compose

    // Bake thread only.
    std::unique_ptr<Passes> passes_;
    std::unique_ptr<ComputeShots> shots_;
    std::unique_ptr<gpu::UploadRing> ring_;
    uint64_t ringFrame_ = 0;
    core::SkyTableCache skyCache_;
    std::shared_ptr<core::SolidSet::Cache> solidCache_;
    std::thread thread_;
};

} // namespace native::light
