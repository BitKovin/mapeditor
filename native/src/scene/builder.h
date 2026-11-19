// scene/builder -- turns document versions into scene snapshots, rebuilding only what a change touches.
//
// Owns: SceneBuilder. submit(version, changes, prefabs) says what changed since the previous submit; the builder expands that into the
// set of brushes whose derived products can differ (the changed brushes; every solid brush a changed subtractive brush overlaps, at its old
// and its new box; every brush of an entity whose class changed; every brush of a prefab instance whose expansion changed), runs one job per
// affected brush on the job pool, and publishes a SceneSnapshot when all finished, or earlier (every 8 ms while jobs finish) with the
// finished objects on their new products and the rest on the previous ones. Products come from the Core's collectSurfacesFor, validated
// by an inputs hash in a DerivedCache, so undo / redo and moving a brush back hit the cache.
// Which core::Document: the builder owns ONE core::Document and patches the changed entities into it from the change set (a structural change
// -- an entity added or removed -- rebuilds it from the version); it is read only by the jobs, and only patched while none runs. A submit
// while jobs run cancels them (they stop at their next step), waits for the last one and then starts the next build from the merged changes.
// Thread: submit / snapshot / stats from any thread (the UI thread submits); jobs run on the pool; the publish listener is called on the
// thread that finished the last job (post to the UI thread there).
// Spec: 04_SCENE_AND_DERIVED.md §2 - §6, 08_MILESTONES.md T3.2, T3.3.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <vector>

#include "base/jobs.h"
#include "scene/cache.h"
#include "scene/snapshot.h"

namespace native::scene {

struct BuilderStats {
    uint64_t submits = 0;
    uint64_t builds = 0;          // builds started (a submit that arrives while jobs run is merged into the next build)
    uint64_t jobs = 0;            // brush jobs run to completion
    uint64_t computed = 0;        // collectSurfacesFor calls (cache misses of additive brushes)
    uint64_t cacheHits = 0;       // additive brushes served from the cache
    uint64_t abandoned = 0;       // jobs that stopped because a newer submit superseded them
    uint64_t reliefJobs = 0;      // relief jobs run to completion
    uint64_t reliefComputed = 0;  // buildRelief calls (cache misses)
    uint64_t reliefHits = 0;      // relief served from the cache
    uint64_t publishes = 0;
    uint64_t partialPublishes = 0;
};

class SceneBuilder {
public:
    using Listener = std::function<void(const std::shared_ptr<const SceneSnapshot>&)>;

    explicit SceneBuilder(base::JobPool& pool, size_t cacheBudgetBytes = size_t(2) << 30);
    ~SceneBuilder();   // supersedes the running build and waits for its jobs
    SceneBuilder(const SceneBuilder&) = delete;
    SceneBuilder& operator=(const SceneBuilder&) = delete;

    // `changes` = what differs from the version of the previous submit (doc::diff, or the change set of a history step); `all` or the first
    // submit builds everything. `prefabs` is the expansion of the version's instances (may be null).
    void submit(std::shared_ptr<const doc::DocumentVersion> version, const doc::ChangeSet& changes,
                std::shared_ptr<const doc::Expansion> prefabs = nullptr);

    // The latest published snapshot (null before the first publish).
    std::shared_ptr<const SceneSnapshot> snapshot() const;
    // True when nothing is building or waiting.
    bool idle() const;
    // Waits until idle (tests); false on timeout.
    bool waitIdle(int timeoutMs = 60000);

    BuilderStats stats() const;
    // The brushes the latest started build rebuilt (their products were fetched from the cache or recomputed), sorted. For tests and the status bar.
    std::vector<ObjectKey> lastAffected() const;
    // Called after every publish, on a worker thread.
    void setListener(Listener l);

