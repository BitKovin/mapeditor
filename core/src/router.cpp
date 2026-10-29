#include "core/router.h"
#include "core/exporter.h"
#include "core/map_import.h"
#include "core/native_dialog.h"
#include "core/profile.h"
#include "core/texgroups.h"
#include "core/protocol.h"
#include "core/textures.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <optional>

namespace fs = std::filesystem;

using nlohmann::json;

namespace core {

namespace {
std::string errorMessage(const std::string& message) {
    return json{{"type", proto::kError}, {"message", message}}.dump();
}

json requestId(const json& req) { return req.contains("requestId") ? req["requestId"] : json(nullptr); }

std::string failedResponse(const json& req, const std::string& message) {
    return json{{"type", proto::kResponse}, {"requestId", requestId(req)}, {"ok", false}, {"error", message}}.dump();
}

// The game profile, plus the export format it picks and the formats there are (REQUIREMENTS §31 X2).
json profileMessage(const GameProfile& profile) {
    json list = json::array();
    for (const Exporter& e : exporters()) list.push_back({{"id", e.id}, {"name", e.name}, {"extension", e.extension}});
    return {{"type", proto::kProfile}, {"profile", profile.toJson()}, {"exporter", exporterFor(profile.exporter).id}, {"exporters", list}};
}
}  // namespace

Router::Router(Scene& scene, fs::path settingsFile, const LightingService::Options& lightingOptions)
    : scene_(scene), lastLibraryFingerprint_(scene.libraryFingerprint()), lastConfigFingerprint_(scene.configFingerprint()),
      settingsFile_(std::move(settingsFile)), lighting_(scene, lightingOptions), lightingOptions_(lightingOptions) {
    if (!settingsFile_.empty()) settings_ = loadSettings(settingsFile_);  // keep what this run does not change (recent maps, games, editor)
    if (pathFromUtf8(settings_.assetsRoot) != scene.assetsRoot()) loadGameSettings(settings_, scene.assetsRoot());  // started on another game folder
    settings_.assetsRoot = pathToUtf8(scene.assetsRoot());
    settings_.lastMap = scene.currentPath();
    settings_.touchRecent(settings_.assetsRoot, settings_.lastMap);  // the map reopened at start-up is the most recent one
}

json Router::settingsJson() const {
    const std::string root = pathToUtf8(scene_.assetsRoot());
    json recent = json::array();
    for (const auto& r : settings_.recentMaps) if (r.assetsRoot == root) recent.push_back(r.path);
    const auto g = settings_.games.find(root);
    const GameLaunch game = g != settings_.games.end() ? g->second : GameLaunch{};
    return json{{"type", proto::kSettings}, {"assetsRoot", root}, {"recentMaps", recent},
                {"game", json{{"exe", game.exe}, {"args", game.args}, {"workingDir", game.workingDir}}}, {"editor", settings_.editor},
                {"nativeDialogs", nativeDialogsAvailable()}, {"settingsFile", pathToUtf8(settingsFile_)}, {"lightingFile", pathToUtf8(lightingFile_)}};
}

bool Router::persistSettings() {
    settings_.assetsRoot = pathToUtf8(scene_.assetsRoot());
    settings_.lastMap = scene_.currentPath();
    return settingsFile_.empty() || saveSettings(settingsFile_, settings_);
}

void Router::pushDerived(std::vector<std::string>& to) {
    if (scene_.hasDerivedGeometry()) to.push_back(scene_.derivedGeometryJson().dump());
}

void Router::pushDerivedAll(RouterOutput& out) {
    pushDerived(out.toAll);
    lighting_.markDirty();
}

void Router::pushConfig(RouterOutput& out) {
    lighting_.markDirty();  // the profile decides which classes block / receive light
    out.toAll.push_back(profileMessage(scene_.profile()).dump());
    out.toAll.push_back(scene_.entityDefsJson().dump());
    out.toAll.push_back(settingsJson().dump());
    lastConfigFingerprint_ = scene_.configFingerprint();
}

RouterOutput Router::onTick() {
    RouterOutput out;
    const std::string cfg = scene_.configFingerprint();
    if (cfg != lastConfigFingerprint_) {
        lastConfigFingerprint_ = cfg;  // remember first: a broken file is reported once, not every second
        try {
            scene_.reloadGameProfile();
            pushConfig(out);
        } catch (const std::exception& e) {
            out.toAll.push_back(errorMessage(std::string("game profile not reloaded: ") + e.what()));
        }
    }
    try {
        pushLibraryIfChanged(out, false);
    } catch (const std::exception& e) {
        out.toAll.push_back(errorMessage(e.what()));
    }
    lighting_.flushInputs();  // a changed prefab / profile: the bake thread works on the new document
    for (auto& m : export_.poll()) out.toAll.push_back(std::move(m));
    return out;
}

void Router::handleExport(const json& req, RouterOutput& out) {
    if (req["type"] == proto::kExportCancel) {
        export_.cancel();
        out.toSender.push_back(json{{"type", proto::kResponse}, {"requestId", requestId(req)}, {"ok", true}}.dump());
        return;
    }
    if (!req.contains("path") || !req["path"].is_string()) { out.toSender.push_back(failedResponse(req, "export_map needs a path")); return; }
    // the game's format (REQUIREMENTS §31 X2); the path gets its extension
    BspExportRequest r;
    r.assetsRoot = scene_.assetsRoot();
    r.document = scene_.documentSyncJson()["doc"];
    r.mapPath = scene_.currentPath();
    r.relief = lastRelief_;
    std::string formatWarning;
    const Exporter& exporter = exporterFor(scene_.profile().exporter, &formatWarning);
    r.format = exporter.id;
    r.outPath = withExporterExtension(req["path"].get<std::string>(), exporter);
    r.lighting = lightingOptions_;
    r.bake = req.value("bake", true);
    const std::string outPath = r.outPath;
    if (!export_.start(std::move(r))) { out.toSender.push_back(failedResponse(req, "an export is already running")); return; }
    json res = {{"type", proto::kResponse}, {"requestId", requestId(req)}, {"ok", true}, {"path", outPath}, {"format", exporter.id}};
    if (!formatWarning.empty()) res["warning"] = formatWarning;
    out.toSender.push_back(res.dump());
}

void Router::pushLibraryIfChanged(RouterOutput& out, bool force) {
    const std::string fp = scene_.libraryFingerprint();
    if (force || fp != lastLibraryFingerprint_) {
        out.toAll.push_back(scene_.prefabLibraryJson().dump());
        lastLibraryFingerprint_ = fp;
        // prefab content is carved and decorated too, so a changed prefab means new derived geometry
        if (!force) pushDerivedAll(out);
    }
}

RouterOutput Router::onConnect() {
    RouterOutput out;
    out.toSender.push_back(scene_.documentSyncJson().dump());
    out.toSender.push_back(scene_.prefabLibraryJson().dump());
    out.toSender.push_back(profileMessage(scene_.profile()).dump());
    out.toSender.push_back(scene_.entityDefsJson().dump());
    out.toSender.push_back(settingsJson().dump());
    pushDerived(out.toSender);
    for (auto& m : lighting_.fullState()) out.toSender.push_back(std::move(m));
    return out;
}

RouterOutput Router::onLightingTick() { return onLightingTick(lighting_.tickBudgetSeconds()); }

RouterOutput Router::onLightingTick(double budgetSeconds) {
    RouterOutput out;
    try {
        for (auto& m : lighting_.tick(budgetSeconds)) out.toAll.push_back(std::move(m));
    } catch (const std::exception& e) {
        out.toAll.push_back(errorMessage(std::string("lighting: ") + e.what()));
    }
    return out;
}

RouterOutput Router::onMessage(const std::string& text) {
    RouterOutput out;
    json req;
    try {
        req = json::parse(text);
    } catch (const json::parse_error& e) {
        out.toSender.push_back(errorMessage(std::string("invalid JSON: ") + e.what()));
        return out;
    }

    try {
        if (!req.is_object() || !req.contains("type") || !req["type"].is_string())
            throw std::runtime_error("message must be an object with a string \"type\"");
        const std::string type = req["type"].get<std::string>();
        if (type == proto::kDocumentUpdate) handleUpdate(req, out);
        else if (type == proto::kOpenDocument) handleOpen(req, out);
        else if (type == proto::kSaveDocument) handleSave(req, out);
        else if (type == proto::kImportMap) handleImportMap(req, out);
        else if (type == proto::kListTextures) handleListTextures(req, out);
        else if (type == proto::kReadTexture) handleReadTexture(req, out);
        else if (type == proto::kWriteTextureProps) handleWriteTextureProps(req, out);
        else if (type == proto::kListTexGroups) handleListTexGroups(req, out);
        else if (type == proto::kReadTexGroup) handleReadTexGroup(req, out);
        else if (type == proto::kWriteTexGroup) handleWriteTexGroup(req, out);
        else if (type == proto::kDeleteTexGroup) handleDeleteTexGroup(req, out);
        else if (type == proto::kReadPrefab) handleReadPrefab(req, out);
        else if (type == proto::kWritePrefab) handleWritePrefab(req, out);
        else if (type == proto::kListDirectory) handleListDirectory(req, out);
        else if (type == proto::kBrowseFilesystem) handleBrowseFilesystem(req, out);
        else if (type == proto::kSetAssetsRoot) handleSetAssetsRoot(req, out);
        else if (type == proto::kLighting) handleLighting(req, out);
        // The live bake is flat: a relief reached the lighting as a second snapshot right behind the edit's own (the bake did the first one's layout and geometry
        // for nothing and the light came seconds late), and Lit mode is for fast light iteration. The export gets the relief from here (RouterExport::relief).
        else if (type == proto::kReliefGeometry) lastRelief_ = req;
        else if (type == proto::kExportMap || type == proto::kExportBsp || type == proto::kExportCancel) handleExport(req, out);
        else if (type == proto::kNativeDialog) handleNativeDialog(req, out);
        else if (type == proto::kSetSettings) handleSetSettings(req, out);
        else if (type == proto::kReadGameProfile || type == proto::kWriteGameProfile) handleGameProfile(req, out);
        else if (type == proto::kReadLightingConfig || type == proto::kWriteLightingConfig) handleLightingConfig(req, out);
        else if (type == proto::kLaunchGame) handleLaunchGame(req, out);
        else throw std::runtime_error("unknown message type '" + type + "'");
    } catch (const std::exception& e) {
        // Anything that escaped a handler (e.g. a missing field). Discard
        // partial output: nothing should be broadcast for a failed message.
        out = RouterOutput{};
        out.toSender.push_back(errorMessage(e.what()));
    }
    try {
        lighting_.flushInputs();  // with a bake thread: hand it the document as it is now (at most one snapshot per message)
    } catch (const std::exception& e) {
        out.toSender.push_back(errorMessage(std::string("lighting: ") + e.what()));
    }
    return out;
}

void Router::handleLighting(const json& req, RouterOutput& out) {
    if (req.contains("exposure") && req["exposure"].is_number()) lighting_.setExposure(std::clamp(req["exposure"].get<double>(), 0.05, 20.0));
    if (req.contains("camera") && req["camera"].is_array() && req["camera"].size() == 3)
        lighting_.setCamera({req["camera"][0].get<double>(), req["camera"][1].get<double>(), req["camera"][2].get<double>()});
    if (req.contains("enabled") && req["enabled"].is_boolean()) lighting_.setEnabled(req["enabled"].get<bool>());
    if (req.contains("requestId"))
        out.toSender.push_back(json{{"type", proto::kResponse}, {"requestId", requestId(req)}, {"ok", true}, {"lighting", lighting_.statusJson()}}.dump());
}

void Router::handleUpdate(const json& req, RouterOutput& out) {
    if (!req.contains("doc")) throw std::runtime_error("document_update requires \"doc\"");
    const Scene::UpdateResult r = scene_.applyUpdate(req["doc"]);
    if (!r.ok) {
        out.toSender.push_back(errorMessage(r.error));
        return;
    }
    out.toSender.push_back(json{{"type", proto::kUpdateAck}, {"rev", r.rev}}.dump());
    out.toOthers.push_back(scene_.documentSyncJson().dump());
    pushDerivedAll(out);  // everyone, sender included: the ack does not carry geometry
    pushLibraryIfChanged(out, false);
}

void Router::handleOpen(const json& req, RouterOutput& out) {
    const std::string path = req.at("path").get<std::string>();
    try {
        scene_.openDocument(path);
    } catch (const std::exception& e) {
        out.toSender.push_back(failedResponse(req, e.what()));
        return;
    }
    settings_.touchRecent(pathToUtf8(scene_.assetsRoot()), scene_.currentPath());
    persistSettings();  // remember the map for next start-up; failure to write is not worth failing the open
    // Opening replaces the document for every window, sender included.
    out.toAll.push_back(scene_.documentSyncJson().dump());
    pushDerivedAll(out);
    pushLibraryIfChanged(out, true);
    out.toAll.push_back(settingsJson().dump());  // the recent maps changed
    out.toSender.push_back(json{{"type", proto::kResponse}, {"requestId", requestId(req)}, {"ok", true},
                                {"path", scene_.currentPath()}, {"rev", scene_.revision()}}
                               .dump());
}

void Router::handleImportMap(const json& req, RouterOutput& out) {
    const std::string path = normalizeAssetPath(req.at("path").get<std::string>());
    const std::string ext = ".map";
    std::string lowerPath = path;
    for (char& c : lowerPath) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (path.empty() || path.rfind("..", 0) == 0 || std::filesystem::path(path).is_absolute() || path.find(':') != std::string::npos ||
        lowerPath.size() <= ext.size() || lowerPath.compare(lowerPath.size() - ext.size(), ext.size(), ext) != 0) {
        out.toSender.push_back(failedResponse(req, "import_map needs a .map file inside the game folder: " + path));
        return;
    }
    const std::string target = path.substr(0, path.size() - ext.size()) + ".map.json";
    const std::filesystem::path full = scene_.assetsRoot() / std::filesystem::path(target);
    if (std::filesystem::exists(full) && !(req.contains("overwrite") && req["overwrite"].is_boolean() && req["overwrite"].get<bool>())) {
        out.toSender.push_back(json{{"type", proto::kResponse}, {"requestId", requestId(req)}, {"ok", false}, {"exists", true}, {"path", target},
                                    {"error", target + " already exists"}}.dump());
        return;
    }
    MapImportResult imported;
    try {
        imported = importQuakeMapFile(scene_.assetsRoot() / std::filesystem::path(path), importOptionsFor(scene_.assetsRoot(), scene_.profile()));
        scene_.writePrefab(target, imported.doc);  // validates like any document and writes atomically
    } catch (const std::exception& e) {
        out.toSender.push_back(failedResponse(req, std::string("import failed: ") + e.what()));
        return;
    }
    handleOpen(json{{"requestId", req.contains("requestId") ? req["requestId"] : json(nullptr)}, {"path", target}}, out);
    if (!out.toSender.empty()) {  // the open's reply carries what the import found
        json res = json::parse(out.toSender.back());
        if (res.value("ok", false)) {
            const MapImportStats& s = imported.stats;
            res["warnings"] = imported.warnings;
            res["stats"] = json{{"entities", s.entities}, {"brushes", s.brushes}, {"patches", s.patches}, {"lights", s.lights}, {"faces", s.faces}, {"skipped", s.skipped}, {"droppedEntities", s.droppedEntities}};
            out.toSender.back() = res.dump();
        }
    }
}

void Router::handleSave(const json& req, RouterOutput& out) {
    const std::string path = req.contains("path") ? req["path"].get<std::string>() : std::string();
    const std::string before = scene_.currentPath();
    std::string saved;
    try {
        saved = scene_.saveDocument(path);
    } catch (const std::exception& e) {
        out.toSender.push_back(failedResponse(req, e.what()));
        return;
    }
    out.toSender.push_back(
        json{{"type", proto::kResponse}, {"requestId", requestId(req)}, {"ok", true}, {"path", saved}}.dump());
    const RecentMap first{pathToUtf8(scene_.assetsRoot()), saved};
    const bool unchanged = !settings_.recentMaps.empty() && settings_.recentMaps.front() == first;
    settings_.touchRecent(first.assetsRoot, first.path);
    persistSettings();
    if (!unchanged) out.toAll.push_back(settingsJson().dump());  // the recent maps changed
    // "Save as" changes the current path; other windows should learn about it. (A plain save only
    // clears the dirty flag, which the saving Shell already knows.)
    if (saved != before) out.toOthers.push_back(scene_.documentSyncJson().dump());
}

}  // namespace core

