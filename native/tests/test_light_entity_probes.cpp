// T10.1: the light volume of the entities the game lights from it (light/entity_probes). CPU only: the Core's probe evaluator on the lattice of the level, for demo_lit's
// door and button (brush entities of classes without lightmaps) and for a model placed in its room.
#include <doctest.h>

#include "scene/snapshot.h"   // first: core headers use `near` as a name

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include "base/jobs.h"
#include "doc/changeset.h"
#include "doc/io.h"
#include "doc/prefabs.h"
#include "light/entity_probes.h"
#include "scene/builder.h"

#include <core/albedo.h>
#include <core/lightbake.h>
#include <core/lightvol.h>
#include <core/live_bake.h>
#include <core/profile.h>
#include <core/scene.h>
#include <core/texplan.h>

using namespace native;
namespace fs = std::filesystem;

namespace {

struct Level {
    fs::path root = NATIVE_CORE_ASSETS_DIR;
    base::JobPool pool{4};
    std::shared_ptr<const scene::SceneSnapshot> snapshot;
    std::shared_ptr<const core::GameProfile> profile;
    bool load(const std::string& map) {
        if (!fs::exists(root / map)) return false;
        const doc::LoadResult loaded = doc::loadDocumentFile(root / map, map);
        if (!loaded.version) return false;
        profile = std::make_shared<core::GameProfile>(core::loadProfile(root));
        auto texEnv = std::make_shared<core::texplan::TexEnvironment>(root, *profile);
        scene::ReliefEnv env;
        env.info = [texEnv](const std::string& p) { return texEnv->info(p); };
        env.groups = [texEnv](const std::string& n) { return texEnv->group(n); };
        env.revision = 1;
        scene::SceneBuilder builder(pool);
        builder.setReliefEnv(env);
        doc::PrefabLibrary prefabLib(root);
        doc::PrefabResolver resolver(prefabLib);
        builder.submit(loaded.version, doc::allChanged(0, loaded.version->version), resolver.expand(*loaded.version));
        if (!builder.waitIdle(180000)) return false;
        snapshot = builder.snapshot();
        return snapshot != nullptr;
    }
    light::EntityProbeInputs inputs() const {
        light::EntityProbeInputs in;
        in.snapshot = snapshot;
        in.profile = profile;
        in.assetsRoot = root;
        return in;
    }
};

const light::BrushEntityLight* find(const light::EntityProbes& p, const std::string& id) {
    for (const light::BrushEntityLight& e : p.brushEntities)
        if (e.key.entityId == id) return &e;
    return nullptr;
}

float luma(const float v[3]) { return 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2]; }

} // namespace

TEST_CASE("entity probes: the door and the button of demo_lit and a model in its room get a light from the volume") {
    Level lv;
    if (!lv.load("maps/demo_lit.map.json")) { MESSAGE("demo_lit not available: skipped"); return; }
    light::EntityProbeInputs in = lv.inputs();
    in.modelCentres = {{0.0, 1.0, 0.0}, {3.0, 1.0, 2.0}};   // two models in the room
    const light::EntityProbes p = light::bakeEntityProbes(in);
    REQUIRE(p.error.empty());
    CHECK(p.probes > 0);
    CHECK(p.seconds < 60.0);
    REQUIRE(p.models.size() == 2);

    // brush entities of classes without lightmaps (the profile decides which): every one found has a centre and a valid light, and the door is among them
    const core::LightingClasses classes = core::LightingClasses::fromProfile(*lv.profile);
    const light::BrushEntityLight* door = find(p, "e_902c5c0f");
    MESSAGE("entities lit from the volume: " << p.brushEntities.size() << ", probes " << p.probes << ", seconds " << p.seconds);
    if (!classes.isLightmapped("func_door")) {
        REQUIRE(door != nullptr);
        CHECK(door->light.valid);
        CHECK(luma(door->light.ambient) + luma(door->light.directed) > 0.0f);
        const float len = std::sqrt(door->light.dir[0] * door->light.dir[0] + door->light.dir[1] * door->light.dir[1] + door->light.dir[2] * door->light.dir[2]);
        CHECK(len == doctest::Approx(1.0f).epsilon(1e-3));
    }
    for (const light::BrushEntityLight& e : p.brushEntities) {
        CHECK(e.key.brushId.empty());
        CHECK(e.light.valid);
    }

    // the model in the room is lit; the lit side is brighter than the opposite side (ambient + directed * max(0, n . dir))
    const light::EntityLightValue& m = p.models[0];
    REQUIRE(m.valid);
    float towards[3] = {m.dir[0], m.dir[1], m.dir[2]}, away[3] = {-m.dir[0], -m.dir[1], -m.dir[2]};
    float a[3], b[3];
    light::entityLightAt(m, towards, a);
    light::entityLightAt(m, away, b);
    CHECK(luma(a) >= luma(b));
    CHECK(luma(b) == doctest::Approx(luma(m.ambient)).epsilon(1e-3));
    CHECK(p.models[1].valid);

    // deterministic: the same inputs give the same numbers
    const light::EntityProbes q = light::bakeEntityProbes(in);
    REQUIRE(q.models.size() == p.models.size());
    for (int c = 0; c < 3; ++c) {
        CHECK(q.models[0].ambient[c] == doctest::Approx(p.models[0].ambient[c]));
        CHECK(q.models[0].directed[c] == doctest::Approx(p.models[0].directed[c]));
    }
}

