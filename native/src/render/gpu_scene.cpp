// render/gpu_scene -- see gpu_scene.h.
#include "render/gpu_scene.h"

#include <core/textures.h>

#include <algorithm>
#include <cstring>

#include "base/log.h"

namespace native::render {

namespace {

constexpr VkDeviceSize kVertexArenaBytes = VkDeviceSize(1) << 30;       // 1 GiB of vertices (40 bytes each)
constexpr VkDeviceSize kIndexArenaBytes = VkDeviceSize(512) << 20;
constexpr VkDeviceSize kVertexSize = sizeof(scene::RenderVertex);
constexpr uint32_t kMaxUploadsPerFrame = 4096;

struct Plane {
    double a, b, c, d;
};

// Frustum planes (inside: a x + b y + c z + d >= 0) of a Vulkan clip-space matrix (column-major, depth 0..1).
void frustumPlanes(const Mat4& m, Plane out[6]) {
    const auto row = [&](int r) { return std::array<double, 4>{m[r], m[4 + r], m[8 + r], m[12 + r]}; };
    const auto r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    const auto make = [](const std::array<double, 4>& v) {
        const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        const double k = l > 0 ? 1.0 / l : 1.0;
        return Plane{v[0] * k, v[1] * k, v[2] * k, v[3] * k};
    };
    out[0] = make({r3[0] + r0[0], r3[1] + r0[1], r3[2] + r0[2], r3[3] + r0[3]});   // left
    out[1] = make({r3[0] - r0[0], r3[1] - r0[1], r3[2] - r0[2], r3[3] - r0[3]});   // right
    out[2] = make({r3[0] + r1[0], r3[1] + r1[1], r3[2] + r1[2], r3[3] + r1[3]});   // bottom / top (y is flipped, both are tested)
    out[3] = make({r3[0] - r1[0], r3[1] - r1[1], r3[2] - r1[2], r3[3] - r1[3]});
    out[4] = make(r2);                                                                // near (z >= 0)
    out[5] = make({r3[0] - r2[0], r3[1] - r2[1], r3[2] - r2[2], r3[3] - r2[3]});   // far
}

bool boxInFrustum(const Plane planes[6], const doc::Box& b) {
    for (int i = 0; i < 6; ++i) {
        const Plane& p = planes[i];
        const double x = p.a >= 0 ? b.mx.x : b.mn.x;
        const double y = p.b >= 0 ? b.mx.y : b.mn.y;
        const double z = p.c >= 0 ? b.mx.z : b.mn.z;
        if (p.a * x + p.b * y + p.c * z + p.d < 0) return false;
    }
    return true;
}

} // namespace

namespace {
// The arenas are also the inputs of the ray tracing scene's BLASes and, later, read by the shading kernels (R1): addressable and readable as storage.
VkBufferUsageFlags rtUsage(const gpu::Device& d) {
    return d.caps().lit() ? (VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
                            : 0;
}
} // namespace

GpuScene::GpuScene(gpu::Device& device, TextureTable& textures)
    : device_(device),
      textures_(textures),
      vertices_(device, kVertexArenaBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | rtUsage(device), "scene vertices"),
      indices_(device, kIndexArenaBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | rtUsage(device), "scene indices") {}

GpuScene::~GpuScene() = default;

void GpuScene::setSnapshot(std::shared_ptr<const scene::SceneSnapshot> snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_ = std::move(snapshot);
}

void GpuScene::setLit(std::shared_ptr<const light::LitSet> lit) {
    std::lock_guard<std::mutex> lock(mutex_);
    pendingLit_ = std::move(lit);
    litPending_ = true;
}

void GpuScene::setHidden(std::shared_ptr<const HiddenObjects> hidden) {
    std::lock_guard<std::mutex> lock(mutex_);
    pendingHidden_ = std::move(hidden);
    hiddenPending_ = true;
}

void GpuScene::setRules(std::shared_ptr<const ObjectRules> rules) {
    std::lock_guard<std::mutex> lock(mutex_);
    pendingRules_ = std::move(rules);
    rulesPending_ = true;
}

SceneStats GpuScene::stats() const {
    SceneStats s;
    s.objects = static_cast<uint32_t>(objects_.size());
    for (const auto& [key, o] : objects_) {
        s.indices += o.indexCount;
        s.vertices += o.vb.size / kVertexSize;
    }
    s.rawObjects = rawObjects_;
    s.queued = static_cast<uint32_t>(queue_.size());
    s.vertexBytesUsed = vertices_.usedBytes();
    s.indexBytesUsed = indices_.usedBytes();
    s.uploads = uploads_;
    s.litObjects = litObjects_;
    return s;
}

bool GpuScene::upload(VkCommandBuffer cmd, gpu::UploadRing& ring, const Work& w) {
    const scene::RenderMesh& mesh = *w.mesh;
    const auto it = objects_.find(w.key);
    // empty meshes (a fully carved brush) own no ranges
    Object fresh;
    fresh.mesh = w.mesh;
    fresh.meshPtr = w.mesh.get();
    fresh.bounds = w.bounds;
    fresh.volume = mesh.kind == scene::RenderMesh::Kind::Volume;
    fresh.raw = w.raw;

    if (!mesh.empty()) {
        const VkDeviceSize vbytes = mesh.vertices.size() * kVertexSize;
        const VkDeviceSize ibytes = mesh.indices.size() * sizeof(uint32_t);
        gpu::Arena::Block vb = vertices_.allocate(vbytes + kVertexSize, 8);
        gpu::Arena::Block ib = indices_.allocate(ibytes, 4);
        if (!vb.ok() || !ib.ok()) {
            if (vb.ok()) vertices_.free(vb);
            if (ib.ok()) indices_.free(ib);
            if (!loggedFull_) {
                base::logWarning("GpuScene: the geometry arena is full (" + std::to_string(vertices_.usedBytes() >> 20) + " MB of vertices): some objects stay as they were");
                loggedFull_ = true;
            }
            return true;   // nothing to retry: the arena does not get emptier by waiting
        }
        const gpu::UploadRing::Allocation sv = ring.allocate(vbytes, 16);
        const gpu::UploadRing::Allocation si = sv.ok() ? ring.allocate(ibytes, 16) : gpu::UploadRing::Allocation{};
        if (!sv.ok() || !si.ok()) {
            vertices_.free(vb);
            indices_.free(ib);
            return false;   // the slice is full: next frame
        }
        // the first vertex of the object must sit at a multiple of the vertex size (draws address vertices by index)
        const VkDeviceSize start = (vb.offset + kVertexSize - 1) / kVertexSize * kVertexSize;
        std::vector<uint32_t> global(mesh.textures.size());
        for (size_t i = 0; i < mesh.textures.size(); ++i) {
            const std::string& name = mesh.textures[i];
            global[i] = core::isCubemapTexture(name) ? textures_.skyIndex(name) : textures_.indexOf(name);
        }
        auto* dst = static_cast<scene::RenderVertex*>(sv.data);
        std::memcpy(dst, mesh.vertices.data(), vbytes);
        for (size_t i = 0; i < mesh.vertices.size(); ++i) {
            const uint32_t slot = dst[i].texture;
            dst[i].texture = slot < global.size() ? global[slot] : 0;
        }
        std::memcpy(si.data, mesh.indices.data(), ibytes);
        ring.flush(sv);
        ring.flush(si);
        VkBufferCopy cv{sv.offset, start, vbytes};
        vkCmdCopyBuffer(cmd, sv.buffer, vertices_.buffer(), 1, &cv);
        VkBufferCopy ci{si.offset, ib.offset, ibytes};
        vkCmdCopyBuffer(cmd, si.buffer, indices_.buffer(), 1, &ci);
        barrierNeeded_ = true;
        fresh.vb = vb;
        fresh.ib = ib;
        fresh.serial = ++serials_;
        {   // FNV-1a over the positions and the indices
            uint64_t h = 1469598103934665603ull;
            const auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
            for (const scene::RenderVertex& v : mesh.vertices) {
                uint32_t b[3];
                std::memcpy(b, v.pos, sizeof b);
                mix(b[0]);
                mix(b[1]);
                mix(b[2]);
            }
            for (uint32_t i : mesh.indices) mix(i);
            fresh.geomHash = h;
        }
        fresh.vertexCount = static_cast<uint32_t>(mesh.vertices.size());
        fresh.vertexOffset = static_cast<int32_t>(start / kVertexSize);
        fresh.firstIndex = static_cast<uint32_t>(ib.offset / sizeof(uint32_t));
        fresh.indexCount = static_cast<uint32_t>(mesh.indices.size());
        std::sort(global.begin(), global.end());
        global.erase(std::unique(global.begin(), global.end()), global.end());
        fresh.textures = std::move(global);
        ++uploads_;
    }
    if (it != objects_.end()) {
        // the frames in flight may still draw the old ranges
        if (it->second.vb.ok()) retired_.push_back({it->second.vb, it->second.ib, ~uint64_t(0)});
        it->second = std::move(fresh);
    } else {
        objects_.emplace(w.key, std::move(fresh));
    }
    drawsDirty_ = true;
    return true;
}

bool GpuScene::sync(VkCommandBuffer cmd, gpu::UploadRing& ring, uint64_t completed, uint64_t submitted) {
    // the snapshot: what differs from what is on the GPU becomes work
    std::shared_ptr<const scene::SceneSnapshot> next;
    bool relit = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        next = std::move(pending_);
        pending_.reset();
        if (litPending_) {
            lit_ = std::move(pendingLit_);
            pendingLit_.reset();
            litPending_ = false;
            relit = true;
        }
        if (hiddenPending_) {
            hidden_ = std::move(pendingHidden_);
            pendingHidden_.reset();
            hiddenPending_ = false;
            drawsDirty_ = true;
        }
        if (rulesPending_) {
            rules_ = std::move(pendingRules_);
            pendingRules_.reset();
            rulesPending_ = false;
            drawsDirty_ = true;
        }
    }
    if (next) current_ = next;
    if ((next || relit) && current_) {
        next = current_;
        queue_.clear();
        std::vector<Work> work;
        work.reserve(next->objects.size());
        litObjects_ = 0;
        for (const auto& [key, p] : next->objects) {
            std::shared_ptr<const scene::RenderMesh> mesh = p.render ? p.render : p.raw;
            if (!mesh) continue;
            if (p.render && lit_) {   // the copy with lightmap coordinates, while it was made from this very mesh
                const auto lit = lit_->objects.find(key);
                if (lit != lit_->objects.end() && lit->second.source == p.render) {
                    mesh = lit->second.lit;
                    ++litObjects_;
                }
            }
            const auto it = objects_.find(key);
            if (it != objects_.end() && !it->second.mesh.expired() && it->second.meshPtr == mesh.get()) {
                if (!(it->second.bounds == p.bounds)) {
                    it->second.bounds = p.bounds;
                    drawsDirty_ = true;
                }
                continue;
            }
            work.push_back({key, mesh, p.bounds, !p.render});
        }
        // the objects that are gone: their ranges are freed once the frames that may read them have finished
        for (auto it = objects_.begin(); it != objects_.end();) {
            if (next->objects.find(it->first) == next->objects.end() || (!next->objects.at(it->first).render && !next->objects.at(it->first).raw)) {
                if (it->second.vb.ok()) retired_.push_back({it->second.vb, it->second.ib, ~uint64_t(0)});
                it = objects_.erase(it);
                drawsDirty_ = true;
            } else {
                ++it;
            }
        }
        // the objects nearest to the origin last (they are popped first): a stable order that gives the first frames the middle of the level
        queue_ = std::move(work);
        std::sort(queue_.begin(), queue_.end(), [](const Work& a, const Work& b) {
            const auto d = [](const Work& w) {
                const double cx = (w.bounds.mn.x + w.bounds.mx.x) * 0.5, cy = (w.bounds.mn.y + w.bounds.mx.y) * 0.5, cz = (w.bounds.mn.z + w.bounds.mx.z) * 0.5;
                return cx * cx + cy * cy + cz * cz;
            };
            return d(a) > d(b);
        });
    }
    for (Retired& r : retired_)
        if (r.value == ~uint64_t(0)) r.value = submitted;   // the frames submitted so far may draw it; later ones will not
    for (size_t i = 0; i < retired_.size();) {
        if (retired_[i].value <= completed) {
            if (retired_[i].vb.ok()) vertices_.free(retired_[i].vb);
            if (retired_[i].ib.ok()) indices_.free(retired_[i].ib);
            retired_[i] = std::move(retired_.back());
            retired_.pop_back();
        } else {
            ++i;
        }
    }

