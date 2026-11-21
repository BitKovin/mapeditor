// doc/level_settings -- the level settings: worldspawn keys with defaults, ranges and hints (port of shell/src/model/levelSettings.ts and the level part of model/sky.ts).
//
// Owns: the field tables the Level settings panel shows (`levelSections`), reading a value from the worldspawn (an absent key means the default), what to store for a typed value
// (`levelKeyFor`: the key goes away for empty input and for the default), the ambient colour / intensity pair of `_ambient`, the Auto sky keys, and `setLevelKeys`, the one edit that
// writes several worldspawn keys as one version (the worldspawn is made when the map has none). Pure: no Qt, no GPU.
// Thread: any (pure); the edit on the UI thread like every VersionEditor use.
// Spec: 07_EDITOR_PORT.md (levelSettings.ts), 08_MILESTONES.md T11.10a.
#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/document.h"
#include "doc/document.h"

namespace native::doc {

struct LevelField {
    std::string key, label;
    std::optional<double> dflt;   // what an absent key means; nullopt = the Core's lighting.json value
    double min = 0, max = 0, step = 1;
    bool isInt = false;
    std::string hint;
};

struct LevelSection {
    std::string title, note;
    std::vector<LevelField> fields;
};

/// Every section of the panel in display order (Light, Colour correction, Ambient occlusion, Quality falloff, Export, Visibility, Optimisation). The Sky section is separate (keys below).
const std::vector<LevelSection>& levelSections();
/// The field of a key, or null.
const LevelField* levelField(const std::string& key);

/// The worldspawn of a document version, or null.
const core::Entity* worldspawnOf(const DocumentVersion& version);

/// The stored value of a field; nullopt when the key is absent, empty or not a number (then the default applies).
std::optional<double> levelValue(const core::Entity* world, const LevelField& field);
/// What to store for a typed value: nullopt (remove the key) for empty input, text that is no number and the default; else the clamped number (rounded for integer fields).
std::optional<std::string> levelKeyFor(const LevelField& field, const std::string& input);
/// A number as stored in the map: up to 6 decimals, no trailing zeros ("2", "0.25").
std::string levelNumber(double value);

struct Ambient {
    std::array<double, 3> color{1, 1, 1};   // 0..1, the brightest channel 1
    double intensity = 0;                   // the brightest channel of `_ambient`; 0 = none
};
/// worldspawn `_ambient` "r g b" as a colour and an intensity.
Ambient ambientOf(const core::Entity* world);
/// The `_ambient` value for a colour and an intensity; nullopt (remove the key) when the intensity is 0.
std::optional<std::string> ambientKey(const std::array<double, 3>& color, double intensity);

/// "#rrggbb" of a 0..1 colour and back (white for text that is not a colour).
std::string hexOfColor(const std::array<double, 3>& color);
std::array<double, 3> colorOfHex(const std::string& hex);

// ---- sky (the keys the Core's skySettingsOf reads) ----
inline constexpr const char* kSkyAutoKey = "_sky_auto";
inline constexpr const char* kSkyMarginKey = "_sky_margin";
inline constexpr const char* kSkyTextureKey = "_sky_texture";
inline constexpr double kSkyMarginDefault = 8, kSkyMarginMax = 1000;
enum class AutoSky { Off, Box, Areas };
struct AutoSkyOption {
    AutoSky value;
    const char* text;    // what the key stores ("" for Off: the key is removed)
    const char* label;
    const char* title;
};
const std::vector<AutoSkyOption>& autoSkyOptions();
AutoSky autoSkyOf(const core::Entity* world);
double skyMarginOf(const core::Entity* world);
/// What to store for a typed margin: nullopt (remove the key) for empty input, text that is no number and the default.
std::optional<std::string> skyMarginKey(const std::string& input);
/// A skybox: the last part of the name ends in "cube" (case-sensitive, as the engine checks it).
bool isCubemapPath(const std::string& path);
/// A skybox of the assets' env folder as the Core lists it ("env/<name>").
bool isEnvSkybox(const std::string& path);

// ---- the edit ----
using LevelChange = std::pair<std::string, std::optional<std::string>>;   // key, value (nullopt = remove the key)
/// The version with the worldspawn keys changed; the worldspawn is made (id "e_worldspawn") when a value is set and there is none. Null when nothing changed or the edit failed.
std::shared_ptr<const DocumentVersion> setLevelKeys(const std::shared_ptr<const DocumentVersion>& base, const std::vector<LevelChange>& changes);

} // namespace native::doc
