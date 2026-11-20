// T6.4: the lighting service end to end. A scene snapshot of demo_lit goes in; the lit meshes, the mailboxes and the display pages come out and equal what
// core::LiveBaker shows (bounces off); every lit triangle samples a baked texel. [gpu]: skips itself without a ray tracing device with a second queue.
#include <doctest.h>

#include "scene/snapshot.h"   // first: core headers use `near` as a name, which windows.h (via volk) defines as a macro

#include <algorithm>
#include <chrono>
#include <set>
#include <cmath>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "base/jobs.h"
#include "doc/changeset.h"
#include "doc/io.h"
#include "doc/prefabs.h"
#include "light/light_link.h"
#include "light/lighting_service.h"
#include "light/mailbox.h"
#include "light/page_images.h"
#include "light_bake_fixture.h"
#include "scene/builder.h"

#include <core/profile.h>
#include <core/texplan.h>

using namespace native;
using native_test::LightFixture;
using native_test::OneShot;
using native_test::SubmitSync;
namespace fs = std::filesystem;

namespace {

float half(uint16_t h) { return core::halfToFloat(h); }

// The render thread's part: scatters the newest mailbox of the current epoch into `display`; returns whether one was scattered.
bool pump(gpu::Device& device, light::LightLink& link, light::Scatter& scatter, OneShot& shot, light::PageImages& display, uint64_t& renderValue) {
    light::Mailbox& mb = link.mailbox();
    const light::Mailbox::Taken t = mb.takeNewest();
    if (t.slot < 0) return false;
    const auto set = link.current();
    ++renderValue;
    SubmitSync sync;
    sync.wait = mb.bakeTimeline();
    sync.waitValue = t.bakeValue;
    sync.signal = mb.renderTimeline();
    sync.signalValue = renderValue;
    if (set && t.bakeValue >= set->firstBakeValue) {
        shot.run([&](VkCommandBuffer cmd) { REQUIRE(scatter.record(cmd, mb.buffer(t.slot), t.entries, display.view())); }, sync);
    } else {
        shot.run([&](VkCommandBuffer) {}, sync);   // an older epoch's mailbox: dropped
    }
    mb.consumed(t.slot, renderValue);
    scatter.resetDescriptors();
    (void)device;
    return true;
}

} // namespace

