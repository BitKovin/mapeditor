// Throughput benchmark of the final bake: the same synchronous LightingService loop as the BSP export (exporter: "final bake"), on a real map and
// the real Vulkan session, with the time split into where it went.
//
//   bench_bake <assetsRoot> <map.map.json> [--quality Q] [--lighting lighting.json] [--no-lightvol] [--tiles] [--validate] [--dump file.bin]
//   bench_bake <assetsRoot> --rooms N [...]   a generated level instead: N x N rooms of 12 m, one point light each and a sun
//
// --quality  LightingService::Options::qualityScale (the export uses the level's _bake_quality, 2 when unset)
// --tiles    also render and encode the tiles for clients after every step, like the editor (the export does not)
// --validate Vulkan validation layer (with synchronisation validation) on the session; its messages are printed
// --dump     writes the base layer's lightmap pages (float rgb, page after page) so two builds can be compared
#include "core/lighting_service.h"
#include "core/scene.h"
#include "test_util.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

using namespace core;
using namespace testutil;
using Clock = std::chrono::steady_clock;

namespace {
// rooms x rooms grid of 12 m rooms (floor, ceiling, walls with a door gap, a pillar, a crate), a point light in each, a sun over all
json roomsDoc(int rooms) {
    std::vector<Brush> world;
    std::vector<json> ents;
    const double R = 12, H = 3.5, T = 0.3;
    for (int gx = 0; gx < rooms; ++gx)
        for (int gz = 0; gz < rooms; ++gz) {
            const std::string id = std::to_string(gx) + "_" + std::to_string(gz);
            const double x0 = gx * R, z0 = gz * R;
            world.push_back(Brush::makeBox("fl" + id, {x0, -T, z0}, {x0 + R, 0, z0 + R}));
            if ((gx + gz) % 2) world.push_back(Brush::makeBox("ce" + id, {x0, H, z0}, {x0 + R, H + T, z0 + R}));  // every other room open to the sky
            world.push_back(Brush::makeBox("wn" + id, {x0, 0, z0}, {x0 + R, H, z0 + T}));
            world.push_back(Brush::makeBox("ws" + id, {x0, 0, z0 + R - T}, {x0 + R, H, z0 + R}));
            world.push_back(Brush::makeBox("ww" + id, {x0, 0, z0 + T}, {x0 + T, H, z0 + R - T}));
            world.push_back(Brush::makeBox("we1" + id, {x0 + R - T, 0, z0 + T}, {x0 + R, H, z0 + 4}));
            world.push_back(Brush::makeBox("we2" + id, {x0 + R - T, 0, z0 + 8}, {x0 + R, H, z0 + R - T}));
            world.push_back(Brush::makeBox("pi" + id, {x0 + 8, 0, z0 + 8}, {x0 + 9, H, z0 + 9}));
            world.push_back(Brush::makeBox("cr" + id, {x0 + 3, 0, z0 + 3}, {x0 + 4.5, 1.5, z0 + 4.5}));
            json l = entityJson("l" + id, "light_point", {}, {x0 + 6, 3.0, z0 + 6});
            l["keyvalues"] = {{"brightness", "20"}, {"radius", "14"}};
            ents.push_back(l);
        }
    std::vector<json> all{entityJson("w", "worldspawn", world)};
    for (auto& l : ents) all.push_back(l);
    json sun = entityJson("sun", "light_directional", {}, {0, 0, 0});
    sun["keyvalues"] = {{"brightness", "2"}, {"angles", "-50 30 0"}};
    all.push_back(sun);
    return docJson(all);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: bench_bake <assetsRoot> <map.map.json> [--quality Q] [--lighting lighting.json] [--no-lightvol] [--dump file.bin]\n";
        return 2;
    }
    LightingService::Options opt;
    double quality = 2.0;
    std::string dump;
    int rooms = 0;
    bool tiles = false;  // like the export; --tiles renders them like the editor
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (i == 2 && a != "--rooms") continue;  // the map
        if (a == "--rooms" && i + 1 < argc) rooms = std::atoi(argv[++i]);
        else if (a == "--quality" && i + 1 < argc) quality = std::atof(argv[++i]);
        else if (a == "--lighting" && i + 1 < argc) opt = LightingService::loadOptions(argv[++i]);
        else if (a == "--tiles") tiles = true;
        else if (a == "--validate") opt.live.gpuValidation = true;
        else if (a == "--no-lightvol") opt.lightvolEnabled = false;
        else if (a == "--dump" && i + 1 < argc) dump = argv[++i];
        else { std::cerr << "unknown argument " << a << "\n"; return 2; }
    }
    opt.qualityScale = quality;
    opt.tiles = tiles;
    std::unique_ptr<TempDir> tmp;
    std::filesystem::path root = argv[1];
    std::string map = argv[2];
    if (rooms > 0) {
        tmp = std::make_unique<TempDir>();
        tmp->write("maps/rooms.map.json", roomsDoc(rooms));
        std::filesystem::copy_file(root / "game_profile.json", tmp->path / "game_profile.json");
        std::filesystem::copy_file(root / "entities.ent", tmp->path / "entities.ent");
        root = tmp->path;
        map = "maps/rooms.map.json";
    }
    Scene scene{root};
    scene.loadGameProfile();
    scene.openDocument(map);
    LightingService svc(scene, opt);
    svc.setLog([](const std::string& l) { std::printf("log: %s\n", l.c_str()); });
    svc.setEnabled(true);

    const auto t0 = Clock::now();
    double lmSeconds = 0, volSeconds = 0;
    int idle = 0, ticks = 0;
    for (;;) {
        const auto a = Clock::now();
        const bool lmBefore = svc.busy() || ticks == 0;
        svc.tick(0.25);
        const double dt = std::chrono::duration<double>(Clock::now() - a).count();
        (lmBefore ? lmSeconds : volSeconds) += dt;
        ++ticks;
        if (!svc.busy() && !svc.lightvolPending()) { if (++idle > 1) break; } else idle = 0;
    }
    const double total = std::chrono::duration<double>(Clock::now() - t0).count();

    const LiveBaker& b = svc.baker();
    const LiveBaker::Stats& st = b.stats();
    const LightingService::Timings& tm = svc.timings();
    std::printf("device       %s (ao %s)\n", b.evaluator().c_str(), b.aoEvaluator().c_str());
    std::printf("samples      %zu, chunks %zu, pages %d, passes %d\n", b.sampleCount(), b.chunkCount(), b.layout().pageCount, b.settings().maxPasses);
    std::printf("total        %.3f s (%d ticks)\n", total, ticks);
    std::printf("  lightmap   %.3f s\n", lmSeconds);
    std::printf("  lightvol   %.3f s\n", volSeconds);
    std::printf("steps        %zu, %zu texels, %.3f s inside step(), %.3f s GPU submit->fence (%.0f%%)\n", st.steps, st.texels, st.stepSeconds, st.gpuSeconds,
                st.stepSeconds > 0 ? 100.0 * st.gpuSeconds / st.stepSeconds : 0.0);
    std::printf("timings ms   surfaces %.1f layout %.1f geometry %.1f lights %.1f session %.1f steps %.1f tiles %.1f denoise %.1f converged %.1f\n", tm.surfaces,
                tm.layout, tm.geometry, tm.lights, tm.session, tm.steps, tm.tiles, tm.denoise, tm.converged);
    const LiveBaker::GeometryTimings& g = b.geometryTimings();
    std::printf("geometry s   samples %.3f occluders %.3f bvh %.3f gi %.3f chunks %.3f total %.3f; session %.3f (resident %.3f)\n", g.samples, g.occluders, g.bvh,
                g.gi, g.chunks, g.total, b.lastSessionSeconds(), b.lastResidentSeconds());
    std::printf("denoise      %.3f s (%zu pages)\n", b.denoiseSeconds(), b.denoisedPages());
    if (opt.live.gpuValidation) std::printf("validation   %zu messages\n", b.gpuValidation().size());
    for (const auto& m : b.gpuValidation()) std::printf("validation: %s\n", m.c_str());
    if (!dump.empty()) {
        const LightmapPages p = b.snapshot();
        std::ofstream out(dump, std::ios::binary);
        size_t values = 0;
        for (const auto& page : p.rgb) {
            out.write(reinterpret_cast<const char*>(page.data()), static_cast<std::streamsize>(page.size() * sizeof(float)));
            values += page.size();
        }
        std::printf("dumped       %zu values to %s\n", values, dump.c_str());
    }
    return 0;
}
