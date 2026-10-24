// Throughput benchmark of the final bake: the same synchronous LightingService loop as the BSP export (exporter: "final bake"), on a real map and
// the real Vulkan session, with the time split into where it went.
//
//   bench_bake <assetsRoot> <map.map.json> [--quality Q] [--lighting lighting.json] [--no-lightvol] [--tiles] [--validate] [--dump file.bin]
//   --edits    after the bake settles, replay edits of the map (sun angle, a prefab instance moved, a detail brush moved) and print, per edit, the time to
//              the first tiles and to convergence plus where the rebuild went (needs a real map, not --rooms)
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
#include <functional>
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
    bool edits = false;
    bool tiles = false;  // like the export; --tiles renders them like the editor
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (i == 2 && a != "--rooms") continue;  // the map
        if (a == "--rooms" && i + 1 < argc) rooms = std::atoi(argv[++i]);
        else if (a == "--quality" && i + 1 < argc) quality = std::atof(argv[++i]);
        else if (a == "--lighting" && i + 1 < argc) opt = LightingService::loadOptions(argv[++i]);
        else if (a == "--tiles") tiles = true;
        else if (a == "--edits") edits = true;
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
    if (std::getenv("BENCH_EDITPATH")) {  // the non-lighting part of one edit: what the Router does for a document_update
        json d;
        { std::ifstream in(root / map); in >> d; }
        for (int k = 0; k < 3; ++k) {
            for (auto& p : d["prefabInstances"]) if (p["id"] == "i_village_a") p["origin"][0] = p["origin"][0].get<double>() + 1.0;
            auto ms = [](Clock::time_point a) { return std::chrono::duration<double, std::milli>(Clock::now() - a).count(); };
            auto a = Clock::now();
            const std::string wire = json{{"type", "document_update"}, {"doc", d}}.dump();
            const double serial = ms(a); a = Clock::now();
            const json parsed = json::parse(wire);
            const double parse = ms(a); a = Clock::now();
            const auto r = scene.applyUpdate(parsed["doc"]);
            const double apply = ms(a); a = Clock::now();
            const std::string sync = scene.documentSyncJson().dump();
            const double syncMs = ms(a); a = Clock::now();
            const json derived = scene.derivedGeometryJson();
            const double derive = ms(a); a = Clock::now();
            const std::string derivedWire = derived.dump();
            const double dump = ms(a); a = Clock::now();
            const auto surf = scene.lightSurfaces();
            const double light = ms(a);
            std::printf("editpath #%d ok %d: wire %.1f MB (Shell stringify ~%.0f ms) parse %.0f apply %.0f sync %.0f ms (%.1f MB) derive %.0f ms dump %.0f ms (%.1f MB) lightSurfaces %.0f ms (%zu)\n", k, r.ok ? 1 : 0,
                        wire.size() / 1e6, serial, parse, apply, syncMs, sync.size() / 1e6, derive, dump, derivedWire.size() / 1e6, light, surf.size());
        }
        return 0;
    }
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
    if (edits && rooms == 0) {
        // Edits of the real map: the file is rewritten (next to the map, removed afterwards) and opened again, like the Shell's document_update.
        const std::filesystem::path src = root / map, tmpRel = "maps/_bench_edit.map.json", editFile = root / tmpRel;
        json base;
        { std::ifstream in(src); in >> base; }
        struct Edit { const char* name; std::function<void(json&, int)> apply; };
        const std::vector<Edit> list = {
            {"sun angle", [](json& d, int k) {
                 for (auto& e : d["entities"]) if (e["classname"] == "light_directional") e["angles"] = {-50 - 3 * k, 30 + 5 * k, 0};
             }},
            {"point light added/moved", [](json& d, int k) {
                 json l = {{"id", "e_bench_light"}, {"classname", "light_point"}, {"origin", {-170.0 + 2.0 * k, 3.0, -60.0}}, {"angles", {0, 0, 0}},
                           {"keyvalues", {{"brightness", "20"}, {"radius", "20"}}}};
                 d["entities"].push_back(l);
             }},
            {"prefab instance moved 1 m", [](json& d, int k) {
                 for (auto& p : d["prefabInstances"]) if (p["id"] == "i_village_a") p["origin"][0] = p["origin"][0].get<double>() + 1.0 * k;
             }},
            {"detail brush moved 0.5 m", [](json& d, int k) {
                 for (auto& e : d["entities"]) if (e["classname"] == "func_detail")
                     for (auto& b : e["brushes"]) for (auto& v : b["vertices"]) v[0] = v[0].get<double>() + 0.5 * k;
             }},
        };
        for (const Edit& ed : list)
            for (int k = 1; k <= 2; ++k) {
                json doc = base;
                ed.apply(doc, k);
                { std::ofstream out(editFile); out << doc.dump(); }
                const auto e0 = Clock::now();
                scene.openDocument(tmpRel.generic_string());
                const double openMs = std::chrono::duration<double, std::milli>(Clock::now() - e0).count();
                svc.markDirty();
                double firstMs = -1, captureMs = 0;
                int ticksN = 0, idleN = 0;
                for (;; ++ticksN) {
                    const auto a = Clock::now();
                    const auto msgs = svc.tick(0.25);
                    if (ticksN == 0) captureMs = std::chrono::duration<double, std::milli>(Clock::now() - a).count();
                    for (const auto& m : msgs)
                        if (firstMs < 0 && (m.find("\"lightmap_tiles\"") != std::string::npos || m.find("\"lit_mesh\"") != std::string::npos))
                            firstMs = std::chrono::duration<double, std::milli>(Clock::now() - e0).count();
                    if (!svc.busy() && !svc.lightvolPending()) { if (++idleN > 1) break; } else idleN = 0;
                }
                const double finalMs = std::chrono::duration<double, std::milli>(Clock::now() - e0).count();
                const LightingService::Timings& t = svc.timings();
                const LiveBaker::GeometryTimings& gt = svc.baker().geometryTimings();
                std::printf("edit %-26s #%d: open doc %.0f ms | first tiles %.0f ms | converged+volume %.0f ms | (first tick %.0f ms) stale chunks %zu\n", ed.name, k, openMs, firstMs, finalMs,
                            captureMs, svc.baker().staleChunks());
                std::printf("      rebuild ms: layout %.0f diff %.0f geometry %.0f (inherit %.0f samples %.0f occluders %.0f bvh %.0f gi %.0f chunks %.0f atlas %.0f stale %.0f) | session %.0f steps %.0f tiles %.0f denoise %.0f | snapshot-relative firstTiles %.0f converged %.0f\n",
                            t.layout, t.diff, t.geometry, gt.inherit * 1e3, gt.samples * 1e3, gt.occluders * 1e3, gt.bvh * 1e3, gt.gi * 1e3, gt.chunks * 1e3, gt.atlas * 1e3, gt.stale * 1e3, t.session, t.steps, t.tiles, t.denoise,
                            t.firstTiles, t.converged);
            }
        std::filesystem::remove(editFile);
    }
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
