// R1: the ray tracing scene of the render thread. The scene builder's snapshot goes to the render thread, the RT scene makes one BLAS per drawn solid object from the
// GpuScene's arenas and a TLAS over them; 100 000 random rays against it give the closest hits core::TriangleBvh gives for the same triangles (what is drawn is
// what is traced), a one-object edit rebuilds one BLAS and one TLAS, hiding an object removes it, and after the idle time the BLASes are rebuilt for tracing and
// compacted without changing a hit. [gpu]: skips itself without a ray tracing device.
#include <doctest.h>

#include "scene/snapshot.h"   // first: core headers use `near` as a name, which windows.h (via volk) defines as a macro

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <unordered_set>

#include "base/jobs.h"
#include "doc/changeset.h"
#include "doc/io.h"
#include "doc/prefabs.h"
#include "gpu/device.h"
#include "ray_test_support.h"
#include "render/render_thread.h"
#include "scene/builder.h"
#include "scene/render_mesh.h"

#include <core/profile.h>
#include <core/texplan.h>

using namespace native;
using namespace raytest;
namespace fs = std::filesystem;

namespace {

std::shared_ptr<const scene::SceneSnapshot> buildSnapshot(const fs::path& root, const std::string& map, base::JobPool& pool) {
    const doc::LoadResult loaded = doc::loadDocumentFile(root / map, map);
    if (!loaded.version) return nullptr;
    auto profile = std::make_shared<core::GameProfile>(core::loadProfile(root));
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
    if (!builder.waitIdle(180000)) return nullptr;
    return builder.snapshot();
}

const scene::RenderMesh* drawnMesh(const scene::ObjectProducts& p) {
    const scene::RenderMesh* m = p.render ? p.render.get() : p.raw.get();
    if (!m || m->empty() || m->kind == scene::RenderMesh::Kind::Volume) return nullptr;
    return m;
}

struct Reference {
    core::OccluderMesh mesh;
    uint32_t objects = 0;
};

using HiddenSet = std::unordered_set<scene::ObjectKey, scene::ObjectKey::Hash>;

// The triangles the render thread draws as solids: every object with a drawn mesh that is not a volume and not hidden.
Reference referenceOf(const scene::SceneSnapshot& snap, const HiddenSet& hidden = {}) {
    Reference r;
    for (const auto& [key, p] : snap.objects) {
        const scene::RenderMesh* m = drawnMesh(p);
        if (!m || hidden.count(key) || m->indices.size() < 3) continue;
        ++r.objects;
        for (size_t i = 0; i + 2 < m->indices.size(); i += 3)
            for (size_t c = 0; c < 3; ++c) {
                const float* v = m->vertices[m->indices[i + c]].pos;
                r.mesh.tris.insert(r.mesh.tris.end(), {v[0], v[1], v[2]});
            }
    }
    return r;
}

bool waitFor(const std::function<bool()>& cond, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

// Waits until the RT scene holds `objects` instances, nothing waits for a BLAS and the TLAS is at least of that generation.
bool settled(render::RenderThread& rt, uint32_t objects, uint64_t minGeneration) {
    const auto ready = [&] {
        const auto i = rt.rtInfo();
        return i.valid && i.stats.objects == objects && !i.stats.backlog && i.generation >= minGeneration && rt.sceneInfo().scene.queued == 0 &&
               rt.sceneInfo().texturesPending == 0;
    };
    // A texture that arrives makes an object alpha (cutout / translucent): its instance flag changes and the TLAS is built again a few frames later, so the generation has to
    // stay the same for a while.
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(120000);
    while (std::chrono::steady_clock::now() < end) {
        if (ready()) {
            const uint64_t gen = rt.rtInfo().generation;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (ready() && rt.rtInfo().generation == gen) return true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    return false;
}

// `tolerance`: the float noise of a hit distance grows with the distance (lvl2 is about 500 m across).
Compare trace(render::RenderThread& rt, RayCaster& caster, const core::OccluderMesh& ref, size_t rays, uint32_t seed, double tolerance) {
    const core::TriangleBvh bvh(ref);
    const std::vector<float> r = randomRays(ref, rays, seed);
    return compare(bvh, r, caster.cast(rt.rtInfo().tlas, r), tolerance);
}

void runScene(const fs::path& root, const std::string& map, size_t rays, uint32_t seed, double tolerance) {
    gpu::DeviceOptions o;
    o.validation = true;
    std::string err;
    auto device = gpu::Device::create(o, &err);
    if (!device || !device->caps().lit() || device->computeQueue() == device->graphicsQueue()) {
        MESSAGE("no ray tracing device with a second queue (" << err << "): GPU test skipped");
        return;
    }
    base::JobPool pool(8);
    const auto snapshot = buildSnapshot(root, map, pool);
    REQUIRE(snapshot);
    const Reference ref = referenceOf(*snapshot);
    REQUIRE(ref.objects > 3);
    {
        render::RenderThread rt(*device);
        rt.setTextureSource(root, &pool);
        rt.setSnapshot(snapshot);
        render::RtOptions ro;
        ro.idleSeconds = 3600;   // the idle upgrade has its own test below
        ro.blasBytes = 256ull << 20;
        rt.setRayTracing(true, ro);
        REQUIRE(settled(rt, ref.objects, 1));
        auto info = rt.rtInfo();
        CHECK(info.stats.triangles == ref.mesh.triangleCount());
        MESSAGE(map << ": " << info.stats.objects << " objects, " << info.stats.triangles << " triangles, blas " << info.stats.blasBytes / 1048576 << " MB, tlas builds "
                    << info.stats.tlasBuilds);

        RayCaster caster(*device);   // the compute queue: the render thread owns the graphics queue
        const Compare c = trace(rt, caster, ref.mesh, rays, seed, tolerance);
        MESSAGE(rays << " rays: " << c.hits << " hits, hit/miss mismatches " << c.hitMismatch << ", worst distance difference " << c.worst);
        CHECK(c.hits > 1000);
        CHECK(c.hitMismatch == 0);
        CHECK(c.distanceMismatch == 0);

        // nothing changed: nothing is rebuilt
        const uint64_t gen = info.generation;
        const uint64_t builds = info.stats.blasBuilds;
        rt.markAllDirty();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(rt.rtInfo().generation == gen);
        CHECK(rt.rtInfo().stats.blasBuilds == builds);

        // one object moves 0.75 m: one BLAS and the TLAS, in the frame the snapshot arrives
        scene::ObjectKey victim;
        size_t most = 0;
        for (const auto& [key, p] : snapshot->objects) {
            const scene::RenderMesh* m = drawnMesh(p);
            if (m && m->indices.size() > most) {
                most = m->indices.size();
                victim = key;
            }
        }
        REQUIRE(most > 0);
        auto edited = std::make_shared<scene::SceneSnapshot>(*snapshot);
        auto moved = std::make_shared<scene::RenderMesh>(*drawnMesh(snapshot->objects.at(victim)));
        for (auto& v : moved->vertices) v.pos[0] += 0.75f;
        edited->objects[victim].render = moved;
        rt.setSnapshot(edited);
        REQUIRE(waitFor([&] { return rt.rtInfo().generation > gen; }, 20000));
        REQUIRE(settled(rt, ref.objects, gen + 1));
        info = rt.rtInfo();
        CHECK(info.generation == gen + 1);
        CHECK(info.stats.blasBuilds == builds + 1);
        CHECK(info.stats.lastBuilt == 1);
        CHECK(info.stats.lastRemoved == 1);
        const Reference ref2 = referenceOf(*edited);
        const Compare c2 = trace(rt, caster, ref2.mesh, rays / 3, seed + 1, tolerance);
        CHECK(c2.hitMismatch == 0);
        CHECK(c2.distanceMismatch == 0);

        // Hide: the object leaves the TLAS
        auto hidden = std::make_shared<HiddenSet>();
        hidden->insert(victim);
        const uint64_t gen2 = rt.rtInfo().generation;
        rt.setHiddenObjects(hidden);
        REQUIRE(settled(rt, ref.objects - 1, gen2 + 1));
        const Reference ref3 = referenceOf(*edited, *hidden);
        CHECK(rt.rtInfo().stats.triangles == ref3.mesh.triangleCount());
        const Compare c3 = trace(rt, caster, ref3.mesh, rays / 3, seed + 2, tolerance);
        CHECK(c3.hitMismatch == 0);
        CHECK(c3.distanceMismatch == 0);

        rt.setRayTracing(false);
        CHECK(waitFor([&] { return !rt.rtInfo().valid; }, 5000));
    }
    CHECK(device->validationErrors() == 0);
}

} // namespace

TEST_CASE("RT scene: 100 000 rays equal core::TriangleBvh on demo_lit, an edit rebuilds one BLAS, Hide removes an object" * doctest::test_suite("gpu")) {
    const fs::path root = NATIVE_CORE_ASSETS_DIR;
    if (!fs::exists(root / "maps/demo_lit.map.json")) return;
    runScene(root, "maps/demo_lit.map.json", 100000, 31, 1e-4);
}

TEST_CASE("RT scene: the same on lvl2" * doctest::test_suite("gpu")) {
    const fs::path game = "F:/Git/SdlWasmEngine/source/Games/ShooterGame/GameData";
    if (!fs::exists(game / "maps/lvl2.map.json")) return;
    runScene(game, "maps/lvl2.map.json", 100000, 41, 3e-4);
}

TEST_CASE("RT scene: after the idle time every BLAS is rebuilt for tracing and compacted, the hits stay the same" * doctest::test_suite("gpu")) {
    const fs::path root = NATIVE_CORE_ASSETS_DIR;
    if (!fs::exists(root / "maps/demo_lit.map.json")) return;
    gpu::DeviceOptions o;
    o.validation = true;
    std::string err;
    auto device = gpu::Device::create(o, &err);
    if (!device || !device->caps().lit() || device->computeQueue() == device->graphicsQueue()) {
        MESSAGE("no ray tracing device with a second queue (" << err << "): GPU test skipped");
        return;
    }
    base::JobPool pool(8);
    const auto snapshot = buildSnapshot(root, "maps/demo_lit.map.json", pool);
    REQUIRE(snapshot);
    const Reference ref = referenceOf(*snapshot);
    {
        render::RenderThread rt(*device);
        rt.setTextureSource(root, &pool);
        rt.setSnapshot(snapshot);
        render::RtOptions ro;
        ro.idleSeconds = 0.4;
        rt.setRayTracing(true, ro);
        REQUIRE(settled(rt, ref.objects, 1));
        REQUIRE(waitFor(
            [&] {
                const auto i = rt.rtInfo();
                return i.stats.compacted == ref.objects && i.stats.fastObjects == 0;
            },
            20000));
        const auto info = rt.rtInfo();
        MESSAGE("compacted " << info.stats.compacted << " BLASes, storage in use " << info.stats.blasBytes << " B, tlas builds " << info.stats.tlasBuilds);
        CHECK(info.stats.compacted == ref.objects);
        RayCaster caster(*device, false, true);   // the HLSL kernel: the DXC step of the shader build
        const Compare c = trace(rt, caster, ref.mesh, 100000, 51, 1e-4);
        CHECK(c.hits > 1000);
        CHECK(c.hitMismatch == 0);
        CHECK(c.distanceMismatch == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(800));   // no further changes after the upgrade
        CHECK(rt.rtInfo().generation == info.generation);
    }
    CHECK(device->validationErrors() == 0);
}
