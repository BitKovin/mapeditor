#pragma once
#include "core/profile.h"
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace core {

// Quake 3 .map importer (DESIGN §12b): the text of a .map file -> a schema 0.2 document (metres, Y up).
//
// Reads every brush syntax the Quake family has: classic planes (Quake 1 / Quake 3 `tex xoff yoff rot xscale yscale [contents surface value]`),
// Valve 220 (`tex [ ux uy uz off ] [ vx vy vz off ] rot xscale yscale`), Quake 3 brush primitives (`brushDef { ... ( ( a b c ) ( d e f ) ) tex }`),
// and curved surfaces (`patchDef2`, `patchDef3`). Entities keep every key; what the editor models itself is converted (see map_import.cpp).
struct MapImportOptions {
    double unitsPerMeter = 32;       // 1 m = 32 Quake units (DESIGN §4)
    double patchThickness = 4;       // Quake units: a patch is a surface, the editor's brushes are solids, so a patch becomes a slab this thick BEHIND its front side
    double patchTolerance = 2;       // Quake units: largest distance between the tessellated patch and the true curve
    int patchMaxSegments = 16;       // per patch span and direction, an upper bound of the tessellation
    std::string patchBackTexture = "common/caulk";  // texture of the slab's back and sides (a Quake patch is one-sided); empty = the patch's own
    // Pixel size of a texture (Quake style path, no extension); false = unknown. Quake and Valve 220 texture coordinates are in pixels, the
    // editor's UVs in repeats, so the importer must know the image size. Null = every texture is unknown.
    std::function<bool(const std::string&, int&, int&)> textureSize;
    int fallbackTextureSize = 64;    // used for textures textureSize does not know (a warning names them)
};

struct MapImportStats {
    int entities = 0;       // entities in the document
    int brushes = 0;        // solid brushes converted
    int patches = 0;        // patchDef2 / patchDef3 converted (each is one brush in the document)
    int lights = 0;         // `light` / `light_point` entities converted to the editor's light classes
    int faces = 0;
    int skipped = 0;        // brushes / patches that could not be converted (each has a warning)
    int droppedEntities = 0;  // editor bookkeeping entities (TrenchBroom layers and groups without geometry)
};

struct MapImportResult {
    nlohmann::json doc;                  // the document (schemaVersion, entities, prefabInstances)
    std::vector<std::string> warnings;   // everything that was not converted exactly; the document is valid regardless
    MapImportStats stats;
};

/// Throws std::runtime_error ("line N: ...") on text that is not a Quake map.
MapImportResult importQuakeMap(const std::string& text, const MapImportOptions& options = {});

/// Pixel size of an image file's bytes by its extension (.tga .png .jpg .jpeg), without decoding it. False when unreadable.
bool imageSize(const std::vector<unsigned char>& bytes, const std::string& extension, int& width, int& height);

/// Options whose `textureSize` looks textures up in the game folder (profile texture root and extensions); results are cached.
MapImportOptions importOptionsFor(const std::filesystem::path& assetsRoot, const GameProfile& profile);

/// Reads `mapFile` (any readable path) and converts it. Throws std::runtime_error when the file is unreadable or not a map.
MapImportResult importQuakeMapFile(const std::filesystem::path& mapFile, const MapImportOptions& options = {});

}  // namespace core
