#include "light/page_allocator.h"

#include <algorithm>

namespace native::light {

PageAllocator::PageAllocator(int atlasSize, int growStep) : atlas_(atlasSize), step_(std::max(1, growStep)) {}

void PageAllocator::addPages(int n) {
    while (static_cast<int>(pages_.size()) < n) {
        Page p;
        p.free.push_back({0, 0, atlas_, atlas_});
        pages_.push_back(std::move(p));
    }
    const int cap = (static_cast<int>(pages_.size()) + step_ - 1) / step_ * step_;
    if (cap > capacity_) {
        capacity_ = cap;
        grown_ = true;
    }
}

bool PageAllocator::place(Page& p, int w, int h, PageRect& out) {
    if (w >= p.minRefusedW && h >= p.minRefusedH) return false;
    int best = -1;
    long long bestArea = 0;
    for (size_t i = 0; i < p.free.size(); ++i) {
        const Free& f = p.free[i];
        if (f.w < w || f.h < h) continue;
        const long long area = static_cast<long long>(f.w) * f.h;
        if (best < 0 || area < bestArea) { best = static_cast<int>(i); bestArea = area; }
    }
    if (best < 0) {
        p.minRefusedW = std::min(p.minRefusedW, w);
        p.minRefusedH = std::min(p.minRefusedH, h);
        return false;
    }
    const Free f = p.free[static_cast<size_t>(best)];
    out = {0, f.x, f.y, w, h};
    p.free.erase(p.free.begin() + best);
    const int rw = f.w - w, rh = f.h - h;
    // the longer leftover stays one whole rectangle: the right strip spans the full height when rw >= rh, else the bottom strip spans the full width
    if (rw >= rh) {
        if (rw > 0) p.free.push_back({f.x + w, f.y, rw, f.h});
        if (rh > 0) p.free.push_back({f.x, f.y + h, w, rh});
    } else {
        if (rh > 0) p.free.push_back({f.x, f.y + h, f.w, rh});
        if (rw > 0) p.free.push_back({f.x + w, f.y, rw, h});
    }
    p.used += static_cast<long long>(w) * h;
    return true;
}

std::optional<PageRect> PageAllocator::tryAlloc(int w, int h) {
    if (w < 1 || h < 1 || w > atlas_ || h > atlas_) return std::nullopt;
    for (size_t i = 0; i < pages_.size(); ++i) {
        PageRect r;
        if (place(pages_[i], w, h, r)) {
            r.page = static_cast<int>(i);
            used_ += static_cast<long long>(w) * h;
            return r;
        }
    }
    return std::nullopt;
}

std::optional<PageRect> PageAllocator::alloc(int w, int h) {
    if (auto r = tryAlloc(w, h)) return r;
    if (w < 1 || h < 1 || w > atlas_ || h > atlas_) return std::nullopt;
    addPages(static_cast<int>(pages_.size()) + 1);
    return tryAlloc(w, h);
}

// Merges the last rectangle of the list (the one just freed) with every free neighbour that shares a whole edge, as often as it grows. The others do not
// need to be looked at: two of them that could merge would have been merged when the second was freed.
void PageAllocator::merge(std::vector<Free>& list) {
    Free r = list.back();
    list.pop_back();
    bool again = true;
    while (again) {
        again = false;
        for (size_t i = 0; i < list.size(); ++i) {
            const Free& b = list[i];
            if (r.y == b.y && r.h == b.h && (r.x + r.w == b.x || b.x + b.w == r.x)) {
                r.x = std::min(r.x, b.x);
                r.w += b.w;
            } else if (r.x == b.x && r.w == b.w && (r.y + r.h == b.y || b.y + b.h == r.y)) {
                r.y = std::min(r.y, b.y);
                r.h += b.h;
            } else {
                continue;
            }
            list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
            again = true;
            break;
        }
    }
    list.push_back(r);
}

void PageAllocator::free(const PageRect& r) {
    if (r.page < 0 || r.page >= pageCount() || r.w < 1 || r.h < 1) return;
    Page& p = pages_[static_cast<size_t>(r.page)];
    p.used -= static_cast<long long>(r.w) * r.h;
    if (p.used == 0) {
        // nothing left on the page: one rectangle again (merging only reunites what a cut separated, a neighbour merged across cuts can leave a tiling that
        // no pair of rectangles joins, so an empty page is reset explicitly)
        p.free.assign(1, {0, 0, atlas_, atlas_});
    } else {
        p.free.push_back({r.x, r.y, r.w, r.h});
        merge(p.free);
    }
    used_ -= static_cast<long long>(r.w) * r.h;
    p.minRefusedW = p.minRefusedH = 1 << 30;   // there is room somewhere new
}

double PageAllocator::pageFill(int page) const {
    return static_cast<double>(pages_[static_cast<size_t>(page)].used) / (static_cast<double>(atlas_) * atlas_);
}

int PageAllocator::usedPageCount() const {
    for (int i = pageCount() - 1; i >= 0; --i)
        if (pages_[static_cast<size_t>(i)].used > 0) return i + 1;
    return 0;
}

} // namespace native::light
