#pragma once
#include "core/bsp_export.h"
#include "core/lighting_service.h"
#include "core/scene.h"
#include "core/settings.h"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace core {

// What to send after handling one incoming message. Pure data, so the whole
// protocol is unit-testable without sockets.
struct RouterOutput {
    std::vector<std::string> toSender;  // just the session that sent the message
    std::vector<std::string> toOthers;  // every session except the sender
    std::vector<std::string> toAll;     // every session, sender included
    // Work that waits for the person (a native file dialog): each job runs on a thread of its own so the server keeps serving;
    // the text it returns is sent to the sender. Tests simply call them.
    std::vector<std::function<std::string()>> background;

    enum class To { Sender, Others, All };
    struct Outgoing {
        To to;
        const std::string* text;
    };

    // The order messages must be delivered in: state first (toAll, toOthers),
    // the sender's reply last. A Shell that awaits the reply to open_document
    // must find the new document already loaded when it resolves.
    std::vector<Outgoing> inDeliveryOrder() const {
        std::vector<Outgoing> v;
        for (const auto& m : toAll) v.push_back({To::All, &m});
        for (const auto& m : toOthers) v.push_back({To::Others, &m});
        for (const auto& m : toSender) v.push_back({To::Sender, &m});
        return v;
    }
};

// Maps protocol messages (protocol.h) onto Scene calls and decides who
// receives what. See DESIGN.md §10 for the reasoning behind the rules:
//   * the sender of an accepted document_update gets an ack, NOT an echo --
//     echoing would overwrite edits the Shell made while the update was in
//     flight, defeating zero-latency editing;
//   * everyone else gets the new document;
//   * a rejected update touches nothing and only the sender hears about it.
// {map}, {mapname}, {bsp} and {game} in a launch argument / folder (see GameLaunch); `bsp` is relative to `gameRoot`.
std::string expandLaunchText(const std::string& text, const std::filesystem::path& gameRoot, const std::string& bsp);

class Router {
public:
    // `settingsFile`: where set_assets_root / open / save persist settings (empty = don't persist).
    explicit Router(Scene& scene, std::filesystem::path settingsFile = {}, const LightingService::Options& lightingOptions = {});

    RouterOutput onConnect();
    RouterOutput onMessage(const std::string& text);
    // Called about once a second by the server: notices files edited outside the editor (prefabs,
    // game_profile.json, the .ent file) and returns what to push to every session. Empty when nothing changed.
    RouterOutput onTick();
    // Called about every 50 ms: refines the live lighting for up to `budgetSeconds` and returns what to push to every session.
    RouterOutput onLightingTick();                       // refines for LightingService::Options::tickBudgetSeconds
    RouterOutput onLightingTick(double budgetSeconds);
    // True while the live lighting has more to refine (the server then ticks again without waiting).
    bool lightingBusy() const { return lighting_.busy(); }
    LightingService& lighting() { return lighting_; }
    // lighting.json (Settings > Lighting reads and writes it); empty = that page is unavailable.
    void setLightingFile(std::filesystem::path file) { lightingFile_ = std::move(file); }
    // BSP export in progress (tests wait on it).
    bool exportRunning() const { return export_.running(); }

private:
    void handleUpdate(const nlohmann::json& req, RouterOutput& out);
    void handleOpen(const nlohmann::json& req, RouterOutput& out);
    void handleSave(const nlohmann::json& req, RouterOutput& out);
    void handleListTextures(const nlohmann::json& req, RouterOutput& out);
    void handleReadTexture(const nlohmann::json& req, RouterOutput& out);
    void handleWriteTextureProps(const nlohmann::json& req, RouterOutput& out);
    void handleListTexGroups(const nlohmann::json& req, RouterOutput& out);
    void handleReadTexGroup(const nlohmann::json& req, RouterOutput& out);
    void handleWriteTexGroup(const nlohmann::json& req, RouterOutput& out);
    void handleDeleteTexGroup(const nlohmann::json& req, RouterOutput& out);
    void handleReadPrefab(const nlohmann::json& req, RouterOutput& out);
    void handleListDirectory(const nlohmann::json& req, RouterOutput& out);
    void handleBrowseFilesystem(const nlohmann::json& req, RouterOutput& out);
    void handleSetAssetsRoot(const nlohmann::json& req, RouterOutput& out);
    void handleLighting(const nlohmann::json& req, RouterOutput& out);
    void handleNativeDialog(const nlohmann::json& req, RouterOutput& out);
    void handleSetSettings(const nlohmann::json& req, RouterOutput& out);
    void handleGameProfile(const nlohmann::json& req, RouterOutput& out);
    void handleLightingConfig(const nlohmann::json& req, RouterOutput& out);
    void handleLaunchGame(const nlohmann::json& req, RouterOutput& out);
    void handleExport(const nlohmann::json& req, RouterOutput& out);
    void pushDerivedAll(RouterOutput& out);  // derived_geometry to everyone, and the lighting notices the document changed
    void pushDerived(std::vector<std::string>& to);  // derived_geometry, only when the document has subtractive brushes
    void pushConfig(RouterOutput& out);  // profile + entity_defs + settings, to everyone
    bool persistSettings();
    nlohmann::json settingsJson() const;
    void pushLibraryIfChanged(RouterOutput& out, bool force);

    Scene& scene_;
    std::string lastLibraryFingerprint_;
    std::string lastConfigFingerprint_;
    std::filesystem::path settingsFile_;
    std::filesystem::path lightingFile_;
    Settings settings_;
    LightingService lighting_;
    LightingService::Options lightingOptions_;  // lighting.json: the export bakes with the same settings as the live view
    nlohmann::json lastRelief_;                 // the last relief_geometry message, handed to the export
    BspExportJob export_;
};

}  // namespace core
