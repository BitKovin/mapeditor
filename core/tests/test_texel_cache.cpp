#include "core/lightbake.h"
#include "core/parallel.h"
#include "test_util.h"
#include "texel_reference.h"
#include <atomic>
#include <doctest.h>
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
