// scene/relief -- the relief of textures as geometry, per scene object.
//
// Owns: ReliefEnv (where texture infos / groups come from) and buildRelief(): for the carved surface pieces of one object, the displaced
// triangles the viewport draws and the bake lights (core::ReliefMesh, one per authored face side), made with the Core's texplan functions
// (core/texplan.h: texturePolygon over each piece, outlines closed by connecting walls). This is the port of the Shell's reliefBake.ts
// `reliefFaces` for one brush at a time; the Core's own export builds the same meshes in bsp_export.cpp for the command line.
// A face key (instance|entity|brushInstance|brush|face|side) can occur in several objects (the walls a subtractor cuts into several brushes);
// each object holds the part its own pieces make, and a consumer that needs one mesh per key concatenates them.
// `around` = the pieces of the objects that touch this one: a polygon edge is the outline of its face only when no coplanar piece of the same
// texture continues it across the edge, and that piece may belong to a neighbour (a wall built from several brushes).
// Thread: pure; any thread (the environment's functions must be thread safe, as TexEnvironment's are).
// Spec: 04_SCENE_AND_DERIVED.md §1, §6, 08_MILESTONES.md T3.3.
#pragma once

#include <core/derived.h>
#include <core/relief_geometry.h>
#include <core/texplan.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace native::scene {

struct ReliefEnv {
    core::texplan::InfoFn info;      // empty = no relief is built
    core::texplan::GroupFn groups;
    uint64_t revision = 0;           // changes whenever a texture's info (its relief) may have changed
    explicit operator bool() const { return static_cast<bool>(info); }
};

// The authored brush and face a surface piece lies on (null when unknown), looked up by the piece's brushInstance / brushId / faceId.
using ParentFaceFn = std::function<std::pair<const core::Brush*, const core::BrushFace*>(const std::string& brushInstance, const std::string& brushId,
                                                                                       uint32_t faceId)>;

using ReliefList = std::vector<std::shared_ptr<const core::ReliefMesh>>;

// Triangles one object may spend on relief (the Shell's send budget is 400 000 for the whole level); beyond it the remaining faces stay flat.
inline constexpr long long kObjectReliefBudget = 100000;

struct RenderMesh;

// What is made for one object from its carved pieces in one pass: the relief meshes and the triangles a viewport draws (the face as authored where
// the texture lays out plainly, `texturePolygon`'s triangles where it has a repetition plan or a relief). Without an environment the pieces are
// drawn with their own projection and no relief is made. Never null members.
struct ObjectGeometry {
    std::shared_ptr<const ReliefList> relief;
    std::shared_ptr<const RenderMesh> mesh;
};
ObjectGeometry buildGeometry(const std::vector<core::LightSurface>& own, const std::vector<const std::vector<core::LightSurface>*>& around,
                             const ParentFaceFn& parent, const ReliefEnv& env, long long budget = kObjectReliefBudget);

// The relief meshes of `own`, sorted by key; empty when no piece's face shows a relief. Never null.
std::shared_ptr<const ReliefList> buildRelief(const std::vector<core::LightSurface>& own, const std::vector<const std::vector<core::LightSurface>*>& around,
                                              const ParentFaceFn& parent, const ReliefEnv& env, long long budget = kObjectReliefBudget);

// Memory estimate for the cache.
size_t reliefBytes(const ReliefList& list);

} // namespace native::scene
