#pragma once
#include "core/lightbake.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Vulkan compute path of the baker (internal). The Vulkan loader is opened at run time (vulkan-1.dll / libvulkan.so.1) with
// vendored headers, so building needs no SDK and a machine without Vulkan simply falls back to the CPU evaluator.
namespace core::gpu {

struct Result {
    bool ok = false;
    std::string device;                  // chosen physical device
    std::string error;                   // why it did not run (no loader, no device, limits ...)
    std::vector<std::string> validation; // validation layer warnings / errors
};

// True when the baking options and scene ask for ray traced shadows: shadows enabled, occluders present, some light casts shadows.
// The CPU evaluator and the GPU path use exactly this rule, so they stay comparable.
bool needsRayTracing(const LightScene& lights, const OccluderMesh* occluders, const BakeOptions& options);

// Thread safe (calls are serialised). Evaluates all lights for all samples; `out` gets 4 floats per sample (same contract as evaluateDirectCpu).
// When needsRayTracing(), the occluders go into a hardware acceleration structure (VK_KHR_ray_query); a device without it fails
// with `error` set, and the caller falls back to the CPU evaluator.
// `db` (optional): receives the direct light weighted by each light's bounce power (4 floats per sample). Needs the ray traced variant
// (needsRayTracing()); otherwise the call fails with `error` set and the caller uses the CPU evaluator.
// `indirectOut` (optional): receives the indirect part of `out` alone (4 floats per sample; zeros when there are no bounces).
// Per sample, TexelSamples::seed / passOffset and BakeOptions::sampleOffset select the random draws exactly like the CPU evaluator.
// `gi` (optional, bounces only): the last bounce is gathered on that finer grid (buildGiSamples); `giOut` receives the raw grid values (one per entry)
// and `out` / `indirectOut` the plain per-texel average of them, like evaluateGiCpu.
// BakeOptions::superSamples lights every texel at its sub-texel positions (TexelSamples::sub) when shadows are traced, like directCpu.
Result evaluateDirect(const TexelSamples& samples, const LightScene& lights, const OccluderMesh* occluders, const BakeOptions& options,
                      std::vector<float>& out, std::vector<float>* db = nullptr, std::vector<float>* indirectOut = nullptr,
                      const GiSamples* gi = nullptr, std::vector<float>* giOut = nullptr);


// ---- resident session for the live baker -----------------------------------------------------------------------------------
// evaluateDirect() builds everything per call (buffers, acceleration structure, descriptors) and the live baker calls it for a few
// hundred texels at a time, so the GPU mostly waits. A LiveSession keeps the scene resident instead: samples, occluders (BLAS/TLAS
// built once), lights, and the bounce source atlas S. A step then only records dispatches and waits once.
// The sample order inside a session is the order of the TexelSamples it was created from (the live baker lays them out chunk by chunk,
// so a chunk is one contiguous range).
struct LiveSessionConfig {
    int areaSamples = 8, skySamples = 16, giSamples = 16;  // new rays per texel and pass (the first two per sub-texel position)
    int superSamples = 8;      // sub-texel positions the direct light is evaluated at (BakeOptions::superSamples); needs TexelSamples::sub
    double albedo = 1.0;
    // Fine GI grid: the resident TexelSamples holds the base samples [0, count - giEntries) followed by `giEntries` grid entries (position, normal and
    // texel in the fine atlas of size giAtlas); the gather then runs over the entries named by Job::giFirst / giCount instead of the base samples.
    size_t giEntries = 0;
    int giAtlas = 0;
    bool gather = true;        // also run the bounce gather (needs lightmap coordinates in the occluders)
    bool validation = false;   // Vulkan validation layer (tests)
};

class LiveSession {
public:
    virtual ~LiveSession() = default;
    struct Job {
        uint32_t first = 0, count = 0;  // contiguous range of resident samples (one chunk)
        uint32_t giFirst = 0, giCount = 0;  // with config.giEntries: the chunk's contiguous range of grid entries (resident indices)
        uint32_t offset = 0;            // first index of this range's random draws (pass * samples per pass)
    };
    // Replaces the lights. False when there are more lights than the session was sized for (the caller creates a new session).
    virtual bool setLights(const LightScene& lights) = 0;
    // Direct light for the ranges (d), the bounce-weighted direct light (db) and, with config.gather, one gather pass (ind) that reads the
    // atlas as it was after the previous step. All outputs: 4 floats per sample, ranges back to back in `jobs` order; `ind` holds one value per
    // grid entry (giCount per job) when config.giEntries > 0.
    virtual bool run(const std::vector<Job>& jobs, std::vector<float>& d, std::vector<float>& db, std::vector<float>* ind) = 0;
    // The same work as run(), in up to `maxParts` submissions that the GPU works through one after the other: `done(first, count, results)` is called on
    // this thread for jobs [first, first + count) as soon as their part is finished, while the GPU goes on with the next parts. results[k] points at the
    // first value of job first + k (valid during the call only): d and db 4 floats per sample, ind (the gather, null with Indirect::None) 4 floats per
    // sample or per grid entry. Every part reads the bounce source atlas() as it was when runParts() was called: write it only after it returned.
    // The default runs everything with run() and calls `done` once.
    enum class Indirect { None, PerSample, PerEntry };
    struct JobResult { const float* d = nullptr; const float* db = nullptr; const float* ind = nullptr; };
    using PartDone = std::function<void(size_t first, size_t count, const std::vector<JobResult>& results)>;
    virtual bool runParts(const std::vector<Job>& jobs, Indirect indirect, size_t maxParts, const PartDone& done) {
        (void)maxParts;
        std::vector<float> d, db, ind;
        if (!run(jobs, d, db, indirect != Indirect::None ? &ind : nullptr)) return false;
        std::vector<JobResult> r(jobs.size());
        size_t at = 0, gat = 0;
        for (size_t k = 0; k < jobs.size(); ++k) {
            r[k].d = d.data() + at * 4;
            r[k].db = db.data() + at * 4;
            if (indirect != Indirect::None) r[k].ind = ind.data() + (indirect == Indirect::PerEntry ? gat : at) * 4;
            at += jobs[k].count;
            gat += jobs[k].giCount;
        }
        if (!jobs.empty()) done(0, jobs.size(), r);
        return true;
    }
    // Baked ambient occlusion (shaders/lightmap_ao.comp, ray queries) of the ranges: `rays` rays per sample that look `radius` far, a hit weighs
    // (1 - d / radius)^falloff; one occluded fraction 0..1 per sample, ranges back to back in `jobs` order (Job::offset and the grid ranges are unused). The same directions and weights
    // as LiveBaker's CPU occlusion. False when it failed (`error()`) or this session cannot do it: then the caller traces on the CPU.
    virtual bool occlusion(const std::vector<Job>& jobs, double radius, int rays, double falloff, std::vector<float>& occ) {
        (void)jobs; (void)radius; (void)rays; (void)falloff; (void)occ;
        return false;
    }
    // The resident bounce source S: pages * atlas * atlas texels of (rgb, valid). Write only (it may be write-combined memory).
    virtual float* atlas() = 0;
    // The resident samples themselves (write only, host visible device memory; null when the session cannot give them out): sampleCount() entries of
    // position xyz + the random id (as the bits of w), normal xyz + 0, and the texel index. A caller whose sample set is too big to keep resident (the light
    // volume's probes: tens of millions of samples) rewrites them batch by batch and runs a Job over the part it filled, so the acceleration structure,
    // textures and atlas are built once. Only between runs.
    virtual float* samplePositions() { return nullptr; }
    virtual float* sampleNormals() { return nullptr; }
    virtual uint32_t* sampleTexels() { return nullptr; }
    virtual size_t sampleCount() const { return 0; }
    virtual const std::string& error() const = 0;
    virtual const std::string& device() const = 0;
    virtual const std::vector<std::string>& validation() const = 0;
    virtual double lastGpuSeconds() const = 0;  // submit to completion of the last run(); of the last runParts(), the GPU's own time when it can measure it
};

// Null (with `error` set) when there is no ray query device, no occluders, or a limit is exceeded. `atlasInit` (pages * atlas * atlas * 4
// floats, may be empty) is copied into the resident atlas.
// Buffers kept for the next live session (see BufferPool in gpu_bake.cpp), summed over the Vulkan contexts. For tests and diagnostics.
struct BufferPoolStats { size_t hits = 0, misses = 0, pooledBuffers = 0, pooledBytes = 0; };
BufferPoolStats liveBufferPoolStats();

std::unique_ptr<LiveSession> createLiveSession(const TexelSamples& resident, const OccluderMesh& occluders, const LightScene& lights,
                                               const LiveSessionConfig& config, const std::vector<float>& atlasInit, std::string& error);

// The Vulkan instance, device and compute pipelines are created on the first call and kept for the next ones (one per ray tracing /
// validation combination). A failed call drops its context. This destroys them all (tests that count validation messages, shutdown).
void releaseContexts();

}  // namespace core::gpu