TEST_CASE("lighting service: demo_lit snapshot to display pages equals LiveBaker (bounces off), lit meshes sample baked texels" * doctest::test_suite("gpu")) {
    gpu::DeviceOptions o;
    o.validation = true;
    std::string err;
    auto device = gpu::Device::create(o, &err);
    if (!device || !device->caps().lit() || device->computeQueue() == device->graphicsQueue()) {
        MESSAGE("no ray tracing device with a second queue (" << err << "): GPU test skipped");
        return;
    }
    const fs::path root = NATIVE_CORE_ASSETS_DIR;
    const std::string map = "maps/demo_lit.map.json";
    if (!fs::exists(root / map)) return;

    // the reference and the page count first (the fixture uses the compute queue, the service thread is not running yet)
    LightFixture f(*device);
    REQUIRE(f.load(root, map));
    const light::DirectParams params;
    const std::unique_ptr<core::LiveBaker> ref = native_test::makeReference(f, 32, params);
    const int atlas = f.live->layout().settings.atlasSize;
    const int pages = f.live->layout().pageCount;
    light::PageImages display(*device, atlas, VK_FORMAT_R16G16B16A16_SFLOAT, true);
    REQUIRE(display.ensureLayers(pages));

    // the scene snapshot, as the editor makes it
    const doc::LoadResult loaded = doc::loadDocumentFile(root / map, map);
    REQUIRE(loaded.version);
    auto profile = std::make_shared<core::GameProfile>(core::loadProfile(root));
    auto texEnv = std::make_shared<core::texplan::TexEnvironment>(root, *profile);
    scene::ReliefEnv env;
    env.info = [texEnv](const std::string& p) { return texEnv->info(p); };
    env.groups = [texEnv](const std::string& n) { return texEnv->group(n); };
    env.revision = 1;
    base::JobPool pool(8);
    scene::SceneBuilder builder(pool);
    builder.setReliefEnv(env);
    doc::PrefabLibrary prefabLib(root);
    doc::PrefabResolver resolver(prefabLib);
    builder.submit(loaded.version, doc::allChanged(0, loaded.version->version), resolver.expand(*loaded.version));
    REQUIRE(builder.waitIdle(120000));
    const auto snapshot = builder.snapshot();
    REQUIRE(snapshot);

    light::LightLink link(*device);
    REQUIRE(link.valid());
    link.setWanted(true);
    light::LightingOptions opts;
    opts.defaultSourceSize = 0;   // the reference takes the lights as authored
    opts.defaultSunSize = 0;
    light::LightingService service(*device, link, opts);
    REQUIRE(service.valid());
    service.setInputs({snapshot, profile, root});

    light::Scatter scatter(*device);
    REQUIRE(scatter.valid());
    OneShot renderShot(*device, false);
    uint64_t renderValue = 0;
    const auto t0 = std::chrono::steady_clock::now();
    bool converged = false;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 120) {
        pump(*device, link, scatter, renderShot, display, renderValue);
        if (service.waitConverged(10)) {
            converged = true;
            break;
        }
    }
    REQUIRE(converged);
    while (pump(*device, link, scatter, renderShot, display, renderValue)) {}   // the last mailboxes
    const light::LightingService::Status st = service.status();
    MESSAGE("epoch " << st.epoch << ": " << st.chunks << " chunks, " << st.samples << " samples, " << st.pages << " pages, build " << st.buildSeconds << " s, refine " << st.refineSeconds
                     << " s");
    CHECK_MESSAGE(st.error.empty(), st.error);
    CHECK(st.chunks > 0);
    CHECK(static_cast<int>(st.pages) == pages);

    // lit meshes
    const auto set = link.current();
    REQUIRE(set);
    CHECK(set->pageCount == pages);
    CHECK(set->atlasSize == atlas);
    CHECK(!set->objects.empty());
    {
        const core::LightmapLayout& a = *set->layout;
        const core::LightmapLayout& b = f.live->layout();
        CHECK(a.charts.size() == b.charts.size());
        CHECK(a.faces.size() == b.faces.size());
        size_t different = 0;
        for (size_t c = 0; c < std::min(a.charts.size(), b.charts.size()); ++c)
            if (a.charts[c].page != b.charts[c].page || a.charts[c].x != b.charts[c].x || a.charts[c].y != b.charts[c].y || a.charts[c].w != b.charts[c].w || a.charts[c].h != b.charts[c].h) ++different;
        CHECK(different == 0);
        {
            std::set<std::string> have;
            for (const core::LightmapFace& fa : a.faces) have.insert(fa.key());
            int shown = 0;
            for (const core::LightmapFace& fb : b.faces)
                if (!have.count(fb.key()) && shown++ < 6) MESSAGE("only in the Core's layout: " << fb.key());
        }
        MESSAGE("charts " << a.charts.size() << " vs " << b.charts.size() << ", " << different << " placed differently; faces " << a.faces.size() << " vs " << b.faces.size());
    }
    std::vector<std::vector<uint16_t>> mine(static_cast<size_t>(pages));
    for (int p = 0; p < pages; ++p) REQUIRE(display.readLayer(p, mine[static_cast<size_t>(p)]));
    std::vector<std::vector<uint16_t>> theirs(static_cast<size_t>(pages));
    for (int page = 0; page < pages; ++page) {
        core::LiveBaker::Rect r;
        r.page = page;
        r.x = 0;
        r.y = 0;
        r.w = atlas;
        r.h = atlas;
        theirs[static_cast<size_t>(page)] = ref->renderTile(r);
        REQUIRE(theirs[static_cast<size_t>(page)].size() == mine[static_cast<size_t>(page)].size());
    }
    size_t tris = 0, sampled = 0, shown = 0, agree = 0;
    for (const auto& [key, lo] : set->objects) {
        const scene::RenderMesh& m = *lo.lit;
        CHECK(m.indices.size() == lo.source->indices.size());
        for (size_t t = 0; t < m.indices.size() / 3; ++t) {
            const scene::RenderVertex& a = m.vertices[m.indices[t * 3]];
            if (!(a.flags & scene::kFlagLit)) continue;
            float u = 0, v = 0;
            const scene::RenderVertex& b = m.vertices[m.indices[t * 3 + 1]];
            const scene::RenderVertex& c = m.vertices[m.indices[t * 3 + 2]];
            const double area = 0.5 * std::fabs((static_cast<double>(b.lm[0]) - a.lm[0]) * (static_cast<double>(c.lm[1]) - a.lm[1]) -
                                                (static_cast<double>(c.lm[0]) - a.lm[0]) * (static_cast<double>(b.lm[1]) - a.lm[1])) * atlas * atlas;
            if (area < 16) continue;   // smaller than a few texels: its centre texel may hold no sample
            ++tris;
            for (int k = 0; k < 3; ++k) {
                const scene::RenderVertex& vk = m.vertices[m.indices[t * 3 + static_cast<size_t>(k)]];
                CHECK(vk.page == a.page);
                u += vk.lm[0] / 3;
                v += vk.lm[1] / 3;
            }
            REQUIRE(a.page < pages);
            const int x = std::clamp(static_cast<int>(u * static_cast<float>(atlas)), 0, atlas - 1), y = std::clamp(static_cast<int>(v * static_cast<float>(atlas)), 0, atlas - 1);
            const size_t at = (static_cast<size_t>(y) * static_cast<size_t>(atlas) + static_cast<size_t>(x)) * 4 + 3;
            const bool baked = mine[a.page][at] != 0;
            if (baked) ++sampled;
            if (baked == (theirs[a.page][at] != 0)) ++agree;
            else if (shown++ < 8) MESSAGE("centre differs from the Core: " << key.entityId << "/" << key.brushId << " page " << a.page << " texel " << x << "," << y);
        }
    }
    // the rest are faces buried in other brushes (a wall's bottom on the floor): no texel of them is baked, in the Core's pages either
    MESSAGE(tris << " lit triangles of 16 texels or more, " << sampled << " of them sample a baked texel at their centre, " << agree << " agree with the Core's pages");
    CHECK(tris > 100);
    CHECK(sampled > tris / 2);
    CHECK(agree == tris);

    // display pages against the Core
    std::vector<double> rel;
    size_t missing = 0, both = 0;
    for (int page = 0; page < pages; ++page) {
        const std::vector<uint16_t>& th = theirs[static_cast<size_t>(page)];
        const std::vector<uint16_t>& m = mine[static_cast<size_t>(page)];
        for (size_t t = 0; t < th.size() / 4; ++t) {
            const bool a = th[t * 4 + 3] != 0, b = m[t * 4 + 3] != 0;
            if (a && !b) ++missing;
            if (!(a && b)) continue;
            ++both;
            for (int ch = 0; ch < 3; ++ch) {
                const float want = half(th[t * 4 + static_cast<size_t>(ch)]);
                if (want > 0.01f) rel.push_back(std::fabs(half(m[t * 4 + static_cast<size_t>(ch)]) - want) / want);
            }
        }
    }
    std::sort(rel.begin(), rel.end());
    REQUIRE(!rel.empty());
    double sum = 0;
    for (double v : rel) sum += v;
    const double mean = sum / static_cast<double>(rel.size()), p99 = rel[std::min(rel.size() - 1, static_cast<size_t>(static_cast<double>(rel.size()) * 0.99))];
    MESSAGE(both << " texels on both sides, " << missing << " only in the Core's page; mean " << mean * 100 << " %, p99 " << p99 * 100 << " %");
    CHECK(both > 100000);
    CHECK(missing == 0);
    CHECK(mean < 0.01);
    CHECK(p99 < 0.05);
    CHECK(device->validationErrors() == 0);
}
