#pragma once
#include "core/derived.h"
#include "core/lighting_classes.h"
#include "core/math3.h"
#include "core/playable.h"
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace core {

// Lightmap UV generation and packing (DESIGN.md §6.4). Charts belong to authored faces (§5.6 rule 3): every surface
// piece, whether a whole face, a boolean fragment or a decal fragment, gets its lightmap coordinates from the pure
// affine function `LightmapFace::texel(worldPosition)` of its parent face. Nothing here depends on how the surface
// was cut, so the Shell preview can reproduce it from the numbers in `toJson()`.

struct LightmapSettings {
    double texelsPerMeter = 16;   // base density
    int atlasSize = 1024;         // every page is atlasSize x atlasSize texels
    int padding = 2;              // empty texels around every chart (gutter for dilation; the views sample bicubic, which reads 2 texels out)
    // Faces that share an edge are unfolded into one chart ("connected edges stay connected") when the angle between
    // their normals is at most this. 0 = only coplanar faces, 90 = box corners connect, 180 = anything.
    // Faces sharing an edge are unfolded into one chart when the dihedral angle is <= this. Kept small on purpose: bilinear filtering reads
    // across the fold, so merging faces at a room corner (90 deg) smears the light of one wall onto the other (bright / dark edges at corners).
    // Coplanar and nearly flat neighbours still merge (continuous light, no seam).
    double mergeAngleDeg = 15;
    int maxChartTexels = 512;     // largest chart side (without padding); a bigger face is scaled down, with a warning
    double edgeEps = 1e-4;        // metres: tolerance for "same edge" and the minimum shared edge length
    // Per-face density multiplier (hook for the density tools), keyed by LightmapFace::key(). Faces with different
    // multipliers are never merged into one chart.
    std::map<std::string, double> densityScale;
    // Quality falloff outside the playable area (playable.h): the density of every face is multiplied by lod.densityAt(distance of the face from the
    // area), so far away faces get fewer texels. Null or empty area = no falloff. Faces in different cascades are never merged into one chart.
    std::shared_ptr<const PlayableArea> playable;
    PlayableLod lod;
    // With `lod.cullUnseen`: whether anything of the area can see a polygon (PlayableVisibility); surfaces it cannot see beyond the full-detail radius go
    // to the last cascade. Null = every surface counts as seen.
    std::function<bool(const std::vector<Vec3>& polygon, const Vec3& normal)> seen;
    // Light styles (light_style_analysis.h): the non-base styles that touch each face, keyed by LightmapFace::key(). Faces with a different set are never
    // merged into one chart, so a chart carries as few style layers as possible; a face absent from the map has only the base style.
    std::map<std::string, std::vector<int>> faceStyles;
};

struct LightmapFace {
    // identity: (instance, entity, brush that owns the authored face, its instance, face id, side)
    std::string instance, entityId, brushInstance, brushId;
    uint32_t faceId = 0;
    FaceSide side = FaceSide::Front;
    std::string texture;
    Vec3 normal;     // unit, outward
    double area = 0; // m^2
    int chart = -1;  // index into LightmapLayout::charts

    // world position -> texel coordinates inside the page: s = p.su + so, t = p.sv + to.
    // The (s, t) lattice is world-aligned: it depends on the face plane only, never on the face's extent.
    Vec3 su, sv;
    double so = 0, to = 0;
    double texelsPerMeter = 0;  // effective density (base * densityScale * any shrink-to-fit factor)

    Vec2 texel(const Vec3& p) const { return {p.dot(su) + so, p.dot(sv) + to}; }
    std::string key() const;
};

struct LightmapChart {
    int page = 0;
    int x = 0, y = 0, w = 0, h = 0;  // rectangle in the page, padding included
    std::string instance, entityId, texture;  // texture = that of the largest face in the chart
    std::vector<int> faces;                   // indices into LightmapLayout::faces
    Vec3 center;                              // world-space centre of the chart's surfaces
    double texelsPerMeter = 0;                // density of the root face
    // Style layers of the chart: always starts with the base style 0, then the styles that touch it (ascending). Every layer has its own lightmap
    // texels; in the editor's pages a layer sits at the same page / rectangle as the base (the style's own page set), the engine export packs layers densely.
    std::vector<int> styles{0};
};

struct LightmapLayout {
    LightmapSettings settings;
    std::vector<LightmapFace> faces;   // sorted by key
    std::vector<LightmapChart> charts;
    int pageCount = 0;
    std::vector<double> pageFill;      // used texel fraction per page (chart rectangles, padding included)
    std::vector<std::string> warnings;

    const LightmapFace* find(const std::string& instance, const std::string& entityId, const std::string& brushInstance,
                             const std::string& brushId, uint32_t faceId, FaceSide side) const;
    // The face a (carved or decal) fragment belongs to, side chosen from the fragment's own normal.
    const LightmapFace* findForFragment(const std::string& instance, const std::string& entityId,
                                        const std::string& brushInstance, const std::string& brushId, uint32_t faceId,
                                        const std::vector<Vec3>& fragmentVerts) const;
    Vec2 uv(const LightmapFace& f, const Vec3& p) const {
        const Vec2 t = f.texel(p);
        return {t.u / settings.atlasSize, t.v / settings.atlasSize};
    }
    // Full contract for the Shell and engine: faces with their affine maps, charts, pages, warnings.
    nlohmann::json toJson() const;
};

// Only surfaces whose classname is lightmapped (`classes`) are laid out; the rest are lit from light volumes.
LightmapLayout buildLightmapLayout(const std::vector<LightSurface>& surfaces, const LightingClasses& classes,
                                   const LightmapSettings& settings = {});

// The layout of one style layer: the charts that carry `style`, at the same page and rectangle as in `layout` (so tiles of every layer address the same
// pixels), faces and chart indices renumbered. Style 0 is every chart.
LightmapLayout layoutForStyle(const LightmapLayout& layout, int style);

}  // namespace core