namespace core {

void Router::handleListTextures(const json& req, RouterOutput& out) {
    out.toSender.push_back(json{{"type", proto::kResponse}, {"requestId", requestId(req)}, {"ok", true},
                                {"textures", listTextures(scene_.assetsRoot(), scene_.profile())}}
                               .dump());
}

void Router::handleReadTexture(const json& req, RouterOutput& out) {
    const std::string path = req.at("path").get<std::string>();
    if (isCubemapTexture(path)) {  // a skybox (REQUIREMENTS §28 K5): the files the engine loads from env/
        const auto sky = readSkybox(scene_.assetsRoot(), path);
        if (!sky) { out.toSender.push_back(failedResponse(req, "skybox '" + path + "' has no image in env/")); return; }
        json images = json::array();
        for (const auto& f : sky->images) images.push_back(json{{"ext", f.ext}, {"data", base64Encode(f.bytes)}});
        out.toSender.push_back(json{{"type", proto::kResponse}, {"requestId", requestId(req)}, {"ok", true}, {"path", path},
                                    {"sky", {{"panorama", sky->panorama}, {"images", images}}}}.dump());
        return;
    }
    auto tex = readTexture(scene_.assetsRoot(), scene_.profile(), path);
    if (!tex) {
        out.toSender.push_back(failedResponse(req, "texture '" + path + "' not found"));
        return;
    }
    json resp{{"type", proto::kResponse}, {"requestId", requestId(req)}, {"ok", true}, {"path", path}, {"ext", tex->ext}, {"data", base64Encode(tex->bytes)}};
    if (auto props = readTextureProps(scene_.assetsRoot(), scene_.profile(), path)) resp["props"] = sanitizeTextureProps(*props);
    out.toSender.push_back(resp.dump());
}

void Router::handleWriteTextureProps(const json& req, RouterOutput& out) {
    const std::string path = req.at("path").get<std::string>();
    const json props = req.contains("props") ? req.at("props") : json::object();
    const std::string err = writeTextureProps(scene_.assetsRoot(), scene_.profile(), path, props);
    if (!err.empty()) { out.toSender.push_back(failedResponse(req, err)); return; }
    lighting_.textureChanged(path);  // e.g. its transparency: the light around the faces that use it changes
    out.toSender.push_back(json{{"type", proto::kResponse}, {"requestId", requestId(req)}, {"ok", true}, {"path", path}, {"props", sanitizeTextureProps(props)}}.dump());
}


void Router::handleListTexGroups(const json& req, RouterOutput& out) {
    out.toSender.push_back(json{{"type", proto::kResponse}, {"requestId", requestId(req)}, {"ok", true},
                                {"texgroups", listTexGroups(scene_.assetsRoot(), scene_.profile())}}.dump());
}

void Router::handleReadTexGroup(const json& req, RouterOutput& out) {
    const std::string path = req.at("path").get<std::string>();
    auto g = readTexGroup(scene_.assetsRoot(), scene_.profile(), path);
    if (!g) { out.toSender.push_back(failedResponse(req, "texture group '" + path + "' not found")); return; }
    out.toSender.push_back(json{{"type", proto::kResponse}, {"requestId", requestId(req)}, {"ok", true}, {"path", path}, {"group", *g}}.dump());
}

void Router::handleWriteTexGroup(const json& req, RouterOutput& out) {
    const std::string path = req.at("path").get<std::string>();
    const json group = req.contains("group") ? req.at("group") : json();
    const std::string err = writeTexGroup(scene_.assetsRoot(), scene_.profile(), path, group);
    if (!err.empty()) { out.toSender.push_back(failedResponse(req, err)); return; }
    out.toSender.push_back(json{{"type", proto::kResponse}, {"requestId", requestId(req)}, {"ok", true}, {"path", path}}.dump());
}

void Router::handleDeleteTexGroup(const json& req, RouterOutput& out) {
    const std::string path = req.at("path").get<std::string>();
    const std::string err = deleteTexGroup(scene_.assetsRoot(), scene_.profile(), path);
    if (!err.empty()) { out.toSender.push_back(failedResponse(req, err)); return; }
    out.toSender.push_back(json{{"type", proto::kResponse}, {"requestId", requestId(req)}, {"ok", true}, {"path", path}}.dump());
}

}  // namespace core

