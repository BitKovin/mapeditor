#pragma once
#include "core/albedo.h"
#include "core/derived.h"
#include "core/lighting_classes.h"
#include "core/lightmap_uv.h"
#include "core/math3.h"
#include <array>
#include <memory>
#include <string>
#include <cstdint>
#include <vector>

namespace core {

// Shadow-casting geometry for the baker (DESIGN.md §6.5): triangles, double-sided and opaque.
struct OccluderMesh {
    std::vector<float> tris;  // 9 floats per triangle (3 vertices), world space
    // Optional, parallel to `tris` (only when built with a layout): 8 floats per triangle = lightmap texel coordinates of its
    // 3 vertices (u0 v0 u1 v1 u2 v2), the page, and 1 if the triangle has a lightmap (0: it only casts shadows). Used by bounces.
    std::vector<float> lightmap;
    bool hasLightmap() const { return !lightmap.empty(); }
    // Albedo of the surfaces for coloured bounces (only built together with `lightmap`).
    //   textureNames / triTexture: texture path of every triangle (index into textureNames, -1 = none), filled by buildOccluders.
    //   albedoUv: 6 floats per triangle, the albedo coordinates (repeats) of its 3 vertices; uvRatio: per triangle, albedo area (repeats^2) per lightmap
    //   texel^2, which with the texture's size gives the level of detail a one-texel footprint needs.
    //   albedo / albedoMeta: set by attachAlbedo(): the decoded textures (no null entries; ids are the indices into it) and the packed per-triangle
    //   record the shader reads, 8 floats = (au0 av0 au1 av1) (au2 av2 texture lod); texture is -1 for "white".
    std::vector<std::string> textureNames;
    std::vector<int32_t> triTexture;
    std::vector<float> albedoUv, uvRatio;
    std::shared_ptr<const AlbedoSet> albedo;
    std::vector<float> albedoMeta;
    // Transparency (REQUIREMENTS §23), set by attachAlbedo when some texture of the mesh is not opaque. One uint array the CPU tracer and the shaders
    // read the same way (surfaceAt / surfaceAt() in the shaders): 8 words per albedo texture id = (data offset, width, height, mode, cutoff bits,
    // opacity bits, tint bits, 0), then the texels of the non-opaque ones, one word each = r | g << 8 | b << 16 | a << 24 (sRGB colour, alpha),
    // rows top to bottom, read with the nearest texel (u * width, v * height, wrapped). Mode 0 opaque, 1 cutout, 2 translucent.
    std::vector<uint32_t> alphaWords;
    size_t transparentTriangles = 0;
    bool hasTransparency() const { return transparentTriangles > 0 && !alphaWords.empty() && hasAlbedo(); }
    bool hasAlbedo() const { return albedo && !albedo->images.empty() && albedoMeta.size() == triangleCount() * 8; }
    size_t triangleCount() const { return tris.size() / 9; }
    bool empty() const { return tris.empty(); }
    Vec3 boundsMin() const;
    Vec3 boundsMax() const;
};

// Every surface piece whose classname casts shadows (`classes.castShadows`), fan-triangulated. With a layout, each triangle also
// carries the lightmap coordinates of its parent face (OccluderMesh::lightmap) so a ray hit can be looked up in the lightmap.
OccluderMesh buildOccluders(const std::vector<LightSurface>& surfaces, const LightingClasses& classes, const LightmapLayout* layout = nullptr);

// Loads the textures the mesh's triangles refer to (through `loader`, which caches) and fills albedo / albedoMeta. Textures that cannot be loaded
// give their triangles "white" (the constant albedo of the bake). Safe to call again after the textures changed.
void attachAlbedo(OccluderMesh& mesh, const AlbedoLoader& loader);

// What a ray meets on a triangle at barycentrics (b1, b2) given its albedo record (8 floats of albedoMeta) and the packed transparency words:
// -1 = a hole (the ray passes), 0 = solid, 1 = translucent (the ray passes, its light times `transmit`). MUST equal surfaceAt() in the shaders.
int surfaceAt(const float* albedoRecord, const std::vector<uint32_t>& alphaWords, double b1, double b2, Vec3& transmit);

// CPU ray tracer: bounding volume hierarchy (median split), any-hit queries. Reference and fallback of the GPU ray query.
// Built from a mesh with transparency, the queries honour it: holes of cutout textures let every ray through (occluded, closestHit, nearestHit),
// translucent surfaces let light through tinted (transmittance; only where the ray enters a surface, against its winding normal, so a closed pane
// dims the light once) and count as hits for closestHit / nearestHit; occluded() is true when an
// opaque surface is hit or the light left after translucent layers is below 1e-3.
class TriangleBvh {
public:
    explicit TriangleBvh(const OccluderMesh& mesh);
    // True when the segment origin + t * dir, t in [0, tmax], hits any triangle. `dir` need not be normalised
    // (tmax is in units of |dir|).
    bool occluded(const Vec3& origin, const Vec3& dir, double tmax) const;
    // Light that gets through along the segment: 0 when an opaque surface is in the way, else the product of the translucent layers' transmission.
    Vec3 transmittance(const Vec3& origin, const Vec3& dir, double tmax) const;
    bool hasTransparency() const { return !alphaWords_.empty(); }
    // Nearest hit distance (units of |dir|) or a negative number: used by tests.
    double nearestHit(const Vec3& origin, const Vec3& dir, double tmax) const;
    struct Hit {
        double t = -1;        // < 0: no hit
        uint32_t triangle = 0;  // index in the mesh the BVH was built from (not the internal order)
        double b1 = 0, b2 = 0;  // barycentric weights of vertices 1 and 2 (vertex 0 has 1 - b1 - b2)
    };
    Hit closestHit(const Vec3& origin, const Vec3& dir, double tmax) const;
    size_t nodeCount() const { return nodes_.size(); }

private:
    struct Node {
        float mn[3], mx[3];
        uint32_t left = 0;   // inner: index of the left child (right = left + 1); leaf: first triangle slot
        uint32_t count = 0;  // 0 = inner node
    };
    std::vector<float> tris_;     // reordered triangles
    std::vector<uint32_t> order_; // internal slot -> original triangle index
    std::vector<Node> nodes_;
    std::vector<float> albedoMeta_;     // copy of the mesh's (original triangle order), only with transparency
    std::vector<uint32_t> alphaWords_;  // copy of the mesh's, only with transparency
    // -1 hole, 0 solid, 1 translucent (transmit) for the triangle in internal slot `slot` hit at barycentrics (b1, b2)
    int surface(uint32_t slot, double b1, double b2, Vec3& transmit) const;
    void build(uint32_t node, std::vector<uint32_t>& order, std::vector<std::array<float, 3>>& centroids, uint32_t begin, uint32_t end,
               const std::vector<float>& src);
};

}  // namespace core
