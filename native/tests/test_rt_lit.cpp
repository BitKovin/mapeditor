// R2: the ray traced Lit viewport. A viewport in Lighting only / Lit on demo_lit shows ambient * aoFactor(o) per pixel, with `o` the running mean of one cosine-weighted
// ray a frame. Checks: the converged picture equals a CPU reference (core::TriangleBvh at the surface point of 2000 pixels, the bake's occlusion model) and the
// accumulation stops tracing once converged; a still camera keeps its picture; resets (camera cut, resize, mode switch, scene edit, occlusion settings) restart it
// and a change of the display only (exposure) does not; the a-trous fallback is closer to the converged picture than the raw first frames; 0 validation errors.
// R3 adds the direct light: point, spot, sun and sky lights with ray traced shadows against a CPU reference of the bake's light model (hard and soft), and the lightmap look.
// [gpu]: skips itself without a ray tracing device with a second queue.
#include <doctest.h>

#include <QImage>

#include "scene/snapshot.h"   // first: core headers use `near` as a name, which windows.h (via volk) defines as a macro

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "base/jobs.h"
#include "doc/changeset.h"
#include "doc/io.h"
#include "doc/prefabs.h"
#include "gpu/device.h"
#include "light/light_link.h"
#include "light/lighting_service.h"
#include "render/camera_rig.h"
#include "render/render_thread.h"
#include "render/rt_lights.h"
#include "scene/builder.h"
#include "scene/render_mesh.h"

#include <core/profile.h>
#include <core/albedo.h>
#include <core/lightbake.h>
#include <core/lightmap_uv.h>
#include <core/raytrace.h>
#include <core/scene.h>
#include <core/texplan.h>

using namespace native;
namespace fs = std::filesystem;

namespace {

LRESULT CALLBACK rtLitWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

void pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

HWND makeWindow(int w, int h) {
    static const wchar_t* kClass = L"native_tests_rtlit_window";
    static const bool registered = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = rtLitWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        RegisterClassW(&wc);
        return true;
    }();
    (void)registered;
    RECT r{0, 0, w, h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClass, L"native rt lit test", WS_OVERLAPPEDWINDOW, 60, 60, r.right - r.left, r.bottom - r.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    // The test windows are never shown (the swapchain presents to a hidden window): a test run must not disturb whoever uses the machine.
    pump();
    return hwnd;
}

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

// The triangles the render thread draws as solids (the same rule as the RT scene's). `blended[i]` marks the triangles of translucent (stained glass) and cutout (window)
// textures: they block rays like every other triangle, but the G-buffer does not hold them (the blended pass draws them, a cutout may be a hole), so the pixel
// comparison skips the pixels whose first hit is one of them.
core::OccluderMesh drawnTriangles(const scene::SceneSnapshot& snap, std::vector<uint8_t>* blended) {
    core::OccluderMesh mesh;
    for (const auto& [key, p] : snap.objects) {
        const scene::RenderMesh* m = p.render ? p.render.get() : p.raw.get();
        if (!m || m->empty() || m->kind == scene::RenderMesh::Kind::Volume || m->indices.size() < 3) continue;
        for (size_t i = 0; i + 2 < m->indices.size(); i += 3) {
            const scene::RenderVertex& first = m->vertices[m->indices[i]];
            const std::string& tex = first.texture < m->textures.size() ? m->textures[first.texture] : std::string();
            if (blended) blended->push_back(tex.find("glass") != std::string::npos || tex.find("window") != std::string::npos ? 1 : 0);
            for (size_t c = 0; c < 3; ++c) {
                const float* v = m->vertices[m->indices[i + c]].pos;
                mesh.tris.insert(mesh.tris.end(), {v[0], v[1], v[2]});
            }
        }
    }
    return mesh;
}

bool waitFor(const std::function<bool()>& cond, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

void savePng(const render::CapturedImage& img, const char* name) {
    QImage out(static_cast<int>(img.width), static_cast<int>(img.height), QImage::Format_RGBA8888);
    for (uint32_t y = 0; y < img.height; ++y) std::memcpy(out.scanLine(static_cast<int>(y)), img.rgba.data() + static_cast<size_t>(y) * img.width * 4, img.width * 4);
    const fs::path dir = fs::path(NATIVE_CORE_ASSETS_DIR).parent_path().parent_path() / "native" / "build";
    out.save(QString::fromStdString((dir / name).string()));
}

struct Shot {
    render::CapturedImage img;
    double at(uint32_t x, uint32_t y) const { return img.rgba[(static_cast<size_t>(y) * img.width + x) * 4 + 1] / 255.0; }   // green: the ambient is grey
};

double rmse(const render::CapturedImage& a, const render::CapturedImage& b) {
    double sum = 0;
    size_t n = 0;
    for (size_t i = 0; i + 3 < a.rgba.size() && i + 3 < b.rgba.size(); i += 4)
        for (size_t c = 0; c < 3; ++c) {
            const double d = (static_cast<double>(a.rgba[i + c]) - b.rgba[i + c]) / 255.0;
            sum += d * d;
            ++n;
        }
    return std::sqrt(sum / std::max<size_t>(1, n));
}

double meanAbsDiff(const render::CapturedImage& a, const render::CapturedImage& b) {
    double sum = 0;
    for (size_t i = 0; i < a.rgba.size() && i < b.rgba.size(); ++i)
        if (i % 4 != 3) sum += std::abs(static_cast<int>(a.rgba[i]) - static_cast<int>(b.rgba[i]));
    return sum / (static_cast<double>(a.rgba.size()) * 0.75);
}

render::RtLighting testLighting() {
    render::RtLighting l;
    l.ambient[0] = l.ambient[1] = l.ambient[2] = 0.8f;
    l.aoActive = true;
    l.aoRadius = 2.0f;
    l.aoIntensity = 0.9f;
    l.aoStrength = 1.0f;
    l.aoPower = 1.5f;   // not 1: the factor is not linear in the occlusion, so applying it to a single sample would be visibly wrong
    l.aoFalloff = 1.0f;
    return l;
}

// The Lighting only view of a fly camera in demo_lit's room; every test works with this scene.
struct Fixture {
    std::unique_ptr<gpu::Device> device;
    base::JobPool pool{8};
    std::shared_ptr<const scene::SceneSnapshot> snapshot;
    HWND hwnd = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    std::unique_ptr<render::RenderThread> rt;
    render::ViewportId id = 0;
    render::CameraRig rig;
    uint32_t w = 0, h = 0;
    unsigned cut = 0;
    int mode = 2;   // ViewState::renderMode: 1 Lit, 2 Lighting only
    int dlss = 0;   // ViewState::dlss: 0 off (the filter), 1 Quality, 2 DLAA (DLSS Ray Reconstruction)

    fs::path root;
    std::string mapName = "maps/demo_lit.map.json";
    bool start(uint32_t width, uint32_t height, bool streamline = false) {
        gpu::DeviceOptions o;
        o.validation = true;
        o.streamline = streamline;
        std::string err;
        device = gpu::Device::create(o, &err);
        if (!device || !device->caps().lit() || device->computeQueue() == device->graphicsQueue()) {
            MESSAGE("no ray tracing device with a second queue (" << err << "): GPU test skipped");
            return false;
        }
        if (root.empty()) root = NATIVE_CORE_ASSETS_DIR;
        if (!fs::exists(root / mapName)) return false;
        snapshot = buildSnapshot(root, mapName, pool);
        if (!snapshot) return false;
        w = width;
        h = height;
        hwnd = makeWindow(static_cast<int>(w), static_cast<int>(h));
        VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        ci.hinstance = GetModuleHandleW(nullptr);
        ci.hwnd = hwnd;
        if (vkCreateWin32SurfaceKHR(device->instance(), &ci, nullptr, &surface) != VK_SUCCESS) return false;
        rt = std::make_unique<render::RenderThread>(*device);
        rt->setTextureSource(root, &pool);
        rt->setSnapshot(snapshot);
        id = rt->addViewport(surface, w, h);
        rig.fly().lookAt({3.55, 1.6, 2.5}, {-0.5, 1.2, -3.5});
        return true;
    }
    ~Fixture() {
        if (rt) {
            rt->setRtLit(false);
            if (id) rt->removeViewport(id);
            rt.reset();
        }
        if (device && surface) vkDestroySurfaceKHR(device->instance(), surface, nullptr);
        if (hwnd) DestroyWindow(hwnd);
    }
    render::ViewState view() {
        render::ViewState vs = rig.viewState(w, h);
        vs.renderMode = mode;
        vs.cut = cut;
        vs.dlss = dlss;
        vs.showGrid = false;   // overlays are not part of the comparison
        vs.showAxes = false;
        return vs;
    }
    render::CapturedImage shot() {
        rt->setView(id, view());
        return rt->capture(id, 20000);
    }
    // Waits for the textures, then for the converged stop of the current view.
    bool converge(int ms = 180000) {
        return waitFor([&] { return rt->rtLitInfo(id).tracingSkipped; }, ms);
    }
    render::RenderThread::RtLitInfo info() const { return rt->rtLitInfo(id); }
};

render::RtLitOptions options(uint32_t target) {
    render::RtLitOptions o;
    o.targetSamples = target;
    o.sampleCap = std::max(4096u, target + 8);
    o.lightmapTexelsPerMeter = 0;   // the smooth picture (the lightmap look has its own test)
    o.bounces = false;              // the parity tests compare direct light and occlusion; the bounce test turns it on
    return o;
}

} // namespace

TEST_CASE("RT Lit: the converged ambient occlusion equals the CPU reference at 2000 pixels, and tracing stops" * doctest::test_suite("gpu")) {
    Fixture f;
    if (!f.start(640, 400)) return;
    std::vector<uint8_t> blended;
    const core::OccluderMesh mesh = drawnTriangles(*f.snapshot, &blended);
    REQUIRE(mesh.triangleCount() > 100);
    const core::TriangleBvh bvh(mesh);
    const render::RtLighting light = testLighting();
    f.rt->setRtLighting(light);
    f.rt->setRtLit(true, options(4096));
    // the textures decode in the background and each new texture changes the scene: wait until a still view stays converged
    REQUIRE(f.shot().width == f.w);
    REQUIRE(waitFor([&] { return f.rt->sceneInfo().texturesPending == 0 && f.rt->sceneInfo().scene.queued == 0; }, 120000));
    f.cut++;
    f.shot();
    REQUIRE(f.converge());
    const auto info = f.info();
    MESSAGE("converged after " << info.frames << " frames, " << info.resets << " resets, render " << info.renderW << "x" << info.renderH);
    CHECK(info.active);
    CHECK(info.stillFrames >= 4097);
    const render::CapturedImage img = f.shot();
    REQUIRE(img.ok());
    savePng(img, "rt_lit_ao_converged.png");

    // the CPU reference
    const render::ViewState vs = f.view();
    const render::Mat4 inv = render::mat4Invert(vs.viewProj);
    const auto unproject = [&](double nx, double ny, double nz) {
        double q[4];
        const double v[4] = {nx, ny, nz, 1.0};
        for (int r = 0; r < 4; ++r) q[r] = inv[0 * 4 + r] * v[0] + inv[1 * 4 + r] * v[1] + inv[2 * 4 + r] * v[2] + inv[3 * 4 + r] * v[3];
        return core::Vec3{q[0] / q[3], q[1] / q[3], q[2] / q[3]};
    };
    const core::Vec3 eye{vs.eye[0], vs.eye[1], vs.eye[2]};
    const auto rayOf = [&](uint32_t x, uint32_t y) {
        const double nx = (x + 0.5) / f.w * 2.0 - 1.0, ny = (y + 0.5) / f.h * 2.0 - 1.0;
        const core::Vec3 a = unproject(nx, ny, 0.0), b = unproject(nx, ny, 1.0);
        return (b - a).normalized();
    };
    struct Sample {
        uint32_t x, y;
        core::Vec3 p, n;
    };
    std::vector<Sample> samples;
    std::mt19937 rng(7);
    const auto hitOf = [&](uint32_t x, uint32_t y) { return bvh.closestHit(eye, rayOf(x, y), 1000.0); };
    for (int tries = 0; tries < 40000 && samples.size() < 2000; ++tries) {
        const uint32_t x = 3 + rng() % (f.w - 6), y = 3 + rng() % (f.h - 6);
        const auto hit = hitOf(x, y);
        if (hit.t < 0 || blended[hit.triangle]) continue;
        // interior pixels only: the neighbours at 3 pixels see the same triangle (a rasterised edge pixel may belong to the next one)
        bool interior = true;
        for (const auto& [dx, dy] : {std::pair{3, 0}, std::pair{-3, 0}, std::pair{0, 3}, std::pair{0, -3}}) {
            const auto o = hitOf(x + dx, y + dy);
            if (o.t < 0 || o.triangle != hit.triangle) interior = false;
        }
        if (!interior) continue;
        const float* t = &mesh.tris[hit.triangle * 9];
        const core::Vec3 v0{t[0], t[1], t[2]}, v1{t[3], t[4], t[5]}, v2{t[6], t[7], t[8]};
        core::Vec3 n = (v1 - v0).cross(v2 - v0).normalized();
        const core::Vec3 d = rayOf(x, y);
        if (n.dot(d) > 0) n = n * -1.0;
        samples.push_back({x, y, eye + d * hit.t, n});
    }
    REQUIRE(samples.size() >= 1000);

    const int kRays = 4096;
    std::vector<double> expected(samples.size());
    const auto work = [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            const Sample& s = samples[i];
            std::mt19937 r(static_cast<uint32_t>(i) * 2654435761u + 1);
            std::uniform_real_distribution<double> u(0.0, 1.0);
            const core::Vec3 helper = std::abs(s.n.x) < 0.9 ? core::Vec3{1, 0, 0} : core::Vec3{0, 1, 0};
            const core::Vec3 tu = s.n.cross(helper).normalized(), tv = s.n.cross(tu);
            double sum = 0;
            for (int k = 0; k < kRays; ++k) {
                const double u1 = (k + u(r)) / kRays, u2 = u(r);   // stratified in u1
                const double rad = std::sqrt(u1), phi = 2.0 * 3.14159265358979323846 * u2;
                const core::Vec3 dir = (tu * (rad * std::cos(phi)) + tv * (rad * std::sin(phi)) + s.n * std::sqrt(std::max(0.0, 1.0 - u1))).normalized();
                const double t = bvh.nearestHit(s.p + s.n * 1e-3, dir, light.aoRadius);
                if (t >= 0 && t < light.aoRadius) sum += 1.0 - t / light.aoRadius;
            }
            expected[i] = std::clamp(0.8 * light.aoFactor(static_cast<float>(sum / kRays)), 0.0, 1.0);
        }
    };
    {
        const size_t threads = std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> ts;
        for (size_t t = 0; t < threads; ++t) ts.emplace_back(work, samples.size() * t / threads, samples.size() * (t + 1) / threads);
        for (auto& t : ts) t.join();
    }
    std::vector<double> rel;
    double sumRel = 0, lo = 1, hi = 0;
    const Shot shot{img};
    for (size_t i = 0; i < samples.size(); ++i) {
        const double got = shot.at(samples[i].x, samples[i].y), want = expected[i];
        const double r = std::abs(got - want) / std::max(want, 1.0 / 255.0);
        rel.push_back(r);
        sumRel += r;
        lo = std::min(lo, want);
        hi = std::max(hi, want);
    }
    std::sort(rel.begin(), rel.end());
    const double mean = sumRel / static_cast<double>(rel.size()), p99 = rel[static_cast<size_t>(0.99 * static_cast<double>(rel.size() - 1))];
    MESSAGE(samples.size() << " pixels: expected values " << lo << " .. " << hi << ", relative difference mean " << mean * 100 << " %, p99 " << p99 * 100 << " %");
    CHECK(hi - lo > 0.1);   // the occlusion shows: the picture is not flat
    CHECK(mean < 0.01);
    CHECK(p99 < 0.05);

