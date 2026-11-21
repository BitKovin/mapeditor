// render/rt_scene -- see rt_scene.h.
#include "render/rt_scene.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>

#include "base/log.h"

namespace native::render {

namespace {

constexpr VkBufferUsageFlags kAsInput = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
constexpr uint32_t kQueryCapacity = 1024;
constexpr VkDeviceSize kVertexSize = sizeof(scene::RenderVertex);

uint64_t alignUp(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

bool makeDeviceBuffer(gpu::Device& d, uint64_t size, VkBufferUsageFlags usage, VkBuffer& buf, VmaAllocation& alloc) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = std::max<uint64_t>(size, 16);
    bci.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    return vmaCreateBuffer(d.allocator(), &bci, &aci, &buf, &alloc, nullptr) == VK_SUCCESS;
}

VkDeviceAddress addressOf(gpu::Device& d, VkBuffer b) {
    VkBufferDeviceAddressInfo i{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    i.buffer = b;
    return vkGetBufferDeviceAddress(d.device(), &i);
}

// vkCmdUpdateBuffer takes at most 64 KiB per call.
void updateBuffer(VkCommandBuffer cmd, VkBuffer dst, const void* data, uint64_t bytes) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (uint64_t off = 0; off < bytes;) {
        const uint64_t n = std::min<uint64_t>(bytes - off, 65536);
        vkCmdUpdateBuffer(cmd, dst, off, n, p + off);
        off += n;
    }
}

} // namespace

RtScene::RtScene(gpu::Device& device, GpuScene& scene, const RtOptions& options)
    : device_(device),
      scene_(scene),
      options_(options),
      blasArena_(device, options.blasBytes, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, "rt blas") {
    if (!device.caps().lit() || !scene.valid() || !blasArena_.valid()) return;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props.pNext = &as;
    vkGetPhysicalDeviceProperties2(device.physicalDevice(), &props);
    scratchAlign_ = std::max<uint64_t>(1, as.minAccelerationStructureScratchOffsetAlignment);
    if (!makeDeviceBuffer(device, uint64_t(options.maxObjects) * sizeof(RtRecord), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, records_, recordsAlloc_)) return;
    if (!makeDeviceBuffer(device, uint64_t(options.maxObjects) * sizeof(VkAccelerationStructureInstanceKHR), kAsInput, instances_, instancesAlloc_)) return;
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
    qi.queryCount = kQueryCapacity;
    if (vkCreateQueryPool(device.device(), &qi, nullptr, &queryPool_) != VK_SUCCESS) return;
    queryCount_ = kQueryCapacity;
    valid_ = true;
}

RtScene::~RtScene() {
    if (device_.device()) vkDeviceWaitIdle(device_.device());
    for (Retired& r : retired_) r.release();
    retired_.clear();
    for (Upgrade& u : upgrades_) {
        retireBlas(u.blas);
        retireBlas(u.compact);
    }
    for (auto& kv : entries_) retireBlas(kv.second.blas);
    for (Retired& r : retired_) r.release();
    retired_.clear();
    if (tlas_) vkDestroyAccelerationStructureKHR(device_.device(), tlas_, nullptr);
    if (tlasBuffer_) vmaDestroyBuffer(device_.allocator(), tlasBuffer_, tlasAlloc_);
    if (scratch_) vmaDestroyBuffer(device_.allocator(), scratch_, scratchAlloc_);
    if (instances_) vmaDestroyBuffer(device_.allocator(), instances_, instancesAlloc_);
    if (records_) vmaDestroyBuffer(device_.allocator(), records_, recordsAlloc_);
    if (queryPool_) vkDestroyQueryPool(device_.device(), queryPool_, nullptr);
}

void RtScene::retireBlas(Blas& b) {
    if (!b.handle && !b.storage.ok()) return;
    Blas c = b;
    b = {};
    retire([this, c]() mutable {
        if (c.handle) vkDestroyAccelerationStructureKHR(device_.device(), c.handle, nullptr);
        if (c.storage.ok()) blasArena_.free(c.storage);
    });
}

void RtScene::barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) const {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = srcStage;
    mb.srcAccessMask = srcAccess;
    mb.dstStageMask = dstStage;
    mb.dstAccessMask = dstAccess;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &di);
}