TEST_CASE("entity probes: muting every light leaves only the ambient (the light filter reaches the volume)") {
    Level lv;
    if (!lv.load("maps/demo_lit.map.json")) { MESSAGE("demo_lit not available: skipped"); return; }
    light::EntityProbeInputs in = lv.inputs();
    in.modelCentres = {{0.0, 1.0, 0.0}};
    const light::EntityProbes lit = light::bakeEntityProbes(in);
    REQUIRE(lit.error.empty());
    REQUIRE(lit.models.size() == 1);
    REQUIRE(lit.models[0].valid);
    auto filter = std::make_shared<light::LightFilter>();
    filter->solo = "no such light";   // only this light gives light: none does
    in.filter = filter;
    const light::EntityProbes dark = light::bakeEntityProbes(in);
    REQUIRE(dark.models.size() == 1);
    CHECK(luma(dark.models[0].directed) <= luma(lit.models[0].directed) + 1e-4f);
    CHECK(luma(dark.models[0].directed) < 0.01f);
    CHECK(luma(lit.models[0].directed) + luma(lit.models[0].ambient) > luma(dark.models[0].directed) + luma(dark.models[0].ambient));
}

// T10.2: the bounce light of the probes. The reference is the Core's own light volume of the same level: probes baked with core::bakeProbesCpu from the bounce source of a
// core::LiveBaker (8+ bounces of the textured albedo 0.7), i.e. what the compiled map's light grid holds. The entity probes gather from surfels instead of the atlas; they
// must come close to it, and be clearly closer than the direct-only probes of T10.1.
TEST_CASE("entity probes: the bounce light comes close to the Core's light volume (demo_lit)") {
    Level lv;
    if (!lv.load("maps/demo_lit.map.json")) { MESSAGE("demo_lit not available: skipped"); return; }
    const std::vector<std::array<double, 3>> points = {{0.0, 1.0, 0.0}, {3.0, 1.0, 2.0}, {-2.0, 1.5, -2.0}};
    light::EntityProbeInputs in = lv.inputs();
    in.modelCentres = points;
    in.bounces = false;
    const light::EntityProbes off = light::bakeEntityProbes(in);
    in.bounces = true;
    const light::EntityProbes on = light::bakeEntityProbes(in);
    REQUIRE(off.error.empty());
    REQUIRE(on.error.empty());
    MESSAGE("entity probes: direct only " << off.seconds << " s; with bounces " << on.seconds << " s, " << on.probes << " probes, " << on.surfels << " surfels, " << on.rays << " rays");

    // the Core's side
    core::Scene cscene{lv.root};
    cscene.loadGameProfile();
    cscene.openDocument("maps/demo_lit.map.json");
    const auto surfaces = cscene.lightSurfaces();
    core::LightScene all = cscene.lightScene();
    core::LightScene lights = core::lightsOfStyle(all, 0);
    lights.playable = all.playable;
    for (core::Light& l : lights.lights) {
        if (l.sourceSize > 0) continue;
        if (l.type == core::LightType::Point || l.type == core::LightType::Spot) l.sourceSize = 0.25;
        else if (l.type == core::LightType::Directional) l.sourceSize = 0.5;
    }
    core::LightmapSettings lcfg;
    lcfg.playable = lights.playable;
    lcfg.lod = lights.level.lod;
    lcfg.texelsPerMeter = 16.0;
    const core::LightmapLayout layout = core::buildLightmapLayout(surfaces, cscene.lightingClasses(), lcfg);
    auto albedo = std::make_shared<core::AlbedoLibrary>(cscene.assetsRoot(), cscene.profile());
    core::LiveSettings ls;
    ls.bounces = true;
    ls.albedo = 0.7;
    ls.useGpu = false;
    ls.denoiseIndirect = false;
    ls.useTextures = true;
    ls.giScale = 1;
    ls.directSupersample = 1;
    ls.maxPasses = 4;   // the probes average many rays of the whole atlas: a few passes of the reference are enough (24 passes take 90 s on the CPU)
    ls.giSamplesPerPass = 32;
    ls.classes = cscene.lightingClasses();
    ls.albedoLoader = [albedo](const std::string& n) { return albedo->get(n); };
    const auto t0 = std::chrono::steady_clock::now();
    core::LiveBaker ref(ls);
    ref.setGeometry(layout, surfaces, {}, cscene.lightSolids());
    ref.setLights(lights);
    const core::Vec3 cam{0, 0, 0};
    for (int guard = 0; !ref.converged() && guard < 100000; ++guard) ref.step(&cam, 200000);
    REQUIRE(ref.converged());
    MESSAGE("Core LiveBaker reference: " << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() << " s, " << layout.pageCount << " pages");
    core::LightvolSettings vs;
    if (lights.level.lightvolGrid > 0) vs.gridSize = lights.level.lightvolGrid;
    vs.useGpu = false;
    vs.classes = cscene.lightingClasses();
    const auto solids = cscene.lightSolids();
    core::LightvolGrid grid = core::buildLightvolGrid(surfaces, solids.get(), vs);
    const std::vector<char> solid = core::solidProbes(grid, solids.get());
    core::LightvolSource src;
    src.lights = &ref.lights();
    src.mesh = &ref.occluders();
    src.bvh = ref.bvh();
    src.atlasS = &ref.bounceSource();
    src.atlasSize = layout.settings.atlasSize;
    src.pages = layout.pageCount;
    std::vector<char> baked;
    core::bakeProbesCpu(grid, solid, baked, 0, grid.count(), src, vs);
    MESSAGE("Core light volume: " << grid.count() << " probes");

    // mean light over the sphere of a probe: ambient + directed / 4, as luminance
    const auto mean = [](const float a[3], const float d[3]) { return 0.2126 * (a[0] + d[0] / 4) + 0.7152 * (a[1] + d[1] / 4) + 0.0722 * (a[2] + d[2] / 4); };
    double errOn = 0, errOff = 0;
    int counted = 0;
    for (size_t i = 0; i < points.size(); ++i) {
        const core::Probe r = grid.sample({points[i][0], points[i][1], points[i][2]});
        if (!r.valid || !on.models[i].valid) continue;
        const float ra[3] = {float(r.ambient.x), float(r.ambient.y), float(r.ambient.z)}, rd[3] = {float(r.directed.x), float(r.directed.y), float(r.directed.z)};
        const double mr = mean(ra, rd), mo = mean(on.models[i].ambient, on.models[i].directed), mf = mean(off.models[i].ambient, off.models[i].directed);
        MESSAGE("point " << i << ": Core " << mr << ", entity probes with bounces " << mo << ", direct only " << mf);
        if (mr > 1e-3) {
            errOn += std::fabs(mo - mr) / mr;
            errOff += std::fabs(mf - mr) / mr;
            ++counted;
        }
        CHECK(mo >= mf - 1e-4);   // bounce light only adds
    }
    REQUIRE(counted > 0);
    errOn /= counted;
    errOff /= counted;
    MESSAGE("mean relative error: with bounces " << errOn * 100 << " %, direct only " << errOff * 100 << " %");
    CHECK(errOn < 0.15);
    CHECK(errOn < errOff);
}

