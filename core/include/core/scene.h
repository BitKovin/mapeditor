#pragma once
#include "core/document.h"
#include "core/entdef.h"
#include <memory>
#include <mutex>
#include "core/lighting_classes.h"
#include "core/lightmap_uv.h"
#include "core/lights.h"
#include "core/prefab.h"
#include "core/relief_geometry.h"
#include "core/profile.h"
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <unordered_map>

namespace core {

// The authoritative open document plus everything Core does with files.
//
// Responsibilities (DESIGN.md §3): validate and adopt documents the Shell
// sends, open/save them, load prefab files, and keep a canonical raw-JSON copy
// so editor-only fields Core doesn't understand survive round trips. It does
// NOT edit geometry -- that is the Shell's job.
//
// No networking or message formats in here: see Router for that.
class ReliefSet;

class Scene {
public:
    struct UpdateResult {
        bool ok = false;
        uint64_t rev = 0;
        std::string error;
    };

    // All paths given to Scene are relative to `assetsRoot` and sandboxed to
    // it: absolute paths and ".." escapes are rejected (documents are
    // untrusted input, and prefab `file` fields come from them).
    explicit Scene(std::filesystem::path assetsRoot);

    // Loads, validates and adopts the document at `path`. Throws
    // std::runtime_error with a readable message on any failure, leaving the
    // current document untouched.
    void openDocument(const std::string& path);

    // Writes the canonical JSON atomically (temp file + rename). Empty `path`
    // means "the current path". Returns the normalized path written; that
    // becomes the current path ("save as").
    std::string saveDocument(const std::string& path);

    // The single edit entry point (DESIGN.md §10): validate and adopt a whole
    // updated document. Never throws; failure is reported in the result and
    // leaves the current document untouched.
    UpdateResult applyUpdate(const nlohmann::json& docJson);

    // Game profile (game_profile.json in the assets root) and the entity definitions it points at.
    // Throws if game_profile.json is malformed; a missing or damaged .ent only yields warnings.
    void loadGameProfile();
    const GameProfile& profile() const { return profile_; }
    // Receive-baked-light / block-light classes: the profile's lists plus worldspawn, func_group, func_detail.
    LightingClasses lightingClasses() const { return LightingClasses::fromProfile(profile_); }
    const EntityDefinitions& entityDefinitions() const { return defs_; }
    const std::filesystem::path& assetsRoot() const { return assetsRoot_; }

    // Points Core at another assets folder: loads its profile and definitions, drops the prefab cache and
    // re-opens the current map there (or starts an empty document when the map does not exist in the new
    // folder). Throws, changing nothing, if `root` is not a folder or its game_profile.json is broken.
    // Unsaved edits are discarded -- the Shell asks first.
    void setAssetsRoot(const std::filesystem::path& root);

    // Files and folders directly inside `relDir` ("" = the assets root), sandboxed like every other path.
    // {"path": normalized, "entries": [{"name", "dir"}]}, folders first, then by name; dot-files hidden.
    nlohmann::json listDirectory(const std::string& relDir) const;

    // Detects edits made outside the editor to game_profile.json or the .ent file.
    std::string configFingerprint() const;
    // Re-reads profile + definitions. Throws (keeping the old ones) if game_profile.json is now malformed.
    void reloadGameProfile();
    nlohmann::json entityDefsJson() const { return defs_.toJson(); }

    // Boolean results for the current document (csg.h). Empty "carved" when nothing is subtractive.
    // Includes prefab instances (their entities are resolved on demand) and decals.
    nlohmann::json derivedGeometryJson();
    bool hasDerivedGeometry();

    // Lightmap UV layout of the current document (lightmap_uv.h): final carved surfaces of every lightmapped class
    // (game_profile.json `lighting`, defaults otherwise), prefab instances included.
    LightmapLayout lightmapLayout(const LightmapSettings& settings = {});
    // The inputs of a bake: final surfaces of the document (prefabs resolved) and its light entities. Sky faces (REQUIREMENTS §28 K4: no lightmap,
    // never block light) are left out unless `withSky` (the BSP export draws them).
    std::vector<LightSurface> lightSurfaces(bool withSky = false);
    /// The relief of textures as geometry, as the Shell sent it (relief_geometry.h); attached to the surfaces `lightSurfaces()` returns. True when it changed.
    bool setReliefGeometry(const nlohmann::json& message) { return relief_.assign(message); }
    // Volumes of light-blocking brushes (lightmap texels buried inside them are skipped).
    std::shared_ptr<SolidSet> lightSolids();
    // The light entities; a sky light takes its light from the level's skybox (REQUIREMENTS §28 K12) when the level has one.
    LightScene lightScene();

