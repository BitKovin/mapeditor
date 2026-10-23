#pragma once
#include <limits>
#include "core/albedo.h"
#include "core/denoise.h"
#include "core/lightbake.h"
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace core {

namespace gpu {
class LiveSession;
struct LiveSessionConfig;
}  // namespace gpu

// Real-time, progressive lighting (REQUIREMENTS §18.1, DESIGN §6.6).
//
// The lightmap is cut into CHUNKS (tiles of the atlas). Each chunk remembers its world bounds and how many refinement PASSES it has
// had. An edit marks only the chunks it can affect as stale; `step()` then refines the stalest / nearest chunks within a texel budget,
// drawing NEW random samples every pass and accumulating them into the running estimate, so quality improves over iterations and the
// editor can show a first rough result within one step.

struct Aabb {
    Vec3 mn{1e300, 1e300, 1e300}, mx{-1e300, -1e300, -1e300};
    bool valid() const { return mn.x <= mx.x; }
    void add(const Vec3& p) {
        mn = {std::min(mn.x, p.x), std::min(mn.y, p.y), std::min(mn.z, p.z)};
        mx = {std::max(mx.x, p.x), std::max(mx.y, p.y), std::max(mx.z, p.z)};
    }
    Aabb expanded(double r) const { Aabb a = *this; a.mn = a.mn - Vec3{r, r, r}; a.mx = a.mx + Vec3{r, r, r}; return a; }
    bool intersects(const Aabb& o) const {
        return valid() && o.valid() && mn.x <= o.mx.x && o.mn.x <= mx.x && mn.y <= o.mx.y && o.mn.y <= mx.y && mn.z <= o.mx.z && o.mn.z <= mx.z;
    }
    bool contains(const Vec3& p) const { return p.x >= mn.x && p.x <= mx.x && p.y >= mn.y && p.y <= mx.y && p.z >= mn.z && p.z <= mx.z; }
    Vec3 center() const { return (mn + mx) * 0.5; }
};

constexpr double kMaxLiveAlbedo = 0.95;

// float -> IEEE 754 half, round to nearest even; negative and NaN become 0, values above 65504 saturate (light is never negative).
uint16_t floatToHalf(float v);
float halfToFloat(uint16_t h);

struct LiveSettings {
    int chunkTexels = 16;           // chunk edge in atlas texels
    int maxPasses = 32;             // a chunk stops refining after this many passes
    int areaSamplesPerPass = 8;     // new shadow rays per texel, light and sub-texel position per pass (lights with a size, rect)
    int skySamplesPerPass = 16;     // new sky rays per texel and sub-texel position per pass
    // Super sampling of the direct light (BakeOptions::superSamples): sub-texel positions every texel is lit at in each pass. 8 anti-aliases
    // shadow edges at the cost of 8x the direct rays per pass; lower it (1 = off) when passes get too slow on weak hardware.
    int directSupersample = 8;
    int giSamplesPerPass = 16;      // new gather rays per texel per pass
    bool bounces = true;            // progressive indirect light: every pass adds one more bounce of depth
    // The bounce light is gathered on a giScale x giScale times finer grid (1, 2 or 4; every texel is split into cells, see buildGiSamples), kept at
    // that resolution, denoised there and averaged back to the texel. It costs giScale^2 times the gather rays per pass and memory for the grid.
    int giScale = 2;
    // Reflectance of every surface for bounces. The live estimate feeds on itself (every pass gathers the previous passes' light), so it must
    // lose energy on each bounce or stale light circulates forever: values above kMaxLiveAlbedo are clamped.
    double albedo = 0.7;
    int indirectWindow = 8;         // the indirect estimate is an exponential moving average over about this many passes
    double indirectRadius = 40;     // m: margin around an edit in which bounce light is refreshed
    int indirectRefreshPasses = 4;  // chunks only indirectly affected fall back to at most this many passes
    // Texels per step when the GPU session runs (the caller's budget is for the CPU path). 0 = use the caller's budget. The baker adapts
    // it between a quarter of and eight times this value so a step takes about 10 to 30 ms: big enough to fill the GPU, short enough for the editor.
    size_t gpuTexelsPerStep = 0;
    // GPU load. A step is one submission; the budget adapts so a step takes about gpuTargetStepMs (clamped to 400 ms: far below a driver's
    // timeout) between a quarter of and gpuMaxStepScale times gpuTexelsPerStep. Bigger steps keep the GPU busy longer per submission and wait
    // less for the CPU; the price is a less fine-grained editor response.
    double gpuTargetStepMs = 100;
    int gpuMaxStepScale = 32;
    // Denoise the indirect light with Intel Open Image Denoise (core/denoise.h). The progressive estimate keeps refining raw (the bounce source
    // S must stay the unfiltered average); once every chunk of a page reached maxPasses, `denoiseNext` filters that page's indirect light and the
    // page is shown with the filtered version until one of its chunks refines again. Direct light is never filtered.
    bool denoiseIndirect = true;
    int denoiseQuality = 1;         // 0 fast, 1 balanced, 2 high (core::denoise::Quality)
    bool useTextures = true;        // bounce light is tinted by the textures of the surfaces it hits (needs LiveSettings::albedoLoader; the service provides it)
    bool useGpu = true;             // direct light of a step on the Vulkan ray query path when available (CPU BVH otherwise)
    bool gpuValidation = false;     // Vulkan validation layer for those calls (tests)
    LightingClasses classes = LightingClasses::defaults();
    // Where the bounce light gets the colour of the surfaces it hits: texture by path -> decoded image (null: not found, the surface is white). Empty = every
    // surface has the constant `albedo` only. The Core's service fills it from the game's texture folder (AlbedoLibrary).
    AlbedoLoader albedoLoader;
};

struct LiveChunk {
    int page = 0, tx = 0, ty = 0;       // tile coordinates inside the page
    std::vector<uint32_t> samples;      // indices into the sample set (their fine GI entries: LiveBaker::giEntriesOf)
    Aabb bounds;                        // world bounds of its samples
    int passes = 0;                     // 0 = stale / never refined
    bool resetIndirect = false;         // its next pass replaces the indirect estimate instead of averaging into it (old bounce light must not survive)
};

class LiveBaker {
public:
    explicit LiveBaker(const LiveSettings& settings = {});
    ~LiveBaker();
    LiveBaker(const LiveBaker&) = delete;
    LiveBaker& operator=(const LiveBaker&) = delete;

    // (Re)builds samples and chunks from a layout. Texels whose world position and normal are unchanged keep their results (and
    // their pass count, unless they lie in `changedRegions`, the world boxes an edit touched); everything else starts stale.
    // `solids` (optional): texels buried inside solid brushes are not baked.
    void setGeometry(const LightmapLayout& layout, const std::vector<LightSurface>& surfaces, const std::vector<Aabb>& changedRegions = {},
                     std::shared_ptr<const SolidSet> solids = nullptr);

    // Replaces the lights. Lights are compared by (instance, id); only chunks inside the influence of added / removed / modified lights
    // go stale (global lights: directional, sky, ambient: all chunks). Chunks within `indirectRadius` of such a change are refreshed
    // for bounce light only.
    void setLights(const LightScene& lights);
    // Which classes block light / receive baked light (the game profile may change while the editor runs). Applies from the next setGeometry.
    void setClasses(const LightingClasses& classes) { cfg_.classes = classes; }
    // Refinement targets from the level settings (REQUIREMENTS §22 P2). More passes: converged chunks go on refining; fewer: chunks beyond count as
    // done. Another super sampling restarts the direct light everywhere. False when nothing changed.
    bool setQuality(int maxPasses, int directSupersample);
    // Texture source of the bounce colours (applies from the next setGeometry).
    void setAlbedoLoader(AlbedoLoader loader) { cfg_.albedoLoader = std::move(loader); }

    struct StepResult {
        std::vector<int> chunks;  // refined this step (indices), in processing order
        size_t texels = 0;
    };
    // Refines chunks until `texelBudget` texels were processed (at least one chunk). Stale chunks first, then the least refined; ties
    // (and the order inside the stale group) by distance to `camera` when given.
    StepResult step(const Vec3* camera, size_t texelBudget);

    // Time accounting since construction: wall time inside step(), of which `gpuSeconds` was spent waiting for the GPU.
    struct Stats { double stepSeconds = 0, gpuSeconds = 0; size_t steps = 0, texels = 0; };
    // Where the last setGeometry() spent its time (seconds), for latency diagnosis.
    struct GeometryTimings { double inherit = 0, samples = 0, samplesGenerate = 0, samplesAssemble = 0, occluders = 0, bvh = 0, gi = 0, chunks = 0, atlas = 0, stale = 0, total = 0; };
    const GeometryTimings& geometryTimings() const { return geomTimings_; }
    // Where the last prepareGpu() session creation spent its time (seconds), 0 when no session was created.
    double lastSessionSeconds() const { return lastSessionSeconds_; }
    // ... of which this much went into assembling the resident sample arrays handed to the session (CPU side).
    double lastResidentSeconds() const { return lastResidentSeconds_; }
    const Stats& stats() const { return stats_; }
    bool gpuActive() const { return session_ != nullptr; }
    // Replaces how GPU sessions are made (tests inject a CPU stand-in). Default: gpu::createLiveSession.
    using SessionFactory = std::function<std::unique_ptr<gpu::LiveSession>(const TexelSamples&, const OccluderMesh&, const LightScene&, const gpu::LiveSessionConfig&,
                                                                          const std::vector<float>& atlas, std::string& error)>;
    void setSessionFactory(SessionFactory f);

    size_t chunkCount() const { return chunks_.size(); }
    const LiveChunk& chunk(int i) const { return chunks_[static_cast<size_t>(i)]; }
    size_t staleChunks() const;       // passes == 0
    // The chunk shows something other than black: it was refined, or it still holds values inherited from before an edit.
    bool chunkHasData(int i) const;
    int minPasses() const;            // over all chunks (0 when something is stale)
    bool converged() const;           // every chunk reached maxPasses
    size_t sampleCount() const { return samples_.count(); }
    // Which evaluator produced the direct light of the last step: the GPU name, or "cpu". `gpuError()` says why the GPU was not used.
    const std::string& evaluator() const { return evaluator_; }
    const std::string& gpuError() const { return gpuError_; }
    const std::vector<std::string>& gpuValidation() const { return gpuValidation_; }

    // Current estimate as pages (gutter filled), for display or transport.
    LightmapPages snapshot() const;
    // Chunks whose pixels changed since the last call (refined, denoised, or holding texels that are new / moved / still unsent from before a rebuild):
    // exactly the tiles a client has to be sent. Cleared by the call.
    std::vector<int> takeDirtyChunks();
    bool hasDirtyChunks() const;
    void markAllChunksDirty();   // e.g. the exposure changed
    // Rectangle (padding included, clipped to the atlas) of chunk `i` in its page: what to re-send after the chunk was refined.
    struct Rect { int page = 0, x = 0, y = 0, w = 0, h = 0; };
    Rect chunkRect(int i) const;
    // RGBA16F pixels (IEEE half floats, linear, as baked: no exposure, no clamping, no encoding: lightmaps keep their full range, REQUIREMENTS §9) of
    // a rectangle of a page, row by row. Texels without a sample get the average of their baked neighbours (gutter); where there are none the pixel is
    // all zero (alpha 0 = nothing here, alpha 1.0 = a value). Values beyond the half range (65504) saturate there.
    std::vector<uint16_t> renderTile(const Rect& r) const;
    // Current value of one sample (direct + indirect, both raw).
    Vec3 value(size_t sample) const;
    // What is displayed: like value(), but with the denoised indirect light where its page has been denoised (renderTile and snapshot use this),
    // times the baked ambient occlusion factor (aoFactor).
    Vec3 shownValue(size_t sample) const;
    // Baked ambient occlusion (level settings `_ao_*`, REQUIREMENTS §22 P2): the factor the sample's light is multiplied by (1 = open, or AO off,
    // or not computed yet), and the raw occluded fraction (NaN = not computed). Occlusion depends on geometry, the radius and the ray count only: it
    // is computed when the sample's chunk is next refined (hardware ray traced on the resident GPU session, else on the CPU bvh with the same rays),
    // kept for unchanged texels, recomputed within the radius of an edit.
    float aoFactor(size_t sample) const;
    float occlusion(size_t sample) const { return sample < occ_.size() ? occ_[sample] : std::numeric_limits<float>::quiet_NaN(); }
    // Which evaluator produced the occlusion of the last step: the GPU name, or "cpu" (empty: none computed yet).
    const std::string& aoEvaluator() const { return aoEvaluator_; }

    // Denoises the indirect light of ONE page whose chunks all reached maxPasses and that changed since its last denoise (OIDN, ~10..500 ms).
    // `chunks` (optional) receives the chunks of that page: their tiles must be re-sent. False when nothing was pending, the denoiser is switched
    // off, or it failed (`denoiseError()` says why; an unavailable library is not retried).
    bool denoiseNext(std::vector<int>* chunks = nullptr);   // beginDenoise + runDenoise + finishDenoise on the calling thread
    // The same in three steps, so that the filter (hundreds of ms) can run on another thread while this object keeps being used on its own one:
    // begin copies what the filter needs and marks the page "in flight", run is a pure function of the job, finish stores the result unless the page
    // was refined or the geometry replaced in between (then it is dropped and the page is pending again).
    struct DenoiseJob {
        int page = -1;
        bool fine = false;
        uint64_t geometry = 0;
        TexelSamples ps;
        std::vector<uint32_t> idx;
        std::vector<int> ids;
        std::vector<float> vals;
    };
    bool beginDenoise(DenoiseJob& job);
    static denoise::Result runDenoise(DenoiseJob& job, int quality);
    int denoiseQuality() const { return cfg_.denoiseQuality; }
    bool finishDenoise(DenoiseJob& job, const denoise::Result& result, std::vector<int>* chunks = nullptr);
    bool denoisePending() const;                 // denoiseNext() would do work
    size_t denoisedPages() const;                // pages currently shown denoised
    const std::string& denoiseError() const { return denoiseError_; }
    double denoiseSeconds() const { return denoiseSeconds_; }
    const TexelSamples& samples() const { return samples_; }
    const LightmapLayout& layout() const { return layout_; }
    // What light volume probes gather from (DESIGN §6.7): the current occluders, lights and bounce source S (rgb + valid per atlas texel).
    const OccluderMesh& occluders() const { return mesh_; }
    const TriangleBvh* bvh() const { return bvh_.get(); }
    const LightScene& lights() const { return lights_; }
    const std::vector<float>& bounceSource() const { return atlasS_; }
    const LiveSettings& settings() const { return cfg_; }

private:
    LiveSettings cfg_;
    LightmapLayout layout_;
    TexelSamples samples_;
    OccluderMesh mesh_;
    std::unique_ptr<TriangleBvh> bvh_;
    LightScene lights_;
    bool haveLights_ = false;
    std::vector<LiveChunk> chunks_;
    std::vector<float> direct_, db_, indirect_;   // 4 floats per sample
    std::vector<float> occ_;                      // ambient occlusion per sample: occluded fraction 0..1, NaN = not computed (see aoFactor)
    void computeOcclusion(const std::vector<int>& chunks);  // fills the unknown occ_ of these chunks' samples (GPU when it can, else CPU)
    bool occlusionOnGpu(const std::vector<int>& chunks);    // the same on the resident session; false = not done (no session, or it failed)
    void forgetOcclusion(const Aabb& box);                  // occlusion of the samples in `box` is unknown again; their chunks refine once more
    std::vector<float> atlasS_;                   // bounce source S = Db + I per atlas texel (rgb) + validity (a)
    GiSamples gi_;                                // the fine GI grid (scale 1 = none: the gather runs on the samples themselves)
    std::vector<float> indirectHi_, indirectHiDn_;  // indirect light per grid entry (moving average) and its denoised copy (valid where pageState_ is 1)
    std::vector<uint32_t> giFirst_;               // first resident grid entry of each chunk (GPU layout: base samples, then the grid entries chunk by chunk)
    bool fineGi() const { return gi_.scale > 1 && gi_.count() > 0; }
    TexelSamples giSamplesOf(const LiveChunk& chunk) const;   // the chunk's grid entries as a sample set (atlas = fine atlas)
    size_t giEntryCount(const LiveChunk& chunk) const;
    void refreshIndirect(const LiveChunk& chunk);            // indirect_ of the chunk's samples = mean of their grid entries
    // Moving average of one sample's grid entries towards `values` (one value per entry, read from `cursor` on, which advances), then indirect_[sample]
    // = their mean.
    void accumulateFine(size_t sample, const float* values, size_t& cursor, float k);
    std::vector<float> indirectDn_;               // denoised indirect light, 4 floats per sample (valid where pageState_ is 1)
    std::vector<char> pageState_;                 // per page: 0 = raw indirect shown (needs a denoise once converged), 1 = denoised, 2 = denoise failed (retry after it refines), 3 = a denoise of it is in flight
    std::string denoiseError_;
    double denoiseSeconds_ = 0;
    bool denoiseUnavailable_ = false;
    uint64_t geometryGen_ = 0;                    // counts setGeometry() calls: sample indices of an older one mean nothing now
    bool denoiseWanted() const { return cfg_.denoiseIndirect && cfg_.bounces && !denoiseUnavailable_ && !chunks_.empty(); }
    TexelSampleCache texelCache_;                 // what every surface piece contributes, kept between edits
    std::vector<char> chunkDirty_;                // per chunk: its pixels changed since takeDirtyChunks()
    std::vector<int32_t> sampleAt_;               // sample index per atlas texel (pages * atlas * atlas), -1 = none

    // Refines the given chunks: one direct-light evaluation for all of them (GPU or CPU), then per chunk accumulation + bounce gather.
    void refineBatch(const std::vector<int>& ids);
    bool refineOnGpu(const std::vector<int>& ids, int perPass);
    bool prepareGpu();
    BakeOptions bakeOptions() const;
    std::unique_ptr<gpu::LiveSession> session_;  // resident GPU state of the current geometry (null: CPU, or not created yet)
    bool sessionGather_ = false, sessionLightsDirty_ = false;
    std::vector<uint32_t> chunkFirst_;           // first resident sample of each chunk (chunks are contiguous ranges)
    size_t gpuBudget_ = 0;
    SessionFactory factory_;
    Stats stats_;
    GeometryTimings geomTimings_;
    double lastSessionSeconds_ = 0, lastResidentSeconds_ = 0;
    TexelSamples samplesOf(const LiveChunk& chunk) const;
    std::string evaluator_ = "cpu", gpuError_, aoEvaluator_;
    std::vector<std::string> gpuValidation_;
    bool gpuDisabled_ = false;  // a GPU call failed: stay on the CPU until geometry or lights change
    void markStale(const Aabb& direct, bool directToo, double indirectRadius);
    void invalidateSource(LiveChunk& c);   // zero the bounce source S of a chunk (CPU atlas and resident GPU atlas): stale light must not be gathered
    void markAllStale();
    void rebuildAtlasSource();
};

// World boxes of surface pieces that differ between two surface sets (added, removed, moved): input for LiveBaker::setGeometry.
std::vector<Aabb> diffSurfaces(const std::vector<LightSurface>& before, const std::vector<LightSurface>& after);

}  // namespace core
