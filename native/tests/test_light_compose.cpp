// T6.4: compose + mailbox + scatter. The display pages the render thread ends up with equal what core::LiveBaker::renderTile shows (bounces off), the mailbox ring
// follows the hand-over rules (06_LIGHTING.md §8). [gpu]: skips itself without a ray tracing device.
#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "light/mailbox.h"
#include "light/page_images.h"
#include "light_bake_fixture.h"

using namespace native;
using native_test::LightFixture;
using native_test::OneShot;
using native_test::SubmitSync;
namespace fs = std::filesystem;

namespace {

float half(uint16_t h) { return core::halfToFloat(h); }

} // namespace

TEST_CASE("compose, mailbox and scatter: the display pages equal LiveBaker::renderTile (demo_lit with ambient and occlusion)" * doctest::test_suite("gpu")) {
    gpu::DeviceOptions o;
    o.validation = true;
    std::string err;
    auto device = gpu::Device::create(o, &err);
    if (!device || !device->caps().lit()) {
        MESSAGE("no ray tracing device (" << err << "): GPU test skipped");
        return;
    }
    LightFixture f(*device);
    const auto edit = [](core::LightScene& l) {
        l.ambient = core::Vec3{0.03, 0.03, 0.04};
        l.level.ao.radius = 2.0;
        l.level.ao.rays = 128;
        l.level.ao.intensity = 0.8;
    };
    if (!f.load(NATIVE_CORE_ASSETS_DIR, "maps/demo_lit.map.json", edit)) return;
    const light::DirectParams params;
    f.bake(32, params);

    const int atlas = f.live->layout().settings.atlasSize, pad = f.live->layout().settings.padding;
    const int pages = f.live->layout().pageCount;
    REQUIRE(pages > 0);
    light::PageImages slotMap(*device, atlas, VK_FORMAT_R32_UINT), display(*device, atlas);
    REQUIRE(slotMap.ensureLayers(pages));
    REQUIRE(display.ensureLayers(pages));

    // chunk rectangles with their gutter
    std::vector<light::ComposeJob> rects;
    for (size_t c = 0; c < f.live->charts().size(); ++c) {
        const auto* e = f.store->chart(static_cast<int>(c));
        if (!e) continue;
        for (uint32_t k = 0; k < e->chunkCount; ++k) {
            const light::GpuChunk& g = f.store->chunk(e->firstChunk + k);
            const int x = static_cast<int>(g.rectXY & 0xffffu), y = static_cast<int>(g.rectXY >> 16), w = static_cast<int>(g.rectWH & 0xffffu), h = static_cast<int>(g.rectWH >> 16);
            const int x0 = std::max(0, x - pad), y0 = std::max(0, y - pad), x1 = std::min(atlas, x + w + pad), y1 = std::min(atlas, y + h + pad);
            rects.push_back({g.page, static_cast<uint32_t>(x0), static_cast<uint32_t>(y0), static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)});
        }
    }
    light::JobList slotJobs(*device, static_cast<uint32_t>(rects.size()) + 1), composeJobs(*device, static_cast<uint32_t>(rects.size()) + 1);
    REQUIRE(slotJobs.set(f.allJobs(0)));
    REQUIRE(composeJobs.setCompose(rects));
    light::Mailbox mailbox(*device, composeJobs.total() + 16);
    REQUIRE(mailbox.valid());
    light::Scatter scatter(*device);
    REQUIRE(scatter.valid());

    // bake side: slot map, then compose into a mailbox, signalling the bake timeline
    const int slot = mailbox.acquire();
    REQUIRE(slot >= 0);
    light::ComposeParams cp;
    cp.atlas = static_cast<uint32_t>(atlas);
    cp.ambient[0] = static_cast<float>(f.lights.ambient.x);
    cp.ambient[1] = static_cast<float>(f.lights.ambient.y);
    cp.ambient[2] = static_cast<float>(f.lights.ambient.z);
    const core::AoSettings& ao = f.lights.level.ao;
    cp.aoActive = ao.active();
    cp.aoStrength = static_cast<float>(ao.strength);
    cp.aoPower = static_cast<float>(ao.power);
    cp.aoIntensity = static_cast<float>(ao.intensity);
    SubmitSync bakeSync;
    bakeSync.signal = mailbox.bakeTimeline();
    bakeSync.signalValue = 1;
    f.shot.run(
        [&](VkCommandBuffer cmd) {
            REQUIRE(f.passes->recordSlotMap(cmd, *f.store, slotJobs, slotMap.view()));
            REQUIRE(f.passes->recordCompose(cmd, *f.results, slotMap.view(), composeJobs, mailbox.buffer(slot), cp));
        },
        bakeSync);
    mailbox.commit(slot, composeJobs.total(), 1);
    CHECK(mailbox.completedBake() == 1);

    // render side: take the newest finished mailbox, scatter it on the graphics queue
    const light::Mailbox::Taken taken = mailbox.takeNewest();
    REQUIRE(taken.slot == slot);
    CHECK(taken.entries == composeJobs.total());
    OneShot renderShot(*device, false);
    SubmitSync renderSync;
    renderSync.wait = mailbox.bakeTimeline();
    renderSync.waitValue = taken.bakeValue;
    renderSync.signal = mailbox.renderTimeline();
    renderSync.signalValue = 1;
    renderShot.run([&](VkCommandBuffer cmd) { REQUIRE(scatter.record(cmd, mailbox.buffer(taken.slot), taken.entries, display.view())); }, renderSync);
    mailbox.consumed(taken.slot, 1);
    CHECK(mailbox.consumedBakeValue() == 1);
    CHECK(mailbox.completedRender() == 1);

    // against the Core
    const std::unique_ptr<core::LiveBaker> ref = native_test::makeReference(f, 32, params);
    std::vector<double> rel;
    size_t missing = 0, extra = 0, both = 0;
    for (int page = 0; page < pages; ++page) {
        std::vector<uint16_t> mine;
        REQUIRE(display.readLayer(page, mine));
        core::LiveBaker::Rect r;
        r.page = page;
        r.x = 0;
        r.y = 0;
        r.w = atlas;
        r.h = atlas;
        const std::vector<uint16_t> theirs = ref->renderTile(r);
        REQUIRE(theirs.size() == mine.size());
        for (size_t t = 0; t < theirs.size() / 4; ++t) {
            const bool a = theirs[t * 4 + 3] != 0, b = mine[t * 4 + 3] != 0;
            if (a && !b) ++missing;
            if (!a && b) ++extra;
            if (!(a && b)) continue;
            ++both;
            for (int ch = 0; ch < 3; ++ch) {
                const float want = half(theirs[t * 4 + static_cast<size_t>(ch)]);
                if (want > 0.01f) rel.push_back(std::fabs(half(mine[t * 4 + static_cast<size_t>(ch)]) - want) / want);
            }
        }
    }
    std::sort(rel.begin(), rel.end());
    REQUIRE(!rel.empty());
    double sum = 0;
    for (double v : rel) sum += v;
    const double mean = sum / static_cast<double>(rel.size()), p99 = rel[std::min(rel.size() - 1, static_cast<size_t>(static_cast<double>(rel.size()) * 0.99))];
    MESSAGE(both << " texels with a value on both sides, " << missing << " only in the Core's page, " << extra << " only here (a wider gutter); mean " << mean * 100 << " %, p99 " << p99 * 100
                 << " %, worst " << rel.back() * 100 << " %");
    CHECK(both > 100000);
    CHECK(missing == 0);
    CHECK(mean < 0.01);
    CHECK(p99 < 0.05);
    CHECK(device->validationErrors() == 0);
}

