// render/texture_table -- see texture_table.h.
#include "render/texture_table.h"

#include <core/albedo.h>
#include <core/profile.h>
#include <core/textures.h>

#include <algorithm>
#include <cstring>

#include <string>

#include "base/log.h"

namespace native::render {

namespace {

constexpr int kMaxSide = 2048;   // larger images are halved until they fit

void downsample(const std::vector<uint8_t>& src, int w, int h, std::vector<uint8_t>& dst, int& nw, int& nh) {
    nw = std::max(1, w / 2);
    nh = std::max(1, h / 2);
    dst.resize(static_cast<size_t>(nw) * nh * 4);
    for (int y = 0; y < nh; ++y)
        for (int x = 0; x < nw; ++x) {
            const int x0 = std::min(w - 1, x * 2), x1 = std::min(w - 1, x * 2 + 1);
            const int y0 = std::min(h - 1, y * 2), y1 = std::min(h - 1, y * 2 + 1);
            for (int c = 0; c < 4; ++c) {
                const int sum = src[(static_cast<size_t>(y0) * w + x0) * 4 + c] + src[(static_cast<size_t>(y0) * w + x1) * 4 + c] +
                                src[(static_cast<size_t>(y1) * w + x0) * 4 + c] + src[(static_cast<size_t>(y1) * w + x1) * 4 + c];
                dst[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<uint8_t>((sum + 2) / 4);
            }
        }
}

void oneTimeSubmit(gpu::Device& device, const std::function<void(VkCommandBuffer)>& record) {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = device.graphicsFamily();
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(device.device(), &pci, nullptr, &pool);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device.device(), &ai, &cmd);
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
    vkQueueSubmit2(device.graphicsQueue(), 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(device.graphicsQueue());
    vkDestroyCommandPool(device.device(), pool, nullptr);
}

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess, uint32_t mips) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

TextureTable::TextureTable(gpu::Device& device) : device_(device) {
    const VkDevice dev = device_.device();
    // layout: 0 textures[], 1 meta (storage buffer)
    VkDescriptorSetLayoutBinding b[2]{};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[0].descriptorCount = kMax;
    b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkDescriptorBindingFlags flags[2] = {VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, 0};
    VkDescriptorSetLayoutBindingFlagsCreateInfo fi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    fi.bindingCount = 2;
    fi.pBindingFlags = flags;
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.pNext = &fi;
    lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    lci.bindingCount = 2;
    lci.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev, &lci, nullptr, &layout_) != VK_SUCCESS) return;
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMax}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pci.maxSets = 1;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(dev, &pci, nullptr, &pool_) != VK_SUCCESS) return;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout_;
    if (vkAllocateDescriptorSets(dev, &ai, &set_) != VK_SUCCESS) {
        set_ = VK_NULL_HANDLE;
        return;
    }

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(device_.physicalDevice(), &props);
    VkPhysicalDeviceFeatures feats{};
    vkGetPhysicalDeviceFeatures(device_.physicalDevice(), &feats);
    if (feats.samplerAnisotropy) {
        sci.anisotropyEnable = VK_TRUE;
        sci.maxAnisotropy = std::min(8.0f, props.limits.maxSamplerAnisotropy);
    }
    vkCreateSampler(dev, &sci, nullptr, &sampler_);

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = sizeof(float) * 4 * kMax;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    vmaCreateBuffer(device_.allocator(), &bci, &aci, &metaBuffer_, &metaAllocation_, &info);
    meta_ = static_cast<float*>(info.pMappedData);
    if (meta_) std::memset(meta_, 0, sizeof(float) * 4 * kMax);
    VkDescriptorBufferInfo mbi{metaBuffer_, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set_;
    w.dstBinding = 1;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &mbi;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);

    entries_.reserve(256);
    entries_.push_back(Entry{});   // 0: the placeholder
    makePlaceholder();
}

