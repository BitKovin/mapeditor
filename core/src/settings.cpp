#include "core/settings.h"
#include <fstream>
#include <nlohmann/json.hpp>

using nlohmann::json;
namespace fs = std::filesystem;

namespace core {

std::string pathToUtf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

fs::path pathFromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

void Settings::touchRecent(const std::string& root, const std::string& path) {
    if (root.empty() || path.empty()) return;
    const RecentMap r{root, path};
    std::erase(recentMaps, r);
    recentMaps.insert(recentMaps.begin(), r);
    if (recentMaps.size() > kMaxRecentMaps) recentMaps.resize(kMaxRecentMaps);
}

namespace {
GameLaunch launchFromJson(const json& g) {
    GameLaunch l;
    if (g.contains("exe") && g["exe"].is_string()) l.exe = g["exe"].get<std::string>();
    if (g.contains("args") && g["args"].is_string()) l.args = g["args"].get<std::string>();
    if (g.contains("workingDir") && g["workingDir"].is_string()) l.workingDir = g["workingDir"].get<std::string>();
    return l;
}
json launchJson(const GameLaunch& g) { return json{{"exe", g.exe}, {"args", g.args}, {"workingDir", g.workingDir}}; }

bool writeAtomically(const fs::path& file, const json& j) {
    try {
        if (file.has_parent_path()) fs::create_directories(file.parent_path());
        fs::path tmp = file;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out << j.dump(2) << "\n";
            out.flush();
            if (!out) return false;
        }
        fs::rename(tmp, file);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
}  // namespace

bool loadGameSettings(Settings& s, const fs::path& assetsRoot) {
    std::ifstream in(assetsRoot / kGameSettingsFile, std::ios::binary);
    if (!in) return false;
    try {
        const json j = json::parse(in);
        if (!j.is_object()) return false;
        if (j.contains("lastMap") && j["lastMap"].is_string()) s.lastMap = j["lastMap"].get<std::string>();
        if (j.contains("game") && j["game"].is_object()) s.games[pathToUtf8(assetsRoot)] = launchFromJson(j["game"]);
        if (j.contains("editor") && j["editor"].is_object()) s.editor = j["editor"];
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

Settings loadSettings(const fs::path& file) {
    Settings s;
    std::ifstream in(file, std::ios::binary);
    if (!in) return s;
    try {
        const json j = json::parse(in);
        if (j.is_object()) {
            if (j.contains("assetsRoot") && j["assetsRoot"].is_string()) s.assetsRoot = j["assetsRoot"].get<std::string>();
            if (j.contains("lastMap") && j["lastMap"].is_string()) s.lastMap = j["lastMap"].get<std::string>();
            if (j.contains("recentMaps") && j["recentMaps"].is_array())
                for (const auto& r : j["recentMaps"])
                    if (r.is_object() && r.value("assetsRoot", json()).is_string() && r.value("path", json()).is_string() && s.recentMaps.size() < Settings::kMaxRecentMaps)
                        s.recentMaps.push_back({r["assetsRoot"].get<std::string>(), r["path"].get<std::string>()});
            if (j.contains("games") && j["games"].is_object())
                for (const auto& [root, g] : j["games"].items())
                    if (g.is_object()) s.games[root] = launchFromJson(g);
            if (j.contains("editor") && j["editor"].is_object()) s.editor = j["editor"];
        }
    } catch (const std::exception&) {
        // fall through with defaults
    }
    if (!s.assetsRoot.empty()) loadGameSettings(s, pathFromUtf8(s.assetsRoot));
    return s;
}

bool saveSettings(const fs::path& file, const Settings& s) {
    json recent = json::array(), games = json::object();
    for (const auto& r : s.recentMaps) recent.push_back(json{{"assetsRoot", r.assetsRoot}, {"path", r.path}});
    std::error_code ec;
    const bool inGame = !s.assetsRoot.empty() && fs::is_directory(pathFromUtf8(s.assetsRoot), ec);  // the game folder's part goes into it
    bool ok = true;
    if (inGame) {
        const auto g = s.games.find(s.assetsRoot);
        ok = writeAtomically(pathFromUtf8(s.assetsRoot) / kGameSettingsFile,
                             json{{"lastMap", s.lastMap}, {"game", launchJson(g != s.games.end() ? g->second : GameLaunch{})}, {"editor", s.editor}});
    }
    for (const auto& [root, g] : s.games) if (!inGame || root != s.assetsRoot) games[root] = launchJson(g);
    json j{{"assetsRoot", s.assetsRoot}, {"recentMaps", recent}, {"games", games}};
    if (!inGame) { j["lastMap"] = s.lastMap; j["editor"] = s.editor; }
    return writeAtomically(file, j) && ok;
}

}  // namespace core
