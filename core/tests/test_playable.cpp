// Playable area (editor-only volume, playable.h): not part of the level, distance, quality falloff of the lightmap, light volume bounds.
#include "core/derived.h"
#include "core/lightmap_uv.h"
#include "core/lights.h"
#include "core/lightvol.h"
#include "core/playable.h"
#include "core/playable_visibility.h"
#include "core/prefab.h"
#include "test_util.h"
#include <doctest.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>

using namespace core;

namespace {
Entity ent(const std::string& id, const std::string& cls, std::vector<Brush> brushes = {}) {
    Entity e;
    e.id = id;
    e.classname = cls;
    e.brushes = std::move(brushes);
    return e;
}
// A long floor along x (0..60 m) with the playable volume over its first 10 m.
Document longFloor(bool withVolume) {
    Document doc;
    doc.entities.push_back(ent("w", "worldspawn", {Brush::makeBox("floor", {0, -1, 0}, {60, 0, 4})}));
    if (withVolume) doc.entities.push_back(ent("p", "editor_playable_area", {Brush::makeBox("vol", {0, 0, 0}, {10, 3, 4})}));
    return doc;
}
}  // namespace

TEST_CASE("playable: the volume has no surfaces, no solids and does not reach the compiler") {
    const Document doc = longFloor(true);
    const auto surfaces = collectSurfaces(doc, {});
    for (const auto& s : surfaces) CHECK(s.classname != "editor_playable_area");
    CHECK(surfaces.size() == 6);
    const SolidSet solids = collectSolids(doc, {}, [](const std::string&) { return true; });
    CHECK(solids.volumeCount() == 1);  // the floor only
    const FlattenResult flat = flattenForCompile(doc, [](const std::string&) -> const Document& { throw std::runtime_error("no prefabs"); });
    for (const auto& e : flat.entities) CHECK(e.entity.classname != "editor_playable_area");
    CHECK(flat.entities.size() == 1);
}

TEST_CASE("playable: distance and cascades") {
    const auto area = collectPlayableArea(longFloor(true), {});
    REQUIRE(!area->empty());
    CHECK(area->distance(Vec3{5, 1, 2}) == doctest::Approx(0));
    CHECK(area->distance(Vec3{20, 1, 2}) == doctest::Approx(10));
    CHECK(area->distance(Vec3{5, 1, 12}) == doctest::Approx(8));
    CHECK(area->distance(std::vector<Vec3>{{50, 0, 0}, {60, 0, 0}, {60, 0, 4}, {50, 0, 4}}) == doctest::Approx(40).epsilon(0.1));
    CHECK(area->distance(std::vector<Vec3>{{5, 0, 0}, {60, 0, 0}, {60, 0, 4}, {5, 0, 4}}) == doctest::Approx(0));  // overlaps the area
    PlayableLod lod;  // radius 16, range 2, density 0.5, 4 cascades
    CHECK(lod.cascade(0) == 0);
    CHECK(lod.cascade(16) == 0);
    CHECK(lod.cascade(17) == 1);
    CHECK(lod.cascade(32) == 1);
    CHECK(lod.cascade(33) == 2);
    CHECK(lod.cascade(1e6) == 4);  // beyond the last cascade stays in it
    CHECK(lod.densityAt(40) == doctest::Approx(0.25));
    lod.levels = 0;
    CHECK(lod.cascade(1000) == 0);
}

TEST_CASE("playable: lightmap density falls off with the distance from the area") {
    const Document doc = longFloor(true);
    const auto surfaces = collectSurfaces(doc, {});
    LightmapSettings cfg;
    cfg.texelsPerMeter = 8;
    const LightmapLayout flat = buildLightmapLayout(surfaces, LightingClasses::defaults(), cfg);
    cfg.playable = collectPlayableArea(doc, {});
    const LightmapLayout lod = buildLightmapLayout(surfaces, LightingClasses::defaults(), cfg);
    auto texels = [](const LightmapLayout& l) { long long n = 0; for (const auto& c : l.charts) n += static_cast<long long>(c.w) * c.h; return n; };
    CHECK(texels(lod) < texels(flat));
    // the floor's top face reaches into the area (distance 0): it keeps the full density
    double top = 0;
    for (const auto& f : lod.faces) if (f.normal.y > 0.9) top = f.texelsPerMeter;
    CHECK(top == doctest::Approx(8));
}

