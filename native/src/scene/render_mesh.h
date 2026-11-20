// scene/render_mesh -- the triangles a viewport draws for one scene object (CPU side, no GPU).
//
// Owns: RenderVertex (the GPU vertex format of 05_RENDERER.md §4), RenderMesh (vertices + indices of one object, textures named per slot) and
// buildRawMesh(): the authored brush as triangles, which the renderer draws while an object's real mesh (carved pieces, texture layout, relief,
// built with them in scene/relief.cpp) is still being made, and for subtractive brushes (translucent volumes).
// A mesh names its textures; the renderer maps names to its global texture table when it uploads (RenderVertex::texture is the slot in
// RenderMesh::textures until then). Immutable once published.
// Thread: pure; any thread.
// Spec: 05_RENDERER.md §4, 08_MILESTONES.md T3.4.
#pragma once

#include <core/brush.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "doc/changeset.h"

namespace native::scene {

struct ReliefEnv;

// 40 bytes. normal = octahedral, two snorm16. lm / page are the lightmap coordinates of the Lit modes: zero in the meshes the scene builder makes, filled
// (with kFlagLit) in the copies light/lit_mesh makes for the live layout.
struct RenderVertex {
    float pos[3];
    uint32_t normal;       // octEncode(): snorm16 x, snorm16 y in the low / high half
    float uv[2];           // albedo, in texture repeats
    float lm[2];           // lightmap
    uint16_t page;         // lightmap page
    uint16_t flags;        // kFlag*
    uint32_t texture;      // slot in RenderMesh::textures; the renderer rewrites it to the global texture index
};
static_assert(sizeof(RenderVertex) == 40);

inline constexpr uint16_t kFlagSky = 1;      // a sky texture: drawn by view direction, no light
inline constexpr uint16_t kFlagVolume = 2;   // an authored volume (a subtractive brush) drawn translucent
inline constexpr uint16_t kFlagLit = 4;      // lm / page address a chart of the live layout

uint32_t octEncode(float x, float y, float z);
void octDecode(uint32_t packed, float out[3]);

// A texture the engine draws as the sky (a skybox, or a name that says sky): model/sky.ts isSkyTexture.
bool isSkyTexture(const std::string& name);

struct RenderMesh {
    enum class Kind : uint8_t { Solid, Volume };
    Kind kind = Kind::Solid;
    std::vector<RenderVertex> vertices;
    std::vector<uint32_t> indices;           // triangles
    std::vector<std::string> textures;       // slot -> texture name; "" never occurs
    // Per triangle: the index of the surface piece (core::LightSurface of the object's ObjectProducts::surfaces) it was made from, kNoPiece for raw meshes.
    // The lit copy takes the lightmap coordinates of the piece's chart.
    std::vector<uint32_t> pieces;
    static constexpr uint32_t kNoPiece = 0xffffffffu;
    bool empty() const { return indices.empty(); }
    size_t bytes() const;

    // Adds a triangle (three vertices, own indices); `texture` is added to the slots when new. Vertices are not shared between triangles.
    uint32_t slotOf(const std::string& texture);
    void addTriangle(const RenderVertex& a, const RenderVertex& b, const RenderVertex& c, uint32_t piece = kNoPiece);
};

// The authored brush as triangles: every face fan-triangulated from its stored loop, albedo coordinates from the face's own stored uvs when it has
// them, else its projection (core::projectAlbedoUv). `volume`: Kind::Volume and kFlagVolume.
std::shared_ptr<const RenderMesh> buildRawMesh(const core::Brush& brush, bool volume);

} // namespace native::scene
