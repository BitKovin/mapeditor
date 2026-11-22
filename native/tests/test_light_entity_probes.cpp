// T10.1: the light volume of the entities the game lights from it (light/entity_probes). CPU only: the Core's probe evaluator on the lattice of the level, for demo_lit's
// door and button (brush entities of classes without lightmaps) and for a model placed in its room.
#include <doctest.h>

#include "scene/snapshot.h"   // first: core headers use `near` as a name

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>

#include "base/jobs.h"
#include "doc/changeset.h"
#include "doc/io.h"
#include "doc/prefabs.h"
#include "light/entity_probes.h"
#include "scene/builder.h"

#include <core/profile.h>
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