    // True once the in-memory document differs from what was last opened or saved.
    bool dirty() const { return dirty_; }

    uint64_t revision() const { return rev_; }
    const std::string& currentPath() const { return currentPath_; }

    // {"type":"document_sync","rev":N,"path":"...","doc":{...canonical...}}
    nlohmann::json documentSyncJson() const;

    // Raw JSON of every prefab file reachable from the current document
    // (recursively), keyed by normalized path. A file that can't be loaded is
    // reported as {"error": "..."} so the Shell can show a placeholder.
    nlohmann::json prefabLibraryJson();

    // Raw JSON of one prefab file (any file inside the assets root, whether or not the current
    // document uses it) so the Shell can preview it before it is placed. Throws if it can't be loaded.
    nlohmann::json readPrefabJson(const std::string& path);

    // Writes `doc` as the prefab file `path` (inside the assets root, must end in .map.json), atomically, replacing any file there.
    // The document is validated first (shape, and no prefab cycle through itself); throws, writing nothing, when it is not valid.
    // Returns the normalized path.
    std::string writePrefab(const std::string& path, const nlohmann::json& docJson);

    // Cheap change detector for the prefab library: paths + mtimes of every
    // reachable prefab file. No file contents are read or serialized.
    std::string libraryFingerprint();

private:
    ReliefSet relief_;
    struct CachedPrefab {
        nlohmann::json raw;
        Document typed;
        std::filesystem::file_time_type mtime;
    };

    std::filesystem::path resolveSandboxed(const std::string& normalizedRelPath) const;
    void resetToEmptyDocument();
    const CachedPrefab& loadPrefab(const std::string& normalizedPath);
    struct ResolvedCache;
    // Throws when the candidate is not valid. `seed`: receives the flattened prefab content the check had to make anyway (see resolvedInside).
    void validateCandidate(const Document& candidate, const std::string& rootPath, ResolvedCache* seed = nullptr);
    void collectReachable(const Document& doc, std::set<std::string>& out);

    // The prefab content of the open document, flattened (instances only). Every derived product (derived geometry, light surfaces, solids, lights) starts
    // from it and flattening the 1000+ instances of a big map takes ~70 ms, so it is made once per revision and kept while the prefab files it came from
    // are unchanged.
    const std::vector<ResolvedEntity>& resolvedInside();
    struct ResolvedCache {
        bool valid = false;
        uint64_t rev = 0;
        std::string path;
        std::vector<std::pair<std::string, std::filesystem::file_time_type>> deps;  // prefab files it was made from, with their times
        std::vector<ResolvedEntity> entities;
    } resolved_;

    std::shared_ptr<SolidSet::Cache> solidCache_;  // the volumes of the brushes between two lightSolids() (see SolidSet::Cache)

    std::filesystem::path assetsRoot_;
    nlohmann::json docJson_;  // canonical copy: exactly what the Shell sent
    Document doc_;            // typed read-only view of docJson_
    std::string currentPath_;
    uint64_t rev_ = 0;
    GameProfile profile_;
    EntityDefinitions defs_;
    bool dirty_ = false;

    // Keyed by normalized path; entries are reloaded when the file's mtime
    // changes, so editing a prefab on disk is picked up on the next access.
    std::unordered_map<std::string, CachedPrefab> prefabCache_;

    // The sky light's tables of the last skybox used, kept while its files are unchanged (sky.h).
    std::shared_ptr<const SkyRadiance> skyTable(const std::string& texture, std::string& warning);
    std::mutex skyMutex_;
    std::string skyKey_, skyWarning_;
    std::shared_ptr<const SkyRadiance> sky_;
};

}  // namespace core
