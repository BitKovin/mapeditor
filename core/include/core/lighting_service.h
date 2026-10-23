#pragma once
#include "core/light_style_analysis.h"
#include "core/light_styles.h"
#include "core/lightvol.h"
#include "core/live_bake.h"
#include "core/scene.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace core {

// Keeps the editor's lighting live (REQUIREMENTS §18.1): owns the LiveBaker, notices document changes, refines the lightmap and produces the
// protocol messages (protocol.h: lit_mesh, lightmap_tiles, lighting_status). Pure data in, strings out, so it is unit-testable without sockets.
//
// Two ways to drive it, sharing one engine:
//  * tick(budget): synchronous, does pending work for about `budget` seconds on the calling thread and returns the messages (tests, tools).
//  * startWorker(sink): a bake thread owns the baker. The caller thread only takes a snapshot of the document when it changed (flushInputs());
//    a newer snapshot interrupts the bake in progress after its current step (latest wins, nothing queues up), and results leave through the
//    sink as soon as they exist, a few times per second per chunk, instead of when a tick ends.
//
// What the clients hold is mirrored in the service (`Mirror`): tiles are only sent when their pixels differ from it, a new lit mesh keeps the
// clients' pages (`keepPages`) when the atlas has the same shape, and a Shell that connects later is served from the mirror.
class LightingService {
public:
    struct Options {
        LightmapSettings layout;
        LiveSettings live;
        double exposure = 1.0;
        size_t texelsPerStep = 1500;  // chunks are refined in steps of about this many texels between clock checks (CPU path)
        // Seconds of refinement per tick() the router asks for (synchronous mode). The GPU path wants this well above one step
        // (gpuTargetStepMs), otherwise the GPU idles while the CPU encodes and sends tiles.
        double tickBudgetSeconds = 0.12;
        // Changed tiles are sent at most this often (ms) while refinement runs; the last ones always go out when it stops.
        double flushIntervalMs = 25;
        // Render and encode lightmap tiles for clients. The export bakes without any client to send them to (it reads the bakers' pages at the end):
        // there, re-rendering every refined chunk after every step cost more than the light itself.
        bool tiles = true;
        // Point / spot lights whose `source_size` is 0 are razor sharp shadow casters (and every texel step shows as a stair). The live view treats them as
        // spheres of this radius (m) instead, and directional lights with 0 as a sun of this angular diameter (degrees). 0 = use the entities as authored.
        // The map's own non-zero source sizes are never changed.
        double defaultSourceSize = 0.25;
        double defaultSunSize = 0.5;
        // A new lit mesh replaces the geometry the Shell shows, so it is held back until the new surfaces have light. The very first one is sent when
        // this fraction of the chunks has a result; after that, when no chunk is without one (the Shell keeps its pages, so inherited chunks are
        // already lit). `maxHoldSeconds` bounds the wait; everything converged also releases it.
        double firstSendFraction = 0.10;
        double maxHoldSeconds = 3.0;
        // Light volumes (DESIGN §6.7): probes are baked after the lightmap converged, `lightvolProbesPerStep` at a time between clock checks.
        bool lightvolEnabled = true;
        // Multiplies the refinement pass target and the rays per light volume probe after the level settings are applied: the BSP export bakes at the
        // level's bake quality multiplier (`_bake_quality`, REQUIREMENTS §22 P2 / §25 E7); the live view runs at 1.
        double qualityScale = 1.0;
        LightvolSettings lightvol;
        size_t lightvolProbesPerStep = 8;
        Options() {
            layout.texelsPerMeter = 8;  // retro look, and quick to bake
            layout.atlasSize = 1024;
            live.chunkTexels = 16;
            live.maxPasses = 24;
            live.gpuTexelsPerStep = 262144;  // big submissions on the GPU path (adapts towards live.gpuTargetStepMs); the CPU path keeps texelsPerStep
        }
    };
    // Options from a JSON file (the keys are listed in DESIGN §6.6; unknown / missing keys keep the defaults, values are clamped to sane ranges).
    // A missing or damaged file gives the defaults; `warning` (optional) says what was wrong.
    static Options loadOptions(const std::filesystem::path& file, std::string* warning = nullptr);
    // Writes the defaults to `file` unless it exists, so there is something to edit. False when it could not be written.
    static bool writeDefaultOptions(const std::filesystem::path& file);
    // Every key of lighting.json with the values `o` stands for (what loadOptions reads back).
    static nlohmann::json optionsJson(const Options& o);
    double tickBudgetSeconds() const { return opt_.tickBudgetSeconds; }
    explicit LightingService(Scene& scene, const Options& options = Options());
    ~LightingService();
    LightingService(const LightingService&) = delete;
    LightingService& operator=(const LightingService&) = delete;

