// Level settings stored as worldspawn keys (REQUIREMENTS §22 P2): parsing, general brightness, baked ambient occlusion, quality overrides.
#include "core/live_bake.h"
#include "core/lights.h"
#include "test_util.h"
#include <doctest.h>
#include <cmath>

using namespace core;

namespace {
Entity ent(const std::string& id, const std::string& cls, std::vector<Brush> brushes = {}) {
    Entity e;
    e.id = id;
    e.classname = cls;
    e.brushes = std::move(brushes);
    return e;
}
Entity pointLight(const std::string& id, Vec3 o, double brightness, double radius) {
    Entity e = ent(id, "light_point");
    e.origin = o;
    e.keyvalues["brightness"] = std::to_string(brightness);
    e.keyvalues["radius"] = std::to_string(radius);
    return e;
}
struct World {
    std::vector<LightSurface> surfaces;
    LightmapLayout layout;
    LightScene lights;
    explicit World(const std::vector<Entity>& es) {
        Document doc;
        doc.entities = es;
        surfaces = collectSurfaces(doc, {});
        LightmapSettings cfg;
        cfg.texelsPerMeter = 4;
        layout = buildLightmapLayout(surfaces, LightingClasses::defaults(), cfg);
        lights = collectLights(doc, {});
    }
};
// A floor (x, z in -4..4, top at y = 0) with a wall standing on it along x = 0 (it rises from the floor to y = 3).
std::vector<Entity> floorAndWall(std::unordered_map<std::string, std::string> keys) {
    Entity w = ent("w", "worldspawn", {Brush::makeBox("floor", {-4, -1, -4}, {4, 0, 4}), Brush::makeBox("wall", {-0.25, 0, -4}, {0.25, 3, 4})});
    w.keyvalues = std::move(keys);
    return {w, pointLight("l", {2, 2, 0}, 10, 30)};
}
LiveSettings cpuOnly() {
    LiveSettings s;
    s.useGpu = false;
    s.bounces = false;
    s.maxPasses = 2;
    s.chunkTexels = 8;
    return s;
}
// Floor samples (normal up, at y = 0) at distance `near` .. `far` from the wall's side.
std::vector<size_t> floorSamples(const LiveBaker& b, double nearX, double farX) {
    std::vector<size_t> out;
    const auto& s = b.samples();
    for (size_t i = 0; i < s.count(); ++i) {
        const double x = s.pos[i * 4], y = s.pos[i * 4 + 1], z = s.pos[i * 4 + 2], ny = s.nrm[i * 4 + 1];
        if (ny > 0.99 && std::abs(y) < 1e-3 && std::abs(z) < 3 && x > nearX && x < farX) out.push_back(i);
    }
    return out;
}
}  // namespace

TEST_CASE("level settings: worldspawn keys are read, clamped and defaulted") {
    Entity w = ent("w", "worldspawn");
    LevelSettings d = levelSettingsOf(w);
    CHECK(d.lightScale == 1);
    CHECK(!d.ao.active());
    CHECK(d.bakeQuality == 2);
    CHECK(d.raysPerTexel == 0);
    w.keyvalues = {{"_light_scale", "2.5"}, {"_ao_radius", "1.5"}, {"_ao_power", "2"}, {"_ao_intensity", "7"}, {"_cc_exposure", "0.01"},
                   {"_cc_brightness", "0.1"}, {"_rays_per_texel", "512"}, {"_supersample", "40"}, {"_lightvol_grid", "0.1"}, {"_rays_per_probe", "64"},
                   {"_bake_quality", "4"}, {"_ao_power_typo", "x"}};
    std::vector<std::string> warnings;
    const LevelSettings s = levelSettingsOf(w, &warnings);
    CHECK(s.lightScale == 2.5);
    CHECK(s.ao.radius == 1.5);
    CHECK(s.ao.power == 2);
    CHECK(s.ao.intensity == 1);  // clamped to 0..1
    CHECK(s.exposure == doctest::Approx(0.05));
    CHECK(s.brightness == doctest::Approx(0.1));
    CHECK(s.raysPerTexel == 512);
    CHECK(s.supersample == 8);
    CHECK(s.lightvolGrid == 0.25);
    CHECK(s.raysPerProbe == 64);
    CHECK(s.bakeQuality == 4);
    CHECK(warnings.empty());
    w.keyvalues = {{"_ao_radius", "far"}};
    CHECK(levelSettingsOf(w, &warnings).ao.radius == 0);
    CHECK(warnings.size() == 1);
}

