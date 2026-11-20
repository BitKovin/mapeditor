#include "light/layout.h"

#include <algorithm>
#include <chrono>

#include "core/parallel.h"

namespace native::light {

namespace {
using Clock = std::chrono::steady_clock;
double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }
}

LiveLayout::LiveLayout(const core::LightmapSettings& settings, int chunkTexels)
    : settings_(settings), chunkTexels_(std::max(1, chunkTexels)), alloc_(settings.atlasSize) {}

void LiveLayout::buildAll(const std::vector<core::LightSurface>& surfaces, const core::LightingClasses& classes) {
    stats_ = {};
    alloc_ = PageAllocator(settings_.atlasSize);
    ++version_;

    auto t = Clock::now();
    faces_ = core::prepareLayoutFaces(surfaces, classes, settings_);
    stats_.prepareSeconds = since(t);

    t = Clock::now();
    groups_ = core::chartGroups(faces_);
    stats_.groupsSeconds = since(t);
    stats_.groups = groups_.size();

    // the island growth of every group on all cores: a group never reaches into another one
    t = Clock::now();
    std::vector<std::vector<core::ChartShape>> perGroup(groups_.size());
    core::parallelFor(groups_.size(), 8, [&](size_t b, size_t e) {
        for (size_t g = b; g < e; ++g) perGroup[g] = core::buildCharts(faces_, groups_[g]);
    });
    stats_.chartsSeconds = since(t);

    // charts of all groups in one list
    t = Clock::now();
    charts_.clear();
    for (size_t g = 0; g < perGroup.size(); ++g)
        for (core::ChartShape& s : perGroup[g]) {
            LiveChart c;
            c.group = static_cast<int>(g);
            c.shape = std::move(s);
            charts_.push_back(std::move(c));
        }
    // placement order: tallest first (06 §3.4), then the widest, ties by root face so the result does not depend on the group order
    std::vector<size_t> order(charts_.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const core::ChartShape& x = charts_[a].shape;
        const core::ChartShape& y = charts_[b].shape;
        if (x.h != y.h) return x.h > y.h;
        if (x.w != y.w) return x.w > y.w;
        return x.root < y.root;
    });
    for (size_t i : order) {
        LiveChart& c = charts_[i];
        // a chart larger than a page was cut to the page by the generator's size limit; the allocator refuses nothing else
        const auto r = alloc_.alloc(std::min(c.shape.w, settings_.atlasSize), std::min(c.shape.h, settings_.atlasSize));
        c.where = r ? *r : PageRect{};
    }
    // chart indices by root face: the same for any group order (the Core's own order is by root area; the shapes are compared as sets, not by index)
    std::stable_sort(charts_.begin(), charts_.end(), [&](const LiveChart& a, const LiveChart& b) {
        return a.shape.root < b.shape.root;
    });
    stats_.placeSeconds = since(t);

    t = Clock::now();
    std::vector<core::ChartShape> shapes;
    std::vector<core::ChartPlacement> at;
    shapes.reserve(charts_.size());
    at.reserve(charts_.size());
    for (const LiveChart& c : charts_) {
        shapes.push_back(c.shape);
        at.push_back({c.where.page, c.where.x, c.where.y});
    }
    layout_ = core::placeChartsAt(faces_, shapes, at, alloc_.pageCount(), settings_);

    // chunks: tiles of the chart's texels (padding excluded)
    chunks_.clear();
    chunkBegin_.assign(charts_.size() + 1, 0);
    const int pad = settings_.padding;
    for (size_t ci = 0; ci < charts_.size(); ++ci) {
        chunkBegin_[ci] = chunks_.size();
        const LiveChart& c = charts_[ci];
        const int x0 = c.where.x + pad, y0 = c.where.y + pad;
        const int x1 = c.where.x + c.where.w - pad, y1 = c.where.y + c.where.h - pad;
        for (int y = y0; y < y1; y += chunkTexels_)
            for (int x = x0; x < x1; x += chunkTexels_) {
                Chunk k;
                k.chart = static_cast<int>(ci);
                k.x = x;
                k.y = y;
                k.w = std::min(chunkTexels_, x1 - x);
                k.h = std::min(chunkTexels_, y1 - y);
                chunks_.push_back(k);
            }
    }
    chunkBegin_[charts_.size()] = chunks_.size();
    stats_.assembleSeconds = since(t);
    stats_.charts = charts_.size();
    stats_.chunks = chunks_.size();
}

} // namespace native::light
