// T6.2: per-chart texel samples on the GPU: the sample arena read back equals the Core's samples, the chunk table (CPU copy and GPU mirror) describes them.
// [gpu]: skips itself without a Vulkan device.
#include <doctest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/lightbake.h"
#include "core/lighting_classes.h"
#include "core/scene.h"
#include "gpu/device.h"
#include "gpu/upload_ring.h"
#include "light/layout.h"
#include "light/samples.h"

using namespace native;
namespace fs = std::filesystem;

namespace {

// A one-shot command buffer on the graphics queue, waited for.
class OneShot {
public:
    explicit OneShot(gpu::Device& d) : d_(d) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = d.graphicsFamily();
        vkCreateCommandPool(d.device(), &pi, nullptr, &pool_);
    }
    ~OneShot() { vkDestroyCommandPool(d_.device(), pool_, nullptr); }
    void run(const std::function<void(VkCommandBuffer)>& record) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(d_.device(), &ai, &cmd);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        record(cmd);
        vkEndCommandBuffer(cmd);
        VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cbi.commandBuffer = cmd;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &cbi;
        vkQueueSubmit2(d_.graphicsQueue(), 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(d_.graphicsQueue());
        vkFreeCommandBuffers(d_.device(), pool_, 1, &cmd);
    }

private:
    gpu::Device& d_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
};

std::vector<uint8_t> readBack(gpu::Device& d, OneShot& shot, VkBuffer src, uint64_t size) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation a = nullptr;
    VmaAllocationInfo info{};
    REQUIRE(vmaCreateBuffer(d.allocator(), &bci, &aci, &buf, &a, &info) == VK_SUCCESS);
    shot.run([&](VkCommandBuffer cmd) {
        VkBufferCopy r{0, 0, size};
        vkCmdCopyBuffer(cmd, src, buf, 1, &r);
    });
    vmaInvalidateAllocation(d.allocator(), a, 0, VK_WHOLE_SIZE);
    std::vector<uint8_t> out(size);
    std::memcpy(out.data(), info.pMappedData, size);
    vmaDestroyBuffer(d.allocator(), buf, a);
    return out;
}

} // namespace