TEST_CASE("level settings: _light_scale multiplies every light's brightness, not the ambient") {
    Document doc;
    Entity w = ent("w", "worldspawn");
    w.keyvalues = {{"_light_scale", "3"}, {"_ambient", "0.1 0.1 0.1"}};
    doc.entities = {w, pointLight("a", {0, 1, 0}, 2, 10), pointLight("b", {0, 1, 0}, 0.5, 10)};
    const LightScene s = collectLights(doc, {});
    REQUIRE(s.lights.size() == 2);
    CHECK(s.lights[0].brightness == doctest::Approx(6));
    CHECK(s.lights[1].brightness == doctest::Approx(1.5));
    CHECK(s.ambient.x == doctest::Approx(0.1));
    CHECK(lightsOfStyle(s, 5).level.lightScale == 3);  // every layer sees the level settings
}

TEST_CASE("level settings: baked ambient occlusion darkens the floor next to a wall, not the open floor") {
    World w(floorAndWall({{"_ao_radius", "1"}, {"_ao_intensity", "1"}}));
    LiveBaker b(cpuOnly());
    b.setGeometry(w.layout, w.surfaces);
    b.setLights(w.lights);
    const auto nearWall = floorSamples(b, 0.25, 0.6), open = floorSamples(b, 2.5, 3.5);
    REQUIRE(!nearWall.empty());
    REQUIRE(!open.empty());
    for (size_t i : nearWall) CHECK(std::isnan(b.occlusion(i)));  // nothing refined yet
    const Vec3 cam{0, 2, 0};
    int guard = 0;
    while (!b.converged() && guard++ < 1000) b.step(&cam, 100000);
    for (size_t i : nearWall) {
        CHECK(b.occlusion(i) > 0.05f);
        CHECK(b.aoFactor(i) < 0.95f);
        const Vec3 v = b.value(i), shown = b.shownValue(i);
        CHECK(shown.x == doctest::Approx(v.x * b.aoFactor(i)).epsilon(1e-4));
    }
    for (size_t i : open) {
        CHECK(b.occlusion(i) == 0.f);
        CHECK(b.aoFactor(i) == 1.f);
    }
}

TEST_CASE("level settings: AO intensity and power only change the shown value; radius 0 switches it off") {
    World w(floorAndWall({{"_ao_radius", "1"}, {"_ao_intensity", "1"}}));
    LiveBaker b(cpuOnly());
    b.setGeometry(w.layout, w.surfaces);
    b.setLights(w.lights);
    const Vec3 cam{0, 2, 0};
    while (!b.converged()) b.step(&cam, 100000);
    const auto nearWall = floorSamples(b, 0.25, 0.6);
    REQUIRE(!nearWall.empty());
    const size_t i = nearWall[0];
    const float occ = b.occlusion(i), full = b.aoFactor(i);
    b.takeDirtyChunks();
    LightScene half = w.lights;
    half.level.ao.intensity = 0.5;
    b.setLights(half);
    CHECK(b.occlusion(i) == occ);  // not recomputed
    CHECK(b.aoFactor(i) == doctest::Approx(1 - 0.5 * (1 - full)));
    CHECK(b.converged());           // nothing to refine
    CHECK(b.hasDirtyChunks());      // but every tile is sent again
    LightScene sharp = half;
    sharp.level.ao.power = 3;
    b.setLights(sharp);
    CHECK(b.aoFactor(i) == doctest::Approx(1 - 0.5 * (1 - std::pow(1 - occ, 3.0))).epsilon(1e-5));
    LightScene off = sharp;
    off.level.ao.radius = 0;
    b.setLights(off);
    CHECK(b.aoFactor(i) == 1.f);
}

