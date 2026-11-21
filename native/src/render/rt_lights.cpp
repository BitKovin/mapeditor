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
           std::memcmp(dim, o.dim, sizeof(dim)) == 0 && records == o.records && index == o.index && cells == o.cells && layerCount == o.layerCount &&
           std::memcmp(layerStyle, o.layerStyle, sizeof(layerStyle)) == 0 && std::memcmp(layerGlobalOffset, o.layerGlobalOffset, sizeof(layerGlobalOffset)) == 0 &&
           std::memcmp(layerGlobalCount, o.layerGlobalCount, sizeof(layerGlobalCount)) == 0 && cellCount == o.cellCount && styles.patterns == o.styles.patterns;
}

void RtLightData::layerWeights(double seconds, float out[4]) const {
    out[0] = 1.0f;
    for (uint32_t l = 1; l < 4; ++l) out[l] = l < layerCount && layerStyle[l] > 0 ? static_cast<float>(styles.value(layerStyle[l], seconds)) : 1.0f;
}

bool RtLightData::animated() const {
    for (uint32_t l = 1; l < layerCount; ++l)
        if (layerStyle[l] > 0 && !styles.patterns[static_cast<size_t>(layerStyle[l])].empty()) return true;
    return false;
}

std::shared_ptr<const RtLightData> makeRtLightData(const std::vector<core::Light>& source, const core::LightStyleTable* table) {
    auto out = std::make_shared<RtLightData>();
    out->styles = table ? *table : core::LightStyleTable::defaults();
    std::vector<core::Light> lights = source;
    // ---- the style layers
    std::vector<int> ids;
    for (core::Light& l : lights) {
        l.style = std::clamp(l.style, 0, core::kMaxStyleId);
        if (l.style != 0) ids.push_back(l.style);
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    const size_t real = ids.size() > 3 ? 2 : ids.size();     // styles that keep a layer of their own (the rest share the last layer as constant light)
    out->layerCount = static_cast<uint32_t>(1 + std::min<size_t>(ids.size(), 3));
    for (size_t i = 0; i < real; ++i) out->layerStyle[1 + i] = ids[i];
    std::vector<uint32_t> layerOf(lights.size(), 0);
    for (size_t k = 0; k < lights.size(); ++k) {
        core::Light& l = lights[k];
        if (l.style == 0) continue;
        const auto it = std::lower_bound(ids.begin(), ids.end(), l.style);
        const size_t i = static_cast<size_t>(it - ids.begin());
        if (i < real) {
            layerOf[k] = static_cast<uint32_t>(1 + i);
            continue;
        }
        layerOf[k] = 3;   // merged: the light scaled by the mean of its pattern
        const std::string& pattern = out->styles.patterns[static_cast<size_t>(l.style)];
        double mean = 1.0;
        if (!pattern.empty()) {
            mean = 0.0;
            for (size_t f = 0; f < pattern.size(); ++f) mean += out->styles.value(l.style, (static_cast<double>(f) + 0.5) / core::kStyleHz);
            mean /= static_cast<double>(pattern.size());
        }
        l.brightness *= mean;
    }
    out->lightCount = static_cast<uint32_t>(lights.size());
    out->records = light::packLights(lights);
    // ---- the lists: the globals of every layer first, then the cells
    std::vector<uint32_t> local[4];
    for (uint32_t layer = 0; layer < out->layerCount; ++layer) {
        out->layerGlobalOffset[layer] = static_cast<uint32_t>(out->index.size());
        for (uint32_t k = 0; k < lights.size(); ++k) {
            if (layerOf[k] != layer) continue;
            const core::LightType t = lights[k].type;
            if (t == core::LightType::Directional || t == core::LightType::Sky) out->index.push_back(k);
            else if (lights[k].radius > 0) local[layer].push_back(k);
        }
        out->layerGlobalCount[layer] = static_cast<uint32_t>(out->index.size()) - out->layerGlobalOffset[layer];
    }
    out->globalCount = out->layerGlobalCount[0];
    const uint32_t listBase = static_cast<uint32_t>(out->index.size());
    bool anyLocal = false;
    for (uint32_t layer = 0; layer < out->layerCount; ++layer) anyLocal = anyLocal || !local[layer].empty();
    if (!anyLocal) {
        out->cells.assign(2u * out->layerCount, 0u);
        return out;
    }
    // the bounds of the lights' ranges, and a cell size that keeps the grid at a few million cells
    double lo[3] = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
    double hi[3] = {std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest()};
    for (uint32_t layer = 0; layer < out->layerCount; ++layer)
        for (uint32_t k : local[layer]) {
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
        if (cells * out->layerCount <= 4.0e6) break;
        cell *= 1.25;
    }
    for (int a = 0; a < 3; ++a) {
        out->origin[a] = static_cast<float>(lo[a]);
        out->dim[a] = dim[a];
    }
    out->cell = static_cast<float>(cell);
    const size_t cellCount = static_cast<size_t>(dim[0]) * dim[1] * dim[2];
    out->cellCount = static_cast<uint32_t>(cellCount);
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
    // per layer: pass 1 counts the lights per cell, pass 2 fills (the same loop twice)
    out->cells.resize(cellCount * 2 * out->layerCount);
    uint32_t offset = listBase;
    std::vector<std::vector<uint32_t>> counts(out->layerCount, std::vector<uint32_t>(cellCount, 0u));
    for (uint32_t layer = 0; layer < out->layerCount; ++layer) {
        for (uint32_t k : local[layer]) forCells(lights[k], [&](size_t c) { ++counts[layer][c]; });
        for (size_t c = 0; c < cellCount; ++c) {
            out->cells[2 * (layer * cellCount + c)] = offset;
            out->cells[2 * (layer * cellCount + c) + 1] = counts[layer][c];
            offset += counts[layer][c];
        }
    }
    out->index.resize(offset);
    for (uint32_t layer = 0; layer < out->layerCount; ++layer) {
        std::fill(counts[layer].begin(), counts[layer].end(), 0u);
        for (uint32_t k : local[layer]) forCells(lights[k], [&](size_t c) { out->index[out->cells[2 * (layer * cellCount + c)] + counts[layer][c]++] = k; });
    }
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
                for (core::Light& l : all.lights) {   // the bake's defaults for lights authored without a source size
                    if (l.sourceSize > 0) continue;
                    if (l.type == core::LightType::Point || l.type == core::LightType::Spot) l.sourceSize = kDefaultSourceSize;
                    else if (l.type == core::LightType::Directional) l.sourceSize = kDefaultSunSize;
                }
                const core::LightStyleTable styles = core::LightStyleTable::fromProfile(*r.profile);
                data = makeRtLightData(all.lights, &styles);
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
    info_.layerCount = std::max(1u, data.layerCount);
    std::memcpy(info_.layerStyle, data.layerStyle, sizeof(info_.layerStyle));
    std::memcpy(info_.layerGlobalOffset, data.layerGlobalOffset, sizeof(info_.layerGlobalOffset));
    std::memcpy(info_.layerGlobalCount, data.layerGlobalCount, sizeof(info_.layerGlobalCount));
    info_.cellCount = std::max(1u, data.cellCount);
    info_.styles = data.styles;
    std::memcpy(info_.origin, data.origin, sizeof(info_.origin));
    std::memcpy(info_.dim, data.dim, sizeof(info_.dim));
    info_.cell = data.cell;
    ++serial_;
    return true;
}

} // namespace native::render