    bool enabled() const { return enabled_; }
    // Synchronous mode: work is pending (a rebuild, a re-tile, chunks that have not reached their pass count, unsent tiles). Always false with a worker.
    bool busy() const;
    void setEnabled(bool on);
    void setExposure(double exposure);       // re-sends every tile that looks different
    void setCamera(const Vec3& camera);
    void markDirty() { dirty_ = true; }      // the document (or a prefab, the profile) changed
    // A texture's properties changed (e.g. its transparency, REQUIREMENTS §23): the faces using it are baked again although the document did not change.
    void textureChanged(const std::string& texture) { { std::lock_guard<std::mutex> lock(texM_); changedTextures_.insert(texture); } dirty_ = true; }
    // Receives one line when a refinement run converges (timing, evaluator); optional. Called on the bake thread when there is one.
    void setLog(std::function<void(const std::string&)> log) { log_ = std::move(log); }
    // Replaces how GPU sessions are made (tests and benchmarks inject a stand-in). Call before lighting is enabled.
    void setSessionFactory(LiveBaker::SessionFactory f);

    // ---- synchronous mode ----
    // Does pending work for about `budgetSeconds` and returns the messages for every session. Does nothing (empty) while a worker runs.
    std::vector<std::string> tick(double budgetSeconds);

    // ---- bake thread ----
    // Messages leave through the sink (any thread: post them to the thread that owns the sockets). The receiver calls delivered() once a batch was
    // handled; while two batches are undelivered the thread holds further tiles back (they are sent, merged, a moment later).
    using Sink = std::function<void(std::vector<std::string>&&)>;
    void startWorker(Sink sink);
    void stopWorker();                       // joins the thread; also done by the destructor
    bool threaded() const { return worker_.joinable(); }
    void delivered();
    // Call from the thread that owns the Scene after it may have changed (the Router does after every message): takes the snapshot the bake thread
    // works on. No-op without a worker or when nothing changed.
    void flushInputs();

    // Everything a Shell that connects now needs (empty unless enabled and a lit mesh was sent): the mirror of what the others hold.
    std::vector<std::string> fullState();
    // The status message: live in synchronous mode; with a worker, the last one the bake thread produced (it owns the numbers).
    nlohmann::json statusJson() const;
    // Synchronous mode only (the bake thread owns the bakers otherwise; with a worker, read these only after stopWorker()).
    const LiveBaker& baker() const { return baker_; }  // the base layer (style 0)
    // Light styles (light_styles.h): the style layers that exist (always 0 first) and their bakers.
    std::vector<int> styles() const;
    const LiveBaker* layerBaker(int style) const;
    const LightStyleTable& styleTable() const { return styleTable_; }
    const std::vector<std::string>& styleWarnings() const { return styleWarnings_; }
    int geometryRevision() const { return rev_; }
    // Light volume state (tests, status): the baked grid is complete and has been published.
    bool lightvolReady() const { return volDone_; }
    // The light volume still has work or messages pending (synchronous mode and tests; not part of busy(), which only covers the lightmap).
    bool lightvolPending() const { return enabled_ && volPending(); }
    const LightvolGrid& lightvol(int style = 0) const { static const LightvolGrid none; const auto it = vols_.find(style); return it == vols_.end() ? none : it->second; }
    const std::string& lightvolDevice() const { return volDevice_; }

