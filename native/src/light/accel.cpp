#include "light/accel.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_set>

#include "base/log.h"
#include "core/relief_geometry.h"

namespace native::light {

namespace {

uint64_t fnv(uint64_t h, const void* data, size_t bytes) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

uint64_t alignUp(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

} // namespace

// ---- occluders per object ------------------------------------------------------------------------------------------------------------

std::vector<AccelObject> buildAccelObjects(const std::vector<core::LightSurface>& surfaces, const core::LightingClasses& classes, const core::LightmapLayout* layout) {
    std::vector<AccelObject> out;
    std::unordered_map<std::string, size_t> index;
    std::unordered_set<const core::ReliefMesh*> reliefDone;
    for (const core::LightSurface& s : surfaces) {
        if (!classes.castsShadows(s.classname) || !s.castsShadow || s.verts.size() < 3) continue;
        const core::ReliefMesh* relief = s.relief.get();
        if (relief && !reliefDone.insert(relief).second) continue;
        const std::string key = s.instance + "|" + s.entityId + "|" + s.brushInstance + "|" + s.brushId;
        const auto ins = index.emplace(key, out.size());
        if (ins.second) {
            out.emplace_back();
            out.back().key = key;
        }
        AccelObject& o = out[ins.first->second];
        const core::LightmapFace* face = layout ? layout->findForFragment(s.instance, s.entityId, s.brushInstance, s.brushId, s.faceId, s.verts) : nullptr;
        const int page = face && face->chart >= 0 ? layout->charts[static_cast<size_t>(face->chart)].page : 0;
        auto addTriangle = [&](const core::Vec3 (&tri)[3], const core::Vec3 (&at)[3]) {
            for (const core::Vec3& v : tri) {
                o.tris.push_back(static_cast<float>(v.x));
                o.tris.push_back(static_cast<float>(v.y));
                o.tris.push_back(static_cast<float>(v.z));
            }
            if (!layout) return;
            core::Vec2 lm[3];
            for (int c = 0; c < 3; ++c) lm[c] = face ? face->texel(at[c]) : core::Vec2{0, 0};
            o.lightmap.insert(o.lightmap.end(), {static_cast<float>(lm[0].u), static_cast<float>(lm[0].v), static_cast<float>(lm[1].u), static_cast<float>(lm[1].v),
                                                 static_cast<float>(lm[2].u), static_cast<float>(lm[2].v), static_cast<float>(page), face ? 1.f : 0.f});
        };
        if (relief) {
            for (size_t i = 0; i + 8 < relief->tris.size(); i += 9) {
                const float* t = &relief->tris[i];
                const core::Vec3 tri[3] = {{t[0], t[1], t[2]}, {t[3], t[4], t[5]}, {t[6], t[7], t[8]}};
                const core::Vec3 at[3] = {relief->footprint(tri[0]), relief->footprint(tri[1]), relief->footprint(tri[2])};
                addTriangle(tri, at);
            }
            continue;
        }
        for (size_t k = 1; k + 1 < s.verts.size(); ++k) {
            const core::Vec3 tri[3] = {s.verts[0], s.verts[k], s.verts[k + 1]};
            addTriangle(tri, tri);
        }
    }
    for (AccelObject& o : out) {
        uint64_t h = 1469598103934665603ull;
        h = fnv(h, o.tris.data(), o.tris.size() * sizeof(float));
        h = fnv(h, o.lightmap.data(), o.lightmap.size() * sizeof(float));
        const uint8_t t = o.transparent ? 1 : 0;
        o.hash = fnv(h, &t, 1);
    }
    return out;
}

// ---- the structures --------------------------------------------------------------------------------------------------------------------

namespace {

constexpr VkBufferUsageFlags kAsInput = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

struct HostBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    uint8_t* mapped = nullptr;
    uint64_t size = 0;
};

bool makeHostBuffer(gpu::Device& d, uint64_t size, HostBuffer& out) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = std::max<uint64_t>(size, 16);
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(d.allocator(), &bci, &aci, &out.buffer, &out.alloc, &info) != VK_SUCCESS) return false;
    out.mapped = static_cast<uint8_t*>(info.pMappedData);
    out.size = bci.size;
    return true;
}

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

} // namespace

