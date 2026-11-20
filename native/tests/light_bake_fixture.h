// A loaded map with the native lighting inputs built the way the editor will: live layout, per-chart samples uploaded to a SampleStore, one BLAS per object,
// result buffers, and helpers to run the passes and read the results back. Used by the T6.4 tests (direct / AO parity, compose, Lit view).
#pragma once

#include <doctest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/lightbake.h"
#include "core/lighting_classes.h"
#include "core/lights.h"
#include "core/live_bake.h"
#include "core/scene.h"
#include "gpu/device.h"
#include "gpu/upload_ring.h"
#include "gpu_test_util.h"
#include "../../core/tests/test_util.h"
#include "light/accel.h"
#include "light/layout.h"
#include "light/light_pack.h"
#include "light/passes.h"
#include "light/results.h"
#include "light/samples.h"

namespace native_test {

struct LightFixture {
    native::gpu::Device& device;
    std::unique_ptr<core::Scene> scene;
    std::vector<core::LightSurface> surfaces;
    std::shared_ptr<core::SolidSet> solids;
    core::LightingClasses classes;
    core::LightScene lights;   // the style 0 layer
    std::unique_ptr<native::light::LiveLayout> live;
    std::vector<native::light::PreparedChart> prepared;
    std::unique_ptr<native::light::SampleStore> store;
    std::unique_ptr<native::light::Accel> accel;
    std::unique_ptr<native::light::Results> results;
    std::unique_ptr<native::gpu::UploadRing> ring;
    std::unique_ptr<native::light::Passes> passes;
    std::unique_ptr<native::light::LightBuffer> lightBuffer;
    OneShot shot;
    std::unique_ptr<testutil::TempDir> tmp;   // a generated map's root
    uint64_t frame = 0;
    size_t sampleCount = 0;

    explicit LightFixture(native::gpu::Device& d) : device(d), shot(d, true) {}

    // rooms x rooms grid of 12 m rooms (floor, ceiling in every other one, walls with a door gap, a pillar, a crate), a point light in each, a sun over all:
    // the scene of core/bench/bench_bake --rooms N.
    bool loadRooms(const std::filesystem::path& assets, int rooms, const std::function<void(core::LightScene&)>& edit = {}, int giScale = 1) {
        using testutil::entityJson;
        std::vector<core::Brush> world;
        std::vector<nlohmann::json> ents;
        const double R = 12, H = 3.5, T = 0.3;
        for (int gx = 0; gx < rooms; ++gx)
            for (int gz = 0; gz < rooms; ++gz) {
                const std::string id = std::to_string(gx) + "_" + std::to_string(gz);
                const double x0 = gx * R, z0 = gz * R;
                world.push_back(core::Brush::makeBox("fl" + id, {x0, -T, z0}, {x0 + R, 0, z0 + R}));
                if ((gx + gz) % 2) world.push_back(core::Brush::makeBox("ce" + id, {x0, H, z0}, {x0 + R, H + T, z0 + R}));
                world.push_back(core::Brush::makeBox("wn" + id, {x0, 0, z0}, {x0 + R, H, z0 + T}));
                world.push_back(core::Brush::makeBox("ws" + id, {x0, 0, z0 + R - T}, {x0 + R, H, z0 + R}));
                world.push_back(core::Brush::makeBox("ww" + id, {x0, 0, z0 + T}, {x0 + T, H, z0 + R - T}));
                world.push_back(core::Brush::makeBox("we1" + id, {x0 + R - T, 0, z0 + T}, {x0 + R, H, z0 + 4}));
                world.push_back(core::Brush::makeBox("we2" + id, {x0 + R - T, 0, z0 + 8}, {x0 + R, H, z0 + R - T}));
                world.push_back(core::Brush::makeBox("pi" + id, {x0 + 8, 0, z0 + 8}, {x0 + 9, H, z0 + 9}));
                world.push_back(core::Brush::makeBox("cr" + id, {x0 + 3, 0, z0 + 3}, {x0 + 4.5, 1.5, z0 + 4.5}));
                nlohmann::json l = entityJson("l" + id, "light_point", {}, {x0 + 6, 3.0, z0 + 6});
                l["keyvalues"] = {{"brightness", "20"}, {"radius", "14"}};
                ents.push_back(l);
            }
        std::vector<nlohmann::json> all{entityJson("w", "worldspawn", world)};
        for (auto& l : ents) all.push_back(l);
        nlohmann::json sun = entityJson("sun", "light_directional", {}, {0, 0, 0});
        sun["keyvalues"] = {{"brightness", "2"}, {"angles", "-50 30 0"}};
        all.push_back(sun);
        tmp = std::make_unique<testutil::TempDir>();
        tmp->write("maps/rooms.map.json", testutil::docJson(all));
        std::filesystem::copy_file(assets / "game_profile.json", tmp->path / "game_profile.json");
        std::filesystem::copy_file(assets / "entities.ent", tmp->path / "entities.ent");
        return load(tmp->path, "maps/rooms.map.json", edit, giScale);
    }

