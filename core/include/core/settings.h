#pragma once
#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace core {

// Persisted Core settings: a small settings.json (next to the executable by default). Only things the
// person can change from the Shell live here; per-game parameters belong in game_profile.json.
// How to start the game for File > Play map. Per game folder: different games have different executables.
// In `args` and `workingDir`: {map} = the exported .bsp relative to the game folder, {mapname} = its name without folder and
// extension, {bsp} = its absolute path, {game} = the game folder (absolute). An empty workingDir means the executable's folder.
struct GameLaunch {
    std::string exe;         // absolute path of the game executable
    std::string args = "+map {mapname}";
    std::string workingDir;
    bool operator==(const GameLaunch&) const = default;
};

struct RecentMap {
    std::string assetsRoot;  // absolute
    std::string path;        // relative to assetsRoot
    bool operator==(const RecentMap&) const = default;
};

struct Settings {
    std::string assetsRoot;  // absolute path; empty = not chosen yet
    std::string lastMap;     // relative to assetsRoot; reopened at start-up when it still exists
    std::vector<RecentMap> recentMaps;          // most recent first, at most kMaxRecentMaps
    std::map<std::string, GameLaunch> games;    // keyed by the absolute game (assets) folder
    nlohmann::json editor = nlohmann::json::object();  // the Shell's editor preferences; Core only stores them
    static constexpr size_t kMaxRecentMaps = 12;

    /// Moves (root, path) to the front of recentMaps.
    void touchRecent(const std::string& root, const std::string& path);
};

/// Missing or unreadable file -> defaults (never throws: a damaged settings file must not stop start-up).
Settings loadSettings(const std::filesystem::path& file);

/// Atomic write (temp file + rename). Returns false (and does not throw) if the file can't be written.
bool saveSettings(const std::filesystem::path& file, const Settings& s);

/// UTF-8 <-> path conversion that is correct on Windows (where `std::string` paths are the ANSI code page).
std::string pathToUtf8(const std::filesystem::path& p);
std::filesystem::path pathFromUtf8(const std::string& s);

}  // namespace core