namespace core {

void Router::handleReadPrefab(const json& req, RouterOutput& out) {
    const std::string path = req.at("path").get<std::string>();
    try {
        out.toSender.push_back(json{{"type", proto::kResponse}, {"requestId", requestId(req)}, {"ok", true}, {"path", normalizeAssetPath(path)},
                                    {"doc", scene_.readPrefabJson(path)}}
                                   .dump());
    } catch (const std::exception& e) {
        out.toSender.push_back(failedResponse(req, e.what()));
    }
}

void Router::handleWritePrefab(const json& req, RouterOutput& out) {
    const std::string path = req.at("path").get<std::string>();
    try {
        const std::string saved = scene_.writePrefab(path, req.at("doc"));
        out.toSender.push_back(json{{"type", proto::kResponse}, {"requestId", requestId(req)}, {"ok", true}, {"path", saved}}.dump());
        pushLibraryIfChanged(out, false);  // the file may be one the open document already uses
    } catch (const std::exception& e) {
        out.toSender.push_back(failedResponse(req, e.what()));
    }
}

void Router::handleListDirectory(const json& req, RouterOutput& out) {
    const std::string path = req.contains("path") && req["path"].is_string() ? req["path"].get<std::string>() : std::string();
    json listing;
    try {
        listing = scene_.listDirectory(path);
    } catch (const std::exception& e) {
        out.toSender.push_back(failedResponse(req, e.what()));
        return;
    }
    listing["type"] = proto::kResponse;
    listing["requestId"] = requestId(req);
    listing["ok"] = true;
    out.toSender.push_back(listing.dump());
}

namespace {
// Folder picker data for choosing the assets root. Folders only; never file names or contents.
json browseFolders(const std::string& pathUtf8) {
    std::error_code ec;
    json drives = json::array();
    fs::path dir;
    if (pathUtf8.empty()) {
#ifdef _WIN32
        for (char c = 'A'; c <= 'Z'; ++c) {
            const std::string root = std::string(1, c) + ":\\";
            if (fs::exists(root, ec)) drives.push_back(root);
        }
        return json{{"path", ""}, {"parent", nullptr}, {"dirs", json::array()}, {"drives", drives}, {"hasProfile", false}};
#else
        const char* home = std::getenv("HOME");
        dir = (home && *home) ? fs::path(home) : fs::path("/");
#endif
    } else {
        dir = pathFromUtf8(pathUtf8);
    }
    if (!dir.is_absolute()) throw std::runtime_error("an absolute path is required");
    dir = dir.lexically_normal();
    if (!fs::is_directory(dir, ec)) throw std::runtime_error("not a folder: " + pathToUtf8(dir));

    struct D { std::string name; fs::path path; };
    std::vector<D> found;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_directory(e2)) continue;
        std::string name = pathToUtf8(it->path().filename());
        if (name.empty() || name[0] == '.') continue;
        found.push_back({std::move(name), it->path()});
        if (found.size() >= 5000) break;
    }
    std::sort(found.begin(), found.end(), [](const D& a, const D& b) {
        return std::lexicographical_compare(a.name.begin(), a.name.end(), b.name.begin(), b.name.end(), [](unsigned char x, unsigned char y) { return std::tolower(x) < std::tolower(y); });
    });
    json dirs = json::array();
    for (const auto& d : found) dirs.push_back(json{{"name", d.name}, {"path", pathToUtf8(d.path)}});

