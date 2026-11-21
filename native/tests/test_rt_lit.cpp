// R2: the ray traced Lit viewport. A viewport in Lighting only / Lit on demo_lit shows ambient * aoFactor(o) per pixel, with `o` the running mean of one cosine-weighted
// ray a frame. Checks: the converged picture equals a CPU reference (core::TriangleBvh at the surface point of 2000 pixels, the bake's occlusion model) and the
// accumulation stops tracing once converged; a still camera keeps its picture; resets (camera cut, resize, mode switch, scene edit, occlusion settings) restart it
// and a change of the display only (exposure) does not; the a-trous fallback is closer to the converged picture than the raw first frames; 0 validation errors.
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
#include <random>
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
#include "render/camera_rig.h"
#include "render/render_thread.h"
#include "scene/builder.h"
#include "scene/render_mesh.h"

#include <core/profile.h>
#include <core/raytrace.h>
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
    HWND hwnd = CreateWindowExW(0, kClass, L"native rt lit test", WS_OVERLAPPEDWINDOW, 60, 60, r.right - r.left, r.bottom - r.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
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
        const fs::path root = NATIVE_CORE_ASSETS_DIR;
        if (!fs::exists(root / "maps/demo_lit.map.json")) return false;
        snapshot = buildSnapshot(root, "maps/demo_lit.map.json", pool);
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