Accel::Accel(gpu::Device& device, const AccelOptions& options)
    : device_(device),
      options_(options),
      vertexArena_(device, options.vertexBytes, kAsInput, "accel vertices"),
      attrArena_(device, options.attrBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, "accel attributes"),
      blasArena_(device, options.blasBytes, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, "accel blas") {
    if (!device.caps().lit()) return;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props.pNext = &as;
    vkGetPhysicalDeviceProperties2(device.physicalDevice(), &props);
    scratchAlign_ = std::max<uint64_t>(1, as.minAccelerationStructureScratchOffsetAlignment);
    if (!vertexArena_.valid() || !attrArena_.valid() || !blasArena_.valid()) return;
    if (!makeDeviceBuffer(device, static_cast<uint64_t>(options.maxObjects) * sizeof(ObjectRecord), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, objects_, objectsAlloc_)) return;
    valid_ = true;
}

Accel::~Accel() {
    if (device_.device()) vkDeviceWaitIdle(device_.device());
    for (Entry& e : entries_) destroyEntry(e);
    destroyTlas();
    if (objects_) vmaDestroyBuffer(device_.allocator(), objects_, objectsAlloc_);
}

void Accel::destroyEntry(Entry& e) {
    if (e.blas) vkDestroyAccelerationStructureKHR(device_.device(), e.blas, nullptr);
    if (e.vertices.ok()) vertexArena_.free(e.vertices);
    if (e.attrs.ok()) attrArena_.free(e.attrs);
    if (e.storage.ok()) blasArena_.free(e.storage);
    e.blas = VK_NULL_HANDLE;
    e.live = false;
}

void Accel::destroyTlas() {
    if (tlas_) vkDestroyAccelerationStructureKHR(device_.device(), tlas_, nullptr);
    if (tlasBuffer_) vmaDestroyBuffer(device_.allocator(), tlasBuffer_, tlasAlloc_);
    tlas_ = VK_NULL_HANDLE;
    tlasBuffer_ = VK_NULL_HANDLE;
    tlasAlloc_ = nullptr;
}

uint32_t Accel::slotOf(const std::string& key) const {
    const auto it = slots_.find(key);
    return it == slots_.end() ? ~0u : it->second;
}