bool RtScene::ensureScratch(uint64_t bytes) {
    bytes += scratchAlign_;
    if (scratch_ && scratchSize_ >= bytes) return true;
    if (scratch_) {
        const VkBuffer b = scratch_;
        const VmaAllocation a = scratchAlloc_;
        retire([this, b, a] { vmaDestroyBuffer(device_.allocator(), b, a); });
        scratch_ = VK_NULL_HANDLE;
        scratchAlloc_ = nullptr;
    }
    const uint64_t size = std::max<uint64_t>(bytes, 4ull << 20) * 3 / 2;
    if (!makeDeviceBuffer(device_, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, scratch_, scratchAlloc_)) {
        scratchSize_ = 0;
        return false;
    }
    scratchSize_ = size;
    return true;
}

bool RtScene::makeBlas(const RtObject& o, VkBuildAccelerationStructureFlagsKHR flags, Blas& out, VkAccelerationStructureGeometryKHR& geom,
                       VkAccelerationStructureBuildGeometryInfoKHR& build, VkAccelerationStructureBuildRangeInfoKHR& range) {
    geom = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    auto& t = geom.geometry.triangles;
    t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    t.vertexData.deviceAddress = scene_.vertexAddress() + uint64_t(o.vertexOffset) * kVertexSize;
    t.vertexStride = kVertexSize;
    t.maxVertex = o.vertexCount - 1;
    t.indexType = VK_INDEX_TYPE_UINT32;
    t.indexData.deviceAddress = scene_.indexAddress() + uint64_t(o.firstIndex) * sizeof(uint32_t);
    build = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    build.flags = flags;
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.geometryCount = 1;
    build.pGeometries = &geom;
    range = {};
    range.primitiveCount = o.indexCount / 3;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    const uint32_t prims = range.primitiveCount;
    vkGetAccelerationStructureBuildSizesKHR(device_.device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &prims, &sizes);
    out.storage = blasArena_.allocate(sizes.accelerationStructureSize, 256);
    if (!out.storage.ok()) return false;
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = blasArena_.buffer();
    ci.offset = out.storage.offset;
    ci.size = sizes.accelerationStructureSize;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    if (vkCreateAccelerationStructureKHR(device_.device(), &ci, nullptr, &out.handle) != VK_SUCCESS) {
        blasArena_.free(out.storage);
        out.handle = VK_NULL_HANDLE;
        return false;
    }
    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = out.handle;
    out.address = vkGetAccelerationStructureDeviceAddressKHR(device_.device(), &ai);
    out.scratchBytes = static_cast<uint32_t>(sizes.buildScratchSize);
    build.dstAccelerationStructure = out.handle;
    return true;
}

std::chrono::steady_clock::time_point RtScene::nextWake() const {
    using clock = std::chrono::steady_clock;
    if (!valid_) return clock::time_point::max();
    if (!upgrades_.empty()) return clock::now() + std::chrono::milliseconds(10);
    if (stats_.fastObjects > 0)
        return lastChange_ + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(options_.idleSeconds));
    return clock::time_point::max();
}

