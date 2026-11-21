// T6.3: acceleration structures per object. [gpu]: skips itself without a ray tracing device.
// Closest-hit results of 100 000 random rays against the native TLAS equal core::TriangleBvh::closestHit on demo_lit (hit / miss identical, distance within 1e-4 m),
// and an object edit rebuilds exactly that object's BLAS.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "core/lighting_classes.h"
#include "core/raytrace.h"
#include "core/scene.h"
#include "gpu/device.h"
#include "light/accel.h"
#include "light/layout.h"
#include "ray_test_support.h"

using namespace native;
using namespace raytest;
namespace fs = std::filesystem;

namespace {

core::OccluderMesh meshOf(const std::vector<light::AccelObject>& objects) {
    core::OccluderMesh m;
    for (const auto& o : objects) m.tris.insert(m.tris.end(), o.tris.begin(), o.tris.end());
    return m;
}

} // namespace

TEST_CASE("accel: closest hits of 100 000 random rays equal core::TriangleBvh on demo_lit, an edit rebuilds one BLAS" * doctest::test_suite("gpu")) {
    gpu::DeviceOptions o;
    o.validation = true;
    std::string err;
    auto device = gpu::Device::create(o, &err);
    if (!device || !device->caps().lit()) {
        MESSAGE("no ray tracing device (" << err << "): GPU test skipped");
        return;
    }
    const fs::path root = NATIVE_CORE_ASSETS_DIR;
    if (!fs::exists(root / "maps/demo_lit.map.json")) return;
    core::Scene scene(root);
    scene.loadGameProfile();
    scene.openDocument("maps/demo_lit.map.json");
    const std::vector<core::LightSurface> surfaces = scene.lightSurfaces();
    const core::LightingClasses classes = scene.lightingClasses();
    light::LiveLayout live(scene.lightmapLayout().settings);
    live.buildAll(surfaces, classes);

    std::vector<light::AccelObject> objects = light::buildAccelObjects(surfaces, classes, &live.layout());
    const core::OccluderMesh reference = core::buildOccluders(surfaces, classes, &live.layout());
    REQUIRE(objects.size() > 3);
    size_t triangles = 0, attrFloats = 0;
    for (const auto& ob : objects) {
        triangles += ob.triangleCount();
        attrFloats += ob.lightmap.size();
    }
    CHECK(triangles == reference.triangleCount());
    CHECK(attrFloats == reference.lightmap.size());   // lightmap attributes per triangle, as the Core's occluders

    light::Accel accel(*device);
    REQUIRE(accel.valid());
    REQUIRE(accel.update(objects));
    REQUIRE(accel.tlas() != VK_NULL_HANDLE);
    CHECK(accel.stats().objects == objects.size());
    CHECK(accel.stats().triangles == triangles);
    CHECK(accel.stats().rebuilt == objects.size());
    MESSAGE("demo_lit: " << objects.size() << " objects, " << triangles << " triangles, blas " << accel.stats().blasBytes << " B, tlas " << accel.stats().tlasBytes
                         << " B, built in " << accel.stats().seconds << " s");

    RayCaster caster(*device);
    const core::TriangleBvh bvh(reference);
    const std::vector<float> rays = randomRays(reference, 100000, 11);
    const std::vector<RayHit> hits = caster.cast(accel.tlas(), rays);
    const Compare c = compare(bvh, rays, hits);
    MESSAGE("100000 rays: " << c.hits << " hits, hit/miss mismatches " << c.hitMismatch << ", worst distance difference " << c.worst);
    CHECK(c.hits > 10000);
    CHECK(c.hitMismatch == 0);
    CHECK(c.distanceMismatch == 0);
    for (const RayHit& h : hits)
        if (h.t >= 0) { REQUIRE(h.object < accel.stats().objects + 64); }

    // nothing changed: nothing is rebuilt, the TLAS stays
    const uint64_t gen = accel.generation();
    REQUIRE(accel.update(objects));
    CHECK(accel.generation() == gen);
    CHECK(accel.stats().rebuilt == 0);

    // move one object by 0.75 m: one BLAS is rebuilt, the rays see the new geometry
    size_t victim = 0;
    for (size_t i = 0; i < objects.size(); ++i)
        if (objects[i].triangleCount() > objects[victim].triangleCount()) victim = i;
    for (size_t i = 0; i < objects[victim].tris.size(); i += 3) objects[victim].tris[i] += 0.75f;
    objects[victim].hash += 1;
    REQUIRE(accel.update(objects));
    CHECK(accel.generation() == gen + 1);
    CHECK(accel.stats().rebuilt == 1);
    CHECK(accel.stats().removed == 0);
    const core::OccluderMesh moved = meshOf(objects);
    const core::TriangleBvh movedBvh(moved);
    const std::vector<float> rays2 = randomRays(moved, 30000, 5);
    const Compare c2 = compare(movedBvh, rays2, caster.cast(accel.tlas(), rays2));
    CHECK(c2.hitMismatch == 0);
    CHECK(c2.distanceMismatch == 0);

    // remove an object: it is gone from the TLAS, its slot is reused by the next new object
    const std::string gone = objects.back().key;
    objects.pop_back();
    REQUIRE(accel.update(objects));
    CHECK(accel.stats().removed == 1);
    CHECK(accel.slotOf(gone) == ~0u);
    CHECK(accel.stats().objects == objects.size());
    const core::OccluderMesh fewer = meshOf(objects);
    const core::TriangleBvh fewerBvh(fewer);
    const std::vector<float> rays3 = randomRays(fewer, 30000, 9);
    const Compare c3 = compare(fewerBvh, rays3, caster.cast(accel.tlas(), rays3));
    CHECK(c3.hitMismatch == 0);
    CHECK(c3.distanceMismatch == 0);
    CHECK(device->validationErrors() == 0);
}