TextureTable::~TextureTable() {
    alive_->store(false);
    {
        std::unique_lock<std::mutex> lock(mutex_);
        idle_.wait(lock, [&] { return inFlight_ == 0; });
    }
    const VkDevice dev = device_.device();
    for (Entry& e : entries_) {
        if (e.view) vkDestroyImageView(dev, e.view, nullptr);
        if (e.image) vmaDestroyImage(device_.allocator(), e.image, e.allocation);
    }
    if (metaBuffer_) vmaDestroyBuffer(device_.allocator(), metaBuffer_, metaAllocation_);
    if (sampler_) vkDestroySampler(dev, sampler_, nullptr);
    if (pool_) vkDestroyDescriptorPool(dev, pool_, nullptr);
    if (layout_) vkDestroyDescriptorSetLayout(dev, layout_, nullptr);
}

bool TextureTable::makeImage(Entry& e, int width, int height, uint32_t mips) {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;   // the textures' own space, no sRGB conversion (DESIGN §6)
    ici.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    ici.mipLevels = mips;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(device_.allocator(), &ici, &aci, &e.image, &e.allocation, nullptr) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = e.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = ici.format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
    return vkCreateImageView(device_.device(), &vci, nullptr, &e.view) == VK_SUCCESS;
}

void TextureTable::writeDescriptor(uint32_t index, VkImageView view) {
    VkDescriptorImageInfo ii{sampler_, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set_;
    w.dstBinding = 0;
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(device_.device(), 1, &w, 0, nullptr);
}

void TextureTable::makePlaceholder() {
    // 8 x 8 checker of two greys: a texture that is not there (yet) still shows its face
    constexpr int N = 8;
    std::vector<uint8_t> px(N * N * 4);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            const uint8_t v = ((x / 4 + y / 4) & 1) ? 150 : 110;
            uint8_t* p = &px[(static_cast<size_t>(y) * N + x) * 4];
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    Entry& e = entries_[0];
    e.name = "";
    if (!makeImage(e, N, N, 1)) return;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation sa = nullptr;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = px.size();
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    vmaCreateBuffer(device_.allocator(), &bci, &aci, &staging, &sa, &info);
    std::memcpy(info.pMappedData, px.data(), px.size());
    vmaFlushAllocation(device_.allocator(), sa, 0, VK_WHOLE_SIZE);
    oneTimeSubmit(device_, [&](VkCommandBuffer cmd) {
        imageBarrier(cmd, e.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT, 1);
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {N, N, 1};
        vkCmdCopyBufferToImage(cmd, staging, e.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        imageBarrier(cmd, e.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1);
    });
    vmaDestroyBuffer(device_.allocator(), staging, sa);
    e.state = Entry::State::Uploaded;
    // every element starts on the placeholder
    std::vector<VkDescriptorImageInfo> infos(kMax, VkDescriptorImageInfo{sampler_, e.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set_;
    w.dstBinding = 0;
    w.descriptorCount = kMax;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = infos.data();
    vkUpdateDescriptorSets(device_.device(), 1, &w, 0, nullptr);
}

void TextureTable::setSource(const std::filesystem::path& assetsRoot, base::JobPool* pool, std::function<void()> onChange) {
    std::vector<std::pair<uint32_t, std::string>> start;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        root_ = assetsRoot;
        jobs_ = pool;
        onChange_ = std::move(onChange);
        profile_ = std::make_shared<core::GameProfile>(core::loadProfile(assetsRoot));
        start.swap(waiting_);
    }
    for (const auto& [i, name] : start) startLoad(i, name);
}

uint32_t TextureTable::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint32_t>(decoded_.size()) + inFlight_;
}

uint32_t TextureTable::indexOf(const std::string& name) {
    if (name.empty() || !valid()) return 0;
    const auto it = byName_.find(name);
    if (it != byName_.end()) return it->second;
    if (entries_.size() >= kMax) return 0;
    const uint32_t index = static_cast<uint32_t>(entries_.size());
    Entry e;
    e.name = name;
    entries_.push_back(std::move(e));
    byName_.emplace(name, index);
    entries_[index].state = Entry::State::Loading;
    startLoad(index, name);
    return index;
}

uint32_t TextureTable::skyIndex(const std::string& name) {
    if (name.empty() || !valid()) return 0;
    const std::string key = "sky:" + name;
    const auto it = byName_.find(key);
    if (it != byName_.end()) return it->second;
    if (entries_.size() + 6 > kMax) return 0;
    const uint32_t base = static_cast<uint32_t>(entries_.size());
    for (uint32_t i = 0; i < 6; ++i) {
        Entry e;
        e.name = key + "#" + std::to_string(i);
        e.state = Entry::State::Loading;
        entries_.push_back(std::move(e));
    }
    byName_.emplace(key, base);
    startLoad(base, key);
    return base;
}

void TextureTable::startLoad(uint32_t index, const std::string& name) {
    base::JobPool* pool = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!jobs_) {
            waiting_.emplace_back(index, name);
            return;
        }
        pool = jobs_;
        ++inFlight_;
    }
    const auto alive = alive_;
    pool->submit([this, alive, index, name] {
        if (alive->load()) load(index, name);
        std::lock_guard<std::mutex> lock(mutex_);   // `this` outlives the job: the destructor waits for inFlight_ == 0
        --inFlight_;
        idle_.notify_all();
    }, 2);
}

