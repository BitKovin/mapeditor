// doc/prefabs -- prefab files and the expansion of prefab instances.
//
// Owns: PrefabLibrary (the parsed prefab files of the assets folder, cached by normalized path, each load gets a version number; a
// stat check finds files changed on disk and reloads them) and PrefabResolver (expands the instances of a DocumentVersion with the Core's
// resolver core::resolvePrefabs, one instance at a time, cached by the instance node pointer plus the versions of every prefab file the
// instance read). So editing something else never re-resolves an instance, an edited instance re-resolves only itself, and a changed prefab
// file re-resolves exactly the instances that read it. Problems (missing file, parse error, cycle, too deep) are reported per instance and
// leave the other instances alone; nothing throws.
// Qt-free. Thread: any thread; the library and the resolver lock internally (loading a file happens on the calling thread, outside the
// lock), so the main window runs them on the job pool.
// Spec: 03_DOCUMENT.md §7, 08_MILESTONES.md T2.6.
#pragma once

#include <core/document.h>
#include <core/prefab.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "doc/document.h"

namespace native::doc {

struct PrefabFile {
    std::string path;                           // normalized, assets-relative, forward slashes
    std::shared_ptr<const core::Document> doc;  // null when it could not be loaded
    std::string error;                          // why (empty when doc is set)
    uint64_t version = 0;                       // new number every time the file is (re)loaded
    int64_t stampTime = 0;                      // last write time and size when it was loaded (change detection)
    uint64_t stampSize = 0;
    bool inMemory = false;                      // set by setMemory (tests): never reloaded from disk
};

class PrefabLibrary {
public:
    explicit PrefabLibrary(std::filesystem::path assetsRoot = {});

    // Another folder: the cache is dropped (versions keep counting up, so nothing stale can look current).
    void setAssetsRoot(const std::filesystem::path& root);
    std::filesystem::path assetsRoot() const;

    // The file at `path` (assets-relative; normalized here), loaded on first use. Never null: errors are in `error`.
    std::shared_ptr<const PrefabFile> get(const std::string& path);
    // The version of an already loaded file, 0 when it is not cached. Cheap (no file access).
    uint64_t versionOf(const std::string& path) const;
    std::vector<std::string> cachedPaths() const;
    bool empty() const;

    // Looks at every cached file on disk and reloads the ones whose time or size changed, that appeared or that vanished; returns their
    // normalized paths. Files set with setMemory are skipped.
    std::vector<std::string> refresh();

    // A prefab that exists only in memory (tests, generated prefabs). Replaces a loaded one (new version).
    void setMemory(const std::string& path, core::Document doc);
    void setMemoryError(const std::string& path, std::string error);

    uint64_t loadCount() const { return loads_; }   // how many times a file was read from disk (tests)

private:
    std::shared_ptr<const PrefabFile> load(const std::string& normalized);
    void store(std::shared_ptr<const PrefabFile> f);

    mutable std::mutex mutex_;
    std::filesystem::path root_;
    std::unordered_map<std::string, std::shared_ptr<const PrefabFile>> files_;
    std::atomic<uint64_t> loads_{0};
};

// The expansion of one instance of the document.
struct InstanceExpansion {
    std::string instanceId;
    std::shared_ptr<const std::vector<core::ResolvedEntity>> entities;   // root space; never null
    std::vector<std::string> problems;                                   // empty when everything resolved
    std::map<std::string, uint64_t> files;                               // every prefab file read (normalized path -> version)
    std::string rootPath;                                                // the document path the cycle check was seeded with
};

struct Expansion {
    uint64_t documentVersion = 0;
    std::vector<std::shared_ptr<const InstanceExpansion>> instances;     // parallel to DocumentVersion::instances

    std::vector<std::string> problems() const;                           // every instance's problems, prefixed with the instance id
    size_t entityCount() const;
    std::vector<core::ResolvedEntity> flat() const;                      // all resolved entities, instance by instance
    // The ids of the instances that read one of `files` (normalized paths).
    std::vector<std::string> instancesUsing(const std::vector<std::string>& files) const;
};

class PrefabResolver {
public:
    explicit PrefabResolver(PrefabLibrary& library, int maxDepth = 16) : library_(library), maxDepth_(maxDepth) {}

    // Expands every instance of `version` (loading the prefab files it needs). Cached results are reused while the instance node and
    // the files it read are unchanged.
    std::shared_ptr<const Expansion> expand(const DocumentVersion& version);

    uint64_t resolveCount() const { return resolves_; }   // how many instances were resolved (not served from the cache); tests

private:
    struct Entry {
        std::shared_ptr<const InstanceNode> node;          // keeps the pointer from being reused for another node
        std::shared_ptr<const InstanceExpansion> result;
    };
    std::shared_ptr<const InstanceExpansion> resolveOne(const InstanceNode& node, const std::string& rootPath);

    PrefabLibrary& library_;
    int maxDepth_;
    std::mutex mutex_;                                     // one expansion at a time
    std::unordered_map<const InstanceNode*, Entry> cache_;
    std::atomic<uint64_t> resolves_{0};
};

} // namespace native::doc
