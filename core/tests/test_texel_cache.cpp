#include "core/lightbake.h"
#include "core/parallel.h"
#include "core/scene.h"
#include "test_util.h"
#include "texel_reference.h"
#include <atomic>
#include <doctest.h>
#include <filesystem>
#include <map>
#include <stdexcept>

using namespace core;
using namespace testutil;

namespace {

Entity ent(const std::string& id, const std::string& cls, std::vector<Brush> brushes) {
    Entity e;
    e.id = id;
    e.classname = cls;
    e.brushes = std::move(brushes);
    return e;
}

struct Level {
    Document doc;
    std::vector<LightSurface> surfaces;
    LightmapLayout layout;
    std::shared_ptr<const SolidSet> solids;
    explicit Level(std::vector<Brush> brushes, double tpm = 4) {
        doc.entities = {ent("w", "worldspawn", std::move(brushes))};
        surfaces = collectSurfaces(doc, {});
        LightmapSettings cfg;
        cfg.texelsPerMeter = tpm;
        layout = buildLightmapLayout(surfaces, LightingClasses::defaults(), cfg);
        const LightingClasses classes = LightingClasses::defaults();
        solids = std::make_shared<SolidSet>(collectSolids(doc, {}, [&](const std::string& c) { return classes.castsShadows(c); }));
    }
};

std::vector<Brush> room(double crateX, bool extra = false) {
    std::vector<Brush> b{Brush::makeBox("floor", {-6, -1, -6}, {6, 0, 6}), Brush::makeBox("wall", {-6, 0, -6}, {6, 3, -5.5}),
                         Brush::makeBox("crate", {crateX, 0, 0}, {crateX + 1.3, 1.1, 1.3}), lPrism("ell")};
    if (extra) b.push_back(Brush::makeBox("tiny", {4, 0, 4}, {4.07, 0.07, 4.07}));
    return b;
}

void requireSame(const TexelSamples& a, const TexelSamples& b) {
    REQUIRE(a.atlas == b.atlas);
    REQUIRE(a.pageCount == b.pageCount);
    REQUIRE(a.texel == b.texel);
    REQUIRE(a.pos == b.pos);
    REQUIRE(a.nrm == b.nrm);
    REQUIRE(a.sub == b.sub);
}

}  // namespace

TEST_CASE("texel cache: returns exactly what the plain rasteriser returns, over a series of edits") {
    TexelSampleCache cache;
    for (double x : {1.0, 1.0, 1.5, 2.0, -3.0}) {
        for (bool extra : {false, true}) {
            Level L(room(x, extra));
            size_t hiddenRef = 0, hiddenNew = 0;
            const TexelSamples ref = texel_reference::buildTexelSamplesReference(L.layout, L.surfaces, L.solids.get(), &hiddenRef);
            const TexelSamples got = cache.build(L.layout, L.surfaces, L.solids.get(), &hiddenNew);
            requireSame(ref, got);
            CHECK(hiddenRef == hiddenNew);
            CHECK(got.count() > 100);
        }
    }
    Level plain(room(1));
    requireSame(texel_reference::buildTexelSamplesReference(plain.layout, plain.surfaces, nullptr, nullptr), buildTexelSamples(plain.layout, plain.surfaces, nullptr));
}

namespace {

// T5.3: the samples of every chart, each worked out from the pieces of its chart alone, are the whole level's samples (same texel, position, normal and
// sub-texel data for every texel, nothing twice, nothing missing; the order is per chart). Returns the number of charts that had samples.
size_t requireChartsEqualWhole(const LightmapLayout& layout, const std::vector<LightSurface>& surfaces, const SolidSet* solids) {
    size_t hiddenWhole = 0;
    const TexelSamples whole = buildTexelSamples(layout, surfaces, solids, &hiddenWhole);
    std::map<uint32_t, size_t> at;
    for (size_t i = 0; i < whole.count(); ++i) REQUIRE(at.emplace(whole.texel[i], i).second);

    std::vector<std::vector<LightSurface>> pieces(layout.charts.size());
    for (const LightSurface& s : surfaces) {
        if (s.verts.size() < 3) continue;
        const LightmapFace* f = layout.findForFragment(s.instance, s.entityId, s.brushInstance, s.brushId, s.faceId, s.verts);
        if (f && f->chart >= 0) pieces[static_cast<size_t>(f->chart)].push_back(s);
    }
    size_t total = 0, charts = 0, hiddenSum = 0;
    for (size_t c = 0; c < layout.charts.size(); ++c) {
        size_t hidden = 0;
        const TexelSamples part = buildChartSamples(layout, static_cast<int>(c), pieces[c], solids, &hidden);
        hiddenSum += hidden;
        REQUIRE(part.atlas == whole.atlas);
        total += part.count();
        charts += part.count() > 0 ? 1 : 0;
        for (size_t i = 0; i < part.count(); ++i) {
            const auto it = at.find(part.texel[i]);
            REQUIRE(it != at.end());
            const size_t j = it->second;
            REQUIRE(std::equal(&part.pos[i * 4], &part.pos[i * 4] + 4, &whole.pos[j * 4]));
            REQUIRE(std::equal(&part.nrm[i * 4], &part.nrm[i * 4] + 4, &whole.nrm[j * 4]));
            REQUIRE(std::equal(&part.sub[i * 12], &part.sub[i * 12] + 12, &whole.sub[j * 12]));
        }
        // the fine GI grid of the chart is made from its samples alone
        if (part.count() > 0 && c % 7 == 0) {
            const GiSamples gi = buildGiSamples(part, 2);
            CHECK(gi.s.atlas == whole.atlas * 2);
            CHECK(gi.begin.size() == part.count() + 1);
        }
    }
    CHECK(total == whole.count());
    CHECK(hiddenSum == hiddenWhole);
    return charts;
}

}  // namespace