    // the parent of a root ("C:\\", "/") is "nothing", which the Shell shows as the drive list / stops at
    json parent = nullptr;
    const fs::path pp = dir.parent_path();
    if (!pp.empty() && pp != dir) parent = pathToUtf8(pp);
#ifdef _WIN32
    else parent = "";  // at a drive root: go up to the drive list
#endif
    return json{{"path", pathToUtf8(dir)}, {"parent", parent}, {"dirs", dirs}, {"drives", drives},
                {"hasProfile", fs::exists(dir / "game_profile.json", ec)}};
}
}  // namespace

void Router::handleBrowseFilesystem(const json& req, RouterOutput& out) {
    const std::string path = req.contains("path") && req["path"].is_string() ? req["path"].get<std::string>() : std::string();
    json r;
    try {
        r = browseFolders(path);
    } catch (const std::exception& e) {
        out.toSender.push_back(failedResponse(req, e.what()));
        return;
    }
    r["type"] = proto::kResponse;
    r["requestId"] = requestId(req);
    r["ok"] = true;
    out.toSender.push_back(r.dump());
}

void Router::handleSetAssetsRoot(const json& req, RouterOutput& out) {
    const std::string path = req.at("path").get<std::string>();
    if (path.empty()) {
        out.toSender.push_back(failedResponse(req, "no folder given"));
        return;
    }
    try {
        scene_.setAssetsRoot(pathFromUtf8(path));
    } catch (const std::exception& e) {
        out.toSender.push_back(failedResponse(req, e.what()));
        return;
    }
    // the new game folder's own settings (REQUIREMENTS §32): its editor_settings.json, and its lighting.json from the next Core start
    loadGameSettings(settings_, scene_.assetsRoot());
    if (!lightingFile_.empty()) {
        lightingFile_ = scene_.assetsRoot() / "lighting.json";
        LightingService::writeDefaultOptions(lightingFile_);
    }
    const bool persisted = persistSettings();
    pushConfig(out);
    out.toAll.push_back(scene_.documentSyncJson().dump());
    pushDerivedAll(out);
    pushLibraryIfChanged(out, true);
    out.toSender.push_back(json{{"type", proto::kResponse}, {"requestId", requestId(req)}, {"ok", true},
                                {"assetsRoot", pathToUtf8(scene_.assetsRoot())}, {"path", scene_.currentPath()}, {"persisted", persisted}}
                               .dump());
}

}  // namespace core

