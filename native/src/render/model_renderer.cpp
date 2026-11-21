// render/model_renderer -- see model_renderer.h.
#include "render/model_renderer.h"

#include <algorithm>
#include <cstring>
#include <set>

#include "base/log.h"
#include "render/frustum.h"

namespace native::render {

namespace {

constexpr VkDeviceSize align16(VkDeviceSize v) { return (v + 15) & ~VkDeviceSize(15); }

// The vertices of a primitive as the shaders read them.
void writeVertices(ModelVertex* out, const scene::ModelPrimitive& p, const scene::PosedPrimitive& pose) {
    const size_t n = p.vertexCount();
    for (size_t v = 0; v < n; ++v) {
        ModelVertex& o = out[v];
        for (size_t k = 0; k < 3; ++k) {
            o.pos[k] = pose.positions[v * 3 + k];
            o.normal[k] = pose.normals[v * 3 + k];
        }
        o.uv[0] = v * 2 + 1 < p.uvs.size() ? p.uvs[v * 2] : 0.0f;
        o.uv[1] = v * 2 + 1 < p.uvs.size() ? p.uvs[v * 2 + 1] : 0.0f;
    }
}

} // namespace

ModelRenderer::ModelRenderer(gpu::Device& device, TextureTable& textures) : device_(device), textures_(textures) {}

ModelRenderer::~ModelRenderer() {
    for (auto& [ptr, m] : models_) destroy(*m);
    for (Retired& r : retired_)
        if (r.buffer) vmaDestroyBuffer(device_.allocator(), r.buffer, r.allocation);
}

void ModelRenderer::destroy(GpuModel& m) {
    if (m.buffer) vmaDestroyBuffer(device_.allocator(), m.buffer, m.allocation);
    m.buffer = VK_NULL_HANDLE;
    m.allocation = nullptr;
}

void ModelRenderer::setScene(std::shared_ptr<const ModelScene> scene) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_ = std::move(scene);
    hasPending_ = true;
}

ModelRenderer::Stats ModelRenderer::stats() const {
    Stats s;
    if (scene_) s.placements = static_cast<uint32_t>(scene_->placements.size());
    for (const auto& [ptr, m] : models_) {
        ++s.models;
        s.bytes += m->bytes;
    }
    return s;
}

std::unique_ptr<ModelRenderer::GpuModel> ModelRenderer::makeModel(const std::shared_ptr<const scene::LoadedModel>& model) {
    const scene::ModelData& data = model->data;
    const std::vector<scene::PosedPrimitive> rest = scene::poseModel(data, -1, 0);
    auto gm = std::make_unique<GpuModel>();
    gm->model = model;
    VkDeviceSize bytes = 0;
    for (const scene::ModelPrimitive& p : data.primitives) {
        Prim pr;
        pr.vertexCount = static_cast<uint32_t>(p.vertexCount());
        pr.indexCount = static_cast<uint32_t>(p.indices.size());
        pr.vertexOffset = bytes;
        bytes = align16(bytes + VkDeviceSize(pr.vertexCount) * sizeof(ModelVertex));
        pr.indexOffset = bytes;
        bytes = align16(bytes + VkDeviceSize(pr.indexCount) * 4);
        for (size_t i = 0; i < 4; ++i) pr.color[i] = p.color[i];
        if (!p.texture.empty()) {
            const uint32_t index = textures_.indexOf(p.texture);
            pr.texture = static_cast<float>(index);
            pr.hasTexture = index != 0 ? 1.0f : 0.0f;
        } else if (p.image >= 0 && static_cast<size_t>(p.image) < model->images.size() && !model->images[static_cast<size_t>(p.image)].rgba.empty()) {
            const scene::DecodedModelImage& im = model->images[static_cast<size_t>(p.image)];
            const uint32_t index = textures_.addImage("model:" + model->path + "#" + std::to_string(p.image), im.width, im.height, im.rgba);
            pr.texture = static_cast<float>(index);
            pr.hasTexture = index != 0 ? 1.0f : 0.0f;
        }
        if (p.alpha != scene::ModelAlpha::Opaque) {   // blended materials are cut at one half: no blended model pass yet
            pr.alphaMode = 1;
            pr.cutoff = p.alpha == scene::ModelAlpha::Mask ? p.cutoff : 0.5f;
        }
        gm->prims.push_back(pr);
    }
    if (bytes == 0) return nullptr;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(device_.allocator(), &bci, &aci, &gm->buffer, &gm->allocation, &info) != VK_SUCCESS || !info.pMappedData) {
        base::logWarning("ModelRenderer: cannot allocate " + std::to_string(bytes) + " bytes for " + model->path);
        destroy(*gm);
        return nullptr;
    }
    uint8_t* mapped = static_cast<uint8_t*>(info.pMappedData);
    for (size_t i = 0; i < data.primitives.size(); ++i) {
        const scene::ModelPrimitive& p = data.primitives[i];
        const Prim& pr = gm->prims[i];
        writeVertices(reinterpret_cast<ModelVertex*>(mapped + pr.vertexOffset), p, rest[i]);
        std::memcpy(mapped + pr.indexOffset, p.indices.data(), p.indices.size() * 4);
    }
    vmaFlushAllocation(device_.allocator(), gm->allocation, 0, VK_WHOLE_SIZE);
    gm->bytes = bytes;
    return gm;
}

