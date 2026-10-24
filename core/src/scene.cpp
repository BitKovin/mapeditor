#include "core/scene.h"
#include "core/albedo.h"
#include "core/bsp_compile.h"
#include "core/derived.h"
#include "core/sky.h"
#include "core/textures.h"
#include "core/settings.h"
#include <algorithm>
#include <fstream>

using nlohmann::json;
namespace fs = std::filesystem;

namespace core {

namespace {
json emptyDocumentJson() {
    return json{{"schemaVersion", kSchemaVersion}, {"entities", json::array()}, {"prefabInstances", json::array()}};
}

json readJsonFile(const fs::path& full, const std::string& label) {
    std::ifstream in(full, std::ios::binary);
    if (!in) throw std::runtime_error("could not open " + label);
    try {
        return json::parse(in);
    } catch (const json::parse_error& e) {
        throw std::runtime_error("invalid JSON in " + label + ": " + e.what());
    }
}
}  // namespace

Scene::Scene(fs::path assetsRoot) : assetsRoot_(std::move(assetsRoot)), docJson_(emptyDocumentJson()) {}

fs::path Scene::resolveSandboxed(const std::string& rel) const {
    if (rel.empty()) throw std::runtime_error("empty path");
    const fs::path p(rel);
    if (p.is_absolute() || p.has_root_name() || p.has_root_directory())
        throw std::runtime_error("absolute paths are not allowed: " + rel);
    const fs::path norm = p.lexically_normal();
    for (const auto& part : norm) {
        if (part == "..") throw std::runtime_error("path escapes the assets root: " + rel);
    }
    return assetsRoot_ / norm;
}

void Scene::loadGameProfile() {
    profile_ = loadProfile(assetsRoot_);
    defs_ = profile_.entityDefinitions.empty() ? EntityDefinitions{} : loadEntityDefinitions(assetsRoot_ / pathFromUtf8(profile_.entityDefinitions));
}

void Scene::reloadGameProfile() { loadGameProfile(); }

std::string Scene::configFingerprint() const {
    auto stamp = [](const fs::path& f) {
        std::error_code ec;
        const auto t = fs::last_write_time(f, ec);
        return ec ? std::string("-") : std::to_string(t.time_since_epoch().count());
    };
    std::string fp = stamp(assetsRoot_ / "game_profile.json");
    if (!profile_.entityDefinitions.empty()) fp += "|" + stamp(assetsRoot_ / pathFromUtf8(profile_.entityDefinitions));
    return fp;
}

void Scene::resetToEmptyDocument() {
    docJson_ = emptyDocumentJson();
    doc_ = Document{};
    currentPath_.clear();
    dirty_ = false;
    ++rev_;
}

void Scene::setAssetsRoot(const fs::path& root) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) throw std::runtime_error("not a folder: " + pathToUtf8(root));
    const fs::path abs = fs::absolute(root, ec).lexically_normal();

    // Build the new state completely before touching ours, so a broken folder changes nothing.
    GameProfile p = loadProfile(abs);
    EntityDefinitions d = p.entityDefinitions.empty() ? EntityDefinitions{} : loadEntityDefinitions(abs / pathFromUtf8(p.entityDefinitions));

    const std::string wasOpen = currentPath_;
    assetsRoot_ = abs;
    profile_ = std::move(p);
    defs_ = std::move(d);
    prefabCache_.clear();

    if (!wasOpen.empty()) {
        try {
            openDocument(wasOpen);
            return;
        } catch (const std::exception&) {
            // the map isn't in the new folder (or is broken there): fall through to an empty document
        }
    }
    resetToEmptyDocument();
}

json Scene::listDirectory(const std::string& relDir) const {
    fs::path dir = assetsRoot_;
    std::string norm;
    if (!relDir.empty() && relDir != "." && relDir != "/") {
        norm = normalizeAssetPath(relDir);
        while (!norm.empty() && norm.back() == '/') norm.pop_back();
        if (norm.empty()) norm = ".";
        dir = norm == "." ? assetsRoot_ : resolveSandboxed(norm);
        if (norm == ".") norm.clear();
    }
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) throw std::runtime_error("not a folder: " + (norm.empty() ? std::string(".") : norm));

    struct Entry { std::string name; bool isDir; };
    std::vector<Entry> found;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::string name = pathToUtf8(it->path().filename());
        if (name.empty() || name[0] == '.') continue;
        std::error_code e2;
        found.push_back({std::move(name), it->is_directory(e2)});
        if (found.size() >= 5000) break;  // bound the reply
    }
    std::sort(found.begin(), found.end(), [](const Entry& a, const Entry& b) {
        if (a.isDir != b.isDir) return a.isDir;
        return std::lexicographical_compare(a.name.begin(), a.name.end(), b.name.begin(), b.name.end(), [](unsigned char x, unsigned char y) { return std::tolower(x) < std::tolower(y); });
    });
    json entries = json::array();
    for (const auto& e : found) entries.push_back(json{{"name", e.name}, {"dir", e.isDir}});
    return json{{"path", norm}, {"entries", entries}};
}

