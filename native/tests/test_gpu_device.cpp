// T1.2: Vulkan device, caps, queues, validation, memory leaks. GPU cases belong to the doctest suite "gpu" (-ts=gpu) and skip
// themselves with a message when the machine has no Vulkan device.
#include <doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "base/log.h"
#include "gpu/device.h"

using namespace native::gpu;

namespace {

std::unique_ptr<Device> makeDevice(bool validation = false) {
    DeviceOptions o;
    o.validation = validation;
    std::string err;
    auto d = Device::create(o, &err);
    if (!d) MESSAGE("no Vulkan device (" << err << "): GPU test skipped");
    return d;
}

bool envSet(const char* name) {
    char* v = nullptr;
    size_t n = 0;
    const bool set = _dupenv_s(&v, &n, name) == 0 && v && *v;
    std::free(v);
    return set;
}

// Records and submits one empty command buffer on the queue and waits for it (a smoke test that the queue is usable and,
// with validation on, that the device was created correctly).
bool submitEmpty(const Device& d, uint32_t family, VkQueue queue) {
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(d.device(), &pi, nullptr, &pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    bool ok = vkAllocateCommandBuffers(d.device(), &ai, &cb) == VK_SUCCESS;
    if (ok) {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        ok = vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS && vkEndCommandBuffer(cb) == VK_SUCCESS;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        ok = ok && vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS && vkQueueWaitIdle(queue) == VK_SUCCESS;
    }
    vkDestroyCommandPool(d.device(), pool, nullptr);
    return ok;
}

} // namespace

TEST_CASE("DeviceCaps rows are complete and in a fixed order" * doctest::test_suite("gpu")) {
    auto d = makeDevice();
    if (!d) return;
    const auto rows = d->caps().rows();
    REQUIRE(rows.size() >= 19);
    CHECK(rows[0].first == "device");
    CHECK_FALSE(rows[0].second.empty());
    CHECK_FALSE(d->caps().driver.empty());
    CHECK(d->caps().apiVersion.rfind("1.", 0) == 0);
    for (const auto& r : rows) MESSAGE(r.first << ": " << r.second);
}

TEST_CASE("the development machine exposes every row of the 05 table (NATIVE_EXPECT_FULL_CAPS=1)" * doctest::test_suite("gpu")) {
    if (!envSet("NATIVE_EXPECT_FULL_CAPS")) { MESSAGE("NATIVE_EXPECT_FULL_CAPS not set: skipped"); return; }
    auto d = makeDevice();
    REQUIRE(d);
    const DeviceCaps& c = d->caps();
    CHECK(c.accelerationStructure);
    CHECK(c.rayQuery);
    CHECK(c.rayTracingPipeline);
    CHECK(c.invocationReorder);
    CHECK(c.opacityMicromap);
    CHECK(c.positionFetch);
    CHECK(c.globalPriority);   // graphics HIGH itself is not allowed on this driver without elevated rights (see log)
    CHECK(c.hostImageCopy);
    CHECK(c.externalMemoryWin32);
    CHECK(c.shaderAtomicFloat);
    CHECK(c.partitionedAccelerationStructure);
    CHECK(c.clusterAccelerationStructure);
    CHECK(c.descriptorHeap);
    CHECK(c.lit());
}

TEST_CASE("both queues accept work" * doctest::test_suite("gpu")) {
    auto d = makeDevice();
    if (!d) return;
    CHECK(submitEmpty(*d, d->graphicsFamily(), d->graphicsQueue()));
    CHECK(submitEmpty(*d, d->computeFamily(), d->computeQueue()));
    if (d->caps().dedicatedComputeQueue) CHECK(d->graphicsFamily() != d->computeFamily());
}

TEST_CASE("create and destroy the device 3 times without leaking allocations" * doctest::test_suite("gpu")) {
    for (int round = 0; round < 3; ++round) {
        auto d = makeDevice();
        if (!d) return;
        CHECK(d->allocationStats().allocationCount == 0);

        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = 1 << 20;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        VkBuffer buf = VK_NULL_HANDLE;
        VmaAllocation bufAlloc = nullptr;
        REQUIRE(vmaCreateBuffer(d->allocator(), &bi, &ai, &buf, &bufAlloc, nullptr) == VK_SUCCESS);

        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = {256, 256, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkImage img = VK_NULL_HANDLE;
        VmaAllocation imgAlloc = nullptr;
        REQUIRE(vmaCreateImage(d->allocator(), &ii, &ai, &img, &imgAlloc, nullptr) == VK_SUCCESS);

        CHECK(d->allocationStats().allocationCount == 2);
        CHECK(d->allocationStats().allocationBytes >= (1u << 20));

        vmaDestroyImage(d->allocator(), img, imgAlloc);
        vmaDestroyBuffer(d->allocator(), buf, bufAlloc);
        CHECK(d->allocationStats().allocationCount == 0);
        CHECK(d->allocationStats().allocationBytes == 0);
    }
}

TEST_CASE("with validation on, creating the device and using both queues gives 0 validation errors" * doctest::test_suite("gpu")) {
    auto d = makeDevice(true);
    if (!d) return;
    if (!d->validationActive()) { MESSAGE("validation layer not installed: skipped"); return; }
    CHECK(submitEmpty(*d, d->graphicsFamily(), d->graphicsQueue()));
    CHECK(submitEmpty(*d, d->computeFamily(), d->computeQueue()));
    CHECK(d->validationErrors() == 0);
    CHECK(d->validationWarnings() == 0);
}

TEST_CASE("a device that must have ray tracing is refused cleanly when none exists" * doctest::test_suite("gpu")) {
    // Not a failure on machines with ray tracing: then the device simply is created.
    DeviceOptions o;
    o.requireRayTracing = true;
    std::string err;
    auto d = Device::create(o, &err);
    if (d) CHECK(d->caps().lit());
    else CHECK_FALSE(err.empty());
}

// ---- T6.7: NVIDIA Streamline, Reflex, crash dumps ---------------------------------------------------------------------------

TEST_CASE("Streamline: the device is made through the interposer and Reflex is on" * doctest::test_suite("gpu")) {
    DeviceOptions o;
    o.validation = true;
    o.streamline = true;
    std::string err;
    auto d = Device::create(o, &err);
    if (!d) { MESSAGE("no Vulkan device (" << err << "): GPU test skipped"); return; }
    if (!d->caps().streamline) { MESSAGE("Streamline unavailable (" << d->caps().streamlineNote << "): skipped"); return; }
    REQUIRE(d->streamline() != nullptr);
    const StreamlineStatus st = d->streamline()->status();
    CHECK(st.loaded);
    CHECK(st.version == "2.14.16");
    CHECK(st.pclSupported);
    if (st.reflexSupported) {
        CHECK(st.reflexActive);
        CHECK(d->caps().reflex);
    } else {
        MESSAGE("this adapter has no Reflex: " << st.note);
    }
    CHECK(submitEmpty(*d, d->graphicsFamily(), d->graphicsQueue()));
    CHECK(submitEmpty(*d, d->computeFamily(), d->computeQueue()));
    // The markers are harmless without a swapchain and the frame counter moves.
    for (int i = 0; i < 5; ++i) {
        d->streamline()->beginFrame();
        d->streamline()->sleep();
        d->streamline()->marker(PclMarker::SimulationStart);
        d->streamline()->marker(PclMarker::SimulationEnd);
    }
    CHECK(d->streamline()->status().frames == 5);
    bool streamlineRow = false;
    for (const auto& row : d->caps().rows())
        if (row.first == "Streamline") streamlineRow = row.second == "on (interposer)";
    CHECK(streamlineRow);
    if (d->validationActive()) {
        CHECK(d->validationErrors() == 0);
    }
}

TEST_CASE("Streamline off (--no-streamline) leaves the plain Vulkan device" * doctest::test_suite("gpu")) {
    DeviceOptions o;
    o.streamline = false;
    std::string err;
    auto d = Device::create(o, &err);
    if (!d) { MESSAGE("no Vulkan device (" << err << "): GPU test skipped"); return; }
    CHECK_FALSE(d->caps().streamline);
    CHECK_FALSE(d->caps().reflex);
    CHECK(d->streamline() == nullptr);
    CHECK(d->caps().streamlineNote.empty());
    CHECK(submitEmpty(*d, d->graphicsFamily(), d->graphicsQueue()));
}

TEST_CASE("Streamline with the interposer missing runs as before and says why" * doctest::test_suite("gpu")) {
    const std::filesystem::path empty = std::filesystem::temp_directory_path() / "native_test_no_streamline";
    std::filesystem::create_directories(empty);
    DeviceOptions o;
    o.validation = true;
    o.streamline = true;
    o.streamlineDir = empty.string();
    std::string err;
    auto d = Device::create(o, &err);
    if (!d) { MESSAGE("no Vulkan device (" << err << "): GPU test skipped"); return; }
    CHECK_FALSE(d->caps().streamline);
    CHECK(d->streamline() == nullptr);
    CHECK_FALSE(d->caps().streamlineNote.empty());
    CHECK(submitEmpty(*d, d->graphicsFamily(), d->graphicsQueue()));
    if (d->validationActive()) CHECK(d->validationErrors() == 0);
    std::filesystem::remove_all(empty);
}

TEST_CASE("GPU crash dumps: the report names the device and is written once" * doctest::test_suite("gpu")) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "native_test_crash_dump";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    DeviceOptions o;
    o.gpuCrashDumps = true;
    o.crashDumpDir = dir.string();
    std::string err;
    auto d = Device::create(o, &err);
    if (!d) { MESSAGE("no Vulkan device (" << err << "): GPU test skipped"); return; }
    if (!d->caps().diagnostics) { MESSAGE("no NV device diagnostics on this GPU: skipped"); return; }
    CHECK(submitEmpty(*d, d->graphicsFamily(), d->graphicsQueue()));   // the device works with the diagnostics config on
    d->noteResult(VK_SUCCESS, "nothing");                              // only a lost device writes
    CHECK(std::filesystem::is_empty(dir));
    const std::string path = d->writeCrashDump("test: simulated loss");
    REQUIRE_FALSE(path.empty());
    std::stringstream text;
    {
        std::ifstream in(path);
        text << in.rdbuf();
    }
    CHECK(text.str().find(d->caps().deviceName) != std::string::npos);
    CHECK(text.str().find("where: test: simulated loss") != std::string::npos);
    CHECK(text.str().find("queue graphics") != std::string::npos);
    CHECK(d->writeCrashDump("second").empty());   // once per process
    std::filesystem::remove_all(dir);
}

TEST_CASE("GPU crash dumps are off unless asked for" * doctest::test_suite("gpu")) {
    auto d = makeDevice();
    if (!d) return;
    CHECK_FALSE(d->caps().diagnostics);
    CHECK(d->writeCrashDump("x").empty());
}
