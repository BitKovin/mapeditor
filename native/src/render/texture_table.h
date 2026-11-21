// render/texture_table -- the global texture array of the renderer: albedo images by name, decoded on the job pool, one descriptor array.
//
// Owns: the descriptor set layout / set of the scene pass (binding 0: sampled images `textures[]` (update after bind, partially bound),
// binding 1: the per-texture meta buffer (transparency), binding 2/3: the sky cube / panorama), the sampler, the images and the loading of
// them: a texture is requested by name when a mesh is uploaded (indexOf), decoded and mip-mapped on a worker (core::readTexture / decodeImage,
// the texture's sidecar for its transparency), and uploaded on the render thread by upload(). Until then (and when it cannot be loaded) its
// array element shows the placeholder image, so a mesh never waits for a texture.
// Thread: indexOf / upload / sky calls from the render thread; the decoding runs on the pool; setSource from any thread.
// Spec: 05_RENDERER.md §2 (binding model), 08_MILESTONES.md T3.4.
#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/jobs.h"
#include "gpu/device.h"
#include "gpu/upload_ring.h"

namespace native::render {

class TextureTable {
public:
    static constexpr uint32_t kMax = 4096;

    explicit TextureTable(gpu::Device& device);
    ~TextureTable();   // the GPU must be idle
    TextureTable(const TextureTable&) = delete;
    TextureTable& operator=(const TextureTable&) = delete;

    bool valid() const { return set_ != VK_NULL_HANDLE; }
    VkDescriptorSetLayout layout() const { return layout_; }
    VkDescriptorSet set() const { return set_; }

    // Where textures are read from (the assets root, the game profile in it) and what runs the decoding. `onChange` is called from a worker when
    // a decoded image is waiting for upload() (wake the render thread). Safe from any thread; textures asked for earlier start loading.
    void setSource(const std::filesystem::path& assetsRoot, base::JobPool* pool, std::function<void()> onChange);
    // Called by upload() before it rewrites descriptors that frames in flight may still read: returns when the GPU finished them.
    void setGpuIdleWait(std::function<void()> wait) { gpuIdle_ = std::move(wait); }

    // The array index of a texture name (0 = the placeholder, for "" and for textures that cannot be loaded). Starts loading on first sight.
    uint32_t indexOf(const std::string& name);
    // The first of the six consecutive entries a skybox ("env/<name> cube") fills with its faces (or its one panorama in the first); the shader
    // finds out which from meta[first].x (3 = faces, 4 = panorama; 0 while not loaded: the placeholder). 0 for a name that is not a skybox.
    uint32_t skyIndex(const std::string& name);

    // Adds an image made in memory (one mip level, RGBA8) under `name` (the glyph atlas); it is uploaded by the next upload(). Returns its index, or 0 when
    // the table is full or the name is taken by a file texture. Asking again for the same name returns the same index. Render thread.
    uint32_t addImage(const std::string& name, int width, int height, std::vector<uint8_t> rgba);

    // Uploads the images decoded since the last call. Returns true when more are waiting (the ring slice was full): call again next frame.
    bool upload(VkCommandBuffer cmd, gpu::UploadRing& ring);

    // True when the texture at `index` blends (transparency mode translucent); false while it is not loaded. Render thread.
    bool translucent(uint32_t index) const { return meta_ && index < kMax && meta_[static_cast<size_t>(index) * 4] == 2.0f; }

    // True when the texture at `index` lets light through or has holes (cutout or translucent); false while it is not loaded. Render thread.
    bool alphaSurface(uint32_t index) const {
        if (!meta_ || index >= kMax) return false;
        const float mode = meta_[static_cast<size_t>(index) * 4];
        return mode == 1.0f || mode == 2.0f;
    }

    uint32_t assigned() const { return static_cast<uint32_t>(entries_.size()); }
    uint32_t loaded() const { return loaded_; }
    uint32_t failed() const { return failed_; }
    uint32_t pending() const;
    uint64_t bytes() const { return bytes_; }

private:
    struct Entry {
        std::string name;
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VkImageView view = VK_NULL_HANDLE;
        enum class State { Queued, Loading, Decoded, Uploaded, Failed } state = State::Queued;
    };
    struct Decoded {
        uint32_t index = 0;
        int width = 0, height = 0;
        std::vector<std::vector<uint8_t>> mips;   // RGBA8, mip l is max(1, w >> l) x max(1, h >> l)
        float meta[4] = {0, 0.5f, 1, 0};          // transparency mode (0 opaque, 1 cutout, 2 translucent), cutoff, opacity, 1 + tint
        bool ok = false;
    };
    void startLoad(uint32_t index, const std::string& name);
    void load(uint32_t index, std::string name);
    void writeDescriptor(uint32_t index, VkImageView view);
    bool makeImage(Entry& e, int width, int height, uint32_t mips);
    void makePlaceholder();

    gpu::Device& device_;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkBuffer metaBuffer_ = VK_NULL_HANDLE;
    VmaAllocation metaAllocation_ = nullptr;
    float* meta_ = nullptr;                    // kMax * 4 floats, host-visible and coherent
    std::function<void()> gpuIdle_;

    std::vector<Entry> entries_;               // render thread; entries_[0] = the placeholder
    std::unordered_map<std::string, uint32_t> byName_;
    uint32_t loaded_ = 0, failed_ = 0;
    uint64_t bytes_ = 0;

    mutable std::mutex mutex_;                 // source and the finished queue
    std::condition_variable idle_;
    std::filesystem::path root_;
    base::JobPool* jobs_ = nullptr;
    std::function<void()> onChange_;
    std::shared_ptr<void> profile_;            // core::GameProfile, kept opaque in this header
    std::deque<Decoded> decoded_;
    std::vector<std::pair<uint32_t, std::string>> waiting_;   // requested before a source was set
    uint32_t inFlight_ = 0;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);   // the loading jobs check it before touching `this`
};

} // namespace native::render