    // Where the time of the last edit went, in milliseconds (also part of statusJson()).
    struct Timings {
        double surfaces = 0, layout = 0, diff = 0, geometry = 0, lights = 0;  // applying a snapshot
        double session = 0;                                                    // GPU session creation (inside the first step after a rebuild)
        double steps = 0, tiles = 0, denoise = 0;                              // accumulated since the last snapshot
        double firstTiles = 0, converged = 0;                                  // since the snapshot was applied
    };
    const Timings& timings() const { return timings_; }

private:
    struct Inputs {
        std::vector<LightSurface> surfaces;
        LightingClasses classes;
        std::shared_ptr<SolidSet> solids;
        LightScene lights;
        AlbedoLoader albedoLoader;   // made on the thread that owns the Scene (it reads the game profile)
        std::set<std::string> changedTextures;  // textures whose properties changed since the last snapshot
    };
    std::mutex texM_;
    std::set<std::string> changedTextures_;
    using Emit = std::function<void(std::vector<std::string>&&)>;
    using Interrupted = std::function<bool()>;

    Scene& scene_;
    Options opt_;
    LiveSettings baseLive_;      // opt_.live as configured (lighting.json): the level settings override parts of it
    LightvolSettings baseVol_;   // the same for opt_.lightvol
    LiveBaker baker_;
    LiveBaker::SessionFactory sessionFactory_;
    std::atomic<bool> enabled_{false}, dirty_{false}, statusDue_{false};
    std::atomic<double> exposure_{1.0};
    // Level settings (REQUIREMENTS §22 P2): the exposure the Shell / lighting.json asked for, overridden by the level's `_cc_exposure` when set;
    // the level's brightness offset (display); the pass target in use (lighting.json's, or the level's `_rays_per_texel`).
    std::atomic<double> baseExposure_{1.0}, brightness_{0.0};
    std::atomic<int> maxPasses_{0};
    LevelSettings level_;
    void applyLevel(const LevelSettings& level);  // bake side, from applyInputs
    std::atomic<int> rev_{0};
    std::atomic<uint64_t> enableGen_{0};  // counts every switch on / off, so a quick off-on is not missed by the bake side
    std::function<void(const std::string&)> log_;

    // bake side state (the bake thread, or the caller of tick())
    bool built_ = false;
    uint64_t seenEnableGen_ = 0;    // enableGen_ as the bake side last saw it
    bool meshPending_ = false;      // a new lit mesh is waiting to be released (see Options)
    bool clientHasMesh_ = false;    // a lit mesh was sent since lighting was last switched on
    bool flushPending_ = false;     // a flush was held back (undelivered batches)
    // A snapshot that was just applied may not be superseded before its first pass was sent (and a held mesh released), otherwise a drag, whose edits
    // come faster than a rebuild takes, would show nothing until the last edit. Capped by kFirstPassGuardMs.
    bool firstPassPending_ = false;
    bool wasConverged_ = true;
    std::string lastDenoiseError_;
    std::chrono::steady_clock::time_point holdStart_, lastFlush_, editStart_, lastEmit_;
    LiveBaker::Stats runStart_;
    std::vector<LightSurface> prevSurfaces_;
    LightingClasses prevClasses_;
    uint64_t prevSolids_ = 0;
    std::shared_ptr<const SolidSet> prevSolidSet_;   // the solids of the geometry in the bakers (the light volume samples them)
    bool prevStyled_ = false;                        // the last snapshot had lights of more than one style
    Timings timings_;
    bool firstTilesSent_ = false;

    // what the clients hold
    mutable struct Mirror {
        std::mutex m;
        bool valid = false;
        int rev = -1, atlas = 0, pageCount = 0;
        std::string mesh;                          // the lit_mesh message
        std::string styles;                        // the light_styles message
        std::vector<int> layerStyles;              // the style layers that existed when the mesh was released (0 first)
        std::map<int, std::vector<std::vector<uint16_t>>> pages;  // per style layer: RGBA16F (halves), as the Shell has them; a layer appears with its first tile
        std::string lightvol, entityLight;         // the last light volume grid / entity light messages ('' = none)
        std::string status;                        // last status message
    } mirror_;

    // hand-over to the bake thread
    std::thread worker_;
    std::mutex m_;
    std::condition_variable cv_;
    std::atomic<uint64_t> epoch_{0};    // bumped by everything that should interrupt the bake in progress
    bool quit_ = false, hasPending_ = false;
    Inputs pending_;
    Vec3 camera_;
    bool haveCamera_ = false;
    Sink sink_;
    std::atomic<int> inFlight_{0};