    // tracing stopped: the picture stays as it is, frames are only made on demand, the counters show no traced pixel
    const uint64_t frames = f.rt->framesRendered(f.id);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(f.rt->framesRendered(f.id) == frames);
    const render::CapturedImage again = f.shot();
    f.shot();
    f.shot();
    CHECK(meanAbsDiff(img, again) == 0.0);
    const auto done = f.info();
    CHECK(done.tracingSkipped);
    CHECK(done.tracedPixels == 0);
    CHECK(done.surfacePixels > 1000);
    CHECK(f.device->validationErrors() == 0);
}

TEST_CASE("RT Lit: a still camera converges and stops, resets restart it, display-only changes do not" * doctest::test_suite("gpu")) {
    Fixture f;
    if (!f.start(320, 200)) return;
    const render::RtLighting light = testLighting();
    f.rt->setRtLighting(light);
    f.rt->setRtLit(true, options(48));
    REQUIRE(f.shot().ok());
    REQUIRE(waitFor([&] { return f.rt->sceneInfo().texturesPending == 0 && f.rt->sceneInfo().scene.queued == 0; }, 120000));
    f.cut++;
    f.shot();
    REQUIRE(f.converge());
    render::RenderThread::RtLitInfo i0 = f.info();
    CHECK(i0.active);
    CHECK(i0.renderW == 320);
    CHECK(i0.stillFrames >= 49);

    // the first frames after a reset trace every surface pixel
    f.cut++;
    f.shot();
    f.shot();
    f.shot();
    f.shot();
    render::RenderThread::RtLitInfo i1 = f.info();
    MESSAGE("after a camera cut: " << i1.resets - i0.resets << " resets, traced " << i1.tracedPixels << " of " << i1.surfacePixels << " pixels");
    CHECK(i1.resets == i0.resets + 1);
    CHECK(i1.tracedPixels == i1.surfacePixels);
    CHECK(i1.surfacePixels > 1000);
    REQUIRE(f.converge());
    const render::CapturedImage base = f.shot();

    // a mode switch (Lit <-> Lighting only) restarts the history
    f.mode = 1;
    f.shot();
    render::RenderThread::RtLitInfo i2 = f.info();
    CHECK(i2.resets == i1.resets + 1);
    REQUIRE(f.converge());
    f.mode = 2;
    f.shot();
    CHECK(f.info().resets == i2.resets + 1);
    REQUIRE(f.converge());
    const uint64_t r3 = f.info().resets;

    // occlusion settings that change the rays restart it; exposure only changes the display
    render::RtLighting l2 = light;
    l2.exposure = 0.5f;
    f.rt->setRtLighting(l2);
    const render::CapturedImage dark = f.shot();
    CHECK(f.info().resets == r3);
    CHECK(f.info().tracingSkipped);   // nothing to trace: the converged history, shown darker
    CHECK(meanAbsDiff(base, dark) > 10.0);
    render::RtLighting l3 = light;
    l3.aoRadius = 1.0f;
    f.rt->setRtLighting(l3);
    f.shot();
    CHECK(f.info().resets == r3 + 1);
    REQUIRE(f.converge());
    f.rt->setRtLighting(light);
    f.shot();
    REQUIRE(f.converge());
    const uint64_t r4 = f.info().resets;

    // a scene edit restarts it
    scene::ObjectKey victim;
    size_t most = 0;
    for (const auto& [key, p] : f.snapshot->objects) {
        const scene::RenderMesh* m = p.render ? p.render.get() : p.raw.get();
        if (m && !m->empty() && m->kind != scene::RenderMesh::Kind::Volume && m->indices.size() > most) {
            most = m->indices.size();
            victim = key;
        }
    }
    auto edited = std::make_shared<scene::SceneSnapshot>(*f.snapshot);
    auto moved = std::make_shared<scene::RenderMesh>(*(edited->objects[victim].render ? edited->objects[victim].render : edited->objects[victim].raw));
    for (auto& v : moved->vertices) v.pos[1] += 0.05f;
    edited->objects[victim].render = moved;
    f.rt->setSnapshot(edited);
    REQUIRE(waitFor([&] { f.shot(); return f.info().resets > r4; }, 20000));
    REQUIRE(f.converge());

    // a resize creates new targets and restarts it
    const uint64_t r5 = f.info().resets;
    RECT r{0, 0, 400, 260};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(f.hwnd, nullptr, 60, 60, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    pump();
    f.w = 400;
    f.h = 260;
    f.rt->resizeViewport(f.id, f.w, f.h);
    REQUIRE(waitFor([&] { f.shot(); return f.info().renderW == 400; }, 20000));
    CHECK(f.info().renderH == 260);
    CHECK(f.info().resets > r5);
    REQUIRE(f.converge());
    CHECK(f.device->validationErrors() == 0);
}

TEST_CASE("RT Lit: the a-trous fallback is closer to the converged picture than the raw first frames" * doctest::test_suite("gpu")) {
    Fixture f;
    if (!f.start(320, 200)) return;
    f.rt->setRtLighting(testLighting());
    render::RtLitOptions o = options(1500);
    o.filter = true;
    f.rt->setRtLit(true, o);
    REQUIRE(f.shot().ok());
    REQUIRE(waitFor([&] { return f.rt->sceneInfo().texturesPending == 0 && f.rt->sceneInfo().scene.queued == 0; }, 120000));
    f.cut++;
    f.shot();
    REQUIRE(f.converge());
    const render::CapturedImage reference = f.shot();
    REQUIRE(reference.ok());

    // right after a reset the picture is the filtered first samples (or the raw ones without the filter)
    const auto early = [&](bool filter) {
        render::RtLitOptions oo = o;
        oo.filter = filter;
        f.rt->setRtLit(true, oo);
        f.cut++;
        const render::CapturedImage a = f.shot();
        return rmse(a, reference);
    };
    double filtered = 0, raw = 0;
    for (int i = 0; i < 3; ++i) {
        filtered += early(true);
        raw += early(false);
    }
    MESSAGE("rmse of the first frames against the converged picture: filtered " << filtered / 3 << ", raw " << raw / 3);
    CHECK(filtered < raw * 0.8);
    CHECK(f.device->validationErrors() == 0);
}

// R2: DLSS Ray Reconstruction denoises and upscales the radiance (needs Streamline with the sl.dlss_d plugin on an RTX GPU).
TEST_CASE("RT Lit: DLSS Ray Reconstruction draws the picture, follows the converged one, resets and falls back" * doctest::test_suite("gpu")) {
    Fixture f;
    if (!f.start(640, 400, true)) return;
    if (!f.device->caps().streamline || !f.device->streamline() || !f.device->streamline()->status().rrSupported) {
        MESSAGE("DLSS Ray Reconstruction unavailable (" << f.device->caps().streamlineNote << "): skipped");
        return;
    }
    f.rt->setRtLighting(testLighting());
    render::RtLitOptions o = options(600);
    f.rt->setRtLit(true, o);
    REQUIRE(f.shot().ok());
    REQUIRE(waitFor([&] { return f.rt->sceneInfo().texturesPending == 0 && f.rt->sceneInfo().scene.queued == 0; }, 120000));

    // the filtered picture of the same view, converged, is the reference
    f.cut++;
    f.shot();
    REQUIRE(f.converge());
    const render::CapturedImage reference = f.shot();
    REQUIRE(reference.ok());
    CHECK_FALSE(f.info().reconstruction);

    // Ray Reconstruction at Quality: a smaller render image, upscaled
    f.dlss = 1;
    f.cut++;
    f.shot();
    REQUIRE(waitFor([&] { f.shot(); return f.info().reconstruction; }, 30000));
    CHECK(f.info().renderW < f.w);
    CHECK(f.info().renderH < f.h);
    const render::CapturedImage early = f.shot();
    REQUIRE(f.converge());
    for (int i = 0; i < 20; ++i) f.shot();   // the upscaler's own history settles on the converged input
    const render::CapturedImage rr = f.shot();
    REQUIRE(rr.ok());
    savePng(rr, "rt_lit_rr_quality.png");
    savePng(early, "rt_lit_rr_early.png");
    savePng(reference, "rt_lit_filter_reference.png");
    const double distance = rmse(rr, reference);
    MESSAGE("rmse of the Ray Reconstruction picture against the filtered, converged one: " << distance << " (early frames: " << rmse(early, reference) << ")");
    CHECK(distance < 0.08);
    CHECK(f.info().tracedPixels == 0);

    // DLAA: the render image is the output size
    f.dlss = 2;
    f.cut++;
    REQUIRE(waitFor([&] { f.shot(); return f.info().reconstruction && f.info().renderW == f.w; }, 30000));
    CHECK(f.info().renderH == f.h);

    // a reset (camera cut) restarts the accumulation, and switching DLSS off falls back to the filter at the output size
    const uint64_t resets = f.info().resets;
    f.cut++;
    REQUIRE(waitFor([&] { f.shot(); return f.info().resets > resets; }, 20000));
    f.dlss = 0;
    REQUIRE(waitFor([&] { f.shot(); return !f.info().reconstruction && f.info().active; }, 30000));
    CHECK(f.info().renderW == f.w);
    CHECK(f.device->validationErrors() == 0);
}

// ---- R3: direct light ------------------------------------------------------------------------------------------------------------------------------------------------

namespace {

core::Light testLight(core::LightType type, core::Vec3 pos, core::Vec3 forward, double brightness, double radius) {
    core::Light l;
    l.type = type;
    l.position = pos;
    l.forward = forward.normalized();
    l.color = {1, 1, 1};
    l.brightness = brightness;
    l.radius = radius;
    l.castShadows = true;
    l.sourceSize = 0;
    return l;
}

std::vector<core::Light> directTestLights() {
    std::vector<core::Light> lights;
    lights.push_back(testLight(core::LightType::Point, {-0.5, 2.0, -3.5}, {0, 0, 1}, 6, 14));
    core::Light spot = testLight(core::LightType::Spot, {3.0, 2.2, 1.0}, {-1, -1, -0.5}, 8, 12);
    spot.innerAngle = 20;
    spot.outerAngle = 40;
    lights.push_back(spot);
    lights.push_back(testLight(core::LightType::Directional, {0, 0, 0}, {0.3, -1, 0.2}, 0.4, 0));
    lights.push_back(testLight(core::LightType::Sky, {0, 0, 0}, {0, 0, 1}, 0.25, 0));
    return lights;
}

// The bake's light maths on the CPU (core/shaders/lightmap_direct.comp) with exact shadows: the green channel (the lights are white) of the direct light at p.
double cpuDirect(const std::vector<core::Light>& lights, const core::TriangleBvh& bvh, const core::Vec3& p, const core::Vec3& n, std::mt19937& rng) {
    const double kPi = 3.14159265358979323846, kBias = 0.01;
    const core::Vec3 o = p + n * kBias;
    const auto window = [](double d, double r) {
        const double q = d / r, w = std::clamp(1.0 - q * q * q * q, 0.0, 1.0);
        return w * w;
    };
    double sum = 0;
    for (const core::Light& L : lights) {
        double v = 0;
        if (L.type == core::LightType::Point || L.type == core::LightType::Spot) {
            const core::Vec3 d = L.position - p;
            const double dist = std::sqrt(d.dot(d));
            if (dist >= L.radius) continue;
            const core::Vec3 l = d * (1.0 / dist);
            double s = window(dist, L.radius) / (std::max(dist, 0.05) * std::max(dist, 0.05)) * std::max(0.0, n.dot(l));
            if (L.type == core::LightType::Spot) {
                const double cosInner = std::cos(L.innerAngle * kPi / 180.0), cosOuter = std::cos(L.outerAngle * kPi / 180.0);
                const double t = std::clamp((L.forward.dot(l * -1.0) - cosOuter) / (cosInner - cosOuter), 0.0, 1.0);
                s *= t * t;
            }
            v = s;
            if (v != 0 && L.castShadows) {
                const core::Vec3 t = L.position - o;
                const double td = std::sqrt(t.dot(t));
                if (td > kBias && bvh.occluded(o, t * (1.0 / td), td - kBias)) v = 0;
            }
        } else if (L.type == core::LightType::Directional) {
            v = std::max(0.0, n.dot(L.forward * -1.0));
            if (v != 0 && L.castShadows && bvh.occluded(o, L.forward * -1.0, 10000.0)) v = 0;
        } else if (L.type == core::LightType::Sky) {
            const core::Vec3 helper = std::abs(n.x) < 0.9 ? core::Vec3{1, 0, 0} : core::Vec3{0, 1, 0};
            const core::Vec3 tu = n.cross(helper).normalized(), tv = n.cross(tu);
            std::uniform_real_distribution<double> u(0.0, 1.0);
            const int kRays = 512;
            int open = 0;
            for (int k = 0; k < kRays; ++k) {
                const double u1 = (k + u(rng)) / kRays, u2 = u(rng);
                const double rad = std::sqrt(u1), phi = 2.0 * kPi * u2;
                const core::Vec3 dir = tu * (rad * std::cos(phi)) + tv * (rad * std::sin(phi)) + n * std::sqrt(std::max(0.0, 1.0 - u1));
                if (dir.y > 0 && !bvh.occluded(o, dir, 10000.0)) ++open;
            }
            v = static_cast<double>(open) / kRays;
        }
        sum += v * L.brightness;
    }
    return sum;
}

struct SurfaceSample {
    uint32_t x, y;
    core::Vec3 p, n;
};

} // namespace

TEST_CASE("RT Lit: point, spot, sun and sky light with ray traced shadows equal the CPU reference of the bake's light model, and the lightmap look is the bilinear texel mean" * doctest::test_suite("gpu")) {
    Fixture f;
    if (!f.start(640, 400)) return;
    std::vector<uint8_t> blended;
    const core::OccluderMesh mesh = drawnTriangles(*f.snapshot, &blended);
    const core::TriangleBvh bvh(mesh);
    const std::vector<core::Light> lights = directTestLights();
    render::RtLighting lighting;
    lighting.ambient[0] = lighting.ambient[1] = lighting.ambient[2] = 0.1f;
    f.rt->setRtLighting(lighting);
    f.rt->setRtLights(render::makeRtLightData(lights));
    f.rt->setRtLit(true, options(2048));
    REQUIRE(f.shot().width == f.w);
    REQUIRE(waitFor([&] { return f.rt->sceneInfo().texturesPending == 0 && f.rt->sceneInfo().scene.queued == 0; }, 120000));
    f.cut++;
    f.shot();
    REQUIRE(f.converge());
    const render::CapturedImage img = f.shot();
    REQUIRE(img.ok());
    savePng(img, "rt_lit_direct_converged.png");

    const render::ViewState vs = f.view();
    const render::Mat4 inv = render::mat4Invert(vs.viewProj);
    const auto unproject = [&](double nx, double ny, double nz) {
        double q[4];
        const double v[4] = {nx, ny, nz, 1.0};
        for (int r = 0; r < 4; ++r) q[r] = inv[0 * 4 + r] * v[0] + inv[1 * 4 + r] * v[1] + inv[2 * 4 + r] * v[2] + inv[3 * 4 + r] * v[3];
        return core::Vec3{q[0] / q[3], q[1] / q[3], q[2] / q[3]};
    };
    const core::Vec3 eye{vs.eye[0], vs.eye[1], vs.eye[2]};
    const auto rayOf = [&](uint32_t x, uint32_t y) {
        const double nx = (x + 0.5) / f.w * 2.0 - 1.0, ny = (y + 0.5) / f.h * 2.0 - 1.0;
        return (unproject(nx, ny, 1.0) - unproject(nx, ny, 0.0)).normalized();
    };
    const auto hitOf = [&](uint32_t x, uint32_t y) { return bvh.closestHit(eye, rayOf(x, y), 1000.0); };
    std::vector<SurfaceSample> samples;
    std::mt19937 rng(11);
    for (int tries = 0; tries < 40000 && samples.size() < 1500; ++tries) {
        const uint32_t x = 3 + rng() % (f.w - 6), y = 3 + rng() % (f.h - 6);
        const auto hit = hitOf(x, y);
        if (hit.t < 0 || blended[hit.triangle]) continue;
        bool interior = true;
        for (const auto& [dx, dy] : {std::pair{3, 0}, std::pair{-3, 0}, std::pair{0, 3}, std::pair{0, -3}}) {
            const auto o = hitOf(x + dx, y + dy);
            if (o.t < 0 || o.triangle != hit.triangle) interior = false;
        }
        if (!interior) continue;
        const float* t = &mesh.tris[hit.triangle * 9];
        const core::Vec3 v0{t[0], t[1], t[2]}, v1{t[3], t[4], t[5]}, v2{t[6], t[7], t[8]};
        core::Vec3 n = (v1 - v0).cross(v2 - v0).normalized();
        const core::Vec3 d = rayOf(x, y);
        if (n.dot(d) > 0) n = n * -1.0;
        samples.push_back({x, y, eye + d * hit.t, n});
    }
    REQUIRE(samples.size() >= 800);

    std::vector<double> expected(samples.size());
    {
        const size_t threads = std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> ts;
        for (size_t t = 0; t < threads; ++t)
            ts.emplace_back([&, t] {
                for (size_t i = samples.size() * t / threads; i < samples.size() * (t + 1) / threads; ++i) {
                    std::mt19937 r(static_cast<uint32_t>(i) * 2654435761u + 5);
                    expected[i] = std::clamp(0.1 + cpuDirect(lights, bvh, samples[i].p, samples[i].n, r), 0.0, 1.0);
                }
            });
        for (auto& t : ts) t.join();
    }
    const Shot shot{img};
    double sum = 0, lo = 1, hi = 0;
    std::vector<double> diffs;
    for (size_t i = 0; i < samples.size(); ++i) {
        const double d = std::abs(shot.at(samples[i].x, samples[i].y) - expected[i]);
        diffs.push_back(d);
        sum += d;
        lo = std::min(lo, expected[i]);
        hi = std::max(hi, expected[i]);
    }
    std::sort(diffs.begin(), diffs.end());
    const double mean = sum / static_cast<double>(diffs.size()), p95 = diffs[static_cast<size_t>(0.95 * static_cast<double>(diffs.size() - 1))];
    MESSAGE(samples.size() << " pixels: expected " << lo << " .. " << hi << ", absolute difference mean " << mean << ", p95 " << p95);
    CHECK(hi - lo > 0.3);   // lit and shadowed areas
    CHECK(mean < 0.012);
    CHECK(p95 < 0.04);
    CHECK(f.info().tracedPixels == 0);
    CHECK(f.device->validationErrors() == 0);

    // ---- the lightmap look: the picture is the bilinear mean of the direct light at the centres of the texels of a planar grid (4 texels per metre here, so it shows)
    render::RtLitOptions look = options(2048);
    look.lightmapTexelsPerMeter = 4.0f;
    f.rt->setRtLit(true, look);
    f.cut++;
    f.shot();
    REQUIRE(f.converge());
    const render::CapturedImage lookImg = f.shot();
    REQUIRE(lookImg.ok());
    savePng(lookImg, "rt_lit_direct_texel4.png");
    const Shot lookShot{lookImg};
    const double density = 4.0;
    std::vector<double> lookDiffs;
    double lookSum = 0, smoothDiff = 0;
    const size_t count = std::min<size_t>(samples.size(), 600);
    std::vector<double> want(count);
    {
        const size_t threads = std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> ts;
        for (size_t t = 0; t < threads; ++t)
            ts.emplace_back([&, t] {
                for (size_t i = count * t / threads; i < count * (t + 1) / threads; ++i) {
                    const SurfaceSample& s = samples[i];
                    const double an[3] = {std::abs(s.n.x), std::abs(s.n.y), std::abs(s.n.z)};
                    const int axis = an[0] >= an[1] && an[0] >= an[2] ? 0 : (an[1] >= an[2] ? 1 : 2);
                    const int ua = axis == 0 ? 1 : 0, va = axis == 2 ? 1 : 2;
                    const auto comp = [](const core::Vec3& v, int a) { return a == 0 ? v.x : a == 1 ? v.y : v.z; };
                    const auto setc = [](core::Vec3& v, int a, double x) { (a == 0 ? v.x : a == 1 ? v.y : v.z) = x; };
                    const double qu = comp(s.p, ua) * density - 0.5, qv = comp(s.p, va) * density - 0.5;
                    const double bu = std::floor(qu), bv = std::floor(qv), fu = qu - bu, fv = qv - bv;
                    double acc = 0;
                    std::mt19937 r(static_cast<uint32_t>(i) + 99);
                    for (int c = 0; c < 4; ++c) {
                        const int iu = c & 1, iv = c >> 1;
                        const double cu = (bu + iu + 0.5) / density, cv = (bv + iv + 0.5) / density;
                        core::Vec3 centre = s.p;
                        setc(centre, ua, cu);
                        setc(centre, va, cv);
                        setc(centre, axis, comp(s.p, axis) - (comp(s.n, ua) * (cu - comp(s.p, ua)) + comp(s.n, va) * (cv - comp(s.p, va))) / comp(s.n, axis));
                        const double w = (iu ? fu : 1.0 - fu) * (iv ? fv : 1.0 - fv);
                        acc += w * cpuDirect(lights, bvh, centre, s.n, r);
                    }
                    want[i] = std::clamp(0.1 + acc, 0.0, 1.0);
                }
            });
        for (auto& t : ts) t.join();
    }
    for (size_t i = 0; i < count; ++i) {
        lookDiffs.push_back(std::abs(lookShot.at(samples[i].x, samples[i].y) - want[i]));
        lookSum += lookDiffs.back();
        smoothDiff += std::abs(lookShot.at(samples[i].x, samples[i].y) - expected[i]);
    }
    std::sort(lookDiffs.begin(), lookDiffs.end());
    MESSAGE("lightmap look at 4 texels / m: difference to the bilinear reference mean " << lookSum / static_cast<double>(count) << ", p95 "
            << lookDiffs[static_cast<size_t>(0.95 * static_cast<double>(count - 1))] << "; to the smooth picture " << smoothDiff / static_cast<double>(count));
    CHECK(lookSum / static_cast<double>(count) < 0.02);
    CHECK(smoothDiff / static_cast<double>(count) > lookSum / static_cast<double>(count));   // the look is visibly not the smooth picture
    CHECK(f.device->validationErrors() == 0);
}

TEST_CASE("RT Lit: lights from a document: the producer collects the level's lights, a styled one on a layer of its own" * doctest::test_suite("gpu")) {
    const fs::path root = NATIVE_CORE_ASSETS_DIR;
    if (!fs::exists(root / "maps/demo_lit.map.json")) return;
    base::JobPool pool(4);
    const auto snapshot = buildSnapshot(root, "maps/demo_lit.map.json", pool);
    REQUIRE(snapshot);
    std::shared_ptr<const render::RtLightData> got;
    std::mutex mutex;
    int calls = 0;
    render::RtLightsProducer producer([&](std::shared_ptr<const render::RtLightData> d) {
        std::lock_guard<std::mutex> lock(mutex);
        got = std::move(d);
        ++calls;
    });
    producer.request({snapshot, std::make_shared<core::GameProfile>(core::loadProfile(root)), root});
    producer.waitIdle();
    std::lock_guard<std::mutex> lock(mutex);
    REQUIRE(got);
    // demo_lit: two point lights (one of them in light style 6: the layer 1) and a sky light (layer 0)
    CHECK(got->lightCount == 3);
    CHECK(got->layerCount == 2);
    CHECK(got->layerStyle[1] == 6);
    CHECK(got->globalCount == 1);
    CHECK(got->layerGlobalCount[1] == 0);
    CHECK(got->cells.size() == static_cast<size_t>(got->dim[0]) * got->dim[1] * got->dim[2] * 2 * got->layerCount);
    CHECK(calls == 1);
    // the same document again: nothing new is reported
    producer.request({snapshot, std::make_shared<core::GameProfile>(core::loadProfile(root)), root});
    producer.waitIdle();
    CHECK(calls == 1);
}

namespace {
std::string rtEnv(const char* name) {
    char buf[512];
    size_t n = 0;
    return getenv_s(&n, buf, sizeof(buf), name) == 0 && n > 1 ? std::string(buf) : std::string();
}
} // namespace

// A look at a real map, not a regression test: runs only when NATIVE_RTLOOK_ROOT (assets root) and NATIVE_RTLOOK_MAP are set. NATIVE_RTLOOK_EYE / _TARGET = "x,y,z",
// NATIVE_RTLOOK_TEXEL = texels per metre of the lightmap look (default 16, 0 = smooth), NATIVE_RTLOOK_SIZE = "WxH" (default 1280x720), NATIVE_RTLOOK_DLSS = 0/1/2, NATIVE_RTLOOK_MODE = 0 unlit / 1 lit / 2 lighting only, NATIVE_RTLOOK_TIMEOUT = seconds per wait (default 120),
// NATIVE_RTLOOK_BOUNCE = 0 / 1 (default 1), NATIVE_RTLOOK_OUT = the png name in native/build (default rt_lit_look.png). The lights come from the document through the producer, like in the editor.
TEST_CASE("RT Lit: look at a map" * doctest::test_suite("gpu") * doctest::skip(rtEnv("NATIVE_RTLOOK_ROOT").empty())) {
    const auto env = [](const char* name, const std::string& fallback) {
        const std::string v = rtEnv(name);
        return v.empty() ? fallback : v;
    };
    const auto numbers = [](std::string text) {
        for (char& c : text)
            if (c == ',' || c == 'x') c = ' ';
        std::istringstream in(text);
        std::vector<double> out;
        double d = 0;
        while (in >> d) out.push_back(d);
        out.resize(3, 0.0);
        return out;
    };
    const auto vec = [&](const std::string& text) {
        const std::vector<double> n = numbers(text);
        return core::Vec3{n[0], n[1], n[2]};
    };
    const std::vector<double> size = numbers(env("NATIVE_RTLOOK_SIZE", "1280x720"));
    const uint32_t w = static_cast<uint32_t>(size[0]), h = static_cast<uint32_t>(size[1]);
    Fixture f;
    f.root = env("NATIVE_RTLOOK_ROOT", "");
    f.mapName = env("NATIVE_RTLOOK_MAP", "maps/lvl2.map.json");
    if (!f.start(w, h, true)) return;
    f.dlss = std::atoi(env("NATIVE_RTLOOK_DLSS", "0").c_str());
    f.mode = std::atoi(env("NATIVE_RTLOOK_MODE", "1").c_str());
    const core::Vec3 eye = vec(env("NATIVE_RTLOOK_EYE", "41,1.2,20")), target = vec(env("NATIVE_RTLOOK_TARGET", "41,1.2,10"));
    f.rig.fly().lookAt({eye.x, eye.y, eye.z}, {target.x, target.y, target.z});
    std::mutex mutex;
    std::shared_ptr<const render::RtLightData> got;
    render::RtLightsProducer producer([&](std::shared_ptr<const render::RtLightData> d) {
        std::lock_guard<std::mutex> lock(mutex);
        got = d;
        f.rt->setRtLights(d);
    });
    producer.request({f.snapshot, std::make_shared<core::GameProfile>(core::loadProfile(f.root)), f.root});
    producer.waitIdle();
    {
        std::lock_guard<std::mutex> lock(mutex);
        REQUIRE(got);
        MESSAGE("lights " << got->lightCount << " (global " << got->globalCount << "), grid " << got->dim[0] << "x" << got->dim[1] << "x" << got->dim[2] << " of " << got->cell << " m");
        for (uint32_t k = 0; k < got->lightCount; ++k) {
            const float* r = &got->records[k * 32];
            MESSAGE("  light " << k << ": type " << r[7] << " at " << r[0] << "," << r[1] << "," << r[2] << " radius " << r[3] << " colour*brightness " << r[4] << "," << r[5] << "," << r[6] << " bounce " << r[24]);
        }
    }
    f.rt->setRtLighting(render::rtLightingOf(*f.snapshot->doc));
    f.rt->setObjectRules(render::makeRtRules(*f.snapshot, core::LightingClasses::fromProfile(core::loadProfile(f.root))));   // MainWindow does this in the editor
    render::RtLitOptions o = options(1024);
    o.lightmapTexelsPerMeter = static_cast<float>(std::atof(env("NATIVE_RTLOOK_TEXEL", "16").c_str()));
    o.bounces = std::atoi(env("NATIVE_RTLOOK_BOUNCE", "1").c_str()) != 0;
    f.rt->setRtLit(true, o);
    REQUIRE(f.shot().width == f.w);
    // every wait is bounded (NATIVE_RTLOOK_TIMEOUT seconds, default 120): a stuck wait fails with a message instead of looking like a frozen run
    const int limitMs = std::max(1, std::atoi(env("NATIVE_RTLOOK_TIMEOUT", "120").c_str())) * 1000;
    REQUIRE_MESSAGE(waitFor([&] { return f.rt->sceneInfo().texturesPending == 0 && f.rt->sceneInfo().scene.queued == 0; }, limitMs), "the textures / scene did not settle in time");
    f.cut++;
    f.shot();
    // only the ray traced modes (Lit, Lighting only) report a converged stop; the unlit mode is a single frame
    if (f.mode == 1 || f.mode == 2) REQUIRE_MESSAGE(f.converge(limitMs), "the accumulation did not converge in time");
    for (int i = 0; i < (f.dlss ? 20 : 1); ++i) f.shot();
    const render::CapturedImage img = f.shot();
    REQUIRE(img.ok());
    savePng(img, env("NATIVE_RTLOOK_OUT", "rt_lit_look.png").c_str());
    MESSAGE("frames " << f.info().frames << ", render " << f.info().renderW << "x" << f.info().renderH << ", validation errors " << f.device->validationErrors());
}

TEST_CASE("RT Lit: the lighting rules of the classes: objects that do not block light cast no shadow, objects that do not receive it are shown unlit" * doctest::test_suite("gpu")) {
    Fixture f;
    if (!f.start(640, 400)) return;
    std::vector<uint8_t> blended;
    const core::OccluderMesh mesh = drawnTriangles(*f.snapshot, &blended);
    const core::TriangleBvh bvh(mesh);
    const std::vector<core::Light> lights = directTestLights();
    render::RtLighting lighting;
    lighting.ambient[0] = lighting.ambient[1] = lighting.ambient[2] = 0.1f;
    f.rt->setRtLighting(lighting);
    f.rt->setRtLights(render::makeRtLightData(lights));
    f.rt->setRtLit(true, options(1024));
    REQUIRE(f.shot().width == f.w);
    REQUIRE(waitFor([&] { return f.rt->sceneInfo().texturesPending == 0 && f.rt->sceneInfo().scene.queued == 0; }, 120000));
    const auto rulesOf = [&](uint32_t flags) {
        auto rules = std::make_shared<render::ObjectRules>();
        for (const auto& [key, p] : f.snapshot->objects) (*rules)[key] = flags;
        return rules;
    };
    const auto settle = [&] {
        f.cut++;
        f.shot();
        REQUIRE(f.converge());
        return f.shot();
    };
    const render::CapturedImage normal = settle();
    REQUIRE(normal.ok());

    // classes that do not block light: nothing is in the way, the picture is brighter and equals the unshadowed light
    f.rt->setObjectRules(rulesOf(render::kObjReceives));
    const render::CapturedImage open = settle();
    savePng(open, "rt_lit_rules_no_casters.png");
    double sumNormal = 0, sumOpen = 0;
    for (size_t i = 0; i < normal.rgba.size(); i += 4) {
        sumNormal += normal.rgba[i + 1];
        sumOpen += open.rgba[i + 1];
    }
    MESSAGE("mean green without / with shadows: " << sumOpen / (normal.rgba.size() / 4) << " / " << sumNormal / (normal.rgba.size() / 4));
    CHECK(sumOpen > sumNormal * 1.02);

    // classes that do not receive light: the unlit look of the chunked Lit mode (lighting only: 0.35 * (0.7 + 0.3 |n.l|)), whatever the lights
    f.rt->setObjectRules(rulesOf(render::kObjCasts));
    const render::CapturedImage flat = settle();
    savePng(flat, "rt_lit_rules_no_receivers.png");
    double lo = 1, hi = 0;
    size_t surface = 0;
    {   // the pixels whose first hit is a drawn solid (not the background, not a blended surface)
        const render::ViewState vs = f.view();
        const render::Mat4 inv = render::mat4Invert(vs.viewProj);
        const auto unproject = [&](double nx, double ny, double nz) {
            double q[4];
            const double v[4] = {nx, ny, nz, 1.0};
            for (int r = 0; r < 4; ++r) q[r] = inv[0 * 4 + r] * v[0] + inv[1 * 4 + r] * v[1] + inv[2 * 4 + r] * v[2] + inv[3 * 4 + r] * v[3];
            return core::Vec3{q[0] / q[3], q[1] / q[3], q[2] / q[3]};
        };
        const core::Vec3 eye{vs.eye[0], vs.eye[1], vs.eye[2]};
        for (uint32_t y = 4; y + 4 < f.h; y += 4)
            for (uint32_t x = 4; x + 4 < f.w; x += 4) {
                const double nx = (x + 0.5) / f.w * 2.0 - 1.0, ny = (y + 0.5) / f.h * 2.0 - 1.0;
                const core::Vec3 dir = (unproject(nx, ny, 1.0) - unproject(nx, ny, 0.0)).normalized();
                const auto hit = bvh.closestHit(eye, dir, 1000.0);
                if (hit.t < 0 || blended[hit.triangle]) continue;
                const double g = flat.rgba[(static_cast<size_t>(y) * f.w + x) * 4 + 1] / 255.0;
                ++surface;
                lo = std::min(lo, g);
                hi = std::max(hi, g);
            }
    }
    MESSAGE("unlit surfaces: " << surface << " pixels, values " << lo << " .. " << hi);
    CHECK(surface > 5000);
    CHECK(hi <= 0.35 + 1.0 / 255.0);        // never brighter than 0.35 * (0.7 + 0.3)
    CHECK(lo >= 0.35 * 0.7 - 1.0 / 255.0);  // never darker than 0.35 * 0.7
    CHECK(f.info().tracedPixels == 0);      // nothing is traced for them

    // the default (no rules) is the first picture again
    f.rt->setObjectRules(nullptr);
    const render::CapturedImage again = settle();
    CHECK(meanAbsDiff(normal, again) < 2.0);   // the noise of two converged runs
    CHECK(f.device->validationErrors() == 0);
}

TEST_CASE("RT Lit: an edit that leaves every shape as it was keeps the accumulated light, a moved object restarts it" * doctest::test_suite("gpu")) {
    Fixture f;
    if (!f.start(480, 300)) return;
    f.rt->setRtLights(render::makeRtLightData(directTestLights()));
    f.rt->setRtLit(true, options(64));
    REQUIRE(f.shot().width == f.w);
    REQUIRE(waitFor([&] { return f.rt->sceneInfo().texturesPending == 0 && f.rt->sceneInfo().scene.queued == 0; }, 120000));
    f.cut++;
    f.shot();
    REQUIRE(f.converge());

    // the same shapes, other texture coordinates (what a texture or projection edit makes): uploaded again, light kept
    auto retextured = std::make_shared<scene::SceneSnapshot>(*f.snapshot);
    retextured->version += 1;
    const auto shifted = [](const std::shared_ptr<const scene::RenderMesh>& m, float du, float dx) {
        if (!m) return m;
        auto c = std::make_shared<scene::RenderMesh>(*m);
        for (scene::RenderVertex& v : c->vertices) {
            v.uv[0] += du;
            v.pos[0] += dx;
        }
        return std::shared_ptr<const scene::RenderMesh>(c);
    };
    for (auto& [key, prod] : retextured->objects) {
        prod.render = shifted(prod.render, 0.5f, 0.0f);
        prod.raw = shifted(prod.raw, 0.5f, 0.0f);
    }
    const uint64_t uploadsBefore = f.rt->sceneInfo().scene.uploads;
    f.rt->setSnapshot(retextured);
    REQUIRE(waitFor([&] { return f.rt->sceneInfo().scene.uploads > uploadsBefore && f.rt->sceneInfo().scene.queued == 0; }, 60000));
    f.shot();
    f.shot();
    CHECK(f.info().tracingSkipped);          // still converged: nothing restarted
    CHECK(f.info().tracedPixels == 0);

    // the same objects moved: the light of the view is traced again
    auto moved = std::make_shared<scene::SceneSnapshot>(*f.snapshot);
    moved->version += 2;
    for (auto& [key, prod] : moved->objects) {
        prod.render = shifted(prod.render, 0.0f, 0.25f);
        prod.raw = shifted(prod.raw, 0.0f, 0.25f);
    }
    const uint64_t uploadsMid = f.rt->sceneInfo().scene.uploads;
    f.rt->setSnapshot(moved);
    REQUIRE(waitFor([&] { return f.rt->sceneInfo().scene.uploads > uploadsMid && f.rt->sceneInfo().scene.queued == 0; }, 60000));
    f.shot();
    CHECK_FALSE(f.info().tracingSkipped);
    CHECK(f.device->validationErrors() == 0);
}

TEST_CASE("RT Lit: bounce light (SHaRC cache of S = Db + I) brightens what the lights only reach indirectly, and is stable" * doctest::test_suite("gpu")) {
    Fixture f;
    if (!f.start(640, 400)) return;
    if (!f.device->caps().bounceCache) {
        MESSAGE("no bounce cache on this device: skipped");
        return;
    }
    f.rt->setRtLights(render::makeRtLightData(directTestLights()));
    render::RtLighting lighting;
    lighting.ambient[0] = lighting.ambient[1] = lighting.ambient[2] = 0.0f;
    f.rt->setRtLighting(lighting);
    const auto run = [&](bool bounces) {
        render::RtLitOptions o = options(256);
        o.bounces = bounces;
        f.rt->setRtLit(true, o);
        REQUIRE(f.shot().width == f.w);
        REQUIRE(waitFor([&] { return f.rt->sceneInfo().texturesPending == 0 && f.rt->sceneInfo().scene.queued == 0; }, 120000));
        f.cut++;
        f.shot();
        REQUIRE(f.converge());
        return f.shot();
    };
    const render::CapturedImage off = run(false);
    const render::CapturedImage on = run(true);
    const render::CapturedImage again = run(true);
    savePng(off, "rt_lit_bounce_off.png");
    savePng(on, "rt_lit_bounce_on.png");
    double sumOff = 0, sumOn = 0;
    size_t brighter = 0, darker = 0, pixels = off.rgba.size() / 4;
    for (size_t i = 0; i < off.rgba.size(); i += 4) {
        const double a = off.rgba[i] + off.rgba[i + 1] + off.rgba[i + 2], b = on.rgba[i] + on.rgba[i + 1] + on.rgba[i + 2];
        sumOff += a;
        sumOn += b;
        brighter += b > a + 3;
        darker += a - b > 30;   // noise of two converged runs on a textured floor
    }
    MESSAGE("mean sum of channels without / with bounces: " << sumOff / pixels << " / " << sumOn / pixels << ", brighter pixels " << brighter << " of " << pixels << ", clearly darker pixels " << darker);
    CHECK(sumOn > sumOff * 1.02);              // light arrives where no light reaches directly
    CHECK(darker < pixels / 1000);             // and (apart from noise) nothing gets darker
    CHECK(brighter > pixels / 50);
    CHECK(meanAbsDiff(on, again) < 3.0);       // converged pictures agree run to run
    CHECK(f.device->validationErrors() == 0);
}

// The baked Lit view (chunked lightmaps, LightingService converged) against the ray traced Lit view for one camera of a real map. Runs only when NATIVE_CMP_ROOT and NATIVE_CMP_MAP
// are set (NATIVE_CMP_EYE / _TARGET / _SIZE / _TIMEOUT / _OUT like the look test; NATIVE_CMP_BOUNCE 0 / 1; NATIVE_CMP_STYLE_TIME seconds: freeze the light styles). Writes native/build/<OUT> = bake | ray traced | |difference| x 4 and prints the
// mean brightness of both and the ratio per 4 x 4 region of the picture.
TEST_CASE("Compare: the baked Lit view against the ray traced Lit view" * doctest::test_suite("gpu") * doctest::skip(rtEnv("NATIVE_CMP_ROOT").empty())) {
    const auto env = [](const char* name, const std::string& fallback) {
        const std::string v = rtEnv(name);
        return v.empty() ? fallback : v;
    };
    const auto numbers = [](std::string text) {
        for (char& c : text)
            if (c == ',' || c == 'x') c = ' ';
        std::istringstream in(text);
        std::vector<double> out;
        double d = 0;
        while (in >> d) out.push_back(d);
        out.resize(3, 0.0);
        return out;
    };
    const std::vector<double> size = numbers(env("NATIVE_CMP_SIZE", "960x540"));
    const uint32_t w = static_cast<uint32_t>(size[0]), h = static_cast<uint32_t>(size[1]);
    const int limitMs = std::max(1, std::atoi(env("NATIVE_CMP_TIMEOUT", "300").c_str())) * 1000;
    Fixture f;
    f.root = env("NATIVE_CMP_ROOT", "");
    f.mapName = env("NATIVE_CMP_MAP", "maps/lvl2.map.json");
    if (!f.start(w, h, false)) return;
    const std::vector<double> eye = numbers(env("NATIVE_CMP_EYE", "30,1.2,-14")), target = numbers(env("NATIVE_CMP_TARGET", "26.5,1.0,-23.5"));
    f.rig.fly().lookAt({eye[0], eye[1], eye[2]}, {target[0], target[1], target[2]});
    const auto profile = std::make_shared<core::GameProfile>(core::loadProfile(f.root));

    // ---- the bake
    f.rt->setRtLit(false);
    auto link = std::make_shared<light::LightLink>(*f.device);
    REQUIRE(link->valid());
    f.rt->setLightLink(link);
    light::LightingService service(*f.device, *link);
    REQUIRE(service.valid());
    service.setInputs({f.snapshot, profile, f.root});
    f.shot();
    REQUIRE(waitFor([&] { return f.rt->sceneInfo().texturesPending == 0 && f.rt->sceneInfo().scene.queued == 0; }, limitMs));
    REQUIRE_MESSAGE(service.waitConverged(limitMs), "the bake did not converge in time");
    render::CapturedImage baked;
    for (int i = 0; i < 6; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        baked = f.shot();
    }
    REQUIRE(baked.ok());
    MESSAGE("bake: " << service.status().chunks << " chunks, error '" << service.status().error << "'");

    // ---- the ray traced view
    std::mutex mutex;
    render::RtLightsProducer producer([&](std::shared_ptr<const render::RtLightData> d) {
        std::lock_guard<std::mutex> lock(mutex);
        f.rt->setRtLights(d);
    });
    producer.request({f.snapshot, profile, f.root});
    producer.waitIdle();
    f.rt->setRtLighting(render::rtLightingOf(*f.snapshot->doc));
    f.rt->setObjectRules(render::makeRtRules(*f.snapshot, core::LightingClasses::fromProfile(*profile)));   // MainWindow does this in the editor
    if (!env("NATIVE_CMP_STYLE_TIME", "").empty()) f.rt->setStyleClock(std::atof(env("NATIVE_CMP_STYLE_TIME", "0").c_str()));   // freezes the light styles (the bake shows style 0 only)
    render::RtLitOptions o = options(1024);
    o.lightmapTexelsPerMeter = 16.0f;
    o.bounces = std::atoi(env("NATIVE_CMP_BOUNCE", "1").c_str()) != 0;
    f.rt->setRtLit(true, o);
    f.cut++;
    f.shot();
    REQUIRE(waitFor([&] { return f.rt->sceneInfo().texturesPending == 0 && f.rt->sceneInfo().scene.queued == 0; }, limitMs));
    f.cut++;
    f.shot();
    REQUIRE_MESSAGE(f.converge(limitMs), "the accumulation did not converge in time");
    const render::CapturedImage rt = f.shot();
    REQUIRE(rt.ok());
    REQUIRE(rt.width == baked.width);
    REQUIRE(rt.height == baked.height);

    {   // NATIVE_CMP_PROBE = "x,y": which objects the ray through that pixel passes (bounds only), nearest first, with their lighting class and rules
        const std::vector<double> probe = numbers(env("NATIVE_CMP_PROBE", ""));
        if (!env("NATIVE_CMP_PROBE", "").empty()) {
            const render::ViewState vs = f.view();
            const render::Mat4 inv = render::mat4Invert(vs.viewProj);
            const auto unproject = [&](double nz) {
                const double nx = (probe[0] + 0.5) / f.w * 2.0 - 1.0, ny = (probe[1] + 0.5) / f.h * 2.0 - 1.0;
                double q[4];
                const double v[4] = {nx, ny, nz, 1.0};
                for (int r = 0; r < 4; ++r) q[r] = inv[0 * 4 + r] * v[0] + inv[1 * 4 + r] * v[1] + inv[2 * 4 + r] * v[2] + inv[3 * 4 + r] * v[3];
                return core::Vec3{q[0] / q[3], q[1] / q[3], q[2] / q[3]};
            };
            const core::Vec3 ro{vs.eye[0], vs.eye[1], vs.eye[2]};
            const core::Vec3 d = (unproject(1.0) - unproject(0.0)).normalized();
            const auto rules = render::makeRtRules(*f.snapshot, core::LightingClasses::fromProfile(*profile));
            std::vector<std::pair<double, std::string>> hits;
            for (const auto& [key, prod] : f.snapshot->objects) {
                if (!prod.bounds.valid()) continue;
                double t0 = 0, t1 = 1e9;
                const double lo[3] = {prod.bounds.mn.x, prod.bounds.mn.y, prod.bounds.mn.z}, hi[3] = {prod.bounds.mx.x, prod.bounds.mx.y, prod.bounds.mx.z};
                const double oo[3] = {ro.x, ro.y, ro.z}, dd[3] = {d.x, d.y, d.z};
                bool ok = true;
                for (int a = 0; a < 3 && ok; ++a) {
                    if (std::abs(dd[a]) < 1e-12) { ok = oo[a] >= lo[a] && oo[a] <= hi[a]; continue; }
                    double a0 = (lo[a] - oo[a]) / dd[a], a1 = (hi[a] - oo[a]) / dd[a];
                    if (a0 > a1) std::swap(a0, a1);
                    t0 = std::max(t0, a0);
                    t1 = std::min(t1, a1);
                    ok = t0 <= t1;
                }
                if (!ok) continue;
                const auto r = rules->find(key);
                std::string cls = prod.surfaces && !prod.surfaces->empty() ? prod.surfaces->front().classname : "?";
                hits.push_back({t0, key.entityId + "/" + key.brushId + " class '" + cls + "' rules " + (r == rules->end() ? std::string("none") : std::to_string(r->second)) +
                                        " bounds " + std::to_string(prod.bounds.mn.x) + "," + std::to_string(prod.bounds.mn.y) + "," + std::to_string(prod.bounds.mn.z) + " .. " +
                                        std::to_string(prod.bounds.mx.x) + "," + std::to_string(prod.bounds.mx.y) + "," + std::to_string(prod.bounds.mx.z)});
            }
            std::sort(hits.begin(), hits.end());
            for (size_t i = 0; i < hits.size() && i < 6; ++i) MESSAGE("probe hit t=" << hits[i].first << ": " << hits[i].second);
        }
    }
    if (!env("NATIVE_CMP_CORE", "").empty()) {
        // NATIVE_CMP_CORE = bounces of the Core's bake (offline bakeLightmaps, the old editor's light): its bounce share (bake with bounces / bake without) at the
        // visible texels against the ray traced bounce share (view with bounces / view without) at the pixels of the same texels, per region.
        render::CapturedImage rtOther;
        o.bounces = !o.bounces;
        f.rt->setRtLit(true, o);
        f.cut++;
        f.shot();
        f.cut++;
        f.shot();
        REQUIRE_MESSAGE(f.converge(limitMs), "the second accumulation did not converge in time");
        rtOther = f.shot();
        REQUIRE(rtOther.ok());
        const render::CapturedImage& rtOn = o.bounces ? rtOther : rt;
        const render::CapturedImage& rtOff = o.bounces ? rt : rtOther;

        core::Scene cscene{std::filesystem::path(f.root)};
        cscene.loadGameProfile();
        cscene.openDocument(f.mapName);
        const auto surfaces = cscene.lightSurfaces();
        core::LightScene clights = cscene.lightScene();
        core::LightmapSettings lcfg;
        lcfg.playable = clights.playable;
        lcfg.lod = clights.level.lod;
        lcfg.texelsPerMeter = 16.0;
        const core::LightmapLayout layout = core::buildLightmapLayout(surfaces, cscene.lightingClasses(), lcfg);
        core::BakeOptions bo;
        bo.solids = cscene.lightSolids();
        bo.classes = cscene.lightingClasses();
        bo.albedo = 0.7;
        bo.giSamples = 64;
        auto lib = std::make_shared<core::AlbedoLibrary>(cscene.assetsRoot(), cscene.profile());
        bo.albedoLoader = [lib](const std::string& name) { return lib->get(name); };
        bo.bounces = 0;
        const core::LightmapPages direct = core::bakeLightmaps(layout, surfaces, clights, bo);
        bo.bounces = std::max(1, std::atoi(env("NATIVE_CMP_CORE", "8").c_str()));
        core::BakeStats cst;
        const core::LightmapPages full = core::bakeLightmaps(layout, surfaces, clights, bo, &cst);
        MESSAGE("Core bake: " << bo.bounces << " bounces, " << cst.texels << " texels, " << cst.seconds << " s, device " << cst.device);
        const core::TexelSamples samples = core::buildTexelSamples(layout, surfaces, bo.solids.get());
        const core::OccluderMesh mesh = core::buildOccluders(surfaces, bo.classes);
        const core::TriangleBvh bvh(mesh);
        const render::ViewState vs = f.view();
        const core::Vec3 eyeP{vs.eye[0], vs.eye[1], vs.eye[2]};
        const auto lum = [](const core::Vec3& v) { return 0.2126 * v.x + 0.7152 * v.y + 0.0722 * v.z; };
        const double shownBrightness = clights.level.brightness * 255.0;
        MESSAGE("level: exposure " << clights.level.exposure << ", brightness " << clights.level.brightness << ", ambient " << clights.ambient.x << " " << clights.ambient.y << " " << clights.ambient.z);
        double bd[17] = {}, bf[17] = {}, rOff[17] = {}, rOn[17] = {};
        size_t cnt[17] = {};
        const size_t atlas = static_cast<size_t>(samples.atlas);
        for (size_t i = 0; i < samples.count(); ++i) {
            const core::Vec3 p{samples.pos[i * 4], samples.pos[i * 4 + 1], samples.pos[i * 4 + 2]}, n{samples.nrm[i * 4], samples.nrm[i * 4 + 1], samples.nrm[i * 4 + 2]};
            const core::Vec3 d = p - eyeP;
            const double len = d.length();
            if (len < 1e-6 || n.x * d.x + n.y * d.y + n.z * d.z >= 0) continue;   // facing away
            double q[4];
            const double v[4] = {p.x, p.y, p.z, 1.0};
            for (int r = 0; r < 4; ++r) q[r] = vs.viewProj[0 * 4 + r] * v[0] + vs.viewProj[1 * 4 + r] * v[1] + vs.viewProj[2 * 4 + r] * v[2] + vs.viewProj[3 * 4 + r] * v[3];
            if (q[3] <= 1e-6) continue;
            const double nx = q[0] / q[3], ny = q[1] / q[3], nz = q[2] / q[3];
            if (nx < -1 || nx >= 1 || ny < -1 || ny >= 1 || nz < 0 || nz > 1) continue;
            const double hit = bvh.nearestHit(eyeP, d, 1.0);
            if (hit >= 0 && hit < 1.0 - 0.03 / len) continue;                      // hidden behind something
            const uint32_t px = std::min<uint32_t>(rt.width - 1, static_cast<uint32_t>((nx * 0.5 + 0.5) * rt.width));
            const uint32_t py = std::min<uint32_t>(rt.height - 1, static_cast<uint32_t>((ny * 0.5 + 0.5) * rt.height));
            const size_t pi = (static_cast<size_t>(py) * rt.width + px) * 4;
            {   // only where the two views agree on the direct light (the translucent cube is drawn unlit by the ray traced view)
                const double lb = 0.2126 * baked.rgba[pi] + 0.7152 * baked.rgba[pi + 1] + 0.0722 * baked.rgba[pi + 2];
                const double lr = 0.2126 * rtOff.rgba[pi] + 0.7152 * rtOff.rgba[pi + 1] + 0.0722 * rtOff.rgba[pi + 2];
                if (std::abs(lr - lb) > 0.3 * std::max(lb, lr) + 3.0) continue;
                const double ln = 0.2126 * rtOn.rgba[pi] + 0.7152 * rtOn.rgba[pi + 1] + 0.0722 * rtOn.rgba[pi + 2];
                if (lr > 20.0 && ln == lr) continue;   // a bright pixel that bounces do not change at all: shown unlit (a surface that does not receive light)
            }
            const uint32_t t = samples.texel[i];
            const int page = static_cast<int>(t / (atlas * atlas)), tx = static_cast<int>((t % (atlas * atlas)) % atlas), ty = static_cast<int>((t % (atlas * atlas)) / atlas);
            const size_t c = (py * 4 / rt.height) * 4 + (px * 4 / rt.width);
            for (const size_t k : {c, size_t{16}}) {
                bd[k] += lum(direct.at(page, tx, ty));
                bf[k] += lum(full.at(page, tx, ty));
                rOff[k] += std::max(0.0, 0.2126 * rtOff.rgba[pi] + 0.7152 * rtOff.rgba[pi + 1] + 0.0722 * rtOff.rgba[pi + 2] - shownBrightness);   // the display adds the level's brightness
                rOn[k] += std::max(0.0, 0.2126 * rtOn.rgba[pi] + 0.7152 * rtOn.rgba[pi + 1] + 0.0722 * rtOn.rgba[pi + 2] - shownBrightness);
                ++cnt[k];
            }
        }
        std::ostringstream g;
        g << std::fixed << std::setprecision(2);
        for (int c = 0; c < 16; ++c) g << (c % 4 == 0 ? "\n   " : " ") << (cnt[c] > 50 && bd[c] > 1e-6 && rOff[c] > 1e-6 ? (rOn[c] / rOff[c]) / (bf[c] / bd[c]) : 0.0);
        MESSAGE("bounce share, ray traced (on/off) over Core bake (with/without): all visible texels (" << cnt[16] << "): ray traced " << rOn[16] / rOff[16] << ", Core " << bf[16] / bd[16]
                                                                                                         << ", ratio " << (rOn[16] / rOff[16]) / (bf[16] / bd[16]) << "; per region (ray traced / Core):" << g.str());
        o.bounces = !o.bounces;   // back to what the first image used
    }
    const auto luma = [](const render::CapturedImage& im, size_t i) { return 0.2126 * im.rgba[i] + 0.7152 * im.rgba[i + 1] + 0.0722 * im.rgba[i + 2]; };
    double sumB = 0, sumR = 0;
    double cellB[16] = {}, cellR[16] = {};
    for (uint32_t y = 0; y < rt.height; ++y)
        for (uint32_t x = 0; x < rt.width; ++x) {
            const size_t i = (static_cast<size_t>(y) * rt.width + x) * 4;
            const double b = luma(baked, i), r = luma(rt, i);
            sumB += b;
            sumR += r;
            const size_t c = (y * 4 / rt.height) * 4 + (x * 4 / rt.width);
            cellB[c] += b;
            cellR[c] += r;
        }
    const double n = static_cast<double>(rt.width) * rt.height;
    std::ostringstream grid;
    for (int c = 0; c < 16; ++c) {
        grid << (c % 4 == 0 ? "\n   " : " ") << std::fixed << std::setprecision(2) << (cellB[c] > 1e-6 ? cellR[c] / cellB[c] : 0.0);
    }
    MESSAGE("mean luma bake " << sumB / n << ", ray traced " << sumR / n << ", ratio " << sumR / sumB << "; ratio per region (ray traced / bake):" << grid.str());
    QImage out(static_cast<int>(rt.width * 3), static_cast<int>(rt.height), QImage::Format_RGBA8888);
    for (uint32_t y = 0; y < rt.height; ++y)
        for (uint32_t x = 0; x < rt.width; ++x) {
            const size_t i = (static_cast<size_t>(y) * rt.width + x) * 4;
            for (int k = 0; k < 4; ++k) {
                out.scanLine(static_cast<int>(y))[x * 4 + k] = baked.rgba[i + k];
                out.scanLine(static_cast<int>(y))[(rt.width + x) * 4 + k] = rt.rgba[i + k];
                const int d = std::abs(static_cast<int>(baked.rgba[i + k]) - static_cast<int>(rt.rgba[i + k])) * 4;
                out.scanLine(static_cast<int>(y))[(2 * rt.width + x) * 4 + k] = k == 3 ? 255 : static_cast<uint8_t>(std::min(255, d));
            }
        }
    const fs::path dir = fs::path(NATIVE_CORE_ASSETS_DIR).parent_path().parent_path() / "native" / "build";
    out.save(QString::fromStdString((dir / env("NATIVE_CMP_OUT", "compare_bake_rt.png")).string()));
    CHECK(f.device->validationErrors() == 0);
}

// ---- R7: light styles ------------------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("RT Lit: a styled light gets a layer of its own, the grid holds the lists of every layer, styles beyond three share the last layer as constant light") {
    std::vector<core::Light> lights = directTestLights();   // style 0: point, spot, sun, sky
    core::Light a = testLight(core::LightType::Point, {0, 1, 0}, {0, 0, 1}, 2, 8);
    a.style = 3;
    core::Light b = testLight(core::LightType::Directional, {0, 0, 0}, {0, -1, 0}, 1, 0);
    b.style = 3;
    core::Light c = testLight(core::LightType::Point, {2, 1, 0}, {0, 0, 1}, 1, 8);
    c.style = 7;
    lights.push_back(a);
    lights.push_back(b);
    lights.push_back(c);
    core::LightStyleTable table = core::LightStyleTable::defaults();
    table.patterns[3] = "am";
    table.patterns[7] = "z";
    const auto data = render::makeRtLightData(lights, &table);
    REQUIRE(data->layerCount == 3);
    CHECK(data->layerStyle[0] == 0);
    CHECK(data->layerStyle[1] == 3);
    CHECK(data->layerStyle[2] == 7);
    CHECK(data->layerGlobalCount[0] == 2);   // the sun and the sky
    CHECK(data->layerGlobalCount[1] == 1);   // the styled sun
    CHECK(data->layerGlobalCount[2] == 0);
    REQUIRE(data->cellCount > 0);
    REQUIRE(data->cells.size() == static_cast<size_t>(data->cellCount) * 2 * data->layerCount);
    for (uint32_t layer = 0; layer < data->layerCount; ++layer) {
        uint32_t entries = 0;
        for (uint32_t cell = 0; cell < data->cellCount; ++cell) {
            const uint32_t off = data->cells[2 * (layer * data->cellCount + cell)], n = data->cells[2 * (layer * data->cellCount + cell) + 1];
            for (uint32_t i = 0; i < n; ++i) {
                const uint32_t k = data->index[off + i];
                ++entries;
                const int style = lights[k].style;
                CHECK(style == data->layerStyle[layer]);   // only the lights of the layer's style
            }
        }
        CHECK(entries > 0);
    }
    float w[4];
    data->layerWeights(0.0, w);
    CHECK(w[0] == 1.0f);
    CHECK(w[1] == 0.0f);                   // 'a'
    CHECK(w[2] == doctest::Approx(25.0 / 12.0));
    data->layerWeights(0.1, w);
    CHECK(w[1] == 1.0f);                   // 'm'
    CHECK(data->animated());
    core::LightStyleTable still = table;
    still.patterns[3].clear();
    still.patterns[7].clear();
    CHECK_FALSE(render::makeRtLightData(lights, &still)->animated());
    CHECK_FALSE(render::makeRtLightData(directTestLights())->animated());
    CHECK(render::makeRtLightData(directTestLights())->layerCount == 1);

    // five styles: the first two keep a layer, the rest is constant light on the last layer scaled by the mean of its pattern
    std::vector<core::Light> many;
    for (int style = 3; style <= 7; ++style) {
        core::Light l = testLight(core::LightType::Point, {double(style), 1, 0}, {0, 0, 1}, 4, 8);
        l.style = style;
        many.push_back(l);
    }
    core::LightStyleTable mt = core::LightStyleTable::defaults();
    mt.patterns[3] = "m";
    mt.patterns[4] = "m";
    mt.patterns[5] = "a";    // mean 0
    mt.patterns[6] = "m";    // mean 1
    mt.patterns[7] = "";     // constant 1
    const auto m = render::makeRtLightData(many, &mt);
    REQUIRE(m->layerCount == 4);
    CHECK(m->layerStyle[1] == 3);
    CHECK(m->layerStyle[2] == 4);
    CHECK(m->layerStyle[3] == -1);
    const auto brightnessOf = [&](size_t light) { return m->records[(light * 8 + 1) * 4]; };
    CHECK(brightnessOf(0) == doctest::Approx(4.0f));
    CHECK(brightnessOf(2) == doctest::Approx(0.0f));
    CHECK(brightnessOf(3) == doctest::Approx(4.0f));
    CHECK(brightnessOf(4) == doctest::Approx(4.0f));
    m->layerWeights(0.0, w);
    CHECK(w[3] == 1.0f);
}

