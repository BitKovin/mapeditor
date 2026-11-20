#include "core/lighting_inputs.h"
#include "core/bsp_compile.h"
#include "core/derived.h"
#include "core/obj_model.h"
#include "core/textures.h"
#include <algorithm>

namespace fs = std::filesystem;

namespace core {

std::shared_ptr<const SkyRadiance> SkyTableCache::get(const fs::path& assetsRoot, const std::string& texture, std::string& warning) {
    // the files the engine would look at, with their sizes and times: a skybox edited on disk is read again
    std::string key = texture;
    if (isCubemapTexture(texture)) {
        const std::string base = texture.substr(texture.find_last_of('/') == std::string::npos ? 0 : texture.find_last_of('/') + 1);
        std::error_code ec;
        for (const char* ext : {".png", ".jpg", ".jpeg"})
            for (const char* f : {"", "_lf", "_rt", "_up", "_dn", "_ft", "_bk"}) {
                const fs::path p = assetsRoot / "env" / (base + f + ext);
                if (!fs::is_regular_file(p, ec)) continue;
                key += "|" + p.filename().string() + ":" + std::to_string(fs::file_size(p, ec)) + ":" + std::to_string(fs::last_write_time(p, ec).time_since_epoch().count());
            }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (key == key_) { warning = warning_; return sky_; }
    key_ = key;
    warning_.clear();
    sky_.reset();
    const auto files = readSkybox(assetsRoot, texture);
    std::vector<DecodedImage> images;
    if (files)
        for (const auto& f : files->images) {
            auto img = decodeImage(f.bytes, f.ext);
            if (!img) break;
            images.push_back(std::move(*img));
        }
    if (files && images.size() == files->images.size()) sky_ = makeSkyRadiance(images, files->panorama, texture);
    if (!sky_) warning_ = "sky light: the skybox " + texture + (files ? " could not be decoded (PNG or baseline JPEG)" : " has no image in env/") + "; the sky is uniform";
    warning = warning_;
    return sky_;
}

std::vector<LightSurface> lightSurfacesOf(const LightingSource& s, bool withSky) {
    std::vector<LightSurface> surfaces = collectSurfaces(s.doc, s.prefabs);
    {   // misc_model: the triangles of static models, baked into the world like brush faces (obj_model.h)
        std::vector<LightSurface> models = collectModelSurfaces(s.doc, s.prefabs, s.assetsRoot);
        surfaces.insert(surfaces.end(), std::make_move_iterator(models.begin()), std::make_move_iterator(models.end()));
    }
    if (!withSky) {
        const BspContentRules rules = BspContentRules::fromJson(s.profile.bsp);
        surfaces.erase(std::remove_if(surfaces.begin(), surfaces.end(), [&](const LightSurface& surface) { return rules.isSky(surface.texture); }), surfaces.end());
    }
    if (s.relief && s.relief->size() > 0)
        for (auto& surface : surfaces) surface.relief = s.relief->find(surface);
    return surfaces;
}

std::shared_ptr<SolidSet> lightSolidsOf(const LightingSource& s, SolidSet::Cache* cache) {
    const LightingClasses classes = LightingClasses::fromProfile(s.profile);
    return std::make_shared<SolidSet>(collectSolids(s.doc, s.prefabs, [&](const std::string& c) { return classes.castsShadows(c); }, cache));
}

LightScene lightSceneOf(const LightingSource& s, SkyTableCache& skyCache) {
    LightScene scene = collectLights(s.doc, s.prefabs);
    if (std::none_of(scene.lights.begin(), scene.lights.end(), [](const Light& l) { return l.type == LightType::Sky; })) return scene;
    const auto w = std::find_if(s.doc.entities.begin(), s.doc.entities.end(), [](const Entity& e) { return e.classname == "worldspawn"; });
    if (w == s.doc.entities.end()) return scene;
    const SkySettings sky = skySettingsOf(*w, [&]() { return listSkyboxes(s.assetsRoot); });
    if (sky.texture.empty()) return scene;  // no skybox: the uniform sky
    std::string warning;
    const auto table = skyCache.get(s.assetsRoot, sky.texture, warning);
    if (!warning.empty()) scene.warnings.push_back(warning);
    for (Light& l : scene.lights) if (l.type == LightType::Sky) l.sky = table;
    return scene;
}

}  // namespace core
