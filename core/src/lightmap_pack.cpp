#include "core/lightmap_pack.h"
#include <algorithm>
#include <limits>
#include <numeric>
#include <tuple>

namespace core {

std::optional<PackRect> PackPage::place(int w, int h) {
    int best = -1;
    long long bestShort = std::numeric_limits<long long>::max(), bestLong = bestShort;
    for (size_t i = 0; i < freeRects.size(); ++i) {
        const PackRect& f = freeRects[i];
        if (f.w < w || f.h < h) continue;
        const long long dw = f.w - w, dh = f.h - h;
        const long long sh = std::min(dw, dh), lg = std::max(dw, dh);
        // ties: lower y, then lower x, for a deterministic and compact result
        if (sh < bestShort || (sh == bestShort && lg < bestLong) ||
            (sh == bestShort && lg == bestLong && best >= 0 && std::tie(f.y, f.x) < std::tie(freeRects[best].y, freeRects[best].x))) {
            best = static_cast<int>(i);
            bestShort = sh;
            bestLong = lg;
        }
    }
    if (best < 0) return std::nullopt;
    const PackRect r{freeRects[best].x, freeRects[best].y, w, h};
    std::vector<PackRect> next;
    std::vector<char> fresh;  // 1 = a piece cut from a rectangle the new one overlapped
    for (const PackRect& f : freeRects) {
        if (r.x >= f.x + f.w || r.x + r.w <= f.x || r.y >= f.y + f.h || r.y + r.h <= f.y) { next.push_back(f); fresh.push_back(0); continue; }
        auto add = [&](const PackRect& piece) { next.push_back(piece); fresh.push_back(1); };
        if (r.x > f.x) add({f.x, f.y, r.x - f.x, f.h});
        if (r.x + r.w < f.x + f.w) add({r.x + r.w, f.y, f.x + f.w - (r.x + r.w), f.h});
        if (r.y > f.y) add({f.x, f.y, f.w, r.y - f.y});
        if (r.y + r.h < f.y + f.h) add({f.x, r.y + r.h, f.w, f.y + f.h - (r.y + r.h)});
    }
    // prune rectangles contained in another one. The rectangles kept from before are not contained in each other, and a piece lies inside
    // the rectangle it was cut from, so only the new pieces can be contained: the others are kept without looking (this was quadratic in the
    // free list for every chart placed, and took seconds on a level with tens of thousands of charts).
    std::vector<PackRect> kept;
    for (size_t i = 0; i < next.size(); ++i) {
        bool contained = false;
        for (size_t j = 0; fresh[i] && j < next.size() && !contained; ++j) {
            if (i == j) continue;
            const PackRect &a = next[i], &b = next[j];
            if (a.x >= b.x && a.y >= b.y && a.x + a.w <= b.x + b.w && a.y + a.h <= b.y + b.h && (i > j || !(a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h))) contained = true;
        }
        if (!contained) kept.push_back(next[i]);
    }
    freeRects = std::move(kept);
    used += static_cast<long long>(w) * h;
    return r;
}

std::vector<PackPlacement> packRects(const std::vector<std::pair<int, int>>& sizes, int pageSize, int& pages) {
    std::vector<size_t> order(sizes.size());
    std::iota(order.begin(), order.end(), size_t(0));
    // biggest side first, then area: the large charts set the skeleton and the small ones fill the holes
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const int ma = std::max(sizes[a].first, sizes[a].second), mb = std::max(sizes[b].first, sizes[b].second);
        if (ma != mb) return ma > mb;
        return static_cast<long long>(sizes[a].first) * sizes[a].second > static_cast<long long>(sizes[b].first) * sizes[b].second;
    });
    std::vector<PackPlacement> out(sizes.size());
    std::vector<PackPage> list;
    std::vector<std::vector<std::pair<int, int>>> refused;  // per page: sizes it could not take (anything at least as big fails too)
    for (size_t i : order) {
        const int w = std::clamp(sizes[i].first, 1, pageSize), h = std::clamp(sizes[i].second, 1, pageSize);
        bool done = false;
        for (size_t p = 0; p < list.size() && !done; ++p) {
            bool skip = false;
            for (const auto& f : refused[p]) if (w >= f.first && h >= f.second) { skip = true; break; }
            if (skip) continue;
            if (const auto r = list[p].place(w, h)) {
                out[i] = {static_cast<int>(p), r->x, r->y};
                done = true;
            } else {
                refused[p].push_back({w, h});
            }
        }
        if (done) continue;
        list.emplace_back(pageSize);
        refused.emplace_back();
        const auto r = list.back().place(w, h);
        out[i] = {static_cast<int>(list.size()) - 1, r ? r->x : 0, r ? r->y : 0};
    }
    pages = static_cast<int>(list.size());
    return out;
}

}  // namespace core