// ---- settings window: native dialogs, editor settings, game profile, lighting.json, starting the game ------------------------------------
namespace core {

namespace {
// `p` relative to `root` with '/' separators, or nullopt when it is not inside it.
std::optional<std::string> insideRoot(const fs::path& root, const fs::path& p) {
    std::error_code ec;
    const fs::path base = fs::weakly_canonical(root, ec), full = fs::weakly_canonical(p, ec);
    const fs::path rel = full.lexically_relative(base);
    if (rel.empty() || rel == "." || *rel.begin() == "..") return std::nullopt;
    const auto u = rel.generic_u8string();
    return std::string(u.begin(), u.end());
}

bool writeJsonFile(const fs::path& file, const json& j) {
    try {
        fs::path tmp = file;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out << j.dump(2) << "\n";
            if (!out) return false;
        }
        fs::rename(tmp, file);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

json readJsonFile(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return json::object();
    try {
        json j = json::parse(in);
        return j.is_object() ? j : json::object();
    } catch (const json::parse_error&) {
        return json::object();
    }
}

void replaceAll(std::string& s, const std::string& what, const std::string& with) {
    for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + with.size())) s.replace(at, what.size(), with);
}
}  // namespace

std::string expandLaunchText(const std::string& text, const fs::path& gameRoot, const std::string& bsp) {
    std::string s = text;
    const fs::path rel = pathFromUtf8(bsp);
    std::string name = pathToUtf8(rel.filename());
    if (name.size() > 4 && name.substr(name.size() - 4) == ".bsp") name.resize(name.size() - 4);
    replaceAll(s, "{mapname}", name);
    replaceAll(s, "{map}", bsp);
    replaceAll(s, "{bsp}", pathToUtf8((gameRoot / rel).make_preferred()));
    replaceAll(s, "{game}", pathToUtf8(fs::path(gameRoot).make_preferred()));
    return s;
}

