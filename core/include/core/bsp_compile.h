#pragma once
#include "core/bsp_format.h"
#include "core/bsp_vis.h"
#include "core/mesh_optimize.h"
#include "core/derived.h"
#include "core/lightbake.h"
#include "core/lighting_classes.h"
#include "core/lightmap_uv.h"
#include "core/light_scale.h"
#include "core/lightvol.h"
#include "core/prefab.h"
#include "core/texplan.h"
#include <cstring>
#include <functional>
#include <nlohmann/json.hpp>
#include <map>
#include <string>
#include <vector>

namespace core {

// The rBSP compiler (REQUIREMENTS §25, DESIGN §12): pure data in, file bytes out. The exporter (bsp_export.h) collects the inputs (flattened map,
// derived surfaces, final bake) and writes the files.

/// Editor (metres, Y up) -> Quake (units, Z up): (x, -z, y) * 32. A rotation, so windings keep their handedness.
inline Vec3 toQuake(const Vec3& p) { return Vec3{p.x, -p.z, p.y} * rbsp::kUnitsPerMeter; }
inline Vec3 toQuakeDir(const Vec3& d) { return Vec3{d.x, -d.z, d.y}; }
inline Vec3 fromQuake(const Vec3& q) { return Vec3{q.x, q.z, -q.y} * (1.0 / rbsp::kUnitsPerMeter); }

// ---- convex pieces (collision brushes) -------------------------------------------------------------------------------------------------------
struct HalfSpace {
    Vec3 normal;          // unit, outward
    double dist = 0;      // inside: normal . p <= dist
    std::string texture;  // of the face the plane came from
};
struct ConvexPiece {
    std::vector<HalfSpace> sides;  // only planes that bound the piece (redundant ones removed)
};
/// The polygon each side of a convex region occupies (empty for a side that does not touch it). `size` bounds the start polygon.
std::vector<std::vector<Vec3>> convexWindings(const std::vector<HalfSpace>& sides, double size = 1e5);
/// Drops sides that do not bound the region; false when the region is empty (volume below `minVolume`).
bool tidyConvex(ConvexPiece& piece, double minVolume = 1e-9);
/// A closed brush as convex pieces: a convex one as itself, a concave one split by its own face planes (solid leafs of its face BSP).
std::vector<ConvexPiece> convexPieces(const Brush& brush);
/// `piece` minus the convex `cutter`: at most one piece per plane of the cutter.
std::vector<ConvexPiece> subtractConvex(const ConvexPiece& piece, const ConvexPiece& cutter);

// ---- contents and surface flags (REQUIREMENTS §26 F4: from the game's config, game_profile.json "bsp") ------------------------------------
// Flags by name (Quake 3's values unless the config gives others), and rules matched in order, first match wins: the config's own rules first,
// then the built-in ones (common/clip, common/trigger, common/caulk, ..., water, lava, slime, sky; classes trigger_*). `match` is a
// case-insensitive glob ('*', '?') on the texture path (e.g. "common/clip") or on the classname. A texture without a matching rule is solid.
// A brush's contents are its class's rule when there is one, else the union of its sides' texture contents. `decal` (0x100000) marks decal
// surfaces of the rBSP (an rBSP addition, not a Quake 3 flag).
struct BspContentRules {
    std::map<std::string, uint32_t> contentFlags, surfaceFlags;
    struct Rule {
        std::string match;
        bool hasContents = false;
        uint32_t contents = 0, surface = 0;
    };
    std::vector<Rule> textures, classes;
    struct Flags { uint32_t contents = 1, surface = 0; };