TEST_CASE("sample arena: the uploaded per-chart samples equal buildTexelSamples (demo_lit), the chunk table describes them" * doctest::test_suite("gpu")) {
    gpu::DeviceOptions o;
    o.validation = true;
    std::string err;
    auto device = gpu::Device::create(o, &err);
    if (!device) {
        MESSAGE("no Vulkan device (" << err << "): GPU test skipped");
        return;
    }
    const fs::path root = NATIVE_CORE_ASSETS_DIR;
    if (!fs::exists(root / "maps/demo_lit.map.json")) return;
    core::Scene scene(root);
    scene.loadGameProfile();
    scene.openDocument("maps/demo_lit.map.json");
    const core::LightmapSettings cfg = scene.lightmapLayout().settings;
    const std::vector<core::LightSurface> surfaces = scene.lightSurfaces();
    const std::shared_ptr<core::SolidSet> solids = scene.lightSolids();

    light::LiveLayout live(cfg);
    live.buildAll(surfaces, scene.lightingClasses());
    const core::LightmapLayout& layout = live.layout();
    const auto pieces = light::piecesByChart(layout, surfaces);
    const core::TexelSamples whole = core::buildTexelSamples(layout, surfaces, solids.get());
    REQUIRE(whole.count() > 1000);
    std::map<uint32_t, size_t> at;
    for (size_t i = 0; i < whole.count(); ++i) at[whole.texel[i]] = i;

    for (const int giScale : {1, 2}) {
        INFO("giScale " << giScale);
        light::SampleStore store(*device, giScale > 1 ? 512ull << 20 : 64ull << 20, 4096);
        REQUIRE(store.valid());
        gpu::UploadRing ring(*device, {giScale > 1 ? 384ull << 20 : 64ull << 20, 2});
        OneShot shot(*device);
        REQUIRE(ring.beginFrame(0));
        std::vector<light::PreparedChart> prepared;
        size_t gridTotal = 0, hiddenTotal = 0;
        for (size_t c = 0; c < live.charts().size(); ++c) {
            prepared.push_back(light::prepareChart(live, static_cast<int>(c), pieces[c], solids.get(), giScale));
            gridTotal += prepared.back().gridCount;
            hiddenTotal += prepared.back().hidden;
        }
        size_t total = 0;
        shot.run([&](VkCommandBuffer cmd) {
            for (const light::PreparedChart& p : prepared) {
                REQUIRE(store.upload(cmd, ring, p, p.page));
                total += p.count;
            }
            VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            mb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
            VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            di.memoryBarrierCount = 1;
            di.pMemoryBarriers = &mb;
            vkCmdPipelineBarrier2(cmd, &di);
        });
        ring.endFrame(1);
        CHECK(total == whole.count());
        CHECK(store.liveCharts() > 4);
        if (giScale == 2) CHECK(gridTotal == core::buildGiSamples(whole, 2).count());
        (void)hiddenTotal;

        const std::vector<uint8_t> arena = readBack(*device, shot, store.sampleBuffer(), store.sampleArena().capacity());
        const std::vector<uint8_t> table = readBack(*device, shot, store.chunkBuffer(), static_cast<uint64_t>(store.chunkRows()) * sizeof(light::GpuChunk));
        REQUIRE(table.size() == store.chunkRows() * sizeof(light::GpuChunk));
        // the GPU copy of the chunk table is the CPU copy
        CHECK(std::memcmp(table.data(), &store.chunk(0), table.size()) == 0);

        size_t seen = 0, chunksSeen = 0, gridSeen = 0;
        const uint32_t atlas = static_cast<uint32_t>(whole.atlas);
        for (size_t c = 0; c < live.charts().size(); ++c) {
            const auto* entry = store.chart(static_cast<int>(c));
            if (!entry) { CHECK(prepared[c].count == 0); continue; }
            CHECK(entry->count == prepared[c].count);
            uint32_t covered = 0;
            for (uint32_t k = 0; k < entry->chunkCount; ++k) {
                const light::GpuChunk& g = store.chunk(entry->firstChunk + k);
                ++chunksSeen;
                REQUIRE(g.chart == c);
                REQUIRE(g.page == static_cast<uint32_t>(live.charts()[c].where.page));
                const uint32_t rx = g.rectXY & 0xffffu, ry = g.rectXY >> 16, rw = g.rectWH & 0xffffu, rh = g.rectWH >> 16;
                REQUIRE(rw >= 1);
                REQUIRE(rh >= 1);
                REQUIRE(rw <= static_cast<uint32_t>(live.chunkTexels()));
                REQUIRE(rh <= static_cast<uint32_t>(live.chunkTexels()));
                covered += g.sampleCount;
                for (uint32_t i = 0; i < g.sampleCount; ++i) {
                    ++seen;
                    const auto* rec = reinterpret_cast<const light::SampleRecord*>(arena.data() + static_cast<size_t>(g.sampleBase + 2 * i) * 16);
                    const uint32_t x = rec->xy & 0xffffu, y = rec->xy >> 16;
                    const uint32_t texel = (rec->page * atlas + y) * atlas + x;
                    const auto it = at.find(texel);
                    REQUIRE(it != at.end());
                    const size_t w = it->second;
                    REQUIRE(rec->page == g.page);
                    REQUIRE(std::memcmp(rec->pos, &whole.pos[w * 4], 3 * sizeof(float)) == 0);
                    float n[3];
                    light::decodeNormalOct(rec->normalOct, n);
                    for (int a = 0; a < 3; ++a) REQUIRE(std::fabs(n[a] - whole.nrm[w * 4 + static_cast<size_t>(a)]) < 2e-4f);
                    REQUIRE(rec->randomId == light::randomIdOfTexel(texel));
                    const auto* sub = reinterpret_cast<const float*>(arena.data() + static_cast<size_t>(g.subBase + 3 * i) * 16);
                    REQUIRE(std::memcmp(sub, &whole.sub[w * 12], 12 * sizeof(float)) == 0);
                    // inside the chunk rectangle unless it is a padding texel (clamped to the nearest chunk)
                    const bool inside = x >= rx && x < rx + rw && y >= ry && y < ry + rh;
                    const bool nearEdge = x + 3 > rx && x < rx + rw + 3 && y + 3 > ry && y < ry + rh + 3;
                    REQUIRE(nearEdge);
                    (void)inside;
                    for (int a = 0; a < 3; ++a) {
                        REQUIRE(rec->pos[a] >= g.boundsMin[a]);
                        REQUIRE(rec->pos[a] <= g.boundsMax[a]);
                    }
                }
                gridSeen += g.gridCount;
                for (uint32_t j = 0; j < g.gridCount; ++j) {
                    const auto* rec = reinterpret_cast<const light::SampleRecord*>(arena.data() + static_cast<size_t>(g.gridBase + 2 * j) * 16);
                    REQUIRE(rec->subFirst < entry->count);
                }
            }
            CHECK(covered == entry->count);
        }
        CHECK(seen == whole.count());
        CHECK(chunksSeen == store.chunkRows());
        CHECK(gridSeen == gridTotal);

        // releasing a chart gives its space back; uploading it again works
        const uint64_t used = store.sampleBytes();
        size_t victim = 0;
        while (!store.chart(static_cast<int>(victim))) ++victim;
        store.release(static_cast<int>(victim));
        CHECK(store.sampleBytes() < used);
        CHECK(store.chart(static_cast<int>(victim)) == nullptr);
        REQUIRE(ring.beginFrame(1));
        shot.run([&](VkCommandBuffer cmd) { REQUIRE(store.upload(cmd, ring, prepared[victim], prepared[victim].page)); });
        ring.endFrame(2);
        CHECK(store.sampleBytes() == used);
        REQUIRE(store.chart(static_cast<int>(victim)));
    }
    CHECK(device->validationErrors() == 0);
}

TEST_CASE("sample arena: the octahedral normal round trip stays within 1e-4") {
    const float dirs[][3] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {0, -1, 0}, {0.577350f, 0.577350f, -0.577350f}, {-0.3f, 0.5f, -0.811f}, {0.9f, -0.1f, 0.4219f}};
    for (const auto& d : dirs) {
        float v[3] = {d[0], d[1], d[2]};
        const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        for (float& x : v) x /= l;
        float back[3];
        light::decodeNormalOct(light::encodeNormalOct(v), back);
        for (int a = 0; a < 3; ++a) CHECK(std::fabs(back[a] - v[a]) < 1e-4f);
    }
}