    // uploads, as many as the slice has room for (the draw lists and the textures share it)
    const VkDeviceSize budget = ring.bytesPerFrame() / 4 * 3;
    uint32_t done = 0;
    while (!queue_.empty() && done < kMaxUploadsPerFrame && ring.usedBytes() < budget) {
        const Work& w = queue_.back();
        if (!upload(cmd, ring, w)) break;
        queue_.pop_back();
        ++done;
    }
    if (barrierNeeded_) {
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
        b.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
        barrierNeeded_ = false;
    }
    if (textures_.loaded() != seenTextures_) drawsDirty_ = true;
    if (drawsDirty_) rebuildDraws();
    return !queue_.empty();
}

void GpuScene::rebuildDraws() {
    ++changes_;
    draws_.clear();
    rt_.clear();
    uint64_t signature = 0;
    seenTextures_ = textures_.loaded();
    draws_.reserve(objects_.size());
    rawObjects_ = 0;
    for (const auto& [key, o] : objects_) {
        if (o.raw) ++rawObjects_;
        if (o.indexCount == 0 || !o.bounds.valid()) continue;
        if (hidden_ && hidden_->count(key)) continue;
        uint32_t flags = kObjReceives | kObjCasts;
        if (rules_) {
            const auto r = rules_->find(key);
            if (r != rules_->end()) flags = r->second;
        }
        Draw d{};
        d.cmd.indexCount = o.indexCount;
        d.cmd.instanceCount = 1;
        d.cmd.firstIndex = o.firstIndex;
        d.cmd.vertexOffset = o.vertexOffset;
        d.cmd.firstInstance = flags;   // the vertex shader reads it as gl_InstanceIndex (the G-buffer marks the pixels of surfaces that do not receive light)
        d.bounds = o.bounds;
        d.volume = o.volume;
        d.textures = &o.textures;
        draws_.push_back(d);
        if (!o.volume) {
            uint32_t rtFlags = flags;
            for (uint32_t t : o.textures)
                if (textures_.alphaSurface(t)) {
                    rtFlags |= kObjAlpha;
                    break;
                }
            rt_.push_back({o.serial, rtFlags, static_cast<uint32_t>(o.vertexOffset), o.firstIndex, o.indexCount, o.vertexCount, o.raw, o.geomHash});
            uint64_t h = o.geomHash ^ (uint64_t(rtFlags) * 0x9E3779B97F4A7C15ull);
            h ^= h >> 29;
            h *= 0xBF58476D1CE4E5B9ull;
            h ^= h >> 32;
            signature += h;   // a sum: the order of the map does not matter
        }
    }
    if (signature != lightingSignature_ || rt_.size() != lightingCount_) {
        lightingSignature_ = signature;
        lightingCount_ = rt_.size();
        ++lightingChanges_;
    }
    drawsDirty_ = false;
}

void GpuScene::buildDraws(const Mat4& viewProj, gpu::UploadRing& ring, DrawLists& out) {
    out = DrawLists{};
    if (draws_.empty()) return;
    Plane planes[6];
    frustumPlanes(viewProj, planes);
    scratchSolid_.clear();
    scratchTranslucent_.clear();
    scratchVolume_.clear();
    for (const Draw& d : draws_) {
        if (!boxInFrustum(planes, d.bounds)) continue;
        if (d.volume) {
            scratchVolume_.push_back(d.cmd);
            continue;
        }
        scratchSolid_.push_back(d.cmd);
        for (uint32_t t : *d.textures)
            if (textures_.translucent(t)) {
                scratchTranslucent_.push_back(d.cmd);
                break;
            }
    }
    out.visible = static_cast<uint32_t>(scratchSolid_.size() + scratchVolume_.size());
    const auto put = [&](const std::vector<VkDrawIndexedIndirectCommand>& list, DrawList& dst) {
        if (list.empty()) return;
        const VkDeviceSize bytes = list.size() * sizeof(VkDrawIndexedIndirectCommand);
        const gpu::UploadRing::Allocation a = ring.allocate(bytes, 16);
        if (!a.ok()) {
            if (!loggedRing_) {
                base::logWarning("GpuScene: the upload ring is full, a frame draws without some objects");
                loggedRing_ = true;
            }
            return;
        }
        std::memcpy(a.data, list.data(), bytes);
        ring.flush(a);
        dst.buffer = a.buffer;
        dst.offset = a.offset;
        dst.count = static_cast<uint32_t>(list.size());
    };
    put(scratchSolid_, out.solid);
    put(scratchTranslucent_, out.translucent);
    put(scratchVolume_, out.volume);
}

} // namespace native::render
