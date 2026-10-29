#pragma once
#include "core/profile.h"
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace core {

// Texture paths are Quake-style: relative to the profile's textureRoot, forward slashes, NO extension
// ("base_wall/concrete01"). The extension is found by trying profile.textureExtensions in order.

/// Every texture under the texture root, deduplicated across extensions, and the skyboxes (listSkyboxes), sorted. `limit` bounds the scan.
std::vector<std::string> listTextures(const std::filesystem::path& assetsRoot, const GameProfile& profile,
                                       size_t limit = 50000);

/// A texture name starting with '/' is relative to the assets root instead of the profile's textureRoot, and its extension is not limited to the profile's
/// list (a model's image, obj_model.h): "/models/props/crate" is <assets>/models/props/crate.png.
bool isRootRelativeTexture(const std::string& name);
/// The image file of a texture name (extension found as described above); nullopt when unsafe or missing. `ext` receives the matching extension.
std::optional<std::filesystem::path> findTextureFile(const std::filesystem::path& assetsRoot, const GameProfile& profile, const std::string& name, std::string* ext = nullptr);

struct TextureFile {
    std::string ext;  // the extension that matched, e.g. ".tga"
    std::vector<unsigned char> bytes;
};

/// nullopt if the path is unsafe (absolute, "..", odd characters), missing, or larger than `maxBytes`.
std::optional<TextureFile> readTexture(const std::filesystem::path& assetsRoot, const GameProfile& profile,
                                        const std::string& path, size_t maxBytes = 32u * 1024u * 1024u);

// ---- skyboxes (REQUIREMENTS §28 K5, DESIGN §12 "Sky") ----------------------------------------------------------------------------------------
// The engine's convention (SdlWasmEngine Quake3Bsp ResolveFaceTextureData + CubemapTexture::DecodeSmart): a texture whose name ends in "cube" is a
// cubemap loaded from <assetsRoot>/env/<last part of the name>, whatever folder the name has. Per extension .png, .jpg, .jpeg in that order: the six
// faces <base>_lf, _rt, _up, _dn, _ft, _bk when all exist, else <base> itself as an equirectangular panorama. Listed as "env/<base>".

/// The last part of the name ends in "cube" (case-sensitive, as the engine checks it).
bool isCubemapTexture(const std::string& path);
/// Every skybox in <assetsRoot>/env as "env/<base>" (faces or panorama; only bases that end in "cube"), sorted.
std::vector<std::string> listSkyboxes(const std::filesystem::path& assetsRoot);
struct SkyboxFiles {
    bool panorama = false;
    std::vector<TextureFile> images;  // the faces in the engine's order lf, rt, up, dn, ft, bk; or the one panorama
};
/// The files the engine would load for a cubemap texture; nullopt when there are none (or the name is unsafe or too large).
std::optional<SkyboxFiles> readSkybox(const std::filesystem::path& assetsRoot, const std::string& path, size_t maxBytes = 32u * 1024u * 1024u);

// ---- texture properties (REQUIREMENTS 19 A7) -----------------------------------------------------------------------------------------------
// Stored next to the texture file as `<texture file name incl. extension>.json` ("wall.tga.json"):
//   { "pixelsPerMeter": 64, "repeat": { "u": AXIS, "v": AXIS } }
//   AXIS = { "mode": "repeat"|"mirror"|"anchor", "anchor": "start"|"end", "split": 0.02..0.98, "fill": "repeat"|"mirror",
//            "alt": null | { "texture": "<texture path>", "offset": [du, dv] } }
// The Shell interprets them (shell/src/model/texprops.ts, uvmesh.ts); Core only validates and stores. Unknown keys are kept.

/// A copy of `props` with every known key checked and clamped, and every unusable one dropped. Non-objects give an empty object.
nlohmann::json sanitizeTextureProps(const nlohmann::json& props);

/// The sidecar of an existing texture. nullopt = no texture, no sidecar, or an unreadable one (the texture then has the defaults).
std::optional<nlohmann::json> readTextureProps(const std::filesystem::path& assetsRoot, const GameProfile& profile, const std::string& path);

/// Merges the sanitised `props` into the sidecar (keys the Core does not know stay). Returns "" on success, else the reason.
std::string writeTextureProps(const std::filesystem::path& assetsRoot, const GameProfile& profile, const std::string& path, const nlohmann::json& props);

std::string base64Encode(const std::vector<unsigned char>& bytes);

}  // namespace core