void Router::handleNativeDialog(const json& req, RouterOutput& out) {
    const json id = requestId(req);
    if (!nativeDialogsAvailable()) {
        out.toSender.push_back(json{{"type", proto::kResponse}, {"requestId", id}, {"ok", false}, {"unsupported", true}, {"error", "no native dialogs on this platform"}}.dump());
        return;
    }
    NativeDialogRequest d;
    const std::string kind = req.value("kind", std::string("open"));
    d.kind = kind == "folder" ? NativeDialogRequest::Kind::Folder : kind == "save" ? NativeDialogRequest::Kind::Save : NativeDialogRequest::Kind::Open;
    d.title = req.value("title", std::string());
    d.fileName = req.value("fileName", std::string());
    d.defaultExt = req.value("defaultExt", std::string());
    if (req.contains("filters") && req["filters"].is_array())
        for (const auto& f : req["filters"])
            if (f.is_object() && f.value("name", json()).is_string() && f.value("pattern", json()).is_string())
                d.filters.emplace_back(f["name"].get<std::string>(), f["pattern"].get<std::string>());
    const bool relative = req.value("relative", false);
    const fs::path root = scene_.assetsRoot();
    const std::string start = req.value("startDir", std::string());
    d.startDir = relative ? pathToUtf8((root / pathFromUtf8(start)).lexically_normal()) : start;
    out.background.push_back([d, relative, root, id]() -> std::string {
        const NativeDialogResult r = showNativeDialog(d);
        json res{{"type", proto::kResponse}, {"requestId", id}, {"ok", r.error.empty()}};
        if (!r.error.empty()) res["error"] = r.error;
        if (r.cancelled) res["cancelled"] = true;
        if (r.error.empty() && !r.cancelled) {
            res["absolutePath"] = r.path;
            if (!relative) res["path"] = r.path;
            else if (auto rel = insideRoot(root, pathFromUtf8(r.path))) res["path"] = *rel;
            else { res["ok"] = false; res["error"] = "choose a location inside the game folder (" + pathToUtf8(root) + ")"; }
        }
        return res.dump();
    });
}

