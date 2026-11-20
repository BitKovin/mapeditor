// tools/profile_form -- the Game profile page of the settings dialog as data: the fields of game_profile.json as editable text, what to write back, and
// the checks that keep the file valid (folders stay inside the assets folder, numbers are numbers). The file is only touched through
// core::writeProfileFile, so keys this form does not know stay as they are.
// Thread: any (pure; load / save touch the disk).
// Spec: 08_MILESTONES.md T4.5 (shell/src/ui/settingsDialog.ts, Game profile page).
#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace native::tools {

struct ProfileForm {
    std::string name;
    std::string textureRoot, mapRoot, prefabRoot, texGroupRoot;   // folders, assets-relative
    std::string entityDefinitions;                                 // file, assets-relative
    std::string textureExtensions, modelExtensions;                // ".tga, .jpg"
    double pixelsPerMeter = 64;
    std::string exporter = "rbsp";                                 // "rbsp" | "obj"
    std::string blockLight, receiveBakedLight;                     // classnames, one per line (or comma / space separated)

    bool operator==(const ProfileForm&) const = default;
};

// The profile of `assetsRoot` as the form shows it (a missing file gives the built-in defaults). `error` is set when the file is malformed (the form then shows defaults).
struct LoadedForm {
    ProfileForm form;
    bool fileExists = false;
    std::string error;
};
LoadedForm loadProfileForm(const std::filesystem::path& assetsRoot);

// A path of the form: relative, no "..", no drive or leading slash. Empty = ok, else the reason.
std::string checkRelativePath(const std::string& path, const char* what);
// The reasons the form cannot be written (empty = fine).
std::vector<std::string> validateProfileForm(const ProfileForm& form);
// The keys that differ from `original`, as the `changes` of core::writeProfileFile (lighting keys under "lighting"). Empty object = nothing changed.
nlohmann::json profileChanges(const ProfileForm& original, const ProfileForm& edited);
// Validates, merges into <assetsRoot>/game_profile.json (created when missing). Empty = written (or nothing to write), else the reason.
std::string saveProfileForm(const std::filesystem::path& assetsRoot, const ProfileForm& original, const ProfileForm& edited);

// ".tga, jpg png" -> {".tga", ".jpg", ".png"}; "a\nb, c" -> {"a", "b", "c"}.
std::vector<std::string> splitList(const std::string& text);
std::vector<std::string> splitExtensions(const std::string& text);
std::string joinList(const std::vector<std::string>& items, const char* separator);

} // namespace native::tools
