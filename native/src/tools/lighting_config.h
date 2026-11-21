// tools/lighting_config -- lighting.json of the game folder: what the export bakes with (bake quality, light volumes, denoising, the live view's options) and how Settings edits it
// (port of the Lighting page of shell/src/ui/settingsDialog.ts and the Core router's read / write_lighting_config).
//
// Pure file work, no Qt: `readLightingConfig` gives every key as the Core reads it (values clamped, missing keys at their defaults) next to the defaults; `writeLightingConfig` merges changed
// keys into the file (keys the editor does not know stay) and checks that the Core still reads it. A BSP export reads the file each time, so a change counts from the next export.
// Thread: any.
// Spec: 07_EDITOR_PORT.md (settingsDialog.ts), 08_MILESTONES.md T11.10d.
#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace native::tools {

struct LightingKey {
    std::string key, label, hint;
};
struct LightingGroup {
    std::string title;
    std::vector<LightingKey> keys;
};

/// The keys the page shows, in groups (Lightmaps, Quality, Denoising, Light volumes, Live view, Performance). Keys the Core has beyond these go to "Other".
const std::vector<LightingGroup>& lightingGroups();

struct LightingConfig {
    std::string file;           // the path as shown ("Stored in ...")
    nlohmann::json values;      // every key in effect
    nlohmann::json defaults;    // every key at the Core's default
    std::string warning;        // what was wrong with the file (it still gives the defaults), "" when nothing
};

/// Reads `file` (a missing or damaged file gives the defaults and a warning for a damaged one).
LightingConfig readLightingConfig(const std::filesystem::path& file);
/// Merges `changes` (an object of key -> number / bool) into the file, creating it (and its folder) when needed. "" = written, else why not. Unknown keys and values of the wrong type are refused.
std::string writeLightingConfig(const std::filesystem::path& file, const nlohmann::json& changes);

} // namespace native::tools
