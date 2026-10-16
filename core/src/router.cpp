#include "core/router.h"
#include "core/texgroups.h"
#include "core/protocol.h"
#include "core/textures.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>

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
}  // namespace

Router::Router(Scene& scene, fs::path settingsFile, const LightingService::Options& lightingOptions)
    : scene_(scene), lastLibraryFingerprint_(scene.libraryFingerprint()), lastConfigFingerprint_(scene.configFingerprint()),
      settingsFile_(std::move(settingsFile)), lighting_(scene, lightingOptions), lightingOptions_(lightingOptions) {
    settings_.assetsRoot = pathToUtf8(scene.assetsRoot());
    settings_.lastMap = scene.currentPath();
}

json Router::settingsJson() const { return json{{"type", proto::kSettings}, {"assetsRoot", pathToUtf8(scene_.assetsRoot())}}; }

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
    out.toAll.push_back(json{{"type", proto::kProfile}, {"profile", scene_.profile().toJson()}}.dump());
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
    if (!req.contains("path") || !req["path"].is_string()) { out.toSender.push_back(failedResponse(req, "export_bsp needs a path")); return; }
    BspExportRequest r;
    r.assetsRoot = scene_.assetsRoot();
    r.document = scene_.documentSyncJson()["doc"];
    r.mapPath = scene_.currentPath();
    r.relief = lastRelief_;
    r.outPath = req["path"].get<std::string>();
    r.lighting = lightingOptions_;
    r.bake = req.value("bake", true);
    if (!export_.start(std::move(r))) { out.toSender.push_back(failedResponse(req, "an export is already running")); return; }
    out.toSender.push_back(json{{"type", proto::kResponse}, {"requestId", requestId(req)}, {"ok", true}}.dump());
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
    out.toSender.push_back(json{{"type", proto::kProfile}, {"profile", scene_.profile().toJson()}}.dump());
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
        else if (type == proto::kListTextures) handleListTextures(req, out);
        else if (type == proto::kReadTexture) handleReadTexture(req, out);
        else if (type == proto::kWriteTextureProps) handleWriteTextureProps(req, out);
        else if (type == proto::kListTexGroups) handleListTexGroups(req, out);
        else if (type == proto::kReadTexGroup) handleReadTexGroup(req, out);
        else if (type == proto::kWriteTexGroup) handleWriteTexGroup(req, out);
        else if (type == proto::kDeleteTexGroup) handleDeleteTexGroup(req, out);
        else if (type == proto::kReadPrefab) handleReadPrefab(req, out);
        else if (type == proto::kListDirectory) handleListDirectory(req, out);
        else if (type == proto::kBrowseFilesystem) handleBrowseFilesystem(req, out);
        else if (type == proto::kSetAssetsRoot) handleSetAssetsRoot(req, out);
        else if (type == proto::kLighting) handleLighting(req, out);
        else if (type == proto::kReliefGeometry) { if (scene_.setReliefGeometry(req)) lighting_.markDirty(); lastRelief_ = req; }
        else if (type == proto::kExportBsp || type == proto::kExportCancel) handleExport(req, out);
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
    persistSettings();  // remember the map for next start-up; failure to write is not worth failing the open
    // Opening replaces the document for every window, sender included.
    out.toAll.push_back(scene_.documentSyncJson().dump());
    pushDerivedAll(out);
    pushLibraryIfChanged(out, true);
    out.toSender.push_back(json{{"type", proto::kResponse}, {"requestId", requestId(req)}, {"ok", true},
                                {"path", scene_.currentPath()}, {"rev", scene_.revision()}}
                               .dump());
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
    persistSettings();
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