    // The textures' environment for relief (T3.3). Without one no relief is built. Setting an environment (or one with another revision) rebuilds the
    // relief of every object of the current version.
    void setReliefEnv(ReliefEnv env);
    // Rebuilds the relief of everything (textures changed on disk).
    void refreshRelief();

    DerivedCache<ObjectKey, std::vector<core::LightSurface>, ObjectKey::Hash>& cache() { return cache_; }
    DerivedCache<ObjectKey, ReliefList, ObjectKey::Hash>& reliefCache() { return reliefCache_; }

private:
    struct BrushInfo;
    struct Build;
    using Index = std::unordered_map<ObjectKey, BrushInfo, ObjectKey::Hash>;

    struct Pending {
        std::shared_ptr<const doc::DocumentVersion> version;
        std::shared_ptr<const doc::Expansion> prefabs;
        doc::ChangeSet changes;
        bool reliefAll = false;   // rebuild the relief of every object (the texture environment changed)
    };

    void startBuildLocked(Pending p, std::vector<std::function<void()>>& afterUnlock);
    void patchCoreDocLocked(const Pending& p, const doc::ChangeSet& sinceBase);
    void runJob(const std::shared_ptr<Build>& build, size_t index);
    void afterSurfacesLocked(const std::shared_ptr<Build>& build, std::vector<std::function<void()>>& afterUnlock);
    void runReliefJob(const std::shared_ptr<Build>& build, size_t index);
    void queueRefreshLocked(std::vector<std::function<void()>>& afterUnlock);
    void finishLocked(std::vector<std::function<void()>>& afterUnlock);
    void produce(const Build& build, const ObjectKey& key, ObjectProducts& out);
    void jobFinished(const std::shared_ptr<Build>& build, size_t index, ObjectProducts products, bool abandoned);
    void reliefJobFinished(const std::shared_ptr<Build>& build, size_t index, std::shared_ptr<const ReliefList> relief, uint64_t hash, bool abandoned);
    void publishLocked(const std::shared_ptr<Build>& build, bool complete, std::vector<std::function<void()>>& afterUnlock);

    base::JobPool& pool_;
    DerivedCache<ObjectKey, std::vector<core::LightSurface>, ObjectKey::Hash> cache_;
    DerivedCache<ObjectKey, ReliefList, ObjectKey::Hash> reliefCache_;
    ReliefEnv env_;

    mutable std::mutex mutex_;
    std::condition_variable idleCv_;
    std::atomic<uint64_t> generation_{0};
    bool shuttingDown_ = false;
    int outstanding_ = 0;                          // jobs submitted to the pool and not finished
    std::optional<Pending> pending_;               // submits that arrived while jobs ran, merged
    std::shared_ptr<Build> current_;               // the build whose jobs are outstanding
    std::shared_ptr<const SceneSnapshot> base_;    // the last complete snapshot
    std::shared_ptr<const SceneSnapshot> published_;
    std::shared_ptr<Index> baseIndex_;             // index of base_
    doc::ChangeSet sinceBase_;                     // changes accumulated since base_ (the builds that were superseded included)
    doc::ChangeSet sincePublish_;                  // changes since the last publish of any kind (becomes snapshot.sincePrevious)
    std::vector<ObjectKey> lastAffected_;
    Listener listener_;
    BuilderStats stats_;
    std::chrono::steady_clock::time_point lastPublish_;

    // The core::Document the jobs read, and what it was made from.
    core::Document coreDoc_;
    std::unordered_map<std::string, size_t> coreIndex_;
    std::vector<core::ResolvedEntity> coreResolved_;
    std::shared_ptr<const doc::Expansion> appliedExpansion_;
    std::shared_ptr<const doc::DocumentVersion> appliedVersion_;

    // Counters the jobs bump without the lock.
    std::atomic<uint64_t> computed_{0}, hits_{0}, jobsDone_{0}, abandoned_{0}, reliefJobs_{0}, reliefComputed_{0}, reliefHits_{0};
};

} // namespace native::scene