TEST_CASE("mailbox ring: newest finished mailbox wins, slots return after the render timeline, nothing is lost" * doctest::test_suite("gpu")) {
    gpu::DeviceOptions o;
    o.validation = true;
    std::string err;
    auto device = gpu::Device::create(o, &err);
    if (!device) {
        MESSAGE("no Vulkan device (" << err << "): GPU test skipped");
        return;
    }
    light::Mailbox mb(*device, 64, 3);
    REQUIRE(mb.valid());
    const auto signal = [&](VkSemaphore s, uint64_t v) {
        VkSemaphoreSignalInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        si.semaphore = s;
        si.value = v;
        REQUIRE(vkSignalSemaphore(device->device(), &si) == VK_SUCCESS);
    };
    const int a = mb.acquire(), b = mb.acquire(), c = mb.acquire();
    CHECK(a >= 0);
    CHECK(b >= 0);
    CHECK(c >= 0);
    CHECK(mb.acquire() == -1);   // all three are in use
    mb.commit(a, 10, 1);
    mb.commit(b, 20, 2);
    mb.abandon(c);
    CHECK(mb.takeNewest().slot == -1);   // the bake timeline has not reached 1
    signal(mb.bakeTimeline(), 1);
    light::Mailbox::Taken t = mb.takeNewest();
    CHECK(t.slot == a);
    CHECK(t.entries == 10);
    signal(mb.bakeTimeline(), 2);
    t = mb.takeNewest();   // a is still committed (not consumed), b is newer: a is dropped
    CHECK(t.slot == b);
    CHECK(t.entries == 20);
    CHECK(t.bakeValue == 2);
    mb.consumed(b, 1);
    CHECK(mb.consumedBakeValue() == 2);
    CHECK(mb.takeNewest().slot == -1);   // nothing newer
    // a was dropped (free), c was abandoned (free); b returns only after the render timeline reached 1
    const int x = mb.acquire(), y = mb.acquire();
    CHECK(x >= 0);
    CHECK(y >= 0);
    CHECK(x != b);
    CHECK(y != b);
    CHECK(mb.acquire() == -1);
    signal(mb.renderTimeline(), 1);
    CHECK(mb.acquire() == b);
    CHECK(device->validationErrors() == 0);
}