bool Accel::update(const std::vector<AccelObject>& objects) {
    if (!valid_) return false;
    const auto t0 = std::chrono::steady_clock::now();
    vkQueueWaitIdle(device_.computeQueue());   // nothing of ours is in flight after this: old resources may be destroyed
    stats_.rebuilt = stats_.removed = 0;

    // ---- which objects are new, changed, gone ------------------------------------------------------------------------------------------
    std::unordered_set<std::string> keep;
    std::vector<size_t> rebuild;   // indices into `objects`
    for (size_t i = 0; i < objects.size(); ++i) {
        const AccelObject& o = objects[i];
        if (o.triangleCount() == 0) continue;
        keep.insert(o.key);
        const auto it = slots_.find(o.key);
        if (it != slots_.end() && entries_[it->second].live && entries_[it->second].hash == o.hash) continue;
        rebuild.push_back(i);
    }
    bool changed = !rebuild.empty();
    for (auto it = slots_.begin(); it != slots_.end();) {
        if (keep.count(it->first)) { ++it; continue; }
        Entry& e = entries_[it->second];
        destroyEntry(e);
        freeSlots_.push_back(it->second);
        it = slots_.erase(it);
        ++stats_.removed;
        changed = true;
    }
    if (!changed && tlas_) return true;

    // ---- resources of the objects to build ---------------------------------------------------------------------------------------------
    struct Job {
        size_t object;
        uint32_t slot;
        uint64_t stagingVertices, stagingAttrs;   // offsets into the staging buffer
        VkAccelerationStructureGeometryKHR geometry{};
        VkAccelerationStructureBuildGeometryInfoKHR build{};
        VkAccelerationStructureBuildRangeInfoKHR range{};
        uint64_t scratchOffset = 0;
    };
    std::vector<Job> jobs;
    uint64_t stagingBytes = 0;
    for (size_t i : rebuild) {
        const AccelObject& o = objects[i];
        uint32_t slot;
        const auto it = slots_.find(o.key);
        if (it != slots_.end()) {
            slot = it->second;
            destroyEntry(entries_[slot]);
        } else if (!freeSlots_.empty()) {
            slot = freeSlots_.back();
            freeSlots_.pop_back();
            slots_[o.key] = slot;
        } else {
            slot = static_cast<uint32_t>(entries_.size());
            entries_.emplace_back();
            slots_[o.key] = slot;
        }
        if (slot >= options_.maxObjects) return false;
        Entry& e = entries_[slot];
        e = {};
        e.key = o.key;
        e.hash = o.hash;
        e.triangles = static_cast<uint32_t>(o.triangleCount());
        e.transparent = o.transparent;
        Job j;
        j.object = i;
        j.slot = slot;
        j.stagingVertices = stagingBytes;
        stagingBytes += alignUp(o.tris.size() * sizeof(float), 16);
        j.stagingAttrs = stagingBytes;
        stagingBytes += alignUp(o.lightmap.size() * sizeof(float), 16);
        jobs.push_back(j);
    }
    bool failed = false;
    uint64_t scratchBytes = 0;
    for (Job& j : jobs) {
        const AccelObject& o = objects[j.object];
        Entry& e = entries_[j.slot];
        e.vertices = vertexArena_.allocate(o.tris.size() * sizeof(float), 16);
        if (!o.lightmap.empty()) e.attrs = attrArena_.allocate(o.lightmap.size() * sizeof(float), 16);
        if (!e.vertices.ok() || (!o.lightmap.empty() && !e.attrs.ok())) { failed = true; break; }
        j.geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        j.geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        j.geometry.flags = o.transparent ? VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR : VK_GEOMETRY_OPAQUE_BIT_KHR;
        j.geometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        j.geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        j.geometry.geometry.triangles.vertexData.deviceAddress = vertexArena_.deviceAddress() + e.vertices.offset;
        j.geometry.geometry.triangles.vertexStride = 3 * sizeof(float);
        j.geometry.geometry.triangles.maxVertex = e.triangles * 3 - 1;
        j.geometry.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
        j.build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        j.build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        j.build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        j.build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        j.build.geometryCount = 1;
        j.build.pGeometries = &j.geometry;   // fixed up below: `jobs` does not move after this loop
        j.range.primitiveCount = e.triangles;
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(device_.device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &j.build, &e.triangles, &sizes);
        e.storage = blasArena_.allocate(sizes.accelerationStructureSize, 256);
        if (!e.storage.ok()) { failed = true; break; }
        VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        ci.buffer = blasArena_.buffer();
        ci.offset = e.storage.offset;
        ci.size = sizes.accelerationStructureSize;
        ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        if (vkCreateAccelerationStructureKHR(device_.device(), &ci, nullptr, &e.blas) != VK_SUCCESS) { failed = true; break; }
        j.build.dstAccelerationStructure = e.blas;
        j.scratchOffset = alignUp(scratchBytes, scratchAlign_);
        scratchBytes = j.scratchOffset + sizes.buildScratchSize;
        e.live = true;
    }
    if (failed) {
        for (const Job& j : jobs) destroyEntry(entries_[j.slot]);
        base::logError("Accel::update: arena full or build setup failed");
        return false;
    }
    // pGeometries point into `jobs` elements, which are stable now
    for (Job& j : jobs) j.build.pGeometries = &j.geometry;

    // ---- staging, scratch, instances -------------------------------------------------------------------------------------------------------
    HostBuffer staging;
    if (!makeHostBuffer(device_, stagingBytes, staging)) return false;
    for (const Job& j : jobs) {
        const AccelObject& o = objects[j.object];
        std::memcpy(staging.mapped + j.stagingVertices, o.tris.data(), o.tris.size() * sizeof(float));
        if (!o.lightmap.empty()) std::memcpy(staging.mapped + j.stagingAttrs, o.lightmap.data(), o.lightmap.size() * sizeof(float));
    }
    vmaFlushAllocation(device_.allocator(), staging.alloc, 0, VK_WHOLE_SIZE);

    const uint32_t slotCount = static_cast<uint32_t>(entries_.size());
    std::vector<ObjectRecord> records(slotCount, ObjectRecord{0, 0, 0});
    std::vector<VkAccelerationStructureInstanceKHR> instances;
    for (uint32_t s = 0; s < slotCount; ++s) {
        const Entry& e = entries_[s];
        if (!e.live) continue;
        records[s].attrAddress = e.attrs.ok() ? attrArena_.deviceAddress() + e.attrs.offset : 0;
        records[s].triangleCount = e.triangles;
        records[s].flags = e.transparent ? 1u : 0u;
        VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        ai.accelerationStructure = e.blas;
        VkAccelerationStructureInstanceKHR inst{};
        inst.transform.matrix[0][0] = inst.transform.matrix[1][1] = inst.transform.matrix[2][2] = 1.f;
        inst.instanceCustomIndex = s;
        inst.mask = 0xFF;
        inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;   // double sided
        inst.accelerationStructureReference = vkGetAccelerationStructureDeviceAddressKHR(device_.device(), &ai);
        instances.push_back(inst);
    }
    HostBuffer stagingTop;
    const uint64_t recordBytes = records.size() * sizeof(ObjectRecord), instanceBytes = instances.size() * sizeof(VkAccelerationStructureInstanceKHR);
    if (!makeHostBuffer(device_, recordBytes + instanceBytes, stagingTop)) {
        vmaDestroyBuffer(device_.allocator(), staging.buffer, staging.alloc);
        return false;
    }
    if (recordBytes) std::memcpy(stagingTop.mapped, records.data(), recordBytes);
    if (instanceBytes) std::memcpy(stagingTop.mapped + recordBytes, instances.data(), instanceBytes);
    vmaFlushAllocation(device_.allocator(), stagingTop.alloc, 0, VK_WHOLE_SIZE);

    VkBuffer scratchBuf = VK_NULL_HANDLE, instanceBuf = VK_NULL_HANDLE, tlasScratchBuf = VK_NULL_HANDLE, newTlasBuf = VK_NULL_HANDLE;
    VmaAllocation scratchAlloc = nullptr, instanceAlloc = nullptr, tlasScratchAlloc = nullptr, newTlasAlloc = nullptr;
    VkAccelerationStructureKHR newTlas = VK_NULL_HANDLE;
    bool ok = makeDeviceBuffer(device_, scratchBytes + scratchAlign_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, scratchBuf, scratchAlloc);
    ok = ok && makeDeviceBuffer(device_, std::max<uint64_t>(instanceBytes, 64), kAsInput, instanceBuf, instanceAlloc);

    VkAccelerationStructureGeometryKHR tlasGeom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    VkAccelerationStructureBuildGeometryInfoKHR tlasBuild{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    VkAccelerationStructureBuildRangeInfoKHR tlasRange{};
    uint64_t tlasBytes = 0;
    const uint32_t instanceCount = static_cast<uint32_t>(instances.size());
    if (ok && instanceCount > 0) {
        tlasGeom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        tlasGeom.flags = 0;
        tlasGeom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        tlasGeom.geometry.instances.arrayOfPointers = VK_FALSE;
        tlasGeom.geometry.instances.data.deviceAddress = addressOf(device_, instanceBuf);
        tlasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        tlasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        tlasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        tlasBuild.geometryCount = 1;
        tlasBuild.pGeometries = &tlasGeom;
        tlasRange.primitiveCount = instanceCount;
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(device_.device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlasBuild, &instanceCount, &sizes);
        tlasBytes = sizes.accelerationStructureSize;
        ok = makeDeviceBuffer(device_, sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, newTlasBuf, newTlasAlloc);
        ok = ok && makeDeviceBuffer(device_, sizes.buildScratchSize + scratchAlign_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, tlasScratchBuf, tlasScratchAlloc);
        if (ok) {
            VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
            ci.buffer = newTlasBuf;
            ci.size = sizes.accelerationStructureSize;
            ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
            ok = vkCreateAccelerationStructureKHR(device_.device(), &ci, nullptr, &newTlas) == VK_SUCCESS;
            tlasBuild.dstAccelerationStructure = newTlas;
            tlasBuild.scratchData.deviceAddress = alignUp(addressOf(device_, tlasScratchBuf), scratchAlign_);
        }
    }

    if (ok) {
        // one command buffer on the compute queue: copies, BLAS builds, TLAS build, object records
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = device_.computeFamily();
        VkCommandPool pool = VK_NULL_HANDLE;
        vkCreateCommandPool(device_.device(), &pci, nullptr, &pool);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(device_.device(), &ai, &cmd);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        for (const Job& j : jobs) {
            const AccelObject& o = objects[j.object];
            const Entry& e = entries_[j.slot];
            VkBufferCopy r{j.stagingVertices, e.vertices.offset, o.tris.size() * sizeof(float)};
            vkCmdCopyBuffer(cmd, staging.buffer, vertexArena_.buffer(), 1, &r);
            if (e.attrs.ok()) {
                VkBufferCopy a{j.stagingAttrs, e.attrs.offset, o.lightmap.size() * sizeof(float)};
                vkCmdCopyBuffer(cmd, staging.buffer, attrArena_.buffer(), 1, &a);
            }
        }
        if (recordBytes) {
            VkBufferCopy r{0, 0, recordBytes};
            vkCmdCopyBuffer(cmd, stagingTop.buffer, objects_, 1, &r);
        }
        if (instanceBytes) {
            VkBufferCopy r{recordBytes, 0, instanceBytes};
            vkCmdCopyBuffer(cmd, stagingTop.buffer, instanceBuf, 1, &r);
        }
        auto barrier = [&](VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
            VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            mb.srcStageMask = srcStage;
            mb.srcAccessMask = srcAccess;
            mb.dstStageMask = dstStage;
            mb.dstAccessMask = dstAccess;
            VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            di.memoryBarrierCount = 1;
            di.pMemoryBarriers = &mb;
            vkCmdPipelineBarrier2(cmd, &di);
        };
        barrier(VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT);
        const VkDeviceAddress scratchBase = alignUp(addressOf(device_, scratchBuf), scratchAlign_);
        for (Job& j : jobs) {
            j.build.scratchData.deviceAddress = scratchBase + j.scratchOffset;
            const VkAccelerationStructureBuildRangeInfoKHR* ranges[1] = {&j.range};
            vkCmdBuildAccelerationStructuresKHR(cmd, 1, &j.build, ranges);
        }
        if (instanceCount > 0) {
            barrier(VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                    VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
            const VkAccelerationStructureBuildRangeInfoKHR* ranges[1] = {&tlasRange};
            vkCmdBuildAccelerationStructuresKHR(cmd, 1, &tlasBuild, ranges);
        }
        barrier(VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_MEMORY_READ_BIT);
        vkEndCommandBuffer(cmd);
        VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cbi.commandBuffer = cmd;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &cbi;
        ok = device_.submitAndWait(device_.computeQueue(), si, "acceleration structure build");
        vkDestroyCommandPool(device_.device(), pool, nullptr);
    }

    auto destroyBuf = [&](VkBuffer b, VmaAllocation a) { if (b) vmaDestroyBuffer(device_.allocator(), b, a); };
    destroyBuf(staging.buffer, staging.alloc);
    destroyBuf(stagingTop.buffer, stagingTop.alloc);
    destroyBuf(scratchBuf, scratchAlloc);
    destroyBuf(instanceBuf, instanceAlloc);
    destroyBuf(tlasScratchBuf, tlasScratchAlloc);
    if (!ok) {
        if (newTlas) vkDestroyAccelerationStructureKHR(device_.device(), newTlas, nullptr);
        destroyBuf(newTlasBuf, newTlasAlloc);
        base::logError("Accel::update: the GPU build failed");
        return false;
    }
    destroyTlas();
    tlas_ = newTlas;
    tlasBuffer_ = newTlasBuf;
    tlasAlloc_ = newTlasAlloc;
    ++generation_;

    stats_.objects = slots_.size();
    stats_.triangles = 0;
    stats_.blasBytes = blasArena_.usedBytes();
    stats_.tlasBytes = tlasBytes;
    stats_.rebuilt = jobs.size();
    for (const Entry& e : entries_) stats_.triangles += e.live ? e.triangles : 0;
    stats_.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

} // namespace native::light
