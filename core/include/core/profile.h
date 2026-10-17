#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>
#include <map>
#include <string>
#include <vector>

namespace core {

// Per-game settings, read from <assetsRoot>/game_profile.json (optional; defaults are Quake 3-like).
// Every path field is relative to the assets root and must stay inside it.
struct GameProfile {
    std::string name = "default";
    std::string textureRoot = "textures";                                       // relative to the assets root
    std::vector<std::string> textureExtensions = {".tga", ".jpg", ".jpeg", ".png"};  // lookup order
    std::vector<std::string> modelExtensions = {".md3"};                        // not used yet
    std::string entityDefinitions = "entities.ent";  // Quake 3 .ent file, relative to the assets root (optional)
    std::string mapRoot = "maps";                     // where the file browser starts for maps
    std::string prefabRoot = "prefabs";               // ... and for prefabs
    std::string texGroupRoot = "texgroups";           // texture groups (REQUIREMENTS 20), relative to the assets root
    // Texture pixels per meter when a texture has no properties file of its own (REQUIREMENTS 19 A2). 1 m = 32 Quake units and Quake's default
    // texture scale is 0.5, so a 64 pixel texture is 1 m.
    double pixelsPerMeter = 64;

    // Lighting (DESIGN §6.5). Entity classnames that BLOCK light (their surfaces cast shadows and bounce light in ray tracing) and that
    // RECEIVE BAKED light (they get lightmaps). worldspawn, func_group and func_detail are always in both lists; the profile only adds
    // to them. A brush entity that does not receive baked light is lit from light volumes instead.
    //   "lighting": { "blockLight": ["func_door"], "receiveBakedLight": ["func_wall"] }
    std::vector<std::string> blockLight;
    std::vector<std::string> receiveBakedLight;
    // Light style patterns (id 1..63 -> letters a..z, 10 Hz, 'a' = 0, 'm' = 1, 'z' = 25/12), overriding the engine's built-in table:
    //   "lighting": { "lightStyles": { "12": "mmmmaaaa" } }
    std::map<int, std::string> lightStyles;
    // BSP export (REQUIREMENTS §26 F4): content / surface flags of textures and entity classes, as the game wants them in the rBSP shader and
    // brush lumps. The object is kept as written and interpreted by the exporter (bsp_compile.h BspContentRules; layout in DESIGN §12):
    //   "bsp": { "contentFlags": {"solid": 1, ...}, "surfaceFlags": {"nodraw": 128, ...},
    //            "textures": [{"match": "common/clip", "contents": ["playerclip"], "surface": ["nodraw"]}, ...],
    //            "classes":  [{"match": "trigger_*", "contents": ["trigger"]}] }
    // Absent: Quake 3's flag values and rules for common/*, water, sky and trigger_* classes.
    nlohmann::json bsp;
    // The export format of this game (REQUIREMENTS §31 X2): an exporter id of exporter.h ("rbsp", "obj"). An unknown id exports rBSP, with a warning.
    std::string exporter = "rbsp";

    nlohmann::json toJson() const;
};

// Missing file -> defaults. Malformed file -> throws std::runtime_error (a broken profile should be
// loud, not silently replaced by defaults).
GameProfile loadProfile(const std::filesystem::path& assetsRoot);
// The same rules on an already parsed object (throws std::runtime_error with the reason).
GameProfile profileFromJson(const nlohmann::json& j);

// The file as written (unknown keys included); a missing or broken file gives {}.
nlohmann::json readProfileFile(const std::filesystem::path& assetsRoot);
// Merges `changes` into game_profile.json (top-level keys replace, `lighting` merges one level, null removes a key), refuses a result
// profileFromJson would refuse, and writes atomically. Empty string = done, else the reason.
std::string writeProfileFile(const std::filesystem::path& assetsRoot, const nlohmann::json& changes);

}  // namespace core
