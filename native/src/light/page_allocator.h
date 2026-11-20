// light/page_allocator -- rectangle allocator for the lightmap pages of the live layout.
//
// Owns: which chart rectangle sits where in the pages (layers of one 2D-array image of atlasSize x atlasSize texels), with free and merging, so a chart that
// is re-made (a brush edit, M8) frees its rectangle and a new chart takes the hole. Qt-free, Vulkan-free; the GPU image that backs the pages is page_images.h.
// Thread: the bake thread's (or whoever builds the layout); not thread safe.
// Spec: 06_LIGHTING.md §3.4, 08 T6.1.
//
// A page keeps a list of disjoint free rectangles. alloc() takes the free rectangle of the smallest area that fits (best area fit) on the first page that has
// one, puts the chart at its top-left corner and splits what is left into two rectangles (guillotine, the longer leftover stays whole). free() returns the
// rectangle and merges it with free neighbours that share a whole edge until nothing merges, so freeing everything gives back whole pages. A rectangle
// is the chart's size with its padding included (the layout's `w` / `h`).
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace native::light {

struct PageRect {
    int page = 0, x = 0, y = 0, w = 0, h = 0;
    bool operator==(const PageRect& o) const { return page == o.page && x == o.x && y == o.y && w == o.w && h == o.h; }
};

class PageAllocator {
public:
    // Pages are atlasSize x atlasSize; the image array grows in steps of `growStep` layers (06 §3.4: 8).
    explicit PageAllocator(int atlasSize, int growStep = 8);

    int atlasSize() const { return atlas_; }
    int pageCount() const { return static_cast<int>(pages_.size()); }   // pages in use (the highest page that was ever needed + 1)
    int capacity() const { return capacity_; }                          // layers the image array must have: pageCount() rounded up to growStep
    // True once since the last call when capacity() grew (the image array must be grown and its old layers copied).
    bool takeGrown() { const bool g = grown_; grown_ = false; return g; }

    // The rectangle of a w x h chart on the first page with room; a new page is added when none has (nullopt only when w or h is not in 1..atlasSize).
    std::optional<PageRect> alloc(int w, int h);
    // The same on existing pages only (no page is added).
    std::optional<PageRect> tryAlloc(int w, int h);
    void free(const PageRect& r);
    // Makes sure pages [0, n) exist (empty pages).
    void addPages(int n);

    long long usedTexels() const { return used_; }
    long long allocatedTexels() const { return static_cast<long long>(pages_.size()) * atlas_ * atlas_; }
    double fill() const { return pages_.empty() ? 1.0 : static_cast<double>(used_) / static_cast<double>(allocatedTexels()); }
    double pageFill(int page) const;
    size_t freeRectCount(int page) const { return pages_[static_cast<size_t>(page)].free.size(); }
    // Free rectangles of a page (for tests and a debug overlay).
    struct Free { int x, y, w, h; };
    const std::vector<Free>& freeRects(int page) const { return pages_[static_cast<size_t>(page)].free; }
    // Highest page that holds a rectangle + 1 (0 when empty): pages above it could be dropped.
    int usedPageCount() const;

private:
    struct Page {
        std::vector<Free> free;
        long long used = 0;
        int minRefusedW = 1 << 30, minRefusedH = 1 << 30;   // a rectangle at least this big was refused: asking again is pointless until something is freed
    };
    bool place(Page& p, int w, int h, PageRect& out);
    static void merge(std::vector<Free>& list);

    int atlas_;
    int step_;
    int capacity_ = 0;
    bool grown_ = false;
    long long used_ = 0;
    std::vector<Page> pages_;
};

} // namespace native::light
