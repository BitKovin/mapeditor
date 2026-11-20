// T6.1: the page allocator (alloc / free / merge / grow) and the live layout (chart shapes equal the Core's buildLightmapLayout, chunks tile every chart).
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <random>
#include <set>
#include <string>

#include "core/lighting_classes.h"
#include "core/lightmap_uv.h"
#include "core/scene.h"
#include "light/layout.h"
#include "light/page_allocator.h"

using namespace native::light;
namespace fs = std::filesystem;

namespace {

bool overlap(const PageRect& a, const PageRect& b) {
    return a.page == b.page && a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

// every page: the free rectangles are disjoint, inside the page, and together with the allocated ones cover it exactly
void checkConsistent(const PageAllocator& a, const std::vector<PageRect>& live) {
    long long used = 0;
    for (const PageRect& r : live) {
        REQUIRE(r.x >= 0);
        REQUIRE(r.y >= 0);
        REQUIRE(r.x + r.w <= a.atlasSize());
        REQUIRE(r.y + r.h <= a.atlasSize());
        used += static_cast<long long>(r.w) * r.h;
    }
    CHECK(a.usedTexels() == used);
    for (int p = 0; p < a.pageCount(); ++p) {
        long long freeArea = 0;
        long long liveArea = 0;
        std::vector<PageRect> freeRects;
        for (const auto& f : a.freeRects(p)) {
            freeRects.push_back({p, f.x, f.y, f.w, f.h});
            freeArea += static_cast<long long>(f.w) * f.h;
            REQUIRE(f.x >= 0);
            REQUIRE(f.y >= 0);
            REQUIRE(f.x + f.w <= a.atlasSize());
            REQUIRE(f.y + f.h <= a.atlasSize());
        }
        for (const PageRect& r : live)
            if (r.page == p) liveArea += static_cast<long long>(r.w) * r.h;
        REQUIRE(freeArea + liveArea == static_cast<long long>(a.atlasSize()) * a.atlasSize());
        for (size_t i = 0; i < freeRects.size(); ++i) {
            for (size_t j = i + 1; j < freeRects.size(); ++j) REQUIRE_FALSE(overlap(freeRects[i], freeRects[j]));
            for (const PageRect& r : live) REQUIRE_FALSE(overlap(freeRects[i], r));
        }
    }
}

} // namespace

TEST_CASE("page allocator: first fit fills a page, adds pages, grows the capacity in steps of 8") {
    PageAllocator a(64);
    CHECK(a.pageCount() == 0);
    CHECK(a.capacity() == 0);
    // four 32 x 32 charts are one page
    std::vector<PageRect> live;
    for (int i = 0; i < 4; ++i) {
        const auto r = a.alloc(32, 32);
        REQUIRE(r);
        CHECK(r->page == 0);
        live.push_back(*r);
    }
    CHECK(a.pageCount() == 1);
    CHECK(a.capacity() == 8);
    CHECK(a.takeGrown());
    CHECK_FALSE(a.takeGrown());
    CHECK(a.pageFill(0) == doctest::Approx(1.0));
    checkConsistent(a, live);
    // the next one needs a second page, the capacity stays
    const auto r5 = a.alloc(10, 10);
    REQUIRE(r5);
    CHECK(r5->page == 1);
    CHECK(a.capacity() == 8);
    CHECK_FALSE(a.takeGrown());
    // the 9th page takes the capacity to 16
    for (int i = 0; i < 7; ++i) REQUIRE(a.alloc(64, 64));
    CHECK(a.pageCount() == 9);
    CHECK(a.capacity() == 16);
    CHECK(a.takeGrown());
    // too big or empty: refused, nothing changes
    CHECK_FALSE(a.alloc(65, 4));
    CHECK_FALSE(a.alloc(4, 0));
    CHECK(a.pageCount() == 9);
    // tryAlloc never adds a page
    PageAllocator b(32);
    CHECK_FALSE(b.tryAlloc(8, 8));
    CHECK(b.pageCount() == 0);
}

TEST_CASE("page allocator: freeing merges the holes back, a hole is reused, freeing everything gives whole pages") {
    PageAllocator a(128);
    std::vector<PageRect> live;
    for (int i = 0; i < 16; ++i) live.push_back(*a.alloc(32, 32));   // 4 x 4 tiles = one page
    REQUIRE(a.pageCount() == 1);
    CHECK(a.freeRectCount(0) == 0);
    // free two neighbours: one 64 x 32 hole (or a 32 x 64 one), not two
    const PageRect x = live[0], y = live[1];
    a.free(x);
    a.free(y);
    live.erase(live.begin(), live.begin() + 2);
    CHECK(a.freeRectCount(0) == 1);
    checkConsistent(a, live);
    // a chart of that size takes the hole instead of a new page
    const auto hole = a.freeRects(0)[0];
    CHECK(hole.w * hole.h == 64 * 32);
    const auto r = a.alloc(hole.w, hole.h);
    REQUIRE(r);
    CHECK(r->page == 0);
    live.push_back(*r);
    CHECK(a.pageCount() == 1);
    // free everything: one free rectangle per page, the whole page
    for (const PageRect& l : live) a.free(l);
    CHECK(a.usedTexels() == 0);
    CHECK(a.freeRectCount(0) == 1);
    CHECK(a.freeRects(0)[0].w == 128);
    CHECK(a.freeRects(0)[0].h == 128);
    CHECK(a.usedPageCount() == 0);
}

TEST_CASE("page allocator: 4000 random allocations and frees never overlap, keep the books, and freeing all restores the pages") {
    PageAllocator a(256);
    std::mt19937 rng(7);
    std::vector<PageRect> live;
    for (int step = 0; step < 4000; ++step) {
        const bool doFree = !live.empty() && (rng() % 100) < 45;
        if (doFree) {
            const size_t i = rng() % live.size();
            a.free(live[i]);
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            const int w = 4 + static_cast<int>(rng() % 60), h = 4 + static_cast<int>(rng() % 60);
            const auto r = a.alloc(w, h);
            REQUIRE(r);
            REQUIRE(r->w == w);
            REQUIRE(r->h == h);
            for (const PageRect& o : live) REQUIRE_FALSE(overlap(*r, o));
            live.push_back(*r);
        }
        if (step % 500 == 0) checkConsistent(a, live);
    }
    checkConsistent(a, live);
    const int pages = a.pageCount();
    for (const PageRect& l : live) a.free(l);
    CHECK(a.usedTexels() == 0);
    for (int p = 0; p < pages; ++p) {
        REQUIRE(a.freeRectCount(p) == 1);
        CHECK(a.freeRects(p)[0].w == 256);
        CHECK(a.freeRects(p)[0].h == 256);
    }
}

TEST_CASE("page allocator: packing tallest first on one page is not wasteful") {
    PageAllocator a(512);
    std::mt19937 rng(3);
    std::vector<std::pair<int, int>> sizes;
    long long area = 0;
    for (int i = 0; i < 600; ++i) {
        sizes.push_back({6 + static_cast<int>(rng() % 70), 6 + static_cast<int>(rng() % 70)});
        area += static_cast<long long>(sizes.back().first) * sizes.back().second;
    }
    std::sort(sizes.begin(), sizes.end(), [](auto& p, auto& q) { return p.second != q.second ? p.second > q.second : p.first > q.first; });
    for (auto& s : sizes) REQUIRE(a.alloc(s.first, s.second));
    const double ideal = std::ceil(static_cast<double>(area) / (512.0 * 512.0));
    MESSAGE("pages " << a.pageCount() << ", ideal " << ideal << ", fill " << a.fill());
    CHECK(a.usedTexels() == area);
    CHECK(a.fill() > 0.75);
}

namespace {

struct FaceShape {
    std::vector<std::string> chartFaces;
    int w = 0, h = 0;
    double k = 0;
    core::Vec3 su, sv;
    double so = 0, to = 0;   // without the chart's offset: the part of the map that does not depend on where the chart sits
    bool operator==(const FaceShape& o) const {
        return chartFaces == o.chartFaces && w == o.w && h == o.h && k == o.k && su.x == o.su.x && su.y == o.su.y && su.z == o.su.z && sv.x == o.sv.x &&
               sv.y == o.sv.y && sv.z == o.sv.z && std::fabs(so - o.so) < 1e-6 && std::fabs(to - o.to) < 1e-6;   // (a + x) - x is not a bitwise
    }
};

std::map<std::string, FaceShape> shapesOf(const core::LightmapLayout& L) {
    std::map<std::string, FaceShape> out;
    for (const core::LightmapFace& f : L.faces) {
        if (f.chart < 0) continue;
        const core::LightmapChart& c = L.charts[static_cast<size_t>(f.chart)];
        FaceShape s;
        for (int fi : c.faces) s.chartFaces.push_back(L.faces[static_cast<size_t>(fi)].key());
        std::sort(s.chartFaces.begin(), s.chartFaces.end());
        s.w = c.w;
        s.h = c.h;
        s.k = c.texelsPerMeter;
        s.su = f.su;
        s.sv = f.sv;
        s.so = f.so - c.x;
        s.to = f.to - c.y;
        out[f.key()] = std::move(s);
    }
    return out;
}

void checkLiveAgainstCore(const char* name, const std::vector<core::LightSurface>& surfaces, const core::LightingClasses& classes, const core::LightmapSettings& cfg) {
    INFO(name);
    const core::LightmapLayout reference = core::buildLightmapLayout(surfaces, classes, cfg);
    LiveLayout live(cfg);
    live.buildAll(surfaces, classes);
    const core::LightmapLayout& got = live.layout();
    REQUIRE(reference.charts.size() > 0);
    CHECK(got.charts.size() == reference.charts.size());
    CHECK(got.faces.size() == reference.faces.size());
    std::vector<std::string> wa = got.warnings, wb = reference.warnings;   // the order follows the charts'
    std::sort(wa.begin(), wa.end());
    std::sort(wb.begin(), wb.end());
    CHECK(wa == wb);
    const auto a = shapesOf(reference), b = shapesOf(got);
    REQUIRE(a.size() == b.size());
    size_t different = 0;
    for (const auto& [key, shape] : a) {
        const auto it = b.find(key);
        REQUIRE(it != b.end());
        if (!(it->second == shape)) ++different;
    }
    CHECK(different == 0);

    // the placement is a valid one: every rectangle is in its page and none overlaps another
    std::vector<PageRect> rects;
    for (const LiveChart& c : live.charts()) {
        REQUIRE(c.where.w == c.shape.w);
        REQUIRE(c.where.h == c.shape.h);
        REQUIRE(c.where.page >= 0);
        REQUIRE(c.where.page < live.allocator().pageCount());
        REQUIRE(c.where.x >= 0);
        REQUIRE(c.where.y >= 0);
        REQUIRE(c.where.x + c.where.w <= cfg.atlasSize);
        REQUIRE(c.where.y + c.where.h <= cfg.atlasSize);
        rects.push_back(c.where);
    }
    checkConsistent(live.allocator(), rects);
    // the Core type carries the same placement
    for (size_t i = 0; i < live.charts().size(); ++i) {
        REQUIRE(got.charts[i].page == live.charts()[i].where.page);
        REQUIRE(got.charts[i].x == live.charts()[i].where.x);
        REQUIRE(got.charts[i].y == live.charts()[i].where.y);
    }
    MESSAGE(name << ": charts " << got.charts.size() << ", groups " << live.groups().size() << ", pages live " << live.allocator().pageCount() << " core "
                 << reference.pageCount << ", fill live " << live.allocator().fill() << " core "
                 << (reference.pageCount ? [&] { double s = 0; for (double f : reference.pageFill) s += f; return s / reference.pageCount; }() : 0.0)
                 << ", chunks " << live.chunks().size() << ", layout " << live.stats().prepareSeconds + live.stats().groupsSeconds + live.stats().chartsSeconds +
                                                                            live.stats().placeSeconds + live.stats().assembleSeconds << " s");

    // chunks: tiles of at most chunkTexels, inside the chart's texel area, covering it exactly
    const int pad = cfg.padding;
    for (size_t ci = 0; ci < live.charts().size(); ++ci) {
        const LiveChart& c = live.charts()[ci];
        long long covered = 0;
        for (size_t k = live.chunkBegin(ci); k < live.chunkBegin(ci + 1); ++k) {
            const Chunk& ch = live.chunks()[k];
            REQUIRE(ch.chart == static_cast<int>(ci));
            REQUIRE(ch.w >= 1);
            REQUIRE(ch.h >= 1);
            REQUIRE(ch.w <= live.chunkTexels());
            REQUIRE(ch.h <= live.chunkTexels());
            REQUIRE(ch.x >= c.where.x + pad);
            REQUIRE(ch.y >= c.where.y + pad);
            REQUIRE(ch.x + ch.w <= c.where.x + c.where.w - pad);
            REQUIRE(ch.y + ch.h <= c.where.y + c.where.h - pad);
            covered += static_cast<long long>(ch.w) * ch.h;
        }
        REQUIRE(covered == static_cast<long long>(c.where.w - 2 * pad) * (c.where.h - 2 * pad));
    }
    CHECK(live.chunkBegin(live.charts().size()) == live.chunks().size());
}

} // namespace

TEST_CASE("live layout: chart shapes of demo_lit and lvl2 equal the Core's, the placement is valid and the chunks tile every chart") {
    struct Source { fs::path root, map; };
    std::vector<Source> sources;
    const fs::path coreAssets = NATIVE_CORE_ASSETS_DIR;
    sources.push_back({coreAssets, "maps/demo_lit.map.json"});
    sources.push_back({coreAssets, "maps/engine_test.map.json"});
    const fs::path game = "F:/Git/SdlWasmEngine/source/Games/ShooterGame/GameData";
    if (fs::exists(game / "maps/lvl2.map.json")) sources.push_back({game, "maps/lvl2.map.json"});
    for (const Source& src : sources) {
        if (!fs::exists(src.root / src.map)) continue;
        core::Scene scene(src.root);
        scene.loadGameProfile();
        scene.openDocument(src.map.generic_string());
        const core::LightmapLayout referenceForSettings = scene.lightmapLayout();
        const core::LightmapSettings cfg = referenceForSettings.settings;
        checkLiveAgainstCore(src.map.stem().stem().string().c_str(), scene.lightSurfaces(), scene.lightingClasses(), cfg);
    }
}