TEST_CASE("RT Lit: the style layers are accumulated apart: a flicker shows at once, without a restart and without smear" * doctest::test_suite("gpu")) {
    Fixture f;
    if (!f.start(480, 300)) return;
    core::Light base = testLight(core::LightType::Point, {-0.5, 2.0, -3.5}, {0, 0, 1}, 6, 14);
    core::Light styled = testLight(core::LightType::Point, {2.5, 2.0, 0.0}, {0, 0, 1}, 6, 14);
    styled.style = 4;
    core::LightStyleTable table = core::LightStyleTable::defaults();
    table.patterns[4] = "am";   // frame 0: off, frame 1 (from 0.1 s): on
    const auto meanDiff = [](const render::CapturedImage& a, const render::CapturedImage& b) {
        double sum = 0;
        for (size_t i = 0; i < a.rgba.size(); ++i) sum += std::abs(static_cast<int>(a.rgba[i]) - static_cast<int>(b.rgba[i]));
        return sum / static_cast<double>(a.rgba.size());
    };
    f.rt->setStyleClock(0.0);
    f.rt->setRtLights(render::makeRtLightData({base}));
    f.rt->setRtLit(true, options(256));
    REQUIRE(f.shot().width == f.w);
    REQUIRE(waitFor([&] { return f.rt->sceneInfo().texturesPending == 0 && f.rt->sceneInfo().scene.queued == 0; }, 120000));
    f.cut++;
    f.shot();
    REQUIRE(f.converge());
    const render::CapturedImage baseOnly = f.shot();
    REQUIRE(baseOnly.ok());

    f.rt->setStyleClock(0.1);   // the styled light is on
    f.rt->setRtLights(render::makeRtLightData({base, styled}, &table));
    f.cut++;
    f.shot();
    REQUIRE(f.converge());
    const render::CapturedImage both = f.shot();
    REQUIRE(both.ok());
    CHECK(meanDiff(both, baseOnly) > 1.5);   // the second light is visible

    const uint64_t resets = f.info().resets;
    f.rt->setStyleClock(0.0);                // off: at once, nothing is traced again
    f.shot();
    const render::CapturedImage off = f.shot();
    CHECK(f.info().tracingSkipped);
    CHECK(f.info().resets == resets);
    CHECK(meanDiff(off, baseOnly) < 1.0);    // the same picture as without the light: the layer's history is not smeared into it
    f.rt->setStyleClock(0.1);                // on again: the accumulated layer is back as it was
    f.shot();
    const render::CapturedImage on = f.shot();
    CHECK(f.info().tracingSkipped);
    CHECK(meanDiff(on, both) < 0.5);
    savePng(both, "rt_lit_styles_on.png");
    savePng(off, "rt_lit_styles_off.png");
    CHECK(f.device->validationErrors() == 0);
}

