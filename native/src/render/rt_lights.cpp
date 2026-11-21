// render/rt_lights -- see rt_lights.h.
#include "render/rt_lights.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "base/log.h"
#include "doc/document.h"
#include "light/light_pack.h"

namespace native::render {

bool RtLightData::operator==(const RtLightData& o) const {
    return lightCount == o.lightCount && globalCount == o.globalCount && cell == o.cell && std::memcmp(origin, o.origin, sizeof(origin)) == 0 &&
           std::memcmp(dim, o.dim, sizeof(dim)) == 0 && records == o.records && index == o.index && cells == o.cells;
}

std::shared_ptr<const RtLightData> makeRtLightData(const std::vector<core::Light>& lights) {
    auto out = std::make_shared<RtLightData>();
    out->lightCount = static_cast<uint32_t>(lights.size());
    out->records = light::packLights(lights);
    std::vector<uint32_t> local;
    for (uint32_t k = 0; k < lights.size(); ++k) {
        const core::LightType t = lights[k].type;
        if (t == core::LightType::Directional || t == core::LightType::Sky) out->index.push_back(k);
        else if (lights[k].radius > 0) local.push_back(k);
    }
    out->globalCount = static_cast<uint32_t>(out->index.size());
    if (local.empty()) {
        out->cells = {0u, 0u};
        return out;
    }
    // the bounds of the lights' ranges, and a cell size that keeps the grid at a few million cells
    double lo[3] = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
    double hi[3] = {std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest()};
    for (uint32_t k : local) {
        const core::Light& L = lights[k];
        const double p[3] = {L.position.x, L.position.y, L.position.z};
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], p[a] - L.radius);
            hi[a] = std::max(hi[a], p[a] + L.radius);
        }
    }
    double cell = 2.0;
    uint32_t dim[3];
    for (;;) {
        double cells = 1;
        for (int a = 0; a < 3; ++a) {
            dim[a] = std::max(1u, static_cast<uint32_t>(std::ceil((hi[a] - lo[a]) / cell)));
            cells *= dim[a];
        }
        if (cells <= 4.0e6) break;
        cell *= 1.25;
    }
    for (int a = 0; a < 3; ++a) {
        out->origin[a] = static_cast<float>(lo[a]);
        out->dim[a] = dim[a];
    }
    out->cell = static_cast<float>(cell);
    const size_t cellCount = static_cast<size_t>(dim[0]) * dim[1] * dim[2];
    // pass 1: how many lights per cell; pass 2: fill (the same loop twice)
    std::vector<uint32_t> counts(cellCount, 0);
    const auto forCells = [&](const core::Light& L, const auto& fn) {
        const double p[3] = {L.position.x, L.position.y, L.position.z};
        int a0[3], a1[3];
        for (int a = 0; a < 3; ++a) {
            a0[a] = std::clamp(static_cast<int>(std::floor((p[a] - L.radius - lo[a]) / cell)), 0, static_cast<int>(dim[a]) - 1);
            a1[a] = std::clamp(static_cast<int>(std::floor((p[a] + L.radius - lo[a]) / cell)), 0, static_cast<int>(dim[a]) - 1);
        }
        for (int z = a0[2]; z <= a1[2]; ++z)
            for (int y = a0[1]; y <= a1[1]; ++y)
                for (int x = a0[0]; x <= a1[0]; ++x) {
                    // the sphere of the light against the cell's box
                    const int c[3] = {x, y, z};
                    double d2 = 0;
                    for (int a = 0; a < 3; ++a) {
                        const double cl = lo[a] + c[a] * cell, ch = cl + cell;
                        const double q = p[a] < cl ? cl - p[a] : p[a] > ch ? p[a] - ch : 0.0;
                        d2 += q * q;
                    }
                    if (d2 > L.radius * L.radius) continue;
                    fn((static_cast<size_t>(z) * dim[1] + y) * dim[0] + x);
                }
    };
    for (uint32_t k : local) forCells(lights[k], [&](size_t c) { ++counts[c]; });
    out->cells.resize(cellCount * 2);
    uint32_t offset = out->globalCount;
    for (size_t c = 0; c < cellCount; ++c) {
        out->cells[2 * c] = offset;
        out->cells[2 * c + 1] = counts[c];
        offset += counts[c];
    }
    out->index.resize(offset);
    std::fill(counts.begin(), counts.end(), 0u);
    for (uint32_t k : local) forCells(lights[k], [&](size_t c) { out->index[out->cells[2 * c] + counts[c]++] = k; });
    return out;
}

std::shared_ptr<const ObjectRules> makeRtRules(const scene::SceneSnapshot& snapshot, const core::LightingClasses& classes) {
    auto rules = std::make_shared<ObjectRules>();
    for (const auto& [key, p] : snapshot.objects) {
        if (!p.surfaces || p.surfaces->empty()) continue;
        const std::string& cls = p.surfaces->front().classname;
        bool anyCasts = false;
        for (const core::LightSurface& s : *p.surfaces)
            if (s.castsShadow) {
                anyCasts = true;
                break;
            }
        uint32_t flags = 0;
        if (classes.isLightmapped(cls)) flags |= kObjReceives;
        if (classes.castsShadows(cls) && anyCasts) flags |= kObjCasts;
        (*rules)[key] = flags;
    }
    return rules;
}

// ---- the producer ---------------------------------------------------------------------------------------------------------------

RtLightsProducer::RtLightsProducer(std::function<void(std::shared_ptr<const RtLightData>)> done) : done_(std::move(done)) { thread_ = std::thread([this] { run(); }); }