TEST_CASE("accel: lvl2 (about two thousand brushes, one BLAS each) traces the same closest hits as core::TriangleBvh" * doctest::test_suite("gpu")) {
    gpu::DeviceOptions o;
    o.validation = true;
    std::string err;
    auto device = gpu::Device::create(o, &err);
    if (!device || !device->caps().lit()) {
        MESSAGE("no ray tracing device (" << err << "): GPU test skipped");
        return;
    }
    const fs::path game = "F:/Git/SdlWasmEngine/source/Games/ShooterGame/GameData";
    if (!fs::exists(game / "maps/lvl2.map.json")) return;
    core::Scene scene(game);
    scene.loadGameProfile();
    scene.openDocument("maps/lvl2.map.json");
    const std::vector<core::LightSurface> surfaces = scene.lightSurfaces();
    const core::LightingClasses classes = scene.lightingClasses();
    const std::vector<light::AccelObject> objects = light::buildAccelObjects(surfaces, classes, nullptr);
    const core::OccluderMesh reference = core::buildOccluders(surfaces, classes, nullptr);
    size_t triangles = 0;
    for (const auto& ob : objects) triangles += ob.triangleCount();
    REQUIRE(triangles == reference.triangleCount());

    light::AccelOptions opt;
    opt.vertexBytes = 256ull << 20;
    opt.blasBytes = 512ull << 20;
    light::Accel accel(*device, opt);
    REQUIRE(accel.valid());
    REQUIRE(accel.update(objects));
    MESSAGE("lvl2: " << objects.size() << " objects, " << triangles << " triangles, blas " << accel.stats().blasBytes / 1048576 << " MB, tlas " << accel.stats().tlasBytes
                     << " B, built in " << accel.stats().seconds << " s");
    RayCaster caster(*device);
    const core::TriangleBvh bvh(reference);
    const std::vector<float> rays = randomRays(reference, 100000, 21);
    const Compare c = compare(bvh, rays, caster.cast(accel.tlas(), rays));
    MESSAGE("100000 rays: " << c.hits << " hits, hit/miss mismatches " << c.hitMismatch << ", worst distance difference " << c.worst);
    CHECK(c.hits > 1000);
    CHECK(c.hitMismatch == 0);
    CHECK(c.distanceMismatch == 0);
    CHECK(device->validationErrors() == 0);
}