    // The denoiser of a converged page runs on its own thread while the bake thread keeps taking edits (bake thread only, except the task's thread).
    struct DenoiseTask {
        LiveBaker::DenoiseJob job;
        denoise::Result result;
        int quality = 2;
    };
    std::thread denoiseThread_;
    std::unique_ptr<DenoiseTask> denoiseTask_;
    int denoiseStyle_ = 0;           // the layer the task belongs to
    bool denoiseDiscard_ = false;    // that layer was replaced meanwhile: drop the result
    bool denoiseInFlight_ = false;
    std::atomic<bool> denoiseFinished_{false};
    void startDenoise(int style, LiveBaker& baker);
    void finishDenoise();

    Inputs captureInputs();
    void applyInputs(Inputs&& in, const Emit& emit);
    bool work(double budgetSeconds, const Emit& emit, const Interrupted& interrupted);
    void flush(const Emit& emit, bool force);
    bool meshReady() const;
    void releaseMesh(std::vector<std::string>& out);
    void goIdle();
    size_t tilesFor(int style, LiveBaker& baker, const std::vector<int>& chunks, std::vector<std::string>& out);
    size_t flushDirty(std::vector<std::string>& out);   // the changed tiles of every layer
    void allChunksOf(const LiveBaker& baker, std::vector<int>& ids) const;
    std::string litMeshMessage(bool keepPages) const;
    std::string lightStylesMessage() const;
    nlohmann::json statusLive() const;   // from the baker: bake side only
    std::string statusMessage();
    void workerMain();
    void deliver(std::vector<std::string>&& messages);
    void emitError(const Emit& emit, const std::string& what);

    // ---- light style layers: one more LiveBaker per style that has lights reaching some chart (light transport is linear: every layer is an
    // independent bake with only that style's lights, the engine sums them with the style's animated value). Bake side only.
    struct StyleLayer { int style = 0; std::unique_ptr<LiveBaker> baker; };
    std::vector<StyleLayer> layers_;
    LightStyleTable styleTable_;
    std::vector<std::string> styleWarnings_;
    uint64_t styleSig_ = 0;
    bool allConverged() const;
    bool anyDenoisePending() const;
    size_t totalChunks() const;
    size_t staleChunks() const;
    bool anyDirtyChunks() const;
    std::vector<std::pair<int, LiveBaker*>> bakers();                      // [(0, base), (style, layer) ...]
    std::vector<std::pair<int, const LiveBaker*>> bakers() const;

    // ---- light volumes (bake side only; what the clients hold is in the mirror)
    std::map<int, LightvolGrid> vols_;       // the published grids, one per style layer (same geometry)
    std::map<int, LightvolGrid> volsWork_;   // finished grids of the run in progress
    std::vector<int> volQueue_;              // the styles of this run, and the one being baked
    size_t volQueuePos_ = 0;
    LightvolGrid volBase_;                   // the grid every style layer starts from
    std::vector<char> volSolid_;
    std::unique_ptr<ProbeBaker> probes_;     // the layer being baked (holds pointers into a baker: reset before any baker's geometry changes)
    bool volStale_ = false, volRunning_ = false, volDone_ = false, volSendDue_ = false, entityLightDue_ = false;
    int volRev_ = 0;
    std::string volDevice_;
    std::string volGpuError_;
    std::shared_ptr<AlbedoLibrary> albedoLib_;   // decoded textures of the bounce colours (rebuilt when the game profile changes; caller thread only)
    std::string albedoKey_;
    AlbedoLoader albedoLoader();
    bool volPending() const { return opt_.lightvolEnabled && built_ && (volStale_ || volRunning_ || volSendDue_ || entityLightDue_); }
    // Returns true while there is more to do. Sends the grid / entity light messages when they are due and nothing is held back.
    bool workLightvol(double budgetSeconds, std::chrono::steady_clock::time_point t0, const Emit& emit, const Interrupted& interrupted);
    std::string lightvolGridMessage() const;
    std::string entityLightMessage() const;
    void sendLightvol(bool grid, std::vector<std::string>& out);
    LightvolSettings volSettings() const;
};

}  // namespace core