    // `edit` may change the light scene (AO settings, ambient, ...) before the lights are packed. False when the map is missing.
    bool load(const std::filesystem::path& assets, const std::string& map, const std::function<void(core::LightScene&)>& edit = {}, int giScale = 1) {
        namespace light = native::light;
        if (!std::filesystem::exists(assets / map)) return false;
        scene = std::make_unique<core::Scene>(assets);
        scene->loadGameProfile();
        scene->openDocument(map);
        surfaces = scene->lightSurfaces();
        solids = scene->lightSolids();
        classes = scene->lightingClasses();
        lights = core::lightsOfStyle(scene->lightScene(), 0);
        if (edit) edit(lights);
        live = std::make_unique<light::LiveLayout>(scene->lightmapLayout().settings);
        live->buildAll(surfaces, classes);
        const auto pieces = light::piecesByChart(live->layout(), surfaces);
        for (size_t c = 0; c < live->charts().size(); ++c) {
            prepared.push_back(light::prepareChart(*live, static_cast<int>(c), pieces[c], solids.get(), giScale));
            sampleCount += prepared.back().count;
        }
        const uint32_t slots = static_cast<uint32_t>(sampleCount + sampleCount / 8 + 4096);
        store = std::make_unique<light::SampleStore>(device, slots, 16, 1u << 16);
        results = std::make_unique<light::Results>(device, slots);
        ring = std::make_unique<native::gpu::UploadRing>(device, native::gpu::UploadRingOptions{256ull << 20, 2});
        REQUIRE(store->valid());
        REQUIRE(results->valid());
        // the charts go up in batches that fit one slice of the upload ring
        size_t next = 0;
        while (next < prepared.size()) {
            REQUIRE(ring->beginFrame(frame));
            const size_t before = next;
            shot.run([&](VkCommandBuffer cmd) {
                while (next < prepared.size()) {
                    const light::PreparedChart& p = prepared[next];
                    const uint64_t need = p.samples.size() * 32ull + p.sub.size() * 4ull + p.grid.size() * 32ull + p.chunks.size() * 64ull + 4096;
                    if (ring->usedBytes() + need > ring->bytesPerFrame()) break;
                    REQUIRE(store->upload(cmd, *ring, p, p.page));
                    ++next;
                }
                transferToAll(cmd);
            });
            ring->endFrame(++frame);
            REQUIRE(next > before);
        }
        accel = std::make_unique<light::Accel>(device);
        REQUIRE(accel->valid());
        REQUIRE(accel->update(light::buildAccelObjects(surfaces, classes, &live->layout())));
        passes = std::make_unique<light::Passes>(device);
        REQUIRE(passes->valid());
        lightBuffer = std::make_unique<light::LightBuffer>(device, 64);
        REQUIRE(lightBuffer->set(light::packLights(lights.lights), static_cast<uint32_t>(lights.lights.size())));
        return true;
    }

    // Every chunk of every uploaded chart as a job at pass count `p`.
    std::vector<native::light::PassJob> allJobs(uint32_t p) const {
        std::vector<native::light::PassJob> jobs;
        for (size_t c = 0; c < live->charts().size(); ++c) {
            const auto* e = store->chart(static_cast<int>(c));
            if (!e) continue;
            for (uint32_t k = 0; k < e->chunkCount; ++k) {
                const native::light::GpuChunk& g = store->chunk(e->firstChunk + k);
                jobs.push_back({g.slotBase, g.slotCount, p});
            }
        }
        return jobs;
    }