    static BspContentRules defaults();
    /// The config's "bsp" object on top of the defaults (missing / empty = the defaults). Unknown flag names are reported and ignored.
    static BspContentRules fromJson(const nlohmann::json& bsp, std::vector<std::string>* warnings = nullptr);
    Flags forTexture(const std::string& texture) const;
    bool forClass(const std::string& classname, uint32_t& contents) const;
    uint32_t decalFlag() const;
    /// A sky texture (REQUIREMENTS §28 K4): a skybox (textures.h isCubemapTexture) or a texture whose rule gives it the `sky` surface flag.
    bool isSky(const std::string& texture) const;
    /// Whether a face with this texture blocks the view: not a sky texture, and the contents (clip, trigger, water: no) have the solid bit.
    bool blocksSight(const std::string& texture) const;
    /// What a face with this texture gets: forTexture, and for a sky texture no contents and the `sky` + `nolightmap` surface flags.
    Flags forFace(const std::string& texture) const;
};
bool globMatch(const std::string& pattern, const std::string& text);  // case-insensitive, '*' and '?'

// ---- sky (REQUIREMENTS §28, DESIGN §12 "Sky") -------------------------------------------------------------------------------------------------
// Level settings (worldspawn keys): _sky_auto "box" | "areas" (absent = off), _sky_margin (m, default 8), _sky_texture (a skybox). The level's sky
// texture is _sky_texture, else (Auto sky on) the first skybox of the assets' env folder, else none.
enum class AutoSky { Off, Box, Areas };
struct SkySettings {
    AutoSky mode = AutoSky::Off;
    double margin = 8;
    std::string texture;  // "" = none
};
/// `skyboxes` lists the skyboxes (textures.h listSkyboxes); it is only called when the default texture is needed.
SkySettings skySettingsOf(const Entity& worldspawn, const std::function<std::vector<std::string>()>& skyboxes, std::vector<std::string>* warnings = nullptr);
struct SkyBox { Vec3 mn, mx; };  // editor space
/// The automatic sky boxes around the surfaces that are not sky: one box (Box), or one per group of brushes whose bounds touch or overlap, groups
/// whose boxes (with the margin) touch or overlap merged until none do (Areas). Each box is the group's bounds grown by `margin`.
std::vector<SkyBox> autoSkyBoxes(const std::vector<LightSurface>& surfaces, AutoSky mode, double margin, const BspContentRules& rules);

// ---- compiler -------------------------------------------------------------------------------------------------------------------------------
struct BspSettings {
    BspContentRules contents = BspContentRules::defaults();
    double decalOffset = 0.25;             // Quake units: decals are lifted this far off their surface (no depth fighting)
    std::string textureRoot = "textures";  // shader name = textureRoot + "/" + texture
    Vec3 visCell{5, 2, 5};                 // m: size of a visibility cell to start with (x, y up, z; grows until the visibility table fits maxVisBytes)
    bool visDilate = false;                // a cluster also gets the view of the clusters it opens into (one ring, never through a wall)
    size_t maxVisBytes = size_t(50) << 20; // the visibility lump may be this big
    double visSplitMinWidth = 2.0;         // m: a wall splits visibility cells when its (merged, coplanar) face has no dimension below this
    int visMaxSplitsPerCell = 16;
    int visSamplePool = 32;                // sample points per cluster
    int visPairSegments = 64;              // segments traced per cluster pair
    double visApertureMax = 0.5;           // m: openings up to this wide get rays aimed through them
    std::string visDevice = "auto";        // where the segments are traced: auto | cpu | compute | rayquery (bsp_vis.h)
    bool visibility = true;                // false: every cluster sees every cluster (the clusters are still made)
    // Fit every lightmap chart to the fewest texels that look the same (lightmap_fit.h): the allowed error in 8-bit sRGB levels after `lightmapExposure`
    // (the level's _cc_exposure); 0 = store the charts as baked.
    double lightmapTolerance = 2.5;
    double lightmapExposure = 1;
    // Pages whose light fits 8 bits (nothing above 1, within the tolerance above) are stored as .jpg instead of .exr, in pages of their own: the charts are
    // grouped by that. The engine has to find lm_NNNN.jpg. The bytes are the light itself (byte / 255): the engine reads no sRGB lightmaps.
    // Needs lightmapTolerance > 0 (its error budget is also the JPEG's).
    bool lightmapJpeg = true;
    int lightmapJpegQuality = 98;
    // The error a chart may have in a .jpg (sRGB levels after the exposure, same measure as lightmapTolerance; 99% of the texels within it, all within twice it).
    // Looser than the fit's: a byte of linear light is 12 levels next to black, so near-black texels always differ by that much; the engine reads no sRGB lightmaps.
    double lightmapJpegTolerance = 8;
    // HDR light from 8 bit pages (light_scale.h, DESIGN §12 "Light scale"): every chart is stored divided by one colour (the peak of all its style layers),
    // which goes into the colour of its vertices (rgb mantissa, A shared exponent, multiplier 1..32). Dark charts then use the page's whole range and
    // bright ones fit a .jpg. Off = pages hold the light itself and the vertex colour is white.
    bool lightmapScale = true;
    // Vertex lit surfaces (no lightmap texels in the file; the engine binds a white lightmap and the light is only in the vertex colour): the lightmap
    // charts of an entity with the keyvalue `_vertex_lit` 1 and the charts whose faces add up to at most `vertexLitMaxArea` m2 (worldspawn `_vertex_lit_area`,
    // 0 = off). They are still baked (same lights, shadows, bounces); the compiler reads the baked light at their vertices and drops the texels. The light of
    // style 0 only: a vertex has one colour, so the other styles' layers of such a chart are dropped (warning).
    double vertexLitMaxArea = 0;
    // Pack all lightmap charts again by size (fewer, fuller pages) when that takes fewer pages than the live layout.
    bool repackLightmaps = true;
    // Surfaces further than this (m) from the playable area (BspInput::playable) get no texture relief: they are laid out flat. 0 = relief everywhere. A relief
    // is hundreds of thousands of triangles on a big level, and nobody gets close enough to see it.
    double reliefDistance = 0;
    // Export-time vertex reduction of the triangle-soup surfaces (relief, texture layouts; mesh_optimize.h): near-equal vertices are welded, coplanar and
    // collinear vertices that carry nothing are collapsed and, with a tolerance, detail below `meshOptimize.tolerance` is thinned. Runs after the exact weld.
    bool optimizeMesh = true;
    MeshOptimizeParams meshOptimize;
    // Identical vertices of one triangle-soup surface are written once and shared by the triangles.
    bool weldVertices = true;
    SkySettings sky;                       // the automatic sky (its texture "" = env/sky_cube with a warning)
    double engineFarPlane = 3000;          // m: the engine's Camera::FarPlane; a sky box larger than that across is clipped (warning)
};

struct BspInput {
    std::vector<FlattenedEntity> entities;   // flattenForCompile
    std::vector<LightSurface> surfaces;      // collectSurfaces (the same entities, resolved)
    std::vector<DecalFragment> decals;       // collectDecals: written before the other surfaces of their model, flagged `decal`
    // How the editor lays textures (texplan.h, REQUIREMENTS §27 G2 / G3): the authored face of a surface piece (brush instance, brush id, face id; null
    // when unknown: the piece keeps the Core's simple UV map) and where textures / texture groups come from.
    std::function<std::pair<const Brush*, const BrushFace*>(const std::string& brushInstance, const std::string& brushId, uint32_t faceId)> parentFace;
    texplan::InfoFn texInfo;
    texplan::GroupFn texGroups;
    LightingClasses classes;
    std::shared_ptr<const PlayableArea> playable;  // the playable area (playable.h); null / empty = none (BspSettings::reliefDistance does nothing)
    const LightmapLayout* layout = nullptr;  // the layout the bake used (all charts, their style lists); null = no lightmaps
    std::map<int, LightmapPages> pages;      // per style: the baked pages, every layer at the base layout's page / rectangle
    std::map<int, LightvolGrid> lightvol;    // per style, same geometry (empty: no light grid)
    const SolidSet* solids = nullptr;        // light-blocking volumes: buried surfaces are dropped, solid cells get no cluster
    const SolidSet* visSolids = nullptr;     // the volumes that block sight (opaque, solid-contents brushes of the static world); null = `solids`
    std::function<bool(const std::string& texture)> opaque;  // textures that block sight (empty = every texture)
    BspSettings settings;
    std::shared_ptr<const VisResult> vis;    // the baked visibility (buildBspVisibility), shared with the lighting; null = compileBsp builds it
};

struct BspLightmapPage {
    int size = 0;
    std::vector<float> rgb;  // size * size * 3
    bool ldr = false;        // an 8 bit page (lightmapJpeg): `jpg` holds its file and `rgb` the light as read back from it
    std::vector<uint8_t> jpg;
};

struct BspOutput {
    std::vector<uint8_t> bsp;
    std::vector<BspLightmapPage> lightmaps;  // lm_0000.exr, lm_0001.exr, ...
    std::vector<std::string> warnings;
    struct Stats {
        size_t surfaces = 0, decals = 0, skySurfaces = 0, laidOutSurfaces = 0, droppedSurfaces = 0, brushes = 0, models = 0, leafs = 0, clusters = 0, lightmapPages = 0, lightmapTexelsBefore = 0, lightmapTexelsAfter = 0, lightmapJpegPages = 0, lightmapJpegRejected = 0, scaledCharts = 0, clippedCharts = 0, vertexLitCharts = 0, vertexLitSurfaces = 0, degenerateTriangles = 0, reliefSkipped = 0, reliefBuried = 0, weldedVertices = 0, meshSurfaces = 0, meshWelded = 0, meshRemovedVertices = 0, meshRemovedTriangles = 0, meshSharedVertices = 0, drawVertsBefore = 0, drawVertsAfter = 0, gridPoints = 0, palette = 0;
        double visCell = 0, visSeconds = 0;
        size_t visSplits = 0, visApertures = 0, visPairs = 0, visBytes = 0, visChecked = 0, visMisses = 0;
        std::string visDevice;
    } stats;
};

using ProgressFn = std::function<void(const std::string& stage, double fraction)>;
/// The baked visibility of the level (bsp_vis.h) from the surfaces, solids, playable area and settings of `in` (what compileBsp writes). Build it once and give it to
/// the lighting (LightingService::Options::visibility) and to the compiler (BspInput::vis).
std::shared_ptr<const VisResult> buildBspVisibility(const BspInput& in, const ProgressFn& progress = {});
BspOutput compileBsp(const BspInput& in, const ProgressFn& progress = {});

// ---- reading back (tests, tools) ------------------------------------------------------------------------------------------------------------
struct BspView {
    const uint8_t* data = nullptr;
    size_t size = 0;
    rbsp::Header header{};
    bool open(const std::vector<uint8_t>& bytes, std::string* error = nullptr);
    template <class T> std::vector<T> lump(rbsp::Lump l) const {
        const auto& e = header.lumps[l];
        std::vector<T> out(static_cast<size_t>(e.length) / sizeof(T));
        if (!out.empty()) std::memcpy(out.data(), data + e.offset, out.size() * sizeof(T));
        return out;
    }
    std::string entities() const;
};

}  // namespace core