void Router::handleSetSettings(const json& req, RouterOutput& out) {
    const std::string root = pathToUtf8(scene_.assetsRoot());
    if (req.contains("game") && req["game"].is_object()) {
        GameLaunch g = settings_.games.count(root) ? settings_.games[root] : GameLaunch{};
        const json& j = req["game"];
        if (j.contains("exe") && j["exe"].is_string()) g.exe = j["exe"].get<std::string>();
        if (j.contains("args") && j["args"].is_string()) g.args = j["args"].get<std::string>();
        if (j.contains("workingDir") && j["workingDir"].is_string()) g.workingDir = j["workingDir"].get<std::string>();
        settings_.games[root] = g;
    }
    if (req.contains("editor") && req["editor"].is_object()) settings_.editor = req["editor"];
    if (req.value("clearRecent", false)) std::erase_if(settings_.recentMaps, [&](const RecentMap& r) { return r.assetsRoot == root; });
    const bool persisted = persistSettings();
    out.toAll.push_back(settingsJson().dump());
    out.toSender.push_back(json{{"type", proto::kResponse}, {"requestId", requestId(req)}, {"ok", true}, {"persisted", persisted}}.dump());
}

void Router::handleGameProfile(const json& req, RouterOutput& out) {
    if (req["type"] == proto::kWriteGameProfile) {
        if (!req.contains("profile") || !req["profile"].is_object()) { out.toSender.push_back(failedResponse(req, "write_game_profile needs a profile object")); return; }
        const std::string err = writeProfileFile(scene_.assetsRoot(), req["profile"]);
        if (!err.empty()) { out.toSender.push_back(failedResponse(req, err)); return; }
        try {
            scene_.reloadGameProfile();
        } catch (const std::exception& e) {
            out.toSender.push_back(failedResponse(req, e.what()));
            return;
        }
        pushConfig(out);
    }
    out.toSender.push_back(json{{"type", proto::kResponse}, {"requestId", requestId(req)}, {"ok", true},
                                {"file", readProfileFile(scene_.assetsRoot())}, {"profile", scene_.profile().toJson()}}.dump());
}