    bool hasLightOf(native::light::LightClass c) const {
        for (const core::Light& l : lights.lights) {
            const native::light::LightClass k = l.type == core::LightType::Directional ? native::light::LightClass::Sun
                                                : l.type == core::LightType::Sky       ? native::light::LightClass::Sky
                                                                                       : native::light::LightClass::Local;
            if (k == c) return true;
        }
        return false;
    }

    // `count` direct passes over every chunk (a class without lights is skipped) and one ambient occlusion pass when the level has it.
    void bake(uint32_t count, const native::light::DirectParams& params) {
        namespace light = native::light;
        const uint32_t jobCapacity = static_cast<uint32_t>(allJobs(0).size()) + 1;
        light::JobList jobs(device, jobCapacity);
        REQUIRE(jobs.valid());
        const core::AoSettings& ao = lights.level.ao;
        if (ao.active() && ao.radius > 0) {
            REQUIRE(jobs.set(allJobs(0)));
            shot.run([&](VkCommandBuffer cmd) { REQUIRE(passes->recordOcclusion(cmd, *store, *results, *accel, jobs, static_cast<uint32_t>(ao.rays), static_cast<float>(ao.radius), static_cast<float>(ao.falloff))); });
            passes->resetDescriptors();
        }
        for (uint32_t p = 0; p < count; ++p) {
            REQUIRE(jobs.set(allJobs(p)));
            shot.run([&](VkCommandBuffer cmd) {
                for (const light::LightClass c : {light::LightClass::Local, light::LightClass::Sun, light::LightClass::Sky})
                    if (hasLightOf(c)) REQUIRE(passes->recordDirect(cmd, *store, *results, *accel, *lightBuffer, jobs, c, params));
            });
            passes->resetDescriptors();
        }
    }

    // Results read back, per slot.
    struct Readback {
        std::vector<uint8_t> samples, direct[3], occlusion;
    };
    Readback readResults() {
        Readback r;
        r.samples = readBack(device, shot, store->sampleBuffer(), static_cast<uint64_t>(store->slotCapacity()) * sizeof(native::light::SampleRecord));
        for (uint32_t c = 0; c < 3; ++c) r.direct[c] = readBack(device, shot, results->direct(static_cast<native::light::LightClass>(c)), static_cast<uint64_t>(store->slotCapacity()) * 16);
        r.occlusion = readBack(device, shot, results->occlusion(), static_cast<uint64_t>(store->slotCapacity()) * 4);
        return r;
    }
};

// The Core's reference for the fixture's scene: a LiveBaker (bounces off, its own GPU session) refined until it converged with the same quality as `params`.
inline std::unique_ptr<core::LiveBaker> makeReference(const LightFixture& f, int maxPasses, const native::light::DirectParams& params) {
    core::LiveSettings s;
    s.bounces = false;
    s.useGpu = true;
    s.denoiseIndirect = false;
    s.useTextures = false;
    s.maxPasses = maxPasses;
    s.areaSamplesPerPass = static_cast<int>(params.areaSamples);
    s.skySamplesPerPass = static_cast<int>(params.skySamples);
    s.directSupersample = static_cast<int>(params.superSamples);
    s.classes = f.classes;
    auto ref = std::make_unique<core::LiveBaker>(s);
    ref->setGeometry(f.live->layout(), f.surfaces, {}, f.solids);
    ref->setLights(f.lights);
    const core::Vec3 cam{0, 0, 0};
    int guard = 0;
    while (!ref->converged() && guard++ < 10000) ref->step(&cam, 1000000);
    REQUIRE(ref->converged());
    return ref;
}

// The factor LiveBaker::aoFactor applies for an occluded fraction `occ` (1 = open / AO off / not computed).
inline float aoFactorOf(const core::AoSettings& ao, float occ) {
    if (!ao.active() || std::isnan(occ)) return 1.f;
    const double open = std::pow(std::clamp(1.0 - ao.strength * static_cast<double>(occ), 0.0, 1.0), ao.power);
    return static_cast<float>(1.0 - ao.intensity * (1.0 - open));
}

} // namespace native_test
