// render/model_renderer -- the GPU side of the models of entities (T11.9c): vertex / index buffers per model, the posed vertices of animated models, the instance matrices.
//
// Owns: one host-visible buffer per loaded model (rest pose vertices and indices; made when a model first appears in a scene, retired when no placement uses it any more), the
// texture table entries of the model's images, and per frame (through the upload ring) the vertices of animated models (posed on the CPU once per iteration and animation, however many
// entities and viewports show them) and the instance matrices of what a viewport sees. `sync` runs once per render-thread iteration, `buildDraws` once per viewport frame.
// Thread: setScene from any thread; the rest on the render thread.
// Spec: 08_MILESTONES.md T11.9c.
#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "gpu/device.h"
#include "gpu/upload_ring.h"
#include "render/math3d.h"
#include "render/model_draws.h"
#include "render/model_scene.h"
#include "render/texture_table.h"

namespace native::render {

class ModelRenderer {
public:
    ModelRenderer(gpu::Device& device, TextureTable& textures);
    ~ModelRenderer();   // the GPU must be idle
    ModelRenderer(const ModelRenderer&) = delete;
    ModelRenderer& operator=(const ModelRenderer&) = delete;

    /// The placements to draw (any thread); takes effect at the next sync. Null = none.
    void setScene(std::shared_ptr<const ModelScene> scene);

    /// Render thread, inside the iteration's upload frame: takes the pending scene, makes the GPU buffers of new models, retires the unused ones, poses the animated models at
    /// `seconds`. `completed` / `submitted` = the timeline values, as GpuScene::sync.
    void sync(gpu::UploadRing& ring, uint64_t completed, uint64_t submitted, double seconds);

    /// Render thread: the draws of the placements inside the frustum of `viewProj`.
    void buildDraws(const Mat4& viewProj, gpu::UploadRing& ring, ModelDrawList& out) const;

    /// True when a placed model plays an animation (the viewports need a frame every few milliseconds).
    bool animated() const { return animated_.load(); }
    /// Grows whenever the set of placements changed.
    uint64_t changeSerial() const { return changes_.load(); }
    struct Stats {
        uint32_t placements = 0, models = 0;
        uint64_t bytes = 0;
    };
    Stats stats() const;

private:
    struct Prim {
        VkDeviceSize vertexOffset = 0, indexOffset = 0;
        uint32_t vertexCount = 0, indexCount = 0;
        float color[4] = {1, 1, 1, 1};
        float texture = 0, hasTexture = 0, alphaMode = 0, cutoff = 0.5f;
    };
    struct GpuModel {
        std::shared_ptr<const scene::LoadedModel> model;   // keeps the CPU data alive for posing
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        std::vector<Prim> prims;
        uint64_t bytes = 0;
    };
    struct Group {
        GpuModel* gpu = nullptr;
        int anim = -1;
        bool animated = false;
        std::vector<uint32_t> placements;                 // into scene_->placements
        std::vector<gpu::UploadRing::Allocation> posed;   // animated: per primitive the vertices of this iteration (in the ring)
    };
    struct Retired {
        VkBuffer buffer;
        VmaAllocation allocation;
        uint64_t value;
    };
    std::unique_ptr<GpuModel> makeModel(const std::shared_ptr<const scene::LoadedModel>& model);
    void destroy(GpuModel& m);

    gpu::Device& device_;
    TextureTable& textures_;
    std::mutex mutex_;
    std::shared_ptr<const ModelScene> pending_;
    bool hasPending_ = false;

    std::shared_ptr<const ModelScene> scene_;
    std::map<const scene::LoadedModel*, std::unique_ptr<GpuModel>> models_;
    std::vector<Group> groups_;
    std::vector<Retired> retired_;
    std::atomic<bool> animated_{false};
    std::atomic<uint64_t> changes_{0};
};

} // namespace native::render