TEST_CASE("chart samples: every chart alone gives its share of the whole level's samples (a room with a crate, an L prism and a tiny brush)") {
    for (bool solids : {false, true}) {
        Level L(room(1.5, true), 6);
        CHECK(requireChartsEqualWhole(L.layout, L.surfaces, solids ? L.solids.get() : nullptr) > 4);
    }
    // pieces of other charts in the list are ignored
    Level L(room(1));
    REQUIRE(L.layout.charts.size() > 2);
    const TexelSamples one = buildChartSamples(L.layout, 0, L.surfaces, L.solids.get());
    std::vector<LightSurface> onlyZero;
    for (const LightSurface& s : L.surfaces) {
        const LightmapFace* f = L.layout.findForFragment(s.instance, s.entityId, s.brushInstance, s.brushId, s.faceId, s.verts);
        if (f && f->chart == 0) onlyZero.push_back(s);
    }
    requireSame(one, buildChartSamples(L.layout, 0, onlyZero, L.solids.get()));
    CHECK(one.count() > 0);
    CHECK(one.count() < buildTexelSamples(L.layout, L.surfaces, L.solids.get()).count());
}

TEST_CASE("chart samples: on the sample maps (demo_lit, lvl2) the charts together are the whole level") {
    namespace fs = std::filesystem;
    struct Source { fs::path root, map; };
    std::vector<Source> sources;
    const fs::path coreAssets = fs::path(TESTDATA_DIR).parent_path() / "core" / "assets";
    sources.push_back({coreAssets, "maps/demo_lit.map.json"});
    const fs::path game = "F:/Git/SdlWasmEngine/source/Games/ShooterGame/GameData";
    if (fs::exists(game / "maps/lvl2.map.json")) sources.push_back({game, "maps/lvl2.map.json"});
    for (const Source& src : sources) {
        if (!fs::exists(src.root / src.map)) continue;
        Scene scene(src.root);
        scene.loadGameProfile();
        scene.openDocument(src.map.generic_string());
        const LightmapLayout layout = scene.lightmapLayout();
        const std::vector<LightSurface> surfaces = scene.lightSurfaces();
        const std::shared_ptr<SolidSet> solids = scene.lightSolids();
        INFO(src.map.generic_string());
        CHECK(requireChartsEqualWhole(layout, surfaces, solids.get()) > 0);
    }
}

TEST_CASE("texel cache: an unchanged level is served from the cache, a moved crate only regenerates what moved") {
    TexelSampleCache cache;
    Level a(room(1));
    cache.build(a.layout, a.surfaces, a.solids.get());
    CHECK(cache.stats().reused == 0);
    cache.build(a.layout, a.surfaces, a.solids.get());
    CHECK(cache.stats().reused == cache.stats().surfaces);
    Level b(room(2));
    cache.build(b.layout, b.surfaces, b.solids.get());
    CHECK(cache.stats().reused < cache.stats().surfaces);
    CHECK(cache.stats().reused >= cache.stats().surfaces / 2);  // floor, wall and the L prism did not move
}

TEST_CASE("texel cache: the result does not depend on the number of threads") {
    Level L(room(1.5, true), 8);
    const unsigned before = parallelHelpers();
    setParallelHelpers(0);
    const TexelSamples one = buildTexelSamples(L.layout, L.surfaces, L.solids.get());
    setParallelHelpers(3);
    const TexelSamples many = buildTexelSamples(L.layout, L.surfaces, L.solids.get());
    setParallelHelpers(before);
    requireSame(one, many);
}

TEST_CASE("parallelFor: covers every index once, rethrows, and nested calls run inline") {
    const unsigned before = parallelHelpers();
    for (unsigned helpers : {0u, 1u, 3u}) {
        setParallelHelpers(helpers);
        std::vector<std::atomic<int>> hits(1000);
        parallelFor(hits.size(), 7, [&](size_t b, size_t e) { for (size_t i = b; i < e; ++i) hits[i]++; });
        for (auto& h : hits) CHECK(h.load() == 1);
        std::atomic<int> nested{0};
        parallelFor(40, 5, [&](size_t b, size_t e) { parallelFor(e - b, 1, [&](size_t, size_t) { nested++; }); });
        CHECK(nested.load() == 40);  // 8 outer ranges x 5 inner ones, whatever the thread count
        CHECK_THROWS_AS(parallelFor(100, 1, [](size_t b, size_t) { if (b == 50) throw std::runtime_error("x"); }), std::runtime_error);
        parallelFor(0, 1, [](size_t, size_t) { FAIL("no range expected"); });
    }
    setParallelHelpers(before);
}