RtLightsProducer::~RtLightsProducer() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void RtLightsProducer::request(Request r) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = std::move(r);
        hasPending_ = true;
    }
    wake_.notify_all();
}

void RtLightsProducer::waitIdle() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [&] { return !hasPending_ && !busy_; });
}

void RtLightsProducer::run() {
    for (;;) {
        Request r;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return stop_ || hasPending_; });
            if (stop_) return;
            r = std::move(pending_);
            hasPending_ = false;
            busy_ = true;
        }
        std::shared_ptr<const RtLightData> data;
        if (r.snapshot && r.snapshot->doc && r.profile) {
            try {
                core::Document coreDoc = doc::toCoreDocument(*r.snapshot->doc);
                const std::vector<core::ResolvedEntity> resolved = r.snapshot->prefabs ? r.snapshot->prefabs->flat() : std::vector<core::ResolvedEntity>();
                const core::LightingSource source{coreDoc, resolved, *r.profile, r.assetsRoot};
                core::LightScene all = core::lightSceneOf(source, skyCache_);
                core::LightScene style0 = core::lightsOfStyle(all, 0);
                for (core::Light& l : style0.lights) {   // the bake's defaults for lights authored without a source size
                    if (l.sourceSize > 0) continue;
                    if (l.type == core::LightType::Point || l.type == core::LightType::Spot) l.sourceSize = kDefaultSourceSize;
                    else if (l.type == core::LightType::Directional) l.sourceSize = kDefaultSunSize;
                }
                data = makeRtLightData(style0.lights);
            } catch (const std::exception& e) {
                base::logError(std::string("ray traced Lit: the lights could not be collected: ") + e.what());
            }
        }
        if (data && !(last_ && *last_ == *data)) {
            last_ = data;
            if (done_) done_(data);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            busy_ = false;
        }
        idle_.notify_all();
    }
}

// ---- the GPU copies -------------------------------------------------------------------------------------------------------------

RtLightsGpu::RtLightsGpu(const gpu::Device& device) : device_(device) {
    RtLightData empty;
    empty.records.assign(64, 0.f);
    empty.cells = {0u, 0u};
    empty.index = {0u};
    upload(empty);
}

RtLightsGpu::~RtLightsGpu() {
    destroy(records_);
    destroy(cells_);
    destroy(index_);
}

bool RtLightsGpu::make(Buf& b, VkDeviceSize bytes, bool host) const {
    bytes = std::max<VkDeviceSize>(bytes, 64);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = host ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (host) aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    if (vmaCreateBuffer(device_.allocator(), &bci, &aci, &b.buffer, &b.alloc, nullptr) != VK_SUCCESS) return false;
    b.size = bytes;
    return true;
}

void RtLightsGpu::destroy(Buf& b) const {
    if (b.buffer) vmaDestroyBuffer(device_.allocator(), b.buffer, b.alloc);
    b = {};
}

bool RtLightsGpu::upload(const RtLightData& data) {
    struct Part {
        const void* src;
        VkDeviceSize bytes;
        Buf* dst;
    };
    Buf nr, nc, ni;
    const Part parts[3] = {{data.records.data(), data.records.size() * sizeof(float), &nr}, {data.cells.data(), data.cells.size() * sizeof(uint32_t), &nc},
                           {data.index.data(), data.index.size() * sizeof(uint32_t), &ni}};
    std::vector<Buf> staging(3);
    bool ok = true;
    for (size_t i = 0; i < 3 && ok; ++i) {
        ok = make(*parts[i].dst, parts[i].bytes, false) && make(staging[i], parts[i].bytes, true);
        if (!ok) break;
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(device_.allocator(), staging[i].alloc, &info);
        std::memset(info.pMappedData, 0, static_cast<size_t>(staging[i].size));
        if (parts[i].bytes) std::memcpy(info.pMappedData, parts[i].src, static_cast<size_t>(parts[i].bytes));
        vmaFlushAllocation(device_.allocator(), staging[i].alloc, 0, VK_WHOLE_SIZE);
    }
    if (ok) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = device_.graphicsFamily();
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
        for (size_t i = 0; i < 3; ++i) {
            const VkBufferCopy c{0, 0, std::max<VkDeviceSize>(parts[i].bytes, 4)};
            vkCmdCopyBuffer(cmd, staging[i].buffer, parts[i].dst->buffer, 1, &c);
        }
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &dep);
        vkEndCommandBuffer(cmd);
        VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cbi.commandBuffer = cmd;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &cbi;
        ok = device_.submitAndWait(device_.graphicsQueue(), si, "ray traced light upload");
        vkDestroyCommandPool(device_.device(), pool, nullptr);
    }
    for (Buf& s : staging) destroy(s);
    if (!ok) {
        destroy(nr);
        destroy(nc);
        destroy(ni);
        base::logError("ray traced Lit: the lights could not be uploaded");
        return false;
    }
    destroy(records_);
    destroy(cells_);
    destroy(index_);
    records_ = nr;
    cells_ = nc;
    index_ = ni;
    info_ = RtLightData{};
    info_.lightCount = data.lightCount;
    info_.globalCount = data.globalCount;
    std::memcpy(info_.origin, data.origin, sizeof(info_.origin));
    std::memcpy(info_.dim, data.dim, sizeof(info_.dim));
    info_.cell = data.cell;
    ++serial_;
    return true;
}

} // namespace native::render