// How long the bake takes on a big level (the bake runs off the UI thread, but a re-bake follows every edit). Only with NATIVE_ENTITY_MAP set (e.g. maps/lvl2.map.json; NATIVE_ENTITY_ROOT = the game folder, default the Core's assets).
TEST_CASE("entity probes: time on a big level (NATIVE_ENTITY_MAP)") {
    char* mapEnv = nullptr;
    size_t mapLen = 0;
    _dupenv_s(&mapEnv, &mapLen, "NATIVE_ENTITY_MAP");
    const std::string map = mapEnv ? mapEnv : "";
    std::free(mapEnv);
    if (map.empty()) { MESSAGE("NATIVE_ENTITY_MAP not set: skipped"); return; }
    Level lv;
    char* rootEnv = nullptr;
    size_t rootLen = 0;
    _dupenv_s(&rootEnv, &rootLen, "NATIVE_ENTITY_ROOT");
    if (rootEnv && *rootEnv) lv.root = rootEnv;
    std::free(rootEnv);
    REQUIRE(lv.load(map));
    light::EntityProbeInputs in = lv.inputs();
    const light::EntityProbes direct = [&] { light::EntityProbeInputs d = in; d.bounces = false; return light::bakeEntityProbes(d); }();
    const light::EntityProbes first = light::bakeEntityProbes(in);
    const light::EntityProbes second = light::bakeEntityProbes(in);
    MESSAGE(map << ": " << first.brushEntities.size() << " brush entities; direct only " << direct.seconds << " s; with bounces " << first.seconds << " s (" << first.probes << " probes, "
                << first.surfels << " surfels, " << first.rays << " rays, " << first.castSeconds << " s casting, " << first.directSeconds << " s direct light), again " << second.seconds << " s");
    size_t valid = 0;
    for (const light::BrushEntityLight& e : first.brushEntities) valid += e.light.valid ? 1 : 0;
    MESSAGE(valid << " of them have a valid light");
    CHECK(first.error.empty());
    CHECK(first.seconds < 30.0);
}
