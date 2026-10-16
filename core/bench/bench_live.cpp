// Latency benchmark of the live lighting pipeline (everything between "document_update arrives" and "tiles are ready to send").
//
// The light itself is replaced by random noise (NoiseSession stands in for the Vulkan LiveSession), so a run takes seconds even on a machine
// without a GPU, and what remains is the overhead of everything around the light: scene snapshot, lightmap layout, texel samples, chunking,
// merging results, tile rendering and encoding, scheduling.
//
//   bench_live [--rooms N] [--moves K] [--tpm T] [--gi S] [--bounces 0|1] [--denoise 0|1] [--sun 0|1] [--gpu-ms X] [--session-ms Y]
//              [--budget SECONDS] [--real-gpu 0|1] [--async 0|1] [--interval MS]
//
// --gpu-ms      emulated GPU time of one LiveSession::run() (the noise fill itself is nearly free)
// --session-ms  emulated cost of creating a LiveSession (device memory, BLAS build); the real Vulkan session is used with --real-gpu 1
// --interval    time between two simulated drag updates in --async mode (the Shell sends the next one when the previous was acked)
#include "core/router.h"
#include "gpu_bake.h"
#include "noise_session.h"
#include "test_util.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>

using namespace core;
using namespace testutil;
using Clock = std::chrono::steady_clock;

namespace {

double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

struct Args {
    int rooms = 3, moves = 6;
    double tpm = 1, gpuMs = 0, sessionMs = 0, budget = 0.12, intervalMs = 33;
    int gi = 1, bounces = 0, denoise = 0, sun = 0, realGpu = 0, async = 0, drag = 0;
};

json lightJson(const std::string& id, const std::string& cls, Vec3 o, double brightness, double radius) {
    json e = entityJson(id, cls, {}, o);
    e["keyvalues"] = {{"brightness", std::to_string(brightness)}, {"radius", std::to_string(radius)}};
    return e;
}

// rooms x rooms grid of 12 m rooms (floor, ceiling, four walls with a door gap on one side, a pillar, a crate). The crate of room 0 is the one that moves.
json buildDoc(const Args& a, double crateX) {
    std::vector<Brush> world;
    std::vector<json> ents;
    const double R = 12, H = 3.5, T = 0.3;
    for (int gx = 0; gx < a.rooms; ++gx)
        for (int gz = 0; gz < a.rooms; ++gz) {
            const std::string id = std::to_string(gx) + "_" + std::to_string(gz);
            const double x0 = gx * R, z0 = gz * R;
            world.push_back(Brush::makeBox("fl" + id, {x0, -T, z0}, {x0 + R, 0, z0 + R}));
            world.push_back(Brush::makeBox("ce" + id, {x0, H, z0}, {x0 + R, H + T, z0 + R}));
            world.push_back(Brush::makeBox("wn" + id, {x0, 0, z0}, {x0 + R, H, z0 + T}));
            world.push_back(Brush::makeBox("ws" + id, {x0, 0, z0 + R - T}, {x0 + R, H, z0 + R}));
            world.push_back(Brush::makeBox("ww" + id, {x0, 0, z0 + T}, {x0 + T, H, z0 + R - T}));
            world.push_back(Brush::makeBox("we1" + id, {x0 + R - T, 0, z0 + T}, {x0 + R, H, z0 + 4}));
            world.push_back(Brush::makeBox("we2" + id, {x0 + R - T, 0, z0 + 8}, {x0 + R, H, z0 + R - T}));
            world.push_back(Brush::makeBox("pi" + id, {x0 + 8, 0, z0 + 8}, {x0 + 9, H, z0 + 9}));
            if (gx != 0 || gz != 0) world.push_back(Brush::makeBox("cr" + id, {x0 + 3, 0, z0 + 3}, {x0 + 4.5, 1.5, z0 + 4.5}));
            ents.push_back(lightJson("l" + id, "light_point", {x0 + 6, 3.0, z0 + 6}, 20, 14));
        }
    world.push_back(Brush::makeBox("crate", {crateX, 0, 3}, {crateX + 1.5, 1.5, 4.5}));  // the moving one (room 0)
    std::vector<json> all{entityJson("w", "worldspawn", world)};
    for (auto& l : ents) all.push_back(l);
    if (a.sun) {
        json s = entityJson("sun", "light_directional", {}, {0, 0, 0});
        s["keyvalues"] = {{"brightness", "2"}, {"angles", "-50 30 0"}};
        all.push_back(s);
    }
    return docJson(all);
}

struct EditResult { double tFirst = -1, tFinal = -1, ackWait = 0; size_t bytes = 0, messages = 0; json status; };

}  // namespace