void Router::handleLightingConfig(const json& req, RouterOutput& out) {
    if (lightingFile_.empty()) { out.toSender.push_back(failedResponse(req, "this Core has no lighting settings file")); return; }
    json warning;
    if (req["type"] == proto::kWriteLightingConfig) {
        if (!req.contains("config") || !req["config"].is_object()) { out.toSender.push_back(failedResponse(req, "write_lighting_config needs a config object")); return; }
        json merged = readJsonFile(lightingFile_);
        for (const auto& [k, v] : req["config"].items()) merged[k] = v;
        if (!writeJsonFile(lightingFile_, merged)) { out.toSender.push_back(failedResponse(req, "cannot write " + pathToUtf8(lightingFile_))); return; }
        std::string w;
        lightingOptions_ = LightingService::loadOptions(lightingFile_, &w);  // BSP exports use it from now on
        if (!w.empty()) warning = w;
    }
    // what is in effect when the file is read (values clamped, missing keys at their defaults)
    const json config = LightingService::optionsJson(LightingService::loadOptions(lightingFile_));
    json res{{"type", proto::kResponse}, {"requestId", requestId(req)}, {"ok", true}, {"config", config}, {"file", pathToUtf8(lightingFile_)},
             {"defaults", LightingService::optionsJson(LightingService::Options{})}};
    if (!warning.is_null()) res["warning"] = warning;
    out.toSender.push_back(res.dump());
}

void Router::handleLaunchGame(const json& req, RouterOutput& out) {
    const std::string root = pathToUtf8(scene_.assetsRoot());
    const auto g = settings_.games.find(root);
    if (g == settings_.games.end() || g->second.exe.empty()) { out.toSender.push_back(failedResponse(req, "no game executable is set (Settings > Game)")); return; }
    const std::string map = req.value("map", std::string());
    const fs::path rel = pathFromUtf8(map);
    if (map.empty() || rel.is_absolute() || rel.has_root_name() || rel.lexically_normal().generic_string().rfind("..", 0) == 0) {
        out.toSender.push_back(failedResponse(req, "launch_game needs an exported map inside the game folder"));
        return;
    }
    const std::string args = expandLaunchText(g->second.args, scene_.assetsRoot(), map), wd = expandLaunchText(g->second.workingDir, scene_.assetsRoot(), map);
    const std::string err = launchProcess(g->second.exe, args, wd);
    if (!err.empty()) { out.toSender.push_back(failedResponse(req, err)); return; }
    out.toSender.push_back(json{{"type", proto::kResponse}, {"requestId", requestId(req)}, {"ok", true}, {"commandLine", g->second.exe + " " + args}}.dump());
}

}  // namespace core