const Scene::CachedPrefab& Scene::loadPrefab(const std::string& key) {
    const fs::path full = resolveSandboxed(key);
    std::error_code ec;
    const auto mtime = fs::last_write_time(full, ec);
    if (ec) throw std::runtime_error("prefab file not found: " + key);

    auto it = prefabCache_.find(key);
    if (it != prefabCache_.end() && it->second.mtime == mtime) return it->second;

    json raw = readJsonFile(full, "prefab '" + key + "'");
    Document typed;
    try {
        typed = documentFromJson(raw);
    } catch (const std::exception& e) {
        throw std::runtime_error("prefab '" + key + "' is malformed: " + e.what());
    }
    if (auto err = validateDocument(typed)) throw std::runtime_error("prefab '" + key + "': " + *err);

    CachedPrefab& slot = prefabCache_[key];
    slot = CachedPrefab{std::move(raw), std::move(typed), mtime};
    return slot;
}

void Scene::validateCandidate(const Document& candidate, const std::string& rootPath, ResolvedCache* seed) {
    if (auto err = validateDocument(candidate)) throw std::runtime_error(*err);
    std::set<std::string> used;
    auto loader = [this, &used](const std::string& p) -> const Document& {
        const std::string key = normalizeAssetPath(p);
        const CachedPrefab& c = loadPrefab(key);
        used.insert(key);
        return c.typed;
    };
    std::vector<ResolvedEntity> all = resolvePrefabs(candidate, loader, ResolveOptions{}, rootPath);
    if (!seed) return;
    *seed = ResolvedCache{};
    for (auto& r : all) if (r.instancePath.size() > 1) seed->entities.push_back(std::move(r));
    for (const std::string& key : used) seed->deps.push_back({key, prefabCache_.at(key).mtime});
    seed->path = rootPath;
    seed->valid = true;
}

void Scene::openDocument(const std::string& path) {
    const std::string key = normalizeAssetPath(path);
    json raw = readJsonFile(resolveSandboxed(key), "'" + key + "'");

    Document typed;
    try {
        typed = documentFromJson(raw);
    } catch (const std::exception& e) {
        throw std::runtime_error("'" + key + "' is malformed: " + e.what());
    }
    ResolvedCache seed;
    validateCandidate(typed, key, &seed);

    docJson_ = std::move(raw);
    doc_ = std::move(typed);
    currentPath_ = key;
    dirty_ = false;
    ++rev_;
    seed.rev = rev_;
    resolved_ = std::move(seed);
}

std::string Scene::saveDocument(const std::string& path) {
    const std::string key = path.empty() ? currentPath_ : normalizeAssetPath(path);
    if (key.empty()) throw std::runtime_error("this document has no path yet; provide one to save");
    const fs::path full = resolveSandboxed(key);

    fs::create_directories(full.parent_path());
    fs::path tmp = full;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("could not write " + key);
        out << docJson_.dump(2) << "\n";
        out.flush();
        if (!out) throw std::runtime_error("write failed for " + key);
    }
    fs::rename(tmp, full);  // atomic replace: a crash never leaves a half-written map

    currentPath_ = key;
    dirty_ = false;
    return key;
}

Scene::UpdateResult Scene::applyUpdate(const json& docJson) {
    UpdateResult r;
    Document typed;
    try {
        typed = documentFromJson(docJson);
    } catch (const std::exception& e) {
        r.error = std::string("malformed document: ") + e.what();
        return r;
    }
    ResolvedCache seed;
    try {
        validateCandidate(typed, currentPath_, &seed);
    } catch (const std::exception& e) {
        r.error = e.what();
        return r;
    }
    docJson_ = docJson;
    doc_ = std::move(typed);
    dirty_ = true;
    r.ok = true;
    r.rev = ++rev_;
    seed.rev = rev_;
    resolved_ = std::move(seed);
    return r;
}