int main(int argc, char** argv) {
    Args a;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        const double v = std::atof(argv[i + 1]);
        if (k == "--rooms") a.rooms = static_cast<int>(v);
        else if (k == "--moves") a.moves = static_cast<int>(v);
        else if (k == "--tpm") a.tpm = v;
        else if (k == "--gi") a.gi = static_cast<int>(v);
        else if (k == "--bounces") a.bounces = static_cast<int>(v);
        else if (k == "--denoise") a.denoise = static_cast<int>(v);
        else if (k == "--sun") a.sun = static_cast<int>(v);
        else if (k == "--gpu-ms") a.gpuMs = v;
        else if (k == "--session-ms") a.sessionMs = v;
        else if (k == "--budget") a.budget = v;
        else if (k == "--real-gpu") a.realGpu = static_cast<int>(v);
        else if (k == "--async") a.async = static_cast<int>(v);
        else if (k == "--interval") a.intervalMs = v;
        else if (k == "--drag") a.drag = static_cast<int>(v);
    }

    TempDir dir;
    dir.write("maps/a.map.json", buildDoc(a, 1));
    std::filesystem::copy_file(std::filesystem::path(BENCH_ASSETS_DIR) / "game_profile.json", dir.path / "game_profile.json");
    std::filesystem::copy_file(std::filesystem::path(BENCH_ASSETS_DIR) / "entities.ent", dir.path / "entities.ent");
    Scene scene(dir.path);
    scene.loadGameProfile();
    scene.openDocument("maps/a.map.json");

    LightingService::Options opt;
    opt.layout.texelsPerMeter = a.tpm;
    opt.live.giScale = a.gi;
    opt.live.bounces = a.bounces != 0;
    opt.live.denoiseIndirect = a.denoise != 0;
    opt.tickBudgetSeconds = a.budget;
    Router router(scene, {}, opt);
    if (!a.realGpu) router.lighting().setSessionFactory(noiseFactory(a.gpuMs, a.sessionMs));
    router.lighting().setLog([](const std::string& l) { std::printf("    log: %s\n", l.c_str()); });

    std::printf("rooms %dx%d, tpm %.1f, gi %d, bounces %d, denoise %d, sun %d, emulated gpu %.1f ms/run, session %.1f ms, tick budget %.0f ms, %s\n", a.rooms, a.rooms,
                a.tpm, a.gi, a.bounces, a.denoise, a.sun, a.gpuMs, a.sessionMs, a.budget * 1000, a.realGpu ? "REAL vulkan session" : "noise session");
    router.onMessage(R"({"type":"lighting","enabled":true,"camera":[3,1.5,3]})");

    if (a.async) {
        // The bake thread runs; this thread plays the socket thread: it receives what the sink delivers (with arrival times) and sends the edits.
        struct Arrival { Clock::time_point at; bool visual = false, converged = false; size_t bytes = 0; json status; };
        std::mutex qm;
        std::condition_variable qcv;
        std::deque<Arrival> queue;
        router.lighting().startWorker([&](std::vector<std::string>&& msgs) {
            std::lock_guard<std::mutex> lock(qm);
            for (const auto& m : msgs) {
                Arrival ar;
                ar.at = Clock::now();
                ar.bytes = m.size();
                const std::string tail = m.size() > 64 ? m.substr(m.size() - 64) : m;
                const std::string head = m.size() < 2000 ? m : m.substr(0, 240);  // (nlohmann sorts the keys: "type" is not always first)
                ar.visual = head.find("lightmap_tiles") != std::string::npos || head.find("lit_mesh") != std::string::npos;
                if (head.find("lighting_status") != std::string::npos) { ar.status = json::parse(m); ar.converged = ar.status["converged"].get<bool>(); }
                queue.push_back(ar);
            }
            qcv.notify_all();
        });
        router.onMessage(R"({"type":"lighting","enabled":true,"camera":[3,1.5,3]})");
        // waits until a converged status arrives (everything before it counts); returns its arrival time
        json lastStatus;
        auto settle = [&](Clock::time_point t0, double& firstVisual, size_t& bytes, size_t& visuals) {
            firstVisual = -1; bytes = 0; visuals = 0;
            for (;;) {
                std::unique_lock<std::mutex> lock(qm);
                if (!qcv.wait_for(lock, std::chrono::seconds(60), [&] { return !queue.empty(); })) return -1.0;
                while (!queue.empty()) {
                    const Arrival ar = queue.front();
                    queue.pop_front();
                    lock.unlock();
                    router.lighting().delivered();
                    bytes += ar.bytes;
                    if (ar.visual) { ++visuals; if (firstVisual < 0) firstVisual = std::chrono::duration<double, std::milli>(ar.at - t0).count(); }
                    if (!ar.status.is_null()) lastStatus = ar.status;
                    if (ar.converged && firstVisual >= 0) return std::chrono::duration<double, std::milli>(ar.at - t0).count();
                    lock.lock();
                }
            }
        };
        {
            const auto t0 = Clock::now();
            double fv; size_t by, vis;
            const double fin = settle(t0, fv, by, vis);
            std::printf("initial bake (async): first tiles %.0f ms, converged %.0f ms | %zu tile/mesh msgs, %.1f MB\n", fv, fin, vis, by / 1e6);
        }
        double sumFirst = 0, sumFinal = 0;
        for (int k = 1; k <= a.moves; ++k) {
            const auto t0 = Clock::now();
            router.onMessage(json{{"type", "document_update"}, {"doc", buildDoc(a, 1 + 0.5 * k)}}.dump());
            const double applyMs = msSince(t0);
            double fv; size_t by, vis;
            const double fin = settle(t0, fv, by, vis);
            std::printf("move %d (async): apply %.1f | first tiles %.0f ms, final %.0f ms | %zu tile/mesh msgs, %.2f MB\n", k, applyMs, fv, fin, vis, by / 1e6);
            const json& tm = lastStatus["timingsMs"];
            std::printf("         worker: layout %.1f geometry %.1f lights %.1f | session %.1f steps %.1f tiles %.1f | first tiles %.0f converged %.0f (from snapshot applied)\n", tm["layout"].get<double>(),
                        tm["geometry"].get<double>(), tm["lights"].get<double>(), tm["session"].get<double>(), tm["steps"].get<double>(), tm["tiles"].get<double>(),
                        tm["firstTiles"].get<double>(), tm["converged"].get<double>());
            sumFirst += fv; sumFinal += fin;
        }
        if (a.moves) std::printf("MEAN over %d moves: first visible %.0f ms, final %.0f ms\n", a.moves, sumFirst / a.moves, sumFinal / a.moves);
        // drag: an edit every `interval` ms whatever the bake is doing; what the Shell would see
        if (a.drag > 0) {
            std::vector<Clock::time_point> sent;
            const auto start = Clock::now();
            std::vector<double> visualAt;
            double lastVisualMs = 0;
            for (int k = 0; k < a.drag; ++k) {
                const auto tk = start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(k * a.intervalMs));
                // drain arrivals until it is time for the next edit
                for (;;) {
                    std::unique_lock<std::mutex> lock(qm);
                    qcv.wait_until(lock, tk, [&] { return !queue.empty(); });
                    while (!queue.empty()) {
                        const Arrival ar = queue.front();
                        queue.pop_front();
                        router.lighting().delivered();
                        if (ar.visual) visualAt.push_back(std::chrono::duration<double, std::milli>(ar.at - start).count());
                        if (std::getenv("BENCH_DEBUG") && ar.visual) std::fprintf(stderr, "  [visual] %zu bytes at %.0f ms\n", ar.bytes, std::chrono::duration<double, std::milli>(ar.at - start).count());
                    }
                    if (Clock::now() >= tk) break;
                }
                sent.push_back(Clock::now());
                if (std::getenv("BENCH_DEBUG")) std::fprintf(stderr, "  [edit %d] at %.0f ms\n", k, msSince(start));
                router.onMessage(json{{"type", "document_update"}, {"doc", buildDoc(a, 1 + 0.05 * k)}}.dump());
            }
            const auto last = Clock::now();
            double fv; size_t by, vis;
            const double fin = settle(last, fv, by, vis);
            std::printf("drag: %d edits every %.0f ms (%.0f ms), %zu visual msgs during the drag; after the last edit: first %.0f ms, final %.0f ms\n", a.drag, a.intervalMs,
                        msSince(start), visualAt.size(), fv, fin);
            lastVisualMs = visualAt.empty() ? 0 : visualAt.back();
            (void)lastVisualMs;
        }
        router.lighting().stopWorker();
        return 0;
    }

    // runs ticks until converged (and denoised); returns per-edit metrics, `t0` = when the edit arrived
    auto drain = [&](Clock::time_point t0, EditResult& r) {
        for (int tick = 0; tick < 100000; ++tick) {
            const RouterOutput o = router.onLightingTick(a.budget);
            bool visual = false, converged = false;
            for (const auto& m : o.toAll) {
                ++r.messages;
                r.bytes += m.size();
                // cheap type sniff: nlohmann sorts keys, "type" is last; look at the tail
                const std::string head = m.size() < 2000 ? m : m.substr(0, 240);  // (nlohmann sorts the keys: "type" is not always first)
                if (head.find("lightmap_tiles") != std::string::npos || head.find("lit_mesh") != std::string::npos) visual = true;
                if (head.find("lighting_status") != std::string::npos) {
                    const json j = json::parse(m);
                    r.status = j;
                    converged = j["converged"].get<bool>() && !router.lightingBusy();
                }
            }
            if (visual && r.tFirst < 0) r.tFirst = msSince(t0);
            if (converged && !router.lightingBusy()) { r.tFinal = msSince(t0); return; }
            if (!router.lightingBusy() && o.toAll.empty()) { r.tFinal = msSince(t0); return; }
        }
    };

    {
        EditResult r;
        const auto t0 = Clock::now();
        drain(t0, r);
        const auto& tm = r.status["timingsMs"];
        std::printf("initial bake: first tiles %.0f ms, converged %.0f ms | samples %d chunks %d pages %d | %.1f MB sent\n", r.tFirst, r.tFinal,
                    r.status["texels"].get<int>(), r.status["chunks"].get<int>(), r.status["pages"].get<int>(), r.bytes / 1e6);
        (void)tm;
    }

    std::vector<EditResult> results;
    for (int k = 1; k <= a.moves; ++k) {
        const double x = 1 + 0.5 * k;
        const json doc = buildDoc(a, x);
        EditResult r;
        const auto t0 = Clock::now();
        const auto o = router.onMessage(json{{"type", "document_update"}, {"doc", doc}}.dump());
        (void)o;
        const double applyMs = msSince(t0);
        drain(t0, r);
        const json& tm = r.status["timingsMs"];
        std::printf("move %d: apply %.1f | first tiles %.0f ms, final %.0f ms | %zu msgs %.2f MB | rebuild: surf %.1f layout %.1f diff %.1f geom %.1f lights %.1f | session %.1f steps %.1f tiles %.1f | stale chunks %d/%d\n",
                    k, applyMs, r.tFirst, r.tFinal, r.messages, r.bytes / 1e6, tm["surfaces"].get<double>(), tm["layout"].get<double>(), tm["diff"].get<double>(),
                    tm["geometry"].get<double>(), tm["lights"].get<double>(), tm["session"].get<double>(), tm["steps"].get<double>(), tm["tiles"].get<double>(),
                    r.status["stale"].get<int>(), r.status["chunks"].get<int>());
        const auto& g = router.lighting().baker().geometryTimings();
        std::printf("         setGeometry: inherit %.1f samples %.1f (generate %.1f, assemble %.1f) occluders %.1f bvh %.1f gi %.1f chunks %.1f atlas %.1f stale %.1f (total %.1f ms)\n", g.inherit * 1e3,
                    g.samples * 1e3, g.samplesGenerate * 1e3, g.samplesAssemble * 1e3, g.occluders * 1e3, g.bvh * 1e3, g.gi * 1e3, g.chunks * 1e3, g.atlas * 1e3, g.stale * 1e3, g.total * 1e3);
        std::printf("         session: %.1f ms of which resident arrays %.1f ms, factory %.1f ms\n", router.lighting().baker().lastSessionSeconds() * 1e3,
                    router.lighting().baker().lastResidentSeconds() * 1e3, (router.lighting().baker().lastSessionSeconds() - router.lighting().baker().lastResidentSeconds()) * 1e3);
        results.push_back(r);
    }
    double first = 0, fin = 0;
    for (const auto& r : results) { first += r.tFirst; fin += r.tFinal; }
    if (!results.empty()) std::printf("MEAN over %zu moves: first visible %.0f ms, final %.0f ms\n", results.size(), first / results.size(), fin / results.size());
    return 0;
}