// ---- R7: translucent surfaces -----------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("RT Lit: the glass of a level is lit by the lights, the sun's shadow rays and the bounce light, not drawn unlit" * doctest::test_suite("gpu")) {
    Fixture f;
    if (!f.start(480, 300)) return;
    // the central part of the translucent cube of demo_lit as seen from the default camera
    const auto regionLuma = [&](const render::CapturedImage& img) {
        double sum = 0;
        size_t n = 0;
        for (uint32_t y = f.h * 30 / 100; y < f.h * 60 / 100; ++y)
            for (uint32_t x = f.w * 47 / 100; x < f.w * 73 / 100; ++x) {
                const size_t i = (static_cast<size_t>(y) * img.width + x) * 4;
                sum += 0.2126 * img.rgba[i] + 0.7152 * img.rgba[i + 1] + 0.0722 * img.rgba[i + 2];
                ++n;
            }
        return sum / static_cast<double>(n);
    };
    render::RtLighting dark;   // no ambient
    f.rt->setRtLighting(dark);
    f.rt->setRtLights(render::makeRtLightData({}));
    f.rt->setRtLit(true, options(64));
    REQUIRE(f.shot().width == f.w);
    REQUIRE(waitFor([&] { return f.rt->sceneInfo().texturesPending == 0 && f.rt->sceneInfo().scene.queued == 0; }, 120000));
    f.cut++;
    f.shot();
    REQUIRE(f.converge());
    const render::CapturedImage black = f.shot();
    REQUIRE(black.ok());
    const double darkLuma = regionLuma(black);
    CHECK(darkLuma < 3.0);   // drawn unlit it would show its colours

    f.rt->setRtLights(render::makeRtLightData(directTestLights()));
    f.cut++;
    f.shot();
    REQUIRE(f.converge());
    const render::CapturedImage lit = f.shot();
    REQUIRE(lit.ok());
    const double litLuma = regionLuma(lit);
    MESSAGE("glass region luma: no light " << darkLuma << ", lit " << litLuma);
    CHECK(litLuma > darkLuma + 8.0);
    savePng(lit, "rt_lit_glass_lit.png");

    // the bounce light adds to the glass too (the walls of demo_lit are lit by the lights, the glass sees them)
    render::RtLitOptions bounce = options(64);
    bounce.bounces = true;
    f.rt->setRtLit(true, bounce);
    f.cut++;
    f.shot();
    REQUIRE(f.converge());
    const render::CapturedImage withBounce = f.shot();
    REQUIRE(withBounce.ok());
    const double bounceLuma = regionLuma(withBounce);
    MESSAGE("glass region luma with bounce light: " << bounceLuma);
    CHECK(bounceLuma >= litLuma - 1.0);
    CHECK(f.device->validationErrors() == 0);
}