// Flattened prefab content; a broken prefab (cycle, missing file) just contributes nothing here -- the
// Shell already reports those problems.
const std::vector<ResolvedEntity>& Scene::resolvedInside() {
    if (resolved_.valid && resolved_.rev == rev_ && resolved_.path == currentPath_) {
        bool same = true;
        std::error_code ec;
        for (const auto& [key, mtime] : resolved_.deps) {
            const auto now = fs::last_write_time(resolveSandboxed(key), ec);
            if (ec || now != mtime) { same = false; break; }
        }
        if (same) return resolved_.entities;
    }
    resolved_ = ResolvedCache{};
    resolved_.rev = rev_;
    resolved_.path = currentPath_;
    if (doc_.prefabInstances.empty()) { resolved_.valid = true; return resolved_.entities; }
    std::set<std::string> used;
    auto loader = [this, &used](const std::string& p) -> const Document& {
        const std::string key = normalizeAssetPath(p);
        const CachedPrefab& c = loadPrefab(key);
        used.insert(key);
        return c.typed;
    };
    try {
        std::vector<ResolvedEntity> all = resolvePrefabs(doc_, loader, ResolveOptions{}, currentPath_);
        // the flattened list also repeats the document's own entities (instance path = just the root); those are derived directly
        for (auto& r : all) if (r.instancePath.size() > 1) resolved_.entities.push_back(std::move(r));
        for (const std::string& key : used) resolved_.deps.push_back({key, prefabCache_.at(key).mtime});
        resolved_.valid = true;
    } catch (const std::exception&) {
        resolved_.entities.clear();  // not cached: the next call tries again
    }
    return resolved_.entities;
}

json Scene::derivedGeometryJson() {
    return deriveGeometry(doc_, resolvedInside(), rev_);
}

std::vector<LightSurface> Scene::lightSurfaces(bool withSky) {
    std::vector<LightSurface> surfaces = collectSurfaces(doc_, resolvedInside());
    if (!withSky) {
        const BspContentRules rules = BspContentRules::fromJson(profile_.bsp);
        surfaces.erase(std::remove_if(surfaces.begin(), surfaces.end(), [&](const LightSurface& s) { return rules.isSky(s.texture); }), surfaces.end());
    }
    if (relief_.size() > 0)
        for (auto& s : surfaces) s.relief = relief_.find(s);
    return surfaces;
}

std::shared_ptr<SolidSet> Scene::lightSolids() {
    const LightingClasses classes = lightingClasses();
    return std::make_shared<SolidSet>(collectSolids(doc_, resolvedInside(), [&](const std::string& c) { return classes.castsShadows(c); }));
}

LightScene Scene::lightScene() {
    LightScene scene = collectLights(doc_, resolvedInside());
    if (std::none_of(scene.lights.begin(), scene.lights.end(), [](const Light& l) { return l.type == LightType::Sky; })) return scene;
    const auto w = std::find_if(doc_.entities.begin(), doc_.entities.end(), [](const Entity& e) { return e.classname == "worldspawn"; });
    if (w == doc_.entities.end()) return scene;
    const SkySettings sky = skySettingsOf(*w, [this]() { return listSkyboxes(assetsRoot_); });
    if (sky.texture.empty()) return scene;  // no skybox: the uniform sky
    std::string warning;
    const auto table = skyTable(sky.texture, warning);
    if (!warning.empty()) scene.warnings.push_back(warning);
    for (Light& l : scene.lights) if (l.type == LightType::Sky) l.sky = table;
    return scene;
}

