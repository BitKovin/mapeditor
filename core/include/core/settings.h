#pragma once
#include <filesystem>
#include <string>

namespace core {

// Persisted Core settings: a small settings.json (next to the executable by default). Only things the
// person can change from the Shell live here; per-game parameters belong in game_profile.json.
struct Settings {
    std::string assetsRoot;  // absolute path; empty = not chosen yet
    std::string lastMap;     // relative to assetsRoot; reopened at start-up when it still exists
};

/// Missing or unreadable file -> defaults (never throws: a damaged settings file must not stop start-up).
Settings loadSettings(const std::filesystem::path& file);

/// Atomic write (temp file + rename). Returns false (and does not throw) if the file can't be written.
bool saveSettings(const std::filesystem::path& file, const Settings& s);

/// UTF-8 <-> path conversion that is correct on Windows (where `std::string` paths are the ANSI code page).
std::string pathToUtf8(const std::filesystem::path& p);
std::filesystem::path pathFromUtf8(const std::string& s);

}  // namespace core
