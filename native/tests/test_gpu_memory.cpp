// T1.5: upload ring and arenas. [gpu] suite: skips itself without a Vulkan device.
// The stress test does 10 000 random allocate / free / upload operations, reads the arena back and compares every live block with
// the pattern that was uploaded to it, so any overlap, lost copy or ring reuse too early shows up as a mismatch.
#include <doctest.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "gpu/arena.h"
#include "gpu/device.h"
#include "gpu/upload_ring.h"

using namespace native::gpu;

namespace {

std::unique_ptr<Device> makeDevice() {
    DeviceOptions o;
    o.validation = true;
    std::string err;
    auto d = Device::create(o, &err);
    if (!d) MESSAGE("no Vulkan device (" << err << "): GPU test skipped");
    return d;
}

uint8_t patternByte(uint32_t seed, uint64_t i) { return static_cast<uint8_t>(seed * 131u + i * 7u + (i >> 8) * 13u); }

void fillPattern(uint8_t* dst, uint64_t size, uint32_t seed) {
    for (uint64_t i = 0; i < size; ++i) dst[i] = patternByte(seed, i);
}

// A graphics-queue command buffer with a timeline semaphore: record, submit (signals the next value), wait.
class Recorder {
public:
    explicit Recorder(Device& d) : d_(d) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = d.graphicsFamily();
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        vkCreateCommandPool(d.device(), &pi, nullptr, &pool_);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = kBuffers;
        vkAllocateCommandBuffers(d.device(), &ai, cmds_);
        VkSemaphoreTypeCreateInfo ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        si.pNext = &ti;
        vkCreateSemaphore(d.device(), &si, nullptr, &timeline_);
    }
    ~Recorder() {
        vkDeviceWaitIdle(d_.device());
        vkDestroySemaphore(d_.device(), timeline_, nullptr);
        vkDestroyCommandPool(d_.device(), pool_, nullptr);
    }
    // Several command buffers in rotation, so a submit may still be executing while the next frame is recorded.
    VkCommandBuffer begin() {
        index_ = (index_ + 1) % kBuffers;
        if (values_[index_] != 0) wait(values_[index_]);
        cmd_ = cmds_[index_];
        vkResetCommandBuffer(cmd_, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd_, &bi);
        recording_ = true;
        return cmd_;
    }
    bool recording() const { return recording_; }
    // Ends and submits; returns the timeline value the submit signals.
    uint64_t submit() {
        vkEndCommandBuffer(cmd_);
        recording_ = false;
        const uint64_t value = ++last_;
        values_[index_] = value;
        VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        ts.signalSemaphoreValueCount = 1;
        ts.pSignalSemaphoreValues = &value;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.pNext = &ts;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd_;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &timeline_;
        vkQueueSubmit(d_.graphicsQueue(), 1, &si, VK_NULL_HANDLE);
        return value;
    }
    uint64_t completed() const {
        uint64_t v = 0;
        vkGetSemaphoreCounterValue(d_.device(), timeline_, &v);
        return v;
    }
    void wait(uint64_t value) {
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount = 1;
        wi.pSemaphores = &timeline_;
        wi.pValues = &value;
        vkWaitSemaphores(d_.device(), &wi, 10'000'000'000ull);
    }
    uint64_t last() const { return last_; }

private:
    Device& d_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    static constexpr uint32_t kBuffers = 4;
    VkCommandBuffer cmds_[kBuffers] = {};
    uint64_t values_[kBuffers] = {};
    uint32_t index_ = 0;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    uint64_t last_ = 0;
    bool recording_ = false;
};

void transferBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = dstStage;
    mb.dstAccessMask = dstAccess;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

TEST_CASE("upload ring: slices, alignment, exhaustion and retire by timeline value" * doctest::test_suite("gpu")) {
    auto dev = makeDevice();
    if (!dev) return;
    {
        UploadRingOptions o;
        o.bytesPerFrame = 4096;
        o.framesInFlight = 2;
        UploadRing ring(*dev, o);
        REQUIRE(ring.valid());
        CHECK(ring.bytesPerFrame() == 4096);

        // Nothing can be allocated before a frame is open.
        CHECK_FALSE(ring.allocate(16).ok());
        REQUIRE(ring.beginFrame(0));
        CHECK(ring.frameOpen());
        const auto a = ring.allocate(100, 16);
        REQUIRE(a.ok());
        CHECK(a.offset % 16 == 0);
        const auto b = ring.allocate(10, 256);
        REQUIRE(b.ok());
        CHECK(b.offset % 256 == 0);
        CHECK(b.offset >= a.offset + 100);
        CHECK(b.data == static_cast<uint8_t*>(a.data) + (b.offset - a.offset));
        CHECK_FALSE(ring.allocate(16, 3).ok());       // alignment must be a power of two
        CHECK_FALSE(ring.allocate(0).ok());
        CHECK_FALSE(ring.allocate(5000).ok());        // larger than a slice
        // Fill the slice: the last allocation that does not fit is refused and the slice stays usable for smaller ones.
        CHECK_FALSE(ring.allocate(4096).ok());
        CHECK(ring.allocate(8, 1).ok());
        std::memset(a.data, 0xAB, 100);               // the memory is writable

        ring.endFrame(1);
        CHECK_FALSE(ring.frameOpen());
        // The next frame uses the other slice (a different offset range), the one after that waits for value 1.
        REQUIRE(ring.beginFrame(0));
        const auto c = ring.allocate(16);
        REQUIRE(c.ok());
        CHECK(c.offset >= 4096);
        ring.endFrame(2);
        CHECK_FALSE(ring.beginFrame(0));              // slice 0 is still in use (retire value 1 > completed 0)
        CHECK(ring.refusedFrames() == 1);
        CHECK_FALSE(ring.frameOpen());
        REQUIRE(ring.beginFrame(1));                  // value 1 reached: slice 0 is free and starts from its beginning again
        const auto d = ring.allocate(16);
        REQUIRE(d.ok());
        CHECK(d.offset == 0);
        ring.endFrame(3);
        CHECK_FALSE(ring.beginFrame(1));              // slice 1 retires at 2
        CHECK(ring.beginFrame(2));
        ring.endFrame(4);
    }
    CHECK(dev->validationErrors() == 0);
    CHECK(dev->allocationStats().allocationCount == 0);
}

TEST_CASE("arena: allocation, alignment, exhaustion, reuse and no leaks" * doctest::test_suite("gpu")) {
    auto dev = makeDevice();
    if (!dev) return;
    {
        Arena arena(*dev, 1 << 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "test");
        REQUIRE(arena.valid());
        CHECK(arena.capacity() == (1u << 20));
        CHECK(arena.usedBytes() == 0);

        auto a = arena.allocate(1000, 256);
        auto b = arena.allocate(5000, 16);
        REQUIRE(a.ok());
        REQUIRE(b.ok());
        CHECK(a.offset % 256 == 0);
        CHECK(b.offset % 16 == 0);
        CHECK((a.offset + a.size <= b.offset || b.offset + b.size <= a.offset));   // no overlap
        CHECK(arena.allocationCount() == 2);
        CHECK(arena.usedBytes() == 6000);
        CHECK_FALSE(arena.allocate(0).ok());
        CHECK_FALSE(arena.allocate(16, 5).ok());
        CHECK_FALSE(arena.allocate((1 << 20) + 1).ok());   // larger than the arena

        arena.free(a);
        CHECK_FALSE(a.ok());
        CHECK(arena.allocationCount() == 1);
        arena.free(a);                                      // freeing an empty block is harmless
        arena.free(b);
        CHECK(arena.usedBytes() == 0);
        // Everything is free again: one allocation of the whole arena fits.
        auto whole = arena.allocate(1 << 20, 1);
        CHECK(whole.ok());
        CHECK(arena.largestFreeRange() == 0);
        CHECK_FALSE(arena.allocate(1).ok());                // full
        arena.free(whole);
        CHECK(arena.largestFreeRange() == (1u << 20));
    }
    CHECK(dev->validationErrors() == 0);
    CHECK(dev->allocationStats().allocationCount == 0);
}

TEST_CASE("10 000 random allocate / free / upload cycles read back equal, nothing leaks" * doctest::test_suite("gpu")) {
    auto dev = makeDevice();
    if (!dev) return;
    {
        constexpr VkDeviceSize kArenaBytes = 1536u * 1024;   // small enough that the arena really fills up and fragments
        Arena arena(*dev, kArenaBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, "stress");
        UploadRingOptions ro;
        ro.bytesPerFrame = 2u << 20;
        ro.framesInFlight = 2;
        UploadRing ring(*dev, ro);
        REQUIRE(arena.valid());
        REQUIRE(ring.valid());
        Recorder rec(*dev);

        // Host read-back buffer for the whole arena.
        VkBuffer readback = VK_NULL_HANDLE;
        VmaAllocation readbackAlloc = nullptr;
        void* readbackPtr = nullptr;
        {
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = kArenaBytes;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            REQUIRE(vmaCreateBuffer(dev->allocator(), &bci, &aci, &readback, &readbackAlloc, &info) == VK_SUCCESS);
            readbackPtr = info.pMappedData;
        }

        struct Live {
            Arena::Block block;
            uint32_t seed;
        };
        std::vector<Live> live;
        std::mt19937 rng(12345);
        std::vector<uint8_t> scratch;

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        int opsInBatch = 0;
        uint64_t batches = 0;
        const auto openFrame = [&] {
            while (!ring.beginFrame(rec.completed())) rec.wait(rec.last());
            cmd = rec.begin();
            opsInBatch = 0;
        };
        const auto closeFrame = [&] {
            const uint64_t v = rec.submit();
            ring.endFrame(v);
            ++batches;
            // Alternate between waiting for this batch and leaving it in flight, so the ring's retire logic is exercised.
            if (batches % 3 == 0) rec.wait(v);
        };
        openFrame();

        uint64_t allocations = 0, frees = 0, allocFailures = 0, verifications = 0;
        const auto verify = [&] {
            closeFrame();
            rec.wait(rec.last());
            cmd = rec.begin();
            const VkBufferCopy whole{0, 0, kArenaBytes};
            vkCmdCopyBuffer(cmd, arena.buffer(), readback, 1, &whole);
            transferBarrier(cmd, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
            const uint64_t v = rec.submit();
            rec.wait(v);
            vmaInvalidateAllocation(dev->allocator(), readbackAlloc, 0, VK_WHOLE_SIZE);
            const auto* mem = static_cast<const uint8_t*>(readbackPtr);
            for (const Live& l : live) {
                bool equal = true;
                for (uint64_t i = 0; i < l.block.size; ++i)
                    if (mem[l.block.offset + i] != patternByte(l.seed, i)) { equal = false; break; }
                if (!equal) {
                    FAIL("block at " << l.block.offset << " size " << l.block.size << " seed " << l.seed << " read back differently");
                }
            }
            ++verifications;
            // The read-back submit was one batch of its own; the next frame starts normally.
            openFrame();
        };

        for (int op = 0; op < 10000; ++op) {
            const bool doAlloc = live.empty() || (rng() % 100) < 55;
            if (doAlloc) {
                static const uint32_t aligns[] = {4, 16, 64, 256};
                const uint64_t size = 16 + rng() % (rng() % 8 == 0 ? 60000 : 3000);
                Arena::Block blk = arena.allocate(size, aligns[rng() % 4]);
                if (!blk.ok()) {                   // full or fragmented: free a few random blocks and go on
                    ++allocFailures;
                    for (int k = 0; k < 4 && !live.empty(); ++k) {
                        const size_t i = rng() % live.size();
                        arena.free(live[i].block);
                        live[i] = live.back();
                        live.pop_back();
                        ++frees;
                    }
                    continue;
                }
                ++allocations;
                const uint32_t seed = static_cast<uint32_t>(rng());
                scratch.resize(size);
                fillPattern(scratch.data(), size, seed);
                if (!ring.upload(cmd, arena.buffer(), blk.offset, scratch.data(), size)) {
                    closeFrame();                  // the slice is full: submit this batch and start the next one
                    openFrame();
                    REQUIRE(ring.upload(cmd, arena.buffer(), blk.offset, scratch.data(), size));
                }
                // A freed range may be reused inside one command buffer: order the copies (transfer -> transfer).
                transferBarrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                live.push_back({blk, seed});
                ++opsInBatch;
            } else {
                const size_t i = rng() % live.size();
                arena.free(live[i].block);
                live[i] = live.back();
                live.pop_back();
                ++frees;
            }
            if (opsInBatch >= 40) {
                closeFrame();
                openFrame();
            }
            if (op % 1000 == 999) verify();
            // The arena's own bookkeeping matches the model.
            if (op % 500 == 0) {
                VkDeviceSize sum = 0;
                for (const Live& l : live) sum += l.block.size;
                REQUIRE(arena.usedBytes() == sum);
                REQUIRE(arena.allocationCount() == live.size());
            }
        }
        verify();
        MESSAGE("allocations " << allocations << ", frees " << frees << ", failures (arena full) " << allocFailures << ", batches "
                               << batches << ", verifications " << verifications << ", live at end " << live.size()
                               << ", ring refusals " << ring.refusedFrames());
        CHECK(allocations > 4000);
        CHECK(allocFailures > 0);                 // the arena really did fill up
        CHECK(live.size() > 0);

        // Free everything: the arena is empty and one allocation of its full size fits again (no fragmentation left behind).
        closeFrame();
        rec.wait(rec.last());
        for (Live& l : live) arena.free(l.block);
        live.clear();
        CHECK(arena.usedBytes() == 0);
        CHECK(arena.allocationCount() == 0);
        auto whole = arena.allocate(kArenaBytes, 1);
        CHECK(whole.ok());
        arena.free(whole);
        vmaDestroyBuffer(dev->allocator(), readback, readbackAlloc);
    }
    CHECK(dev->validationErrors() == 0);
    CHECK(dev->allocationStats().allocationCount == 0);
}
