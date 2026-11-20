// light/layout -- the live lightmap layout: chart groups, chart shapes, placement in the page allocator, chunk records.
//
// Owns: the staged layout of core/lightmap_uv.h run over the whole level (on load; incremental re-layout per change set comes in M8), the placement of
// every chart in light/page_allocator, the chart table the renderer and the bake read (`charts()`), the chunk records (the scheduling unit, 16 x 16 texels)
// and the layout in the Core's own type (`layout()`), made by core::placeChartsAt, which the per-chart texel samples (T6.2) and the parity tests use.
// Thread: built on the bake thread / a job; the result is immutable except through the build / free calls, which the owner serialises.
// Spec: 06_LIGHTING.md §1 (parity: chart shapes equal the Core's), §3, §4 (chunks), 08 T6.1.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "core/lightmap_uv.h"
#include "light/page_allocator.h"

namespace native::light {

struct LiveChart {
    int group = 0;                  // index into LiveLayout::groups()
    core::ChartShape shape;         // faces, size (padding included), unfolding maps without the placement offset
    PageRect where;                 // its rectangle (shape.w x shape.h) in the pages
};

// A scheduling unit: a tile of one chart's texels (padding excluded), at most chunkTexels on a side, in page coordinates.
struct Chunk {
    int chart = 0;
    int x = 0, y = 0, w = 0, h = 0;    // page rectangle; the page is the chart's
};

struct LiveLayoutStats {
    double prepareSeconds = 0, groupsSeconds = 0, chartsSeconds = 0, placeSeconds = 0, assembleSeconds = 0;
    size_t groups = 0, charts = 0, chunks = 0;
};

class LiveLayout {
public:
    explicit LiveLayout(const core::LightmapSettings& settings, int chunkTexels = 16);

    // Charts the whole level (the previous content is dropped; the allocator starts empty again). Charts are placed tallest first.
    void buildAll(const std::vector<core::LightSurface>& surfaces, const core::LightingClasses& classes);

    const core::LightmapSettings& settings() const { return settings_; }
    int chunkTexels() const { return chunkTexels_; }
    const core::LayoutFaces& faces() const { return faces_; }
    const std::vector<std::vector<int>>& groups() const { return groups_; }
    const std::vector<LiveChart>& charts() const { return charts_; }
    const std::vector<Chunk>& chunks() const { return chunks_; }
    // The chunks of chart `c` are chunks()[chunkBegin(c), chunkBegin(c + 1)).
    size_t chunkBegin(size_t chart) const { return chunkBegin_[chart]; }
    PageAllocator& allocator() { return alloc_; }
    const PageAllocator& allocator() const { return alloc_; }
    // The layout as the Core type (chart order = charts()): faces with their affine maps including the placement, pages, fill.
    const core::LightmapLayout& layout() const { return layout_; }
    // Increases with every build; the renderer's chart offset table carries it (M8).
    uint64_t version() const { return version_; }
    const LiveLayoutStats& stats() const { return stats_; }

private:
    core::LightmapSettings settings_;
    int chunkTexels_;
    PageAllocator alloc_;
    core::LayoutFaces faces_;
    std::vector<std::vector<int>> groups_;
    std::vector<LiveChart> charts_;
    std::vector<Chunk> chunks_;
    std::vector<size_t> chunkBegin_;
    core::LightmapLayout layout_;
    uint64_t version_ = 0;
    LiveLayoutStats stats_;
};

} // namespace native::light