void ModelRenderer::sync(gpu::UploadRing& ring, uint64_t completed, uint64_t submitted, double seconds) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (hasPending_) {
            scene_ = std::move(pending_);
            hasPending_ = false;
            ++changes_;
        }
    }
    for (size_t i = 0; i < retired_.size();) {
        if (retired_[i].value <= completed) {
            if (retired_[i].buffer) vmaDestroyBuffer(device_.allocator(), retired_[i].buffer, retired_[i].allocation);
            retired_[i] = retired_.back();
            retired_.pop_back();
        } else {
            ++i;
        }
    }
    groups_.clear();
    std::set<const scene::LoadedModel*> used;
    if (scene_) {
        std::map<std::pair<const scene::LoadedModel*, int>, size_t> index;
        for (size_t i = 0; i < scene_->placements.size(); ++i) {
            const ModelPlacement& p = scene_->placements[i];
            const scene::LoadedModel* key = p.model.get();
            auto it = models_.find(key);
            if (it == models_.end()) {
                std::unique_ptr<GpuModel> gm = makeModel(p.model);
                if (!gm) continue;
                it = models_.emplace(key, std::move(gm)).first;
            }
            used.insert(key);
            const auto gk = std::make_pair(key, p.animation);
            auto g = index.find(gk);
            if (g == index.end()) {
                Group group;
                group.gpu = it->second.get();
                group.anim = p.animation;
                group.animated = scene::isAnimated(p.model->data, p.animation);
                g = index.emplace(gk, groups_.size()).first;
                groups_.push_back(std::move(group));
            }
            groups_[g->second].placements.push_back(static_cast<uint32_t>(i));
        }
    }
    for (auto it = models_.begin(); it != models_.end();) {   // a model nothing shows any more goes once the frames that may read it are done
        if (used.count(it->first)) {
            ++it;
            continue;
        }
        if (it->second->buffer) retired_.push_back({it->second->buffer, it->second->allocation, submitted});
        it->second->buffer = VK_NULL_HANDLE;
        it->second->allocation = nullptr;
        it = models_.erase(it);
    }
    bool anyAnimated = false;
    for (Group& g : groups_) {
        if (!g.animated) continue;
        anyAnimated = true;
        const scene::ModelData& data = g.gpu->model->data;
        const std::vector<scene::PosedPrimitive> posed = scene::poseModel(data, g.anim, seconds);
        g.posed.assign(data.primitives.size(), {});
        for (size_t i = 0; i < data.primitives.size(); ++i) {
            const scene::ModelPrimitive& p = data.primitives[i];
            const gpu::UploadRing::Allocation a = ring.allocate(VkDeviceSize(p.vertexCount()) * sizeof(ModelVertex), 16);
            if (!a.ok()) continue;   // the slice is full: this primitive is not drawn this iteration
            writeVertices(static_cast<ModelVertex*>(a.data), p, posed[i]);
            ring.flush(a);
            g.posed[i] = a;
        }
    }
    animated_ = anyAnimated;
}

void ModelRenderer::buildDraws(const Mat4& viewProj, gpu::UploadRing& ring, ModelDrawList& out) const {
    out.draws.clear();
    out.instances = 0;
    if (!scene_ || groups_.empty()) return;
    Plane planes[6];
    frustumPlanes(viewProj, planes);
    std::vector<uint32_t> visible;
    for (const Group& g : groups_) {
        visible.clear();
        for (const uint32_t pi : g.placements) {
            const ModelPlacement& p = scene_->placements[pi];
            doc::Box b;
            b.mn = {p.bounds.min[0], p.bounds.min[1], p.bounds.min[2]};
            b.mx = {p.bounds.max[0], p.bounds.max[1], p.bounds.max[2]};
            if (g.animated || boxInFrustum(planes, b)) visible.push_back(pi);   // an animated model may leave its rest bounds
        }
        if (visible.empty()) continue;
        const gpu::UploadRing::Allocation inst = ring.allocate(VkDeviceSize(visible.size()) * kModelInstanceBytes, 16);
        if (!inst.ok()) return;
        float* m = static_cast<float*>(inst.data);
        for (size_t k = 0; k < visible.size(); ++k) std::memcpy(m + k * 16, scene_->placements[visible[k]].matrix.data(), kModelInstanceBytes);
        ring.flush(inst);
        for (size_t i = 0; i < g.gpu->prims.size(); ++i) {
            const Prim& pr = g.gpu->prims[i];
            if (pr.indexCount == 0) continue;
            ModelDraw d;
            if (g.animated) {
                if (i >= g.posed.size() || !g.posed[i].ok()) continue;
                d.vertexBuffer = g.posed[i].buffer;
                d.vertexOffset = g.posed[i].offset;
            } else {
                d.vertexBuffer = g.gpu->buffer;
                d.vertexOffset = pr.vertexOffset;
            }
            d.indexBuffer = g.gpu->buffer;
            d.indexOffset = pr.indexOffset;
            d.instanceBuffer = inst.buffer;
            d.instanceOffset = inst.offset;
            d.indexCount = pr.indexCount;
            d.instanceCount = static_cast<uint32_t>(visible.size());
            for (int c = 0; c < 4; ++c) d.color[c] = pr.color[c];
            d.texture = pr.texture;
            d.hasTexture = pr.hasTexture;
            d.alphaMode = pr.alphaMode;
            d.cutoff = pr.cutoff;
            out.draws.push_back(d);
        }
        out.instances += static_cast<uint32_t>(visible.size());
    }
}

} // namespace native::render