namespace {

// Decodes an image file, halves it until it fits, makes its mip chain. False when the file is not an image.
bool decodeFile(const core::TextureFile& file, int& outW, int& outH, std::vector<std::vector<uint8_t>>& mips, core::AlphaKind* kind) {
    auto img = core::decodeImage(file.bytes, file.ext);
    if (!img || img->width <= 0 || img->height <= 0 || img->rgba.empty()) return false;
    int w = img->width, h = img->height;
    std::vector<uint8_t> level = std::move(img->rgba);
    while (std::max(w, h) > kMaxSide) {
        std::vector<uint8_t> next;
        int nw, nh;
        downsample(level, w, h, next, nw, nh);
        level = std::move(next);
        w = nw;
        h = nh;
    }
    outW = w;
    outH = h;
    if (kind) *kind = core::classifyAlpha(level.data(), static_cast<size_t>(w) * h);
    mips.push_back(std::move(level));
    while (w > 1 || h > 1) {
        std::vector<uint8_t> next;
        int nw, nh;
        downsample(mips.back(), w, h, next, nw, nh);
        mips.push_back(std::move(next));
        w = nw;
        h = nh;
    }
    return true;
}

} // namespace

void TextureTable::load(uint32_t index, std::string name) {
    std::filesystem::path root;
    std::shared_ptr<core::GameProfile> profile;
    std::function<void()> notify;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        root = root_;
        profile = std::static_pointer_cast<core::GameProfile>(profile_);
        notify = onChange_;
    }
    std::vector<Decoded> out;
    const bool sky = name.rfind("sky:", 0) == 0;
    if (sky) {
        // a skybox: six faces in the entries index .. index + 5 (_lf _rt _up _dn _ft _bk), or one panorama in `index`
        const auto files = core::readSkybox(root, name.substr(4));
        bool ok = files.has_value() && !files->images.empty();
        if (ok) {
            const size_t n = files->panorama ? 1 : std::min<size_t>(6, files->images.size());
            if (!files->panorama && n < 6) ok = false;
            for (size_t i = 0; ok && i < n; ++i) {
                Decoded d;
                d.index = index + static_cast<uint32_t>(i);
                if (!decodeFile(files->images[i], d.width, d.height, d.mips, nullptr)) {
                    ok = false;
                    break;
                }
                d.meta[0] = i == 0 ? (files->panorama ? 4.0f : 3.0f) : 0.0f;   // read by the shader at the first entry: 3 = cube faces, 4 = panorama
                d.meta[1] = 0.5f;
                d.meta[2] = 1.0f;
                d.meta[3] = 0.0f;
                d.ok = true;
                out.push_back(std::move(d));
            }
        }
        if (!ok) {
            out.clear();
            for (uint32_t i = 0; i < 6; ++i) {
                Decoded d;
                d.index = index + i;
                out.push_back(std::move(d));
            }
        }
    } else {
        Decoded d;
        d.index = index;
        if (profile) {
            if (auto file = core::readTexture(root, *profile, name)) {
                core::AlphaKind kind = core::AlphaKind::None;
                if (decodeFile(*file, d.width, d.height, d.mips, &kind)) {
                    std::string propsJson;
                    if (auto props = core::readTextureProps(root, *profile, name)) propsJson = props->dump();
                    const core::Transparency t = core::resolveTransparency(propsJson, kind);
                    d.meta[0] = static_cast<float>(static_cast<int>(t.mode));
                    d.meta[1] = t.cutoff;
                    d.meta[2] = t.opacity;
                    d.meta[3] = kind == core::AlphaKind::None ? 0.0f : 1.0f;
                    d.ok = true;
                }
            }
        }
        out.push_back(std::move(d));
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (Decoded& d : out) decoded_.push_back(std::move(d));
    }
    if (notify) notify();
}