std::shared_ptr<const SkyRadiance> Scene::skyTable(const std::string& texture, std::string& warning) {
    // the files the engine would look at, with their sizes and times: a skybox edited on disk is read again
    std::string key = texture;
    if (isCubemapTexture(texture)) {
        const std::string base = texture.substr(texture.find_last_of('/') == std::string::npos ? 0 : texture.find_last_of('/') + 1);
        std::error_code ec;
        for (const char* ext : {".png", ".jpg", ".jpeg"})
            for (const char* f : {"", "_lf", "_rt", "_up", "_dn", "_ft", "_bk"}) {
                const fs::path p = assetsRoot_ / "env" / (base + f + ext);
                if (!fs::is_regular_file(p, ec)) continue;
                key += "|" + p.filename().string() + ":" + std::to_string(fs::file_size(p, ec)) + ":" + std::to_string(fs::last_write_time(p, ec).time_since_epoch().count());
            }
    }
    std::lock_guard<std::mutex> lock(skyMutex_);
    if (key == skyKey_) { warning = skyWarning_; return sky_; }
    skyKey_ = key;
    skyWarning_.clear();
    sky_.reset();
    const auto files = readSkybox(assetsRoot_, texture);
    std::vector<DecodedImage> images;
    if (files)
        for (const auto& f : files->images) {
            auto img = decodeImage(f.bytes, f.ext);
            if (!img) break;
            images.push_back(std::move(*img));
        }
    if (files && images.size() == files->images.size()) sky_ = makeSkyRadiance(images, files->panorama, texture);
    if (!sky_) skyWarning_ = "sky light: the skybox " + texture + (files ? " could not be decoded (PNG or baseline JPEG)" : " has no image in env/") + "; the sky is uniform";
    warning = skyWarning_;
    return sky_;
}

LightmapLayout Scene::lightmapLayout(const LightmapSettings& settings) {
    return buildLightmapLayout(lightSurfaces(), lightingClasses(), settings);
}

bool Scene::hasDerivedGeometry() {
    return hasDerivedWork(doc_, resolvedInside());
}

json Scene::documentSyncJson() const {
    return json{{"type", "document_sync"}, {"rev", rev_}, {"path", currentPath_}, {"dirty", dirty_}, {"doc", docJson_}};
}

void Scene::collectReachable(const Document& doc, std::set<std::string>& out) {
    for (const auto& inst : doc.prefabInstances) {
        const std::string key = normalizeAssetPath(inst.file);
        if (!out.insert(key).second) continue;  // also what stops cycles from recursing forever
        try {
            collectReachable(loadPrefab(key).typed, out);
        } catch (const std::exception&) {
            // Reported per-file by prefabLibraryJson(); nothing to descend into.
        }
    }
}

json Scene::prefabLibraryJson() {
    std::set<std::string> keys;
    collectReachable(doc_, keys);
    json prefabs = json::object();
    for (const auto& key : keys) {
        try {
            prefabs[key] = loadPrefab(key).raw;
        } catch (const std::exception& e) {
            prefabs[key] = json{{"error", e.what()}};
        }
    }
    return json{{"type", "prefab_library"}, {"prefabs", prefabs}};
}

std::string Scene::writePrefab(const std::string& path, const json& docJson) {
    const std::string key = normalizeAssetPath(path);
    const std::string ext = ".map.json";
    if (key.size() <= ext.size() || key.compare(key.size() - ext.size(), ext.size(), ext) != 0)
        throw std::runtime_error("a prefab file name must end in " + ext + ": " + key);
    if (!docJson.is_object()) throw std::runtime_error("the prefab is not a document");
    const fs::path full = resolveSandboxed(key);

    Document typed;
    try {
        typed = documentFromJson(docJson);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("malformed prefab: ") + e.what());
    }
    validateCandidate(typed, key);  // also catches a prefab that (indirectly) contains itself

    fs::create_directories(full.parent_path());
    fs::path tmp = full;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("could not write " + key);
        out << docJson.dump(2) << "\n";
        out.flush();
        if (!out) throw std::runtime_error("write failed for " + key);
    }
    fs::rename(tmp, full);  // atomic replace
    prefabCache_.erase(key);  // never serve an older version of this file, even when the timestamps are too close to tell
    return key;
}

nlohmann::json Scene::readPrefabJson(const std::string& path) {
    return loadPrefab(normalizeAssetPath(path)).raw;
}

std::string Scene::libraryFingerprint() {
    std::set<std::string> keys;
    collectReachable(doc_, keys);
    std::string fp;
    for (const auto& key : keys) {
        std::error_code ec;
        fs::path full;
        try {
            full = resolveSandboxed(key);
        } catch (const std::exception&) {
            fp += key + ":unsafe;";
            continue;
        }
        const auto mtime = fs::last_write_time(full, ec);
        fp += key + ":" + (ec ? std::string("missing") : std::to_string(mtime.time_since_epoch().count())) + ";";
    }
    return fp;
}

}  // namespace core
