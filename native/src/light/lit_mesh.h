// light/lit_mesh -- the copy of an object's render mesh that carries the lightmap coordinates of the live layout.
//
// Owns: makeLitMesh(): every vertex of a mesh gets the page and the (u, v) the layout's chart of its surface piece gives (the faces' affine maps, the same
// ones the bake's samples use) and kFlagLit; vertices of pieces without a chart (not lightmapped, culled) stay unlit. The source mesh is not changed.
// Thread: pure; any thread.
// Spec: 05_RENDERER.md §4 (lightmap UVs refer to the live layout), 08_MILESTONES.md T6.4. Deviation: made for every object of the snapshot after each layout
// build (M8 patches only the charts that moved).
#pragma once

#include <core/lightmap_uv.h>

#include <memory>
#include <vector>

#include "scene/render_mesh.h"

namespace native::light {

struct LitMeshStats {
    uint32_t litTriangles = 0, unlitTriangles = 0;
};

// `surfaces` = the object's pieces (ObjectProducts::surfaces), `source.pieces` indexes them. Returns null when no triangle found a chart (the caller keeps the
// source mesh).
std::shared_ptr<const scene::RenderMesh> makeLitMesh(const scene::RenderMesh& source, const std::vector<core::LightSurface>& surfaces, const core::LightmapLayout& layout,
                                                     LitMeshStats* stats = nullptr);

} // namespace native::light