bool TextureTable::upload(VkCommandBuffer cmd, gpu::UploadRing& ring) {
    std::deque<Decoded> ready;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ready.swap(decoded_);
    }
    if (ready.empty()) return false;
    // Elements may be read by frames still in flight (the placeholder, a few frames ago): let them finish before rewriting.
    if (gpuIdle_) gpuIdle_();
    bool more = false;
    while (!ready.empty()) {
        Decoded d = std::move(ready.front());
        ready.pop_front();
        Entry& e = entries_[d.index];
        if (!d.ok) {
            e.state = Entry::State::Failed;
            ++failed_;
            base::logWarning("texture '" + e.name + "' could not be loaded");
            continue;
        }
        VkDeviceSize total = 0;
        for (const auto& m : d.mips) total += (m.size() + 15) & ~size_t(15);
        const gpu::UploadRing::Allocation a = ring.allocate(total, 16);
        if (!a.ok() || !makeImage(e, d.width, d.height, static_cast<uint32_t>(d.mips.size()))) {
            if (!a.ok()) {
                // the slice is full: try again next frame
                ready.push_front(std::move(d));
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    for (auto it = ready.rbegin(); it != ready.rend(); ++it) decoded_.push_front(std::move(*it));
                }
                more = true;
                break;
            }
            e.state = Entry::State::Failed;
            ++failed_;
            continue;
        }
        std::vector<VkBufferImageCopy> regions;
        VkDeviceSize offset = 0;
        for (size_t l = 0; l < d.mips.size(); ++l) {
            std::memcpy(static_cast<uint8_t*>(a.data) + offset, d.mips[l].data(), d.mips[l].size());
            VkBufferImageCopy r{};
            r.bufferOffset = a.offset + offset;
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, static_cast<uint32_t>(l), 0, 1};
            r.imageExtent = {static_cast<uint32_t>(std::max(1, d.width >> l)), static_cast<uint32_t>(std::max(1, d.height >> l)), 1};
            regions.push_back(r);
            offset += (d.mips[l].size() + 15) & ~size_t(15);
        }
        ring.flush(a);
        const uint32_t mips = static_cast<uint32_t>(d.mips.size());
        imageBarrier(cmd, e.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT, mips);
        vkCmdCopyBufferToImage(cmd, a.buffer, e.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(regions.size()), regions.data());
        imageBarrier(cmd, e.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, mips);
        writeDescriptor(d.index, e.view);
        if (meta_) std::memcpy(meta_ + static_cast<size_t>(d.index) * 4, d.meta, sizeof(d.meta));
        e.state = Entry::State::Uploaded;
        ++loaded_;
        bytes_ += total;
    }
    return more;
}

} // namespace native::render
