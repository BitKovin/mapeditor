#pragma once
#include <optional>
#include <vector>

namespace core {

// MaxRects rectangle packing (best short side fit, no rotation) into square pages. Shared by the layout (lightmap_uv.cpp) and the export, which packs
// the charts of the finished lightmaps again (bsp_compile.cpp).
struct PackRect {
    int x, y, w, h;
};

struct PackPage {
    std::vector<PackRect> freeRects;
    long long used = 0;
    explicit PackPage(int size) { freeRects.push_back({0, 0, size, size}); }
    std::optional<PackRect> place(int w, int h);
};

struct PackPlacement {
    int page = 0, x = 0, y = 0;
};

// Packs `sizes` (w, h) into as few pages of `pageSize` as it can: biggest first, each on the first page with room (pages that already refused a rectangle at
// least as small are not asked again). A rectangle larger than a page is cut to the page. `pages` receives the page count.
std::vector<PackPlacement> packRects(const std::vector<std::pair<int, int>>& sizes, int pageSize, int& pages);

}  // namespace core