TEST_CASE("playable: the far faces of a level lose density, the near ones do not") {
    Document doc;
    doc.entities.push_back(ent("w", "worldspawn", {Brush::makeBox("near", {0, -1, 0}, {8, 0, 8}), Brush::makeBox("far", {100, -1, 0}, {108, 0, 8})}));
    doc.entities.push_back(ent("p", "editor_playable_area", {Brush::makeBox("vol", {0, 0, 0}, {8, 3, 8})}));
    LightmapSettings cfg;
    cfg.texelsPerMeter = 8;
    cfg.playable = collectPlayableArea(doc, {});
    const LightmapLayout l = buildLightmapLayout(collectSurfaces(doc, {}), LightingClasses::defaults(), cfg);
    double nearD = 0, farD = 0;
    for (const auto& f : l.faces) if (f.normal.y > 0.9) (f.brushId == "near" ? nearD : farD) = f.texelsPerMeter;
    CHECK(nearD == doctest::Approx(8));
    CHECK(farD == doctest::Approx(8 * 0.125));  // 92 m away: the third cascade (16 * 2^3 = 128 m)
}

TEST_CASE("playable: the light volume covers the area plus the margin") {
    const Document doc = longFloor(true);
    const auto surfaces = collectSurfaces(doc, {});
    LightvolSettings s;
    s.gridSize = 1;
    const LightvolGrid whole = buildLightvolGrid(surfaces, nullptr, s);
    const auto area = collectPlayableArea(doc, {});
    const LightvolGrid g = buildLightvolGrid(surfaces, nullptr, s, area.get(), 2.0);
    CHECK(g.count() > 0);
    CHECK(g.count() < whole.count() / 3);
    CHECK(g.origin.x <= 0);  // the floor starts at x = 0: the margin is clipped to the surfaces
    CHECK(g.origin.x + g.nx * g.cell < 20);
    const std::vector<char> skip = solidProbes(g, nullptr, area.get(), 2.0);
    size_t out = 0;
    for (char c : skip) out += c;
    CHECK(out > 0);                // the cells of the box corners further than the margin from the volume
    CHECK(out < g.count() / 2);
}

TEST_CASE("playable: level settings read the falloff keys") {
    Entity w = ent("w", "worldspawn");
    LevelSettings d = levelSettingsOf(w);
    CHECK(d.lod.radius == 16);
    CHECK(d.lod.levels == 4);
    w.keyvalues = {{"_lod_radius", "30"}, {"_lod_levels", "2"}, {"_lod_range", "3"}, {"_lod_density", "0.25"}, {"_lightvol_margin", "5"}};
    const LevelSettings s = levelSettingsOf(w);
    CHECK(s.lod.radius == 30);
    CHECK(s.lod.levels == 2);
    CHECK(s.lod.range == 3);
    CHECK(s.lod.density == 0.25);
    CHECK(s.lod.lightvolMargin == 5);
}

TEST_CASE("playable: a surface no point of the area can see goes to the last cascade") {
    // a floor inside the area, a far closed room (floor, ceiling, 4 walls, 0.5 m thick) 60 m away, and a far open slab in plain view
    Document doc;
    Entity w = ent("w", "worldspawn", {Brush::makeBox("floor", {-10, -1, -10}, {10, 0, 10})});
    w.brushes.push_back(Brush::makeBox("rf", {60, -1, 0}, {70, 0, 10}));          // the room's floor
    w.brushes.push_back(Brush::makeBox("rc", {60, 3, 0}, {70, 4, 10}));           // ceiling
    w.brushes.push_back(Brush::makeBox("rw0", {59.5, 0, 0}, {60, 3, 10}));        // walls
    w.brushes.push_back(Brush::makeBox("rw1", {70, 0, 0}, {70.5, 3, 10}));
    w.brushes.push_back(Brush::makeBox("rw2", {60, 0, -0.5}, {70, 3, 0}));
    w.brushes.push_back(Brush::makeBox("rw3", {60, 0, 10}, {70, 3, 10.5}));
    w.brushes.push_back(Brush::makeBox("slab", {0, -1, 60}, {10, 0, 70}));        // open, in view
    doc.entities.push_back(w);
    doc.entities.push_back(ent("p", "editor_playable_area", {Brush::makeBox("vol", {-10, 0, -10}, {10, 3, 10})}));
    const auto surfaces = collectSurfaces(doc, {});
    const auto area = collectPlayableArea(doc, {});
    PlayableVisibility vis(*area, surfaces, LightingClasses::defaults());
    REQUIRE(vis.viewpointCount() > 0);
    LightmapSettings cfg;
    cfg.texelsPerMeter = 8;
    cfg.playable = area;
    cfg.seen = [&](const std::vector<Vec3>& poly, const Vec3& n) { return vis.seen(poly, n); };
    const LightmapLayout l = buildLightmapLayout(surfaces, LightingClasses::defaults(), cfg);
    cfg.seen = nullptr;
    const LightmapLayout all = buildLightmapLayout(surfaces, LightingClasses::defaults(), cfg);
    auto density = [](const LightmapLayout& lay, const std::string& brush, double ny) {
        for (const auto& f : lay.faces) if (f.brushId == brush && std::abs(f.normal.y - ny) < 0.1) return f.texelsPerMeter;
        return -1.0;
    };
    // the room's floor (top) is hidden by the walls and ceiling: last cascade; the open slab keeps its distance cascade
    CHECK(density(l, "rf", 1) == doctest::Approx(8 * 0.0625));
    CHECK(density(all, "rf", 1) > density(l, "rf", 1));
    CHECK(density(l, "slab", 1) == density(all, "slab", 1));
    CHECK(density(l, "floor", 1) == doctest::Approx(8));
}

