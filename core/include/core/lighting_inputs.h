#pragma once
// The inputs of a bake made from a document, without a Scene: final surfaces (prefabs resolved, models, sky left out), the volumes of the light-blocking
// brushes and the light entities (a sky light carries its skybox tables). Scene's lightSurfaces / lightSolids / lightScene call these; the native editor's scene
// builder calls them on the document it owns (native_rewrite/06_LIGHTING.md §2, 08_MILESTONES.md T5.1).
// Thread: the functions are pure over their arguments; SkyTableCache and SolidSet::Cache are internally synchronised / single-owner as documented there.
#include "core/document.h"
#include "core/lighting_classes.h"
#include "core/lightmap_uv.h"
#include "core/lights.h"
#include "core/prefab.h"
#include "core/profile.h"
#include "core/relief_geometry.h"
#include "core/sky.h"
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace core {

// The sky light tables of the last skybox used, kept while the skybox files (names, sizes, times) are unchanged. Safe to call from several threads.
class SkyTableCache {
public:
    // `warning` receives why the sky is uniform (no image / not decodable); empty when the tables are fine or the texture is empty.
    std::shared_ptr<const SkyRadiance> get(const std::filesystem::path& assetsRoot, const std::string& texture, std::string& warning);

private:
    std::mutex mutex_;
    std::string key_, warning_;
    std::shared_ptr<const SkyRadiance> sky_;
};

// What the inputs are made from. All references must outlive the call.
struct LightingSource {
    const Document& doc;
    const std::vector<ResolvedEntity>& prefabs;   // the prefab instances flattened (Scene::resolvedInside)
    const GameProfile& profile;
    const std::filesystem::path& assetsRoot;      // models, skyboxes
    const ReliefSet* relief = nullptr;            // attached to the surfaces when it holds anything
};

// Final surfaces of the document and of its static models. Sky faces (REQUIREMENTS §28 K4: no lightmap, never block light) are left out unless `withSky`.
std::vector<LightSurface> lightSurfacesOf(const LightingSource& source, bool withSky = false);
// Volumes of the light-blocking brushes; `cache` (optional) keeps the volumes of unchanged brushes between two calls (SolidSet::Cache).
std::shared_ptr<SolidSet> lightSolidsOf(const LightingSource& source, SolidSet::Cache* cache = nullptr);
// The light entities; a sky light takes its light from the level's skybox (REQUIREMENTS §28 K12) when the level has one.
LightScene lightSceneOf(const LightingSource& source, SkyTableCache& skyCache);

}  // namespace core
