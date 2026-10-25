#pragma once
// Export-time reduction of the draw vertices of triangle-soup surfaces (texture relief, texture group layouts, repeat modes: texplan.h).
//
// A relief is laid over a face once per texture repeat and clipped to the face, so a wall of a few square metres is easily tens of thousands of
// triangles, and every vertex costs 128 bytes in the rBSP (8 light style slots). Most of that is redundant:
//   1. Vertices that are the same point computed twice (a cell corner reached from two cells, a point on the diagonal of two fan triangles) differ by
//      float noise, so the exact byte-equal weld of the compiler misses them. They are welded within `weldDistance` (positions first, so the vertices of
//      a crease share one position bit for bit, then every attribute).
//   2. Flat parts of a relief (the border of every tile, the top of a brick, the floor of a recess) are many coplanar triangles whose texture
//      coordinates, lightmap coordinates and normal are all affine over the plane. Collapsing their interior vertices changes nothing the player sees.
//   3. Dense reliefs (a heightfield, a noisy stone) have detail below what is visible. The same collapse, allowed an error of `tolerance`, thins them.
// The collapse is meshoptimizer's quadric edge collapse (vendored/meshoptimizer, MIT) with the texture coordinates, the lightmap coordinates, the
// normal and (vertex lit surfaces) the baked colour as attributes, so a vertex that carries a texture seam, a crease or a lighting change is kept.
//
// Cracks: surfaces are cut from each other (the fragments of a carved face, the textures of a layout, the faces of a brush), and the engine draws each
// with its own vertices. A vertex on the outline of a surface that another surface also has a vertex at (within `shareDistance`) is never removed or
// moved; so the seam between two surfaces keeps its vertices on both sides. Outline vertices nobody else uses may go when they are collinear (the
// lossless pass), which is the common case of a long edge cut into tile-sized pieces.
#include "core/bsp_format.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace core {

struct MeshOptimizeParams {
    /// Quake units. The largest distance a vertex may move the surface by (the lossy pass). Below `losslessTolerance` the lossy pass is skipped and only
    /// the exact merges remain (coplanar vertices, collinear outline vertices).
    double tolerance = 1.0 / 32;
    double losslessTolerance = 1.0 / 256;
    /// Quake units: vertices closer than this, and alike in every attribute, are one.
    double weldDistance = 1.0 / 256;
    /// Quake units: an outline vertex within this distance of an outline vertex of another surface is shared with it and stays.
    double shareDistance = 1.0 / 64;
    /// What counts as a visible change of an attribute (the simplifier weighs attribute error against position error with these): a unit normal, a
    /// colour channel (0..1). The texture and lightmap coordinates differ per surface (MeshItem).
    double normalTolerance = 0.03;
    double colorTolerance = 3.0 / 255.0;
    /// Surfaces with fewer triangles than this are left as they are (after welding).
    size_t minTriangles = 4;
    /// Quake units: an edge shorter than this is collapsed (its end points become one vertex) when the two vertices are alike in every attribute, and a
    /// triangle that keeps two corners closer than this is dropped with the others below. The clipping of a relief at repeat borders and the welding leave
    /// slivers with one very short edge (0.01 units wide); the engine's physics mesh welds close vertices (so the sliver collapses into a triangle with two
    /// equal corners and the mesh is refused). Collapsing the edge keeps the surface closed (a dropped sliver would leave a crack). 1/32 unit = about 1 mm.
    double minEdge = 1.0 / 32;
    /// Units^2: a triangle whose corners (as the file stores them: floats) span a cross product shorter than this is dropped from the result. The welding
    /// and the collapses leave such triangles behind (three collinear vertices of a straight outline; a sliver that the welding squeezed), and the engine
    /// builds its physics mesh from these triangles and refuses them. The compiler passes its own limit (kMinTriangleCross) so that the two agree.
    double minTriangleCross = 0.01;
};

/// One surface: its vertices and its triangle list (indices relative to `verts`).
struct MeshItem {
    std::vector<rbsp::DrawVert> verts;
    std::vector<int32_t> indexes;
    bool optimize = false;            // a triangle-soup surface to reduce; the others (polygons, decals, sky) only count as neighbours that share vertices
    int lightmapSlots = 0;            // lightmap coordinate slots in use
    bool vertexLit = false;           // the colour of the vertices is baked light, not a constant
    double stTolerance = 1.0 / 4096;  // texture coordinate (texture repeats) that is a visible change: a fraction of a texel
    double lightmapTolerance = 1.0 / 4096;  // lightmap coordinate (page fractions) that is a visible change: a fraction of a texel
};

struct MeshOptimizeStats {
    size_t surfaces = 0;           // surfaces reduced
    size_t weldedVertices = 0;     // vertices merged into an equal one
    size_t removedVertices = 0;    // vertices that went with their triangles (simplification, degenerate triangles)
    size_t removedTriangles = 0;
    size_t sharedVertices = 0;     // outline vertices kept because another surface has one at the same point
    size_t collapsedEdges = 0;     // short edges whose end points were merged (MeshOptimizeParams::minEdge)
};

/// Welds and simplifies every `optimize` item in place. The other items are only read.
void optimizeSurfaces(std::vector<MeshItem>& items, const MeshOptimizeParams& params, MeshOptimizeStats* stats = nullptr);

}  // namespace core