TEST_CASE("level settings: occlusion is kept across an unrelated edit and recomputed next to an edit") {
    auto es = floorAndWall({{"_ao_radius", "1"}});
    World w(es);
    LiveBaker b(cpuOnly());
    b.setGeometry(w.layout, w.surfaces);
    b.setLights(w.lights);
    const Vec3 cam{0, 2, 0};
    while (!b.converged()) b.step(&cam, 100000);
    // a crate far away (x = -3.5): the texels next to the wall keep their occlusion and the chunks there stay converged
    es[0].brushes.push_back(Brush::makeBox("crate", {-3.9, 0, 3.2}, {-3.2, 0.7, 3.9}));
    World w2(es);
    Aabb changed;
    changed.add({-3.9, 0, 3.2});
    changed.add({-3.2, 0.7, 3.9});
    b.setGeometry(w2.layout, w2.surfaces, {changed});
    for (size_t i : floorSamples(b, 0.25, 0.6)) if (std::abs(b.samples().pos[i * 4 + 2]) < 1.5) CHECK(!std::isnan(b.occlusion(i)));
    // texels next to the crate: unknown until refined, then occluded
    std::vector<size_t> byCrate;
    const auto& s = b.samples();
    for (size_t i = 0; i < s.count(); ++i)
        if (s.nrm[i * 4 + 1] > 0.99 && std::abs(s.pos[i * 4 + 1]) < 1e-3 && s.pos[i * 4] > -3.2 && s.pos[i * 4] < -2.9 && s.pos[i * 4 + 2] > 3.3 && s.pos[i * 4 + 2] < 3.8) byCrate.push_back(i);
    REQUIRE(!byCrate.empty());
    for (size_t i : byCrate) CHECK(std::isnan(b.occlusion(i)));
    while (!b.converged()) b.step(&cam, 100000);
    for (size_t i : byCrate) CHECK(b.occlusion(i) > 0.f);
}

TEST_CASE("level settings: setQuality changes the pass target; another super sampling restarts the direct light") {
    World w(floorAndWall({}));
    LiveBaker b(cpuOnly());
    b.setGeometry(w.layout, w.surfaces);
    b.setLights(w.lights);
    const Vec3 cam{0, 2, 0};
    while (!b.converged()) b.step(&cam, 100000);
    CHECK(!b.setQuality(2, b.settings().directSupersample));  // nothing changed
    CHECK(b.setQuality(5, b.settings().directSupersample));
    CHECK(!b.converged());
    CHECK(b.staleChunks() == 0);  // results kept, refining goes on
    while (!b.converged()) b.step(&cam, 100000);
    CHECK(b.minPasses() == 5);
    CHECK(b.setQuality(5, 1));
    CHECK(b.staleChunks() == b.chunkCount());
}

TEST_CASE("retexture detection (§22 P3): a change only in the newer UV fields (kind, basis, shear, stored uvs, repetition) counts as a change") {
    auto face = [](const char* extra) {
        std::string s = R"({"id":"b","vertices":[[0,0,0],[1,0,0],[1,1,0],[0,1,0],[0,0,1],[1,0,1],[1,1,1],[0,1,1]],"faces":[{"id":0,"loop":[0,3,2,1],"texture":"t","projection":{"scale":[2,2]})";
        s += extra;
        s += R"(}]})";
        return brushFromJson(nlohmann::json::parse(s)).faces[0];
    };
    const BrushFace plain = face(""), sameAgain = face("");
    CHECK(plain.lookHash != 0);
    CHECK(plain.lookHash == sameAgain.lookHash);
    const BrushFace withUvs = face(R"(,"uvs":[[0,0],[1,0],[1,1],[0,1]])");
    const BrushFace withRepeat = face(R"(,"repeat":{"u":"mirror"})");
    CHECK(withUvs.lookHash != plain.lookHash);
    CHECK(withRepeat.lookHash != plain.lookHash);
    // the projection numbers the Core reads are equal; only the Shell-side kind / shear differ
    auto kinded = [&](const char* proj) {
        std::string s = R"({"id":"b","vertices":[[0,0,0],[1,0,0],[1,1,0]],"faces":[{"id":0,"loop":[0,1,2],"projection":)";
        s += proj;
        s += "}]}";
        return brushFromJson(nlohmann::json::parse(s)).faces[0];
    };
    const BrushFace a = kinded(R"({"scale":[2,2]})"), b = kinded(R"({"scale":[2,2],"kind":"free","shear":0.1})");
    CHECK(a.projection.scaleU == b.projection.scaleU);
    CHECK(a.lookHash != b.lookHash);
    // and the light baker's change detection sees it
    World w(floorAndWall({}));
    auto changed = w.surfaces;
    changed[0].lookHash ^= 0x9e3779b97f4a7c15ull;
    CHECK(!diffSurfaces(w.surfaces, changed).empty());
    CHECK(diffSurfaces(changed, changed).empty());
}
