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
#include "native_shaders/accel_test_comp.spv.h"

using namespace native;
namespace fs = std::filesystem;

namespace {

struct RayHit {
    float t, b1, b2;
    uint32_t object;
};

// Casts the rays (origin, tmax, direction) against the TLAS with the test shader.
class RayCaster {
public:
    explicit RayCaster(gpu::Device& d) : d_(d) {
        VkDescriptorSetLayoutBinding b[3] = {{0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                             {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                             {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 3;
        li.pBindings = b;
        vkCreateDescriptorSetLayout(d.device(), &li, nullptr, &setLayout_);
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &setLayout_;
        vkCreatePipelineLayout(d.device(), &pl, nullptr, &layout_);
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = sizeof(shaders::k_accel_test_comp_spv);
        mi.pCode = shaders::k_accel_test_comp_spv;
        vkCreateShaderModule(d.device(), &mi, nullptr, &module_);
        VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module_, "main", nullptr};
        pi.layout = layout_;
        vkCreateComputePipelines(d.device(), VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline_);
        VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 8}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 8;
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes = sizes;
        vkCreateDescriptorPool(d.device(), &dpi, nullptr, &pool_);
        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cp.queueFamilyIndex = d.computeFamily();
        vkCreateCommandPool(d.device(), &cp, nullptr, &cmdPool_);
    }
    ~RayCaster() {
        vkDeviceWaitIdle(d_.device());
        vkDestroyCommandPool(d_.device(), cmdPool_, nullptr);
        vkDestroyDescriptorPool(d_.device(), pool_, nullptr);
        vkDestroyPipeline(d_.device(), pipeline_, nullptr);
        vkDestroyShaderModule(d_.device(), module_, nullptr);
        vkDestroyPipelineLayout(d_.device(), layout_, nullptr);
        vkDestroyDescriptorSetLayout(d_.device(), setLayout_, nullptr);
    }

    std::vector<RayHit> cast(VkAccelerationStructureKHR tlas, const std::vector<float>& rays) {
        const size_t n = rays.size() / 8;
        Buf in = makeBuf(rays.size() * sizeof(float));
        Buf out = makeBuf(n * 16);
        std::memcpy(in.mapped, rays.data(), rays.size() * sizeof(float));
        vmaFlushAllocation(d_.allocator(), in.alloc, 0, VK_WHOLE_SIZE);
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &setLayout_;
        VkDescriptorSet set = VK_NULL_HANDLE;
        vkAllocateDescriptorSets(d_.device(), &ai, &set);
        VkWriteDescriptorSetAccelerationStructureKHR as{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        as.accelerationStructureCount = 1;
        as.pAccelerationStructures = &tlas;
        VkDescriptorBufferInfo bi[2] = {{in.buffer, 0, VK_WHOLE_SIZE}, {out.buffer, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet w[3] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &as, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, nullptr, nullptr, nullptr},
                                     {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bi[0], nullptr},
                                     {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bi[1], nullptr}};
        vkUpdateDescriptorSets(d_.device(), 3, w, 0, nullptr);

        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = cmdPool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(d_.device(), &cai, &cmd);
        VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &cbi);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(cmd, static_cast<uint32_t>((n + 63) / 64), 1, 1);
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &di);
        vkEndCommandBuffer(cmd);
        VkCommandBufferSubmitInfo sbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        sbi.commandBuffer = cmd;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &sbi;
        vkQueueSubmit2(d_.computeQueue(), 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(d_.computeQueue());
        vmaInvalidateAllocation(d_.allocator(), out.alloc, 0, VK_WHOLE_SIZE);
        std::vector<RayHit> hits(n);
        for (size_t i = 0; i < n; ++i) {
            const float* h = reinterpret_cast<const float*>(out.mapped) + i * 4;
            hits[i].t = h[0];
            hits[i].b1 = h[1];
            hits[i].b2 = h[2];
            std::memcpy(&hits[i].object, &h[3], 4);
        }
        vkFreeCommandBuffers(d_.device(), cmdPool_, 1, &cmd);
        vkResetDescriptorPool(d_.device(), pool_, 0);
        vmaDestroyBuffer(d_.allocator(), in.buffer, in.alloc);
        vmaDestroyBuffer(d_.allocator(), out.buffer, out.alloc);
        return hits;
    }

private:
    struct Buf {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation alloc = nullptr;
        void* mapped = nullptr;
    };
    Buf makeBuf(size_t size) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = std::max<size_t>(size, 16);
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        Buf b;
        VmaAllocationInfo info{};
        REQUIRE(vmaCreateBuffer(d_.allocator(), &bci, &aci, &b.buffer, &b.alloc, &info) == VK_SUCCESS);
        b.mapped = info.pMappedData;
        return b;
    }
    gpu::Device& d_;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule module_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkCommandPool cmdPool_ = VK_NULL_HANDLE;
};

core::OccluderMesh meshOf(const std::vector<light::AccelObject>& objects) {
    core::OccluderMesh m;
    for (const auto& o : objects) m.tris.insert(m.tris.end(), o.tris.begin(), o.tris.end());
    return m;
}

// Random rays through the mesh's bounds: origin, tmax, direction, 0.
std::vector<float> randomRays(const core::OccluderMesh& m, size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(0.f, 1.f);
    const core::Vec3 mn = m.boundsMin(), mx = m.boundsMax();
    const core::Vec3 ext = mx - mn;
    const float diag = static_cast<float>(ext.length());
    std::vector<float> r(n * 8);
    for (size_t i = 0; i < n; ++i) {
        float* p = &r[i * 8];
        p[0] = static_cast<float>(mn.x - 0.1 * ext.x + u(rng) * 1.2 * ext.x);
        p[1] = static_cast<float>(mn.y - 0.1 * ext.y + u(rng) * 1.2 * ext.y);
        p[2] = static_cast<float>(mn.z - 0.1 * ext.z + u(rng) * 1.2 * ext.z);
        p[3] = (i % 2 == 0 ? 2.f : 0.05f + 0.5f * u(rng)) * diag;
        const float z = 2.f * u(rng) - 1.f, a = 6.2831853f * u(rng), s = std::sqrt(1.f - z * z);
        p[4] = s * std::cos(a);
        p[5] = s * std::sin(a);
        p[6] = z;
        p[7] = 0.f;
    }
    return r;
}

struct Compare {
    size_t hitMismatch = 0, distanceMismatch = 0, hits = 0;
    double worst = 0;
};

Compare compare(const core::TriangleBvh& bvh, const std::vector<float>& rays, const std::vector<RayHit>& gpuHits) {
    Compare c;
    for (size_t i = 0; i < gpuHits.size(); ++i) {
        const float* p = &rays[i * 8];
        const core::TriangleBvh::Hit ref = bvh.closestHit({p[0], p[1], p[2]}, {p[4], p[5], p[6]}, p[3]);
        const bool refHit = ref.t >= 0, gpuHit = gpuHits[i].t >= 0;
        if (refHit != gpuHit) { ++c.hitMismatch; continue; }
        if (!refHit) continue;
        ++c.hits;
        const double d = std::fabs(ref.t - gpuHits[i].t);
        c.worst = std::max(c.worst, d);
        if (d > 1e-4) ++c.distanceMismatch;
    }
    return c;
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