bool RtScene::update(VkCommandBuffer cmd, uint64_t completed, uint64_t submitted) {
    if (!valid_) return false;
    const uint64_t myValue = submitted + 1;
    for (Retired& r : retired_)
        if (r.value == ~uint64_t(0)) r.value = submitted;   // the frames submitted so far may use it; later ones will not
    for (size_t i = 0; i < retired_.size();) {
        if (retired_[i].value <= completed) {
            retired_[i].release();
            retired_[i] = std::move(retired_.back());
            retired_.pop_back();
        } else {
            ++i;
        }
    }
    if (upgrades_.empty()) queriesUsed_ = 0;

    bool recorded = false;       // the command buffer needs the closing barrier
    bool changedScene = false;   // objects came or went (restarts the idle clock)
    const auto startBarrier = [&] {
        if (recorded) return;
        // everything recorded earlier in this submission (the scene's uploads) and earlier submissions (frames reading the TLAS, the records) before our writes
        barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                    VK_ACCESS_2_SHADER_READ_BIT);   // SHADER_READ is how a build reads its vertex and index input
        recorded = true;
    };

    // ---- the idle upgrade, step 2 and 3: compacted sizes are known -> compacting copy; copy done -> swap -----------------------------------
    for (size_t i = 0; i < upgrades_.size();) {
        Upgrade& u = upgrades_[i];
        const auto it = entries_.find(u.serial);
        const bool alive = it != entries_.end() && it->second.quality == Quality::Upgrading;
        bool drop = false;
        if (!alive) {
            drop = u.copied ? completed >= u.copyValue : completed >= u.value;   // its commands may still be running
            if (drop) {
                retireBlas(u.blas);
                retireBlas(u.compact);
            }
        } else if (!u.copied && completed >= u.value) {
            uint64_t size = 0;
            const VkResult r = vkGetQueryPoolResults(device_.device(), queryPool_, u.query, 1, sizeof(size), &size, sizeof(size), VK_QUERY_RESULT_64_BIT);
            Entry& e = it->second;
            if (r != VK_SUCCESS || size == 0) {
                e.quality = Quality::Final;   // keep the fast one
                retireBlas(u.blas);
                drop = true;
            } else {
                u.compact.storage = blasArena_.allocate(size, 256);
                VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
                ci.buffer = blasArena_.buffer();
                ci.offset = u.compact.storage.offset;
                ci.size = size;
                ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
                if (!u.compact.storage.ok() || vkCreateAccelerationStructureKHR(device_.device(), &ci, nullptr, &u.compact.handle) != VK_SUCCESS) {
                    if (u.compact.storage.ok()) blasArena_.free(u.compact.storage);
                    u.compact = {};
                    e.quality = Quality::Final;
                    retireBlas(u.blas);
                    drop = true;
                } else {
                    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
                    ai.accelerationStructure = u.compact.handle;
                    u.compact.address = vkGetAccelerationStructureDeviceAddressKHR(device_.device(), &ai);
                    startBarrier();
                    VkCopyAccelerationStructureInfoKHR ci2{VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
                    ci2.src = u.blas.handle;
                    ci2.dst = u.compact.handle;
                    ci2.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
                    vkCmdCopyAccelerationStructureKHR(cmd, &ci2);
                    u.copied = true;
                    u.copyValue = myValue;
                }
            }
        } else if (u.copied && completed >= u.copyValue) {
            Entry& e = it->second;
            retireBlas(e.blas);
            retireBlas(u.blas);
            e.blas = u.compact;
            u.compact = {};
            e.quality = Quality::Final;
            ++stats_.compacted;
            tlasDirty_ = true;
            drop = true;
        }
        if (drop) {
            upgrades_[i] = std::move(upgrades_.back());
            upgrades_.pop_back();
        } else {
            ++i;
        }
    }

    // ---- which objects are new, which are gone ---------------------------------------------------------------------------------------------
    struct Job {
        RtObject object;
        Blas blas;
        VkAccelerationStructureGeometryKHR geometry{};
        VkAccelerationStructureBuildGeometryInfoKHR build{};
        VkAccelerationStructureBuildRangeInfoKHR range{};
        uint64_t scratchOffset = 0;
        bool upgrade = false;
        uint32_t query = 0;
    };
    std::vector<Job> jobs;
    uint64_t scratchTotal = 0;
    const uint64_t changes = scene_.changeSerial();
    uint32_t built = 0, removed = 0;
    if (changes != seenChanges_ || backlog_) {
        seenChanges_ = changes;
        backlog_ = false;
        const std::vector<RtObject>& objects = scene_.rtObjects();
        std::unordered_set<uint64_t> live;
        live.reserve(objects.size() * 2);
        std::vector<const RtObject*> toBuild;
        for (const RtObject& o : objects) {
            live.insert(o.serial);
            const auto known = entries_.find(o.serial);
            if (known == entries_.end()) {
                if (o.indexCount >= 3 && o.vertexCount > 0) toBuild.push_back(&o);
            } else if (known->second.object.flags != o.flags) {   // the lighting rules changed: only the instance mask differs
                known->second.object.flags = o.flags;
                tlasDirty_ = true;
                changedScene = true;
            }
        }
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (live.count(it->first)) { ++it; continue; }
            retireBlas(it->second.blas);
            freeSlots_.push_back(it->second.slot);
            it = entries_.erase(it);
            ++removed;
            tlasDirty_ = true;
            changedScene = true;
        }
        jobs.reserve(toBuild.size());
        for (const RtObject* o : toBuild) {
            if (!jobs.empty() && scratchTotal >= options_.scratchBudget) { backlog_ = true; break; }
            Job j;
            j.object = *o;
            if (!makeBlas(*o, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR, j.blas, j.geometry, j.build, j.range)) {
                static bool logged = false;
                if (!logged) {
                    base::logWarning("RtScene: the BLAS storage is full, some objects are not traced");
                    logged = true;
                }
                continue;
            }
            j.scratchOffset = alignUp(scratchTotal, scratchAlign_);
            scratchTotal = j.scratchOffset + j.blas.scratchBytes;
            jobs.push_back(std::move(j));
        }
    }
    // assign slots and entries (jobs are final now)
    for (Job& j : jobs) {
        uint32_t slot;
        if (!freeSlots_.empty()) {
            slot = freeSlots_.back();
            freeSlots_.pop_back();
        } else {
            slot = slotCount_++;
        }
        if (slot >= options_.maxObjects) {
            static bool logged = false;
            if (!logged) {
                base::logWarning("RtScene: more objects than instance slots");
                logged = true;
            }
            slotCount_ = std::min(slotCount_, options_.maxObjects);
            retireBlas(j.blas);
            j.object.indexCount = 0;   // skipped below
            continue;
        }
        Entry e;
        e.object = j.object;
        e.slot = slot;
        e.blas = j.blas;
        entries_[j.object.serial] = e;
        ++built;
        tlasDirty_ = true;
        changedScene = true;
    }
    jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [](const Job& j) { return j.object.indexCount == 0; }), jobs.end());
    const size_t newJobs = jobs.size();

    // ---- the idle upgrade, step 1: fast-trace rebuild with compaction for what was built fast ---------------------------------------------
    const bool idle = std::chrono::duration<double>(std::chrono::steady_clock::now() - lastChange_).count() >= options_.idleSeconds;
    if (newJobs == 0 && !changedScene && !backlog_ && idle && queriesUsed_ + 1 <= queryCount_) {
        uint32_t picked = 0;
        for (auto& kv : entries_) {
            Entry& e = kv.second;
            if (e.quality != Quality::Fast) continue;
            if (picked >= options_.upgradesPerUpdate || queriesUsed_ + picked >= queryCount_) break;
            if (!jobs.empty() && scratchTotal >= options_.scratchBudget) break;
            Job j;
            j.object = e.object;
            j.upgrade = true;
            if (!makeBlas(e.object, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR, j.blas, j.geometry,
                          j.build, j.range)) {
                e.quality = Quality::Final;   // no room for a second copy: it stays as it is
                continue;
            }
            j.scratchOffset = alignUp(scratchTotal, scratchAlign_);
            scratchTotal = j.scratchOffset + j.blas.scratchBytes;
            j.query = queriesUsed_ + picked++;
            e.quality = Quality::Upgrading;
            jobs.push_back(std::move(j));
        }
    }

    // ---- the TLAS needs room too: its scratch is reused after the BLAS builds -------------------------------------------------------------
    std::vector<VkAccelerationStructureInstanceKHR> instances;
    std::vector<RtRecord> records;
    VkAccelerationStructureGeometryKHR tlasGeom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    VkAccelerationStructureBuildGeometryInfoKHR tlasBuild{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    VkAccelerationStructureBuildRangeInfoKHR tlasRange{};
    VkAccelerationStructureKHR newTlas = VK_NULL_HANDLE;
    VkBuffer newTlasBuf = VK_NULL_HANDLE;
    VmaAllocation newTlasAlloc = nullptr;
    const bool rebuildTlas = tlasDirty_;
    if (rebuildTlas) {
        records.assign(slotCount_, RtRecord{});
        instances.reserve(entries_.size());
        for (auto& kv : entries_) {
            const Entry& e = kv.second;
            const RtObject& o = e.object;
            RtRecord& r = records[e.slot];
            r.vertexAddress = scene_.vertexAddress() + uint64_t(o.vertexOffset) * kVertexSize;
            r.indexAddress = scene_.indexAddress() + uint64_t(o.firstIndex) * sizeof(uint32_t);
            r.triangleCount = o.indexCount / 3;
            r.flags = (o.raw ? 1u : 0u) | ((o.flags & kObjReceives) != 0 ? 2u : 0u);
            r.vertexCount = o.vertexCount;
            VkAccelerationStructureInstanceKHR inst{};
            inst.transform.matrix[0][0] = inst.transform.matrix[1][1] = inst.transform.matrix[2][2] = 1.f;
            inst.instanceCustomIndex = e.slot;
            inst.mask = (o.flags & kObjCasts) != 0 ? kRtMaskSolid : kRtMaskReceiver;
            inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;   // double sided
            if ((o.flags & kObjAlpha) != 0) inst.flags |= VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;   // the shadow rays see its triangles as candidates and read the texels
            inst.accelerationStructureReference = e.blas.address;
            instances.push_back(inst);
        }
        const uint32_t count = static_cast<uint32_t>(instances.size());
        uint64_t tlasScratch = 0;
        if (count > 0) {
            tlasGeom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
            tlasGeom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
            tlasGeom.geometry.instances.data.deviceAddress = addressOf(device_, instances_);
            tlasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
            tlasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            tlasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            tlasBuild.geometryCount = 1;
            tlasBuild.pGeometries = &tlasGeom;
            tlasRange.primitiveCount = count;
            VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            vkGetAccelerationStructureBuildSizesKHR(device_.device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlasBuild, &count, &sizes);
            tlasScratch = sizes.buildScratchSize;
            bool ok = makeDeviceBuffer(device_, sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, newTlasBuf, newTlasAlloc);
            if (ok) {
                VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
                ci.buffer = newTlasBuf;
                ci.size = sizes.accelerationStructureSize;
                ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
                ok = vkCreateAccelerationStructureKHR(device_.device(), &ci, nullptr, &newTlas) == VK_SUCCESS;
            }
            if (!ok) {
                if (newTlas) vkDestroyAccelerationStructureKHR(device_.device(), newTlas, nullptr);
                if (newTlasBuf) vmaDestroyBuffer(device_.allocator(), newTlasBuf, newTlasAlloc);
                newTlas = VK_NULL_HANDLE;
                newTlasBuf = VK_NULL_HANDLE;
                base::logError("RtScene: the TLAS could not be created");
            }
            tlasBuild.dstAccelerationStructure = newTlas;
        }
        scratchTotal = std::max(scratchTotal, tlasScratch);
    }
    if (!jobs.empty() || newTlas) {
        if (!ensureScratch(scratchTotal)) base::logError("RtScene: no scratch buffer");
    }
    const VkDeviceAddress scratchBase = scratch_ ? alignUp(addressOf(device_, scratch_), scratchAlign_) : 0;

    // ---- record ------------------------------------------------------------------------------------------------------------------------
    if (!jobs.empty() && scratch_) {
        startBarrier();
        uint32_t queries = 0;
        for (const Job& j : jobs) queries += j.upgrade ? 1 : 0;
        if (queries) vkCmdResetQueryPool(cmd, queryPool_, queriesUsed_, queries);
        for (Job& j : jobs) {
            j.build.pGeometries = &j.geometry;
            j.build.scratchData.deviceAddress = scratchBase + j.scratchOffset;
            const VkAccelerationStructureBuildRangeInfoKHR* ranges[1] = {&j.range};
            vkCmdBuildAccelerationStructuresKHR(cmd, 1, &j.build, ranges);
        }
        if (queries) {
            barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                    VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
            for (const Job& j : jobs)
                if (j.upgrade) vkCmdWriteAccelerationStructuresPropertiesKHR(cmd, 1, &j.blas.handle, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, queryPool_, j.query);
            queriesUsed_ += queries;
        }
        for (Job& j : jobs) {
            if (j.upgrade) {
                Upgrade u;
                u.serial = j.object.serial;
                u.blas = j.blas;
                u.query = j.query;
                u.value = myValue;
                upgrades_.push_back(u);
            } else {
                ++stats_.blasBuilds;
            }
        }
    }
    if (rebuildTlas) {
        startBarrier();
        const VkAccelerationStructureKHR oldTlas = tlas_;
        const VkBuffer oldBuf = tlasBuffer_;
        const VmaAllocation oldAlloc = tlasAlloc_;
        if (oldTlas || oldBuf) {
            retire([this, oldTlas, oldBuf, oldAlloc] {
                if (oldTlas) vkDestroyAccelerationStructureKHR(device_.device(), oldTlas, nullptr);
                if (oldBuf) vmaDestroyBuffer(device_.allocator(), oldBuf, oldAlloc);
            });
        }
        if (!records.empty()) updateBuffer(cmd, records_, records.data(), records.size() * sizeof(RtRecord));
        if (newTlas && scratch_) {
            updateBuffer(cmd, instances_, instances.data(), instances.size() * sizeof(VkAccelerationStructureInstanceKHR));
            barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                    VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT);   // the TLAS build reuses the BLAS scratch and reads the instances
            tlasBuild.scratchData.deviceAddress = scratchBase;
            const VkAccelerationStructureBuildRangeInfoKHR* ranges[1] = {&tlasRange};
            vkCmdBuildAccelerationStructuresKHR(cmd, 1, &tlasBuild, ranges);
            tlas_ = newTlas;
            tlasBuffer_ = newTlasBuf;
            tlasAlloc_ = newTlasAlloc;
        } else {
            tlas_ = VK_NULL_HANDLE;
            tlasBuffer_ = VK_NULL_HANDLE;
            tlasAlloc_ = nullptr;
        }
        ++generation_;
        ++stats_.tlasBuilds;
        tlasDirty_ = false;
    }
    if (recorded) {
        barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_MEMORY_READ_BIT);
    }

    if (changedScene) lastChange_ = std::chrono::steady_clock::now();
    if (built || removed) {
        stats_.lastBuilt = built;
        stats_.lastRemoved = removed;
    }
    stats_.objects = static_cast<uint32_t>(entries_.size());
    stats_.triangles = 0;
    stats_.fastObjects = 0;
    for (const auto& kv : entries_) {
        stats_.triangles += kv.second.object.indexCount / 3;
        if (kv.second.quality == Quality::Fast) ++stats_.fastObjects;
    }
    stats_.blasBytes = blasArena_.usedBytes();
    stats_.backlog = backlog_;
    return backlog_;   // the idle upgrade is woken by nextWake(), not by the loop spinning
}

} // namespace native::render