// Measurement on a real level (not a regression test): MAPEDITOR_PLAYABLE_MAP=<.map.json> [MAPEDITOR_PLAYABLE_BOX="x0 y0 z0 x1 y1 z1"]
TEST_CASE("playable: measure the lightmap size of a real level with and without the quality falloff" * doctest::skip(std::getenv("MAPEDITOR_PLAYABLE_MAP") == nullptr)) {
    std::ifstream f(std::getenv("MAPEDITOR_PLAYABLE_MAP"));
    REQUIRE(f.good());
    Document doc = documentFromJson(nlohmann::json::parse(f));
    double b[6] = {-6, -34, -38, 115, 10, 110};
    if (const char* box = std::getenv("MAPEDITOR_PLAYABLE_BOX")) std::sscanf(box, "%lf %lf %lf %lf %lf %lf", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
    const auto t0 = std::chrono::steady_clock::now();
    const auto surfaces = collectSurfaces(doc, {});
    LightmapSettings cfg;
    cfg.texelsPerMeter = 8;
    auto stats = [&](const char* what, const LightmapLayout& l) {
        long long texels = 0;
        for (const auto& c : l.charts) texels += static_cast<long long>(c.w) * c.h;
        std::printf("%-34s pages %3d  charts %6zu  chart texels %9lld (%.1f M)\n", what, l.pageCount, l.charts.size(), texels, texels / 1e6);
        std::fflush(stdout);
    };
    const auto a = std::chrono::steady_clock::now();
    const LightmapLayout flat = buildLightmapLayout(surfaces, LightingClasses::defaults(), cfg);
    stats("no playable area", flat);
    doc.entities.push_back(ent("playable", "editor_playable_area", {Brush::makeBox("vol", {b[0], b[1], b[2]}, {b[3], b[4], b[5]})}));
    cfg.playable = collectPlayableArea(doc, {});
    const auto c = std::chrono::steady_clock::now();
    const LightmapLayout lod = buildLightmapLayout(surfaces, LightingClasses::defaults(), cfg);
    stats("cascades (distance only)", lod);
    const auto d = std::chrono::steady_clock::now();
    PlayableVisibility vis(*cfg.playable, surfaces, LightingClasses::defaults());
    const auto e = std::chrono::steady_clock::now();
    cfg.seen = [&](const std::vector<Vec3>& poly, const Vec3& n) { return vis.seen(poly, n); };
    const LightmapLayout culled = buildLightmapLayout(surfaces, LightingClasses::defaults(), cfg);
    const auto g = std::chrono::steady_clock::now();
    stats("cascades + unseen culled", culled);
    auto ms = [](auto x, auto y) { return std::chrono::duration<double, std::milli>(y - x).count(); };
    std::printf("surfaces %zu | layout flat %.0f ms, cascades %.0f ms | visibility build %.0f ms (%zu viewpoints), culled layout %.0f ms\n", surfaces.size(), ms(a, c), ms(c, d), ms(d, e), vis.viewpointCount(), ms(e, g));
    (void)t0;
    CHECK(culled.pageCount <= lod.pageCount);
}
