// render/render_thread -- see render_thread.h.
#include "render/render_thread.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "base/log.h"
#include "base/trace.h"
#include "gpu/swapchain.h"
#include "render/lightmap_view.h"
#include "render/mesh_pass.h"
#include "render/scene_pass.h"
#include "render/texture_table.h"

namespace native::render {

namespace {

constexpr int kFramesInFlight = 2;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage,
                  VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
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
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

struct RenderThread::Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore acquire = VK_NULL_HANDLE;
    uint64_t timelineValue = 0;           // renderTimeline value that marks this frame's submit as finished
};

struct RenderThread::Viewport {
    ViewportId id = 0;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    // Cross-thread state: guarded by RenderThread::mutex_.
    uint32_t width = 0, height = 0;
    bool dirty = true;
    bool removing = false, removed = false;
    float clear[3] = {0.105f, 0.105f, 0.115f};
    ViewState view;
    bool band = false;
    float bandRect[4] = {0, 0, 0, 0};
    std::shared_ptr<const std::vector<TextItem>> sizeLabels;
    std::shared_ptr<const std::vector<LineVertex>> gizmo;
    std::shared_ptr<std::promise<CapturedImage>> capture;     // pending capture request
    uint64_t frames = 0, recreations = 0;
    // Render thread only.
    std::unique_ptr<gpu::Swapchain> swapchain;
    Frame frame[kFramesInFlight];
    uint32_t frameCounter = 0;
    uint32_t swapW = 0, swapH = 0;        // size the swapchain was created for (0 = must be recreated)
    VkImage depth = VK_NULL_HANDLE;
    VmaAllocation depthAlloc = nullptr;
    VkImageView depthView = VK_NULL_HANDLE;
    bool gpuReady = false;
};

RenderThread::RenderThread(gpu::Device& device) : device_(device) {
    VkSemaphoreTypeCreateInfo tci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    tci.initialValue = 0;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &tci;
    vkCreateSemaphore(device_.device(), &sci, nullptr, &renderTimeline_);
    // the scene's GPU side is made here, before the render thread runs (it submits one-time commands)
    ring_ = std::make_unique<gpu::UploadRing>(device_);
    textures_ = std::make_unique<TextureTable>(device_);
    scene_ = std::make_unique<GpuScene>(device_, *textures_);
    // the display pages of the lit modes: one empty layer until the bake publishes its layout (the shader needs something to sample)
    lightmapView_ = std::make_unique<LightmapView>(device_);
    display_ = std::make_unique<light::PageImages>(device_, 1024, VK_FORMAT_R16G16B16A16_SFLOAT, true);
    display_->ensureLayers(1);
    lightmapView_->update(display_->view());
    scatter_ = std::make_unique<light::Scatter>(device_);
    textures_->setGpuIdleWait([this] {
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        const uint64_t value = nextTimelineValue_.load();
        wi.semaphoreCount = 1;
        wi.pSemaphores = &renderTimeline_;
        wi.pValues = &value;
        vkWaitSemaphores(device_.device(), &wi, 5'000'000'000ull);
    });
    for (int i = 0; i < 2; ++i) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = device_.graphicsFamily();
        vkCreateCommandPool(device_.device(), &pci, nullptr, &uploadPool_[i]);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = uploadPool_[i];
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(device_.device(), &ai, &uploadCmd_[i]);
    }
    thread_ = std::thread([this] { loop(); });
}

RenderThread::~RenderThread() {
    setLightLink(nullptr);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    vkQueueWaitIdle(device_.graphicsQueue());
    mesh_.reset();
    pass_.reset();
    scatter_.reset();
    display_.reset();
    lightmapView_.reset();
    scene_.reset();
    textures_.reset();
    ring_.reset();
    for (VkCommandPool p : uploadPool_)
        if (p) vkDestroyCommandPool(device_.device(), p, nullptr);
    vkDestroySemaphore(device_.device(), renderTimeline_, nullptr);
}

void RenderThread::setLightLink(std::shared_ptr<light::LightLink> link) {
    std::shared_ptr<light::LightLink> old;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        old = std::move(link_);
        link_ = std::move(link);
        if (link_) {
            bool lit = false;
            for (auto& kv : viewports_) lit = lit || kv.second->view.renderMode > 0;
            link_->setWanted(lit);
        }
        workPending_ = true;
    }
    if (old) old->setNotify({});
    std::shared_ptr<light::LightLink> now;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        now = link_;
    }
    if (now)
        now->setNotify([this] {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                workPending_ = true;
                for (auto& kv : viewports_)
                    if (kv.second->view.renderMode > 0) kv.second->dirty = true;
            }
            wake_.notify_all();
        });
    wake_.notify_all();
}

void RenderThread::setHiddenObjects(std::shared_ptr<const std::unordered_set<scene::ObjectKey, scene::ObjectKey::Hash>> hidden) {
    scene_->setHidden(std::move(hidden));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        workPending_ = true;
        for (auto& kv : viewports_) kv.second->dirty = true;
    }
    wake_.notify_all();
}

void RenderThread::setSnapshot(std::shared_ptr<const scene::SceneSnapshot> snapshot) {
    scene_->setSnapshot(std::move(snapshot));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        workPending_ = true;
    }
    wake_.notify_all();
}

void RenderThread::setTextureSource(const std::filesystem::path& assetsRoot, base::JobPool* pool) {
    textures_->setSource(assetsRoot, pool, [this] {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            workPending_ = true;
        }
        wake_.notify_all();
    });
}

void RenderThread::setSkyTexture(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (skyName_ == name) return;
        skyName_ = name;
        workPending_ = true;
    }
    wake_.notify_all();
}

RenderThread::SceneInfo RenderThread::sceneInfo() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return info_;
}

ViewportId RenderThread::addViewport(VkSurfaceKHR surface, uint32_t width, uint32_t height) {
    auto v = std::make_shared<Viewport>();
    v->surface = surface;
    v->width = width;
    v->height = height;
    ViewportId id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = nextId_++;
        v->id = id;
        viewports_[id] = std::move(v);
    }
    wake_.notify_all();
    return id;
}

void RenderThread::removeViewport(ViewportId id) {
    std::shared_ptr<Viewport> v;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        auto it = viewports_.find(id);
        if (it == viewports_.end()) return;
        v = it->second;
        v->removing = true;
        wake_.notify_all();
        removed_.wait(lock, [&] { return v->removed; });
        viewports_.erase(id);
    }
}

void RenderThread::resizeViewport(ViewportId id, uint32_t width, uint32_t height) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = viewports_.find(id);
        if (it == viewports_.end()) return;
        it->second->width = width;
        it->second->height = height;
        it->second->dirty = true;
    }
    wake_.notify_all();
}

void RenderThread::markDirty(ViewportId id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = viewports_.find(id);
        if (it == viewports_.end()) return;
        it->second->dirty = true;
    }
    wake_.notify_all();
}

void RenderThread::markAllDirty() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& kv : viewports_) kv.second->dirty = true;
    }
    wake_.notify_all();
}

void RenderThread::setView(ViewportId id, const ViewState& view) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = viewports_.find(id);
        if (it == viewports_.end()) return;
        it->second->view = view;
        it->second->dirty = true;
        if (link_) {
            bool lit = false;
            for (auto& kv : viewports_) lit = lit || kv.second->view.renderMode > 0;
            link_->setWanted(lit);
        }
    }
    wake_.notify_all();
}

void RenderThread::setSelectionLines(std::shared_ptr<const std::vector<LineVertex>> lines) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        selectionLines_ = std::move(lines);
        for (auto& kv : viewports_) kv.second->dirty = true;
    }
    wake_.notify_all();
}

void RenderThread::setEntityOverlay(std::shared_ptr<const EntityOverlay> overlay) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entityOverlay_ = std::move(overlay);
        for (auto& kv : viewports_) kv.second->dirty = true;
    }
    wake_.notify_all();
}

void RenderThread::setGlyphAtlas(std::shared_ptr<const GlyphAtlas> atlas) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        glyphAtlas_ = std::move(atlas);
        workPending_ = true;
        for (auto& kv : viewports_) kv.second->dirty = true;
    }
    wake_.notify_all();
}

void RenderThread::setSizeLabels(ViewportId id, std::shared_ptr<const std::vector<TextItem>> labels) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = viewports_.find(id);
        if (it == viewports_.end()) return;
        it->second->sizeLabels = std::move(labels);
        it->second->dirty = true;
    }
    wake_.notify_all();
}

void RenderThread::setGizmoLines(ViewportId id, std::shared_ptr<const std::vector<LineVertex>> lines) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = viewports_.find(id);
        if (it == viewports_.end()) return;
        it->second->gizmo = std::move(lines);
        it->second->dirty = true;
    }
    wake_.notify_all();
}

void RenderThread::setRubberBand(ViewportId id, bool active, float x0, float y0, float x1, float y1) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = viewports_.find(id);
        if (it == viewports_.end()) return;
        Viewport& v = *it->second;
        v.band = active;
        v.bandRect[0] = x0; v.bandRect[1] = y0; v.bandRect[2] = x1; v.bandRect[3] = y1;
        v.dirty = true;
    }
    wake_.notify_all();
}

void RenderThread::setClearColor(ViewportId id, float r, float g, float b) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = viewports_.find(id);
        if (it == viewports_.end()) return;
        it->second->clear[0] = r;
        it->second->clear[1] = g;
        it->second->clear[2] = b;
        it->second->dirty = true;
    }
    wake_.notify_all();
}

CapturedImage RenderThread::capture(ViewportId id, int timeoutMs) {
    auto promise = std::make_shared<std::promise<CapturedImage>>();
    std::future<CapturedImage> future = promise->get_future();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = viewports_.find(id);
        if (it == viewports_.end()) return {};
        it->second->capture = promise;
        it->second->dirty = true;
    }
    wake_.notify_all();
    if (future.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = viewports_.find(id);
        if (it != viewports_.end() && it->second->capture == promise) it->second->capture.reset();
        return {};
    }
    return future.get();
}

uint64_t RenderThread::framesRendered(ViewportId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = viewports_.find(id);
    return it == viewports_.end() ? 0 : it->second->frames;
}

uint64_t RenderThread::swapchainRecreations(ViewportId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = viewports_.find(id);
    return it == viewports_.end() ? 0 : it->second->recreations;
}

uint64_t RenderThread::completedFrames() const {
    uint64_t value = 0;
    vkGetSemaphoreCounterValue(device_.device(), renderTimeline_, &value);
    return value;
}

void RenderThread::releaseViewport(Viewport& v) {
    if (v.gpuReady) {
        vkQueueWaitIdle(device_.graphicsQueue());
        for (Frame& f : v.frame) {
            if (f.acquire) vkDestroySemaphore(device_.device(), f.acquire, nullptr);
            if (f.pool) vkDestroyCommandPool(device_.device(), f.pool, nullptr);
            f = Frame{};
        }
    }
    if (v.depthView) vkDestroyImageView(device_.device(), v.depthView, nullptr);
    if (v.depth) vmaDestroyImage(device_.allocator(), v.depth, v.depthAlloc);
    v.depthView = VK_NULL_HANDLE;
    v.depth = VK_NULL_HANDLE;
    v.depthAlloc = nullptr;
    v.swapchain.reset();
    v.gpuReady = false;
}

// Waits until every frame and upload submitted so far finished (the display pages may change, the descriptor set may be rewritten).
void RenderThread::waitForFrames() {
    const uint64_t value = nextTimelineValue_.load();
    if (value == 0) return;
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &renderTimeline_;
    wi.pValues = &value;
    vkWaitSemaphores(device_.device(), &wi, 5'000'000'000ull);
}

// The lit modes' part of an upload iteration: a new epoch of the bake (new layout: the display pages are sized and emptied, the scene takes the lit meshes), then the
// newest finished mailbox scattered into the display pages. Recorded into the iteration's upload command buffer; the returned values go into its submit.
RenderThread::LightSubmit RenderThread::syncLighting(light::LightLink& link, VkCommandBuffer cmd) {
    LightSubmit out;
    const auto barrier = [&] {
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        mb.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &di);
    };
    const std::shared_ptr<const light::LitSet> set = link.current();
    if (set != litSeen_) {
        litSeen_ = set;
        if (set) {
            const int atlas = std::max(1, set->atlasSize), pages = std::max(1, set->pageCount);
            bool changed = false;
            if (display_->atlasSize() != atlas) {
                waitForFrames();
                display_ = std::make_unique<light::PageImages>(device_, atlas, VK_FORMAT_R16G16B16A16_SFLOAT, true);
                changed = display_->ensureLayers(pages);
            } else if (display_->layers() < pages) {
                waitForFrames();
                changed = display_->ensureLayers(pages);
            }
            if (changed) lightmapView_->update(display_->view());
            // the old layout's texels mean nothing in the new one
            barrier();
            const VkClearColorValue zero{};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, static_cast<uint32_t>(display_->layers())};
            vkCmdClearColorImage(cmd, display_->image(), VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
            barrier();
        }
        scene_->setLit(set);
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& kv : viewports_) kv.second->dirty = true;
    }
    light::Mailbox& mailbox = link.mailbox();
    const light::Mailbox::Taken t = mailbox.takeNewest();
    if (t.slot < 0) return out;
    const uint64_t renderValue = ++mailboxRenderValue_;
    if (set && t.bakeValue >= set->firstBakeValue && t.entries > 0) {   // a mailbox of an older layout is dropped
        if (scatterUses_ >= 8) {
            VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            const uint64_t last = renderValue - 1;
            wi.semaphoreCount = 1;
            VkSemaphore s = mailbox.renderTimeline();
            wi.pSemaphores = &s;
            wi.pValues = &last;
            vkWaitSemaphores(device_.device(), &wi, 5'000'000'000ull);
            scatter_->resetDescriptors();
            scatterUses_ = 0;
        }
        barrier();
        if (scatter_->record(cmd, mailbox.buffer(t.slot), t.entries, display_->view())) ++scatterUses_;
    }
    mailbox.consumed(t.slot, renderValue);
    out.wait = out.signal = true;
    out.waitValue = t.bakeValue;
    out.signalValue = renderValue;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& kv : viewports_)
            if (kv.second->view.renderMode > 0) kv.second->dirty = true;
    }
    return out;
}

void RenderThread::loop() {
    base::traceSetThreadName("render");
    for (;;) {
        struct Work {
            std::shared_ptr<Viewport> v;
            uint32_t w, h;
            float clear[3];
            ViewState view;
            std::shared_ptr<std::promise<CapturedImage>> capture;
            std::shared_ptr<const std::vector<LineVertex>> selection;
            bool band = false;
            float bandRect[4] = {0, 0, 0, 0};
            std::shared_ptr<const std::vector<TextItem>> sizeLabels;
            std::shared_ptr<const std::vector<LineVertex>> gizmo;
        };
        std::vector<Work> work;
        bool doUpload = false;
        std::string skyName;
        std::shared_ptr<const EntityOverlay> overlay;
        std::shared_ptr<const GlyphAtlas> atlas;
        std::shared_ptr<light::LightLink> link;
        std::vector<std::shared_ptr<Viewport>> toRelease;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] {
                if (stop_ || workPending_) return true;
                for (auto& kv : viewports_)
                    if (kv.second->removing || kv.second->dirty || kv.second->capture) return true;
                return false;
            });
            doUpload = workPending_;
            workPending_ = false;
            skyName = skyName_;
            overlay = entityOverlay_;
            atlas = glyphAtlas_;
            link = link_;
            if (stop_) {
                for (auto& kv : viewports_) toRelease.push_back(kv.second);
            } else {
                for (auto& kv : viewports_) {
                    Viewport& v = *kv.second;
                    if (v.removing) { toRelease.push_back(kv.second); continue; }
                    if (!v.dirty && !v.capture && !doUpload) continue;
                    work.push_back({kv.second, v.width, v.height, {v.clear[0], v.clear[1], v.clear[2]}, v.view, std::move(v.capture), selectionLines_, v.band,
                                    {v.bandRect[0], v.bandRect[1], v.bandRect[2], v.bandRect[3]}, v.sizeLabels, v.gizmo});
                    v.capture.reset();
                    v.dirty = false;
                }
            }
        }
        for (auto& v : toRelease) {
            releaseViewport(*v);
            std::lock_guard<std::mutex> lock(mutex_);
            v->removed = true;
        }
        if (!toRelease.empty()) removed_.notify_all();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) break;
        }

        // the iteration's upload ring slice and upload commands (scene geometry, textures), then every viewport's frame
        uint64_t lastValue = 0;
        LightSubmit lightSubmit;
        const uint32_t slot = static_cast<uint32_t>(iteration_++ % 2);
        const bool active = !work.empty() || doUpload;
        if (active) {
            if (ringValue_[slot] > 0) {
                VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
                wi.semaphoreCount = 1;
                wi.pSemaphores = &renderTimeline_;
                wi.pValues = &ringValue_[slot];
                vkWaitSemaphores(device_.device(), &wi, 5'000'000'000ull);
            }
            while (!ring_->beginFrame(completedFrames())) {
                VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
                const uint64_t value = nextTimelineValue_.load();
                wi.semaphoreCount = 1;
                wi.pSemaphores = &renderTimeline_;
                wi.pValues = &value;
                vkWaitSemaphores(device_.device(), &wi, 5'000'000'000ull);
            }
            vkResetCommandPool(device_.device(), uploadPool_[slot], 0);
            VkCommandBufferBeginInfo ubi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            ubi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(uploadCmd_[slot], &ubi);
            if (link) lightSubmit = syncLighting(*link, uploadCmd_[slot]);
            if (atlas.get() != atlasSeen_) {
                atlasSeen_ = atlas.get();
                atlasIndex_ = atlas && atlas->ok() ? textures_->addImage("glyphs" + std::to_string(++atlasSerial_), atlas->width, atlas->height, atlas->rgba) : 0;
            }
            skyIndex_ = skyName.empty() ? 0 : textures_->skyIndex(skyName);
            bool more = scene_->sync(uploadCmd_[slot], *ring_, completedFrames(), nextTimelineValue_.load());
            more = textures_->upload(uploadCmd_[slot], *ring_) || more;
            vkEndCommandBuffer(uploadCmd_[slot]);
            const uint64_t value = ++nextTimelineValue_;
            VkSemaphoreSubmitInfo sigs[2]{{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}};
            sigs[0].semaphore = renderTimeline_;
            sigs[0].value = value;
            sigs[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            VkSemaphoreSubmitInfo lightWait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
            VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
            cbi.commandBuffer = uploadCmd_[slot];
            VkSubmitInfo2 usi{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
            usi.commandBufferInfoCount = 1;
            usi.pCommandBufferInfos = &cbi;
            usi.signalSemaphoreInfoCount = 1;
            usi.pSignalSemaphoreInfos = sigs;
            if (lightSubmit.wait && link) {   // the mailbox's compose must have finished; the frame says when the mailbox may be reused
                lightWait.semaphore = link->mailbox().bakeTimeline();
                lightWait.value = lightSubmit.waitValue;
                lightWait.stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                usi.waitSemaphoreInfoCount = 1;
                usi.pWaitSemaphoreInfos = &lightWait;
            }
            if (lightSubmit.signal && link) {
                sigs[1].semaphore = link->mailbox().renderTimeline();
                sigs[1].value = lightSubmit.signalValue;
                sigs[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                usi.signalSemaphoreInfoCount = 2;
            }
            device_.noteResult(vkQueueSubmit2(device_.graphicsQueue(), 1, &usi, VK_NULL_HANDLE), "upload submit");
            lastValue = value;
            if (more) {
                std::lock_guard<std::mutex> lock(mutex_);
                workPending_ = true;   // the slice was full: the rest next iteration
            }
        }

        for (Work& w : work) {
            base::TraceScope trace("viewport frame", "render");
            Viewport& v = *w.v;
            if (!ensureSwapchain(v)) {
                // Minimised or no surface area: nothing to draw. A later resize marks the viewport dirty again.
                if (w.capture) w.capture->set_value(CapturedImage{});
                continue;
            }
            // Use the size the swapchain really has: it may differ from the requested one during a drag-resize.
            Frame& f = v.frame[v.frameCounter++ % kFramesInFlight];
            if (f.timelineValue > 0) {
                VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
                wi.semaphoreCount = 1;
                wi.pSemaphores = &renderTimeline_;
                wi.pValues = &f.timelineValue;
                vkWaitSemaphores(device_.device(), &wi, 5'000'000'000ull);
            }
            gpu::Swapchain& sc = *v.swapchain;
            // NVIDIA Reflex / PCL (T6.7): one frame token per viewport frame; the sleep is the pacing point before the frame's CPU work.
            gpu::Streamline* sl = device_.streamline();
            if (sl) {
                sl->beginFrame();
                sl->sleep();
                sl->marker(gpu::PclMarker::SimulationStart);
            }
            uint32_t imageIndex = 0;
            const VkResult acq = sc.acquire(f.acquire, &imageIndex);
            if (acq == VK_ERROR_OUT_OF_DATE_KHR || acq == VK_ERROR_SURFACE_LOST_KHR) {
                v.swapW = 0;                                  // recreate before the next try
                std::lock_guard<std::mutex> lock(mutex_);
                v.dirty = true;
                if (w.capture) v.capture = w.capture;         // deliver the capture with the retried frame
                continue;
            }
            if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
                base::logError("vkAcquireNextImageKHR failed (" + std::to_string(acq) + ")");
                if (w.capture) w.capture->set_value(CapturedImage{});
                continue;
            }

            if (sl) {
                sl->marker(gpu::PclMarker::SimulationEnd);
                sl->marker(gpu::PclMarker::RenderSubmitStart);
            }
            const VkExtent2D extent = sc.extent();
            const bool doCapture = w.capture != nullptr && sc.canCapture();
            vkResetCommandPool(device_.device(), f.pool, 0);
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(f.cmd, &bi);
            const VkImage image = sc.image(imageIndex);
            imageBarrier(f.cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            att.imageView = sc.view(imageIndex);
            att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            att.clearValue.color = {{w.clear[0], w.clear[1], w.clear[2], 1.0f}};
            imageBarrier(f.cmd, v.depth, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, 0,
                         VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
            VkRenderingAttachmentInfo datt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            datt.imageView = v.depthView;
            datt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            datt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            datt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            datt.clearValue.depthStencil = {1.0f, 0};
            DrawLists lists;
            scene_->buildDraws(w.view.viewProj, *ring_, lists);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                info_.visibleObjects = lists.visible;
            }
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, extent};
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &att;
            ri.pDepthAttachment = &datt;
            vkCmdBeginRendering(f.cmd, &ri);
            {
                const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
                const VkRect2D scissor{{0, 0}, extent};
                vkCmdSetViewport(f.cmd, 0, 1, &viewport);
                vkCmdSetScissor(f.cmd, 0, 1, &scissor);
            }
            LineList selLines, bandLines;
            const auto uploadLines = [&](const LineVertex* v, size_t n) {
                LineList l;
                if (n == 0) return l;
                const gpu::UploadRing::Allocation a = ring_->allocate(n * sizeof(LineVertex), 16);
                if (!a.ok()) return l;
                std::memcpy(a.data, v, n * sizeof(LineVertex));
                ring_->flush(a);
                l.buffer = a.buffer;
                l.offset = a.offset;
                l.count = static_cast<uint32_t>(n);
                return l;
            };
            if (w.selection && !w.selection->empty()) selLines = uploadLines(w.selection->data(), w.selection->size() & ~size_t(1));
            if (w.band) {
                // pixels -> Vulkan NDC; the frame is the rectangle's outline, a white line over a translucent dark one so it reads on any background
                const auto nx = [&](float x) { return x / static_cast<float>(extent.width) * 2.0f - 1.0f; };
                const auto ny = [&](float y) { return y / static_cast<float>(extent.height) * 2.0f - 1.0f; };
                const float x0 = nx(w.bandRect[0]), y0 = ny(w.bandRect[1]), x1 = nx(w.bandRect[2]), y1 = ny(w.bandRect[3]);
                const uint32_t c = packColor(1.0f, 1.0f, 1.0f, 0.9f);
                const LineVertex q[8] = {{{x0, y0, 0}, c}, {{x1, y0, 0}, c}, {{x1, y0, 0}, c}, {{x1, y1, 0}, c},
                                         {{x1, y1, 0}, c}, {{x0, y1, 0}, c}, {{x0, y1, 0}, c}, {{x0, y0, 0}, c}};
                bandLines = uploadLines(q, 8);
            }
            FrameOverlays ov;
            ov.selection = selLines;
            ov.screen = bandLines;
            if (w.gizmo && !w.gizmo->empty()) ov.gizmo = uploadLines(w.gizmo->data(), w.gizmo->size() & ~size_t(1));
            if (overlay) {
                ov.entityLines = uploadLines(overlay->lines.data(), overlay->lines.size() & ~size_t(1));
                ov.fills = uploadLines(overlay->fills.data(), overlay->fills.size() / 3 * 3);
            }
            if (atlas && atlas->ok() && atlasIndex_ != 0) {
                const auto uploadText = [&](const std::vector<TextItem>& items) {
                    std::vector<LabelVertex> verts;
                    for (const TextItem& t : items) layoutText(*atlas, t.text, t.at.data(), t.rgba, verts);
                    LineList l;
                    if (verts.empty()) return l;
                    const gpu::UploadRing::Allocation a = ring_->allocate(verts.size() * sizeof(LabelVertex), 16);
                    if (!a.ok()) return l;
                    std::memcpy(a.data, verts.data(), verts.size() * sizeof(LabelVertex));
                    ring_->flush(a);
                    l.buffer = a.buffer;
                    l.offset = a.offset;
                    l.count = static_cast<uint32_t>(verts.size());
                    return l;
                };
                if (overlay) ov.labels = uploadText(overlay->labels);
                if (w.sizeLabels) ov.sizeLabels = uploadText(*w.sizeLabels);
                ov.atlasTexture = atlasIndex_;
            }
            ov.extent[0] = static_cast<float>(extent.width);
            ov.extent[1] = static_cast<float>(extent.height);
            if (mesh_) {
                LitParams lit;
                lit.mode = w.view.renderMode;
                if (litSeen_) {
                    lit.exposure = litSeen_->exposure;
                    lit.brightness = litSeen_->brightness;
                }
                mesh_->record(f.cmd, w.view, *scene_, lists, textures_->set(), skyIndex_, true, ov, lightmapView_->set(), lit);
            }
            if (pass_) pass_->record(f.cmd, extent, w.view, !scene_->hasObjects());
            vkCmdEndRendering(f.cmd);

            VmaAllocation stagingAlloc = nullptr;
            VkBuffer staging = VK_NULL_HANDLE;
            void* mapped = nullptr;
            if (doCapture) {
                const VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;
                VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                bci.size = bytes;
                bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                VmaAllocationCreateInfo aci{};
                aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
                aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VmaAllocationInfo info{};
                vmaCreateBuffer(device_.allocator(), &bci, &aci, &staging, &stagingAlloc, &info);
                mapped = info.pMappedData;
                imageBarrier(f.cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {extent.width, extent.height, 1};
                vkCmdCopyImageToBuffer(f.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &region);
                VkBufferMemoryBarrier2 hb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                hb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                hb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                hb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
                hb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
                hb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                hb.buffer = staging;
                hb.size = VK_WHOLE_SIZE;
                VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dep.bufferMemoryBarrierCount = 1;
                dep.pBufferMemoryBarriers = &hb;
                vkCmdPipelineBarrier2(f.cmd, &dep);
                imageBarrier(f.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                             VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
            } else {
                imageBarrier(f.cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
            }
            vkEndCommandBuffer(f.cmd);

            const uint64_t signalValue = ++nextTimelineValue_;
            VkSemaphoreSubmitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
            waitInfo.semaphore = f.acquire;
            waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            VkSemaphoreSubmitInfo signalInfos[2]{{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}};
            signalInfos[0].semaphore = sc.renderDone(imageIndex);
            signalInfos[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            signalInfos[1].semaphore = renderTimeline_;
            signalInfos[1].value = signalValue;
            signalInfos[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            VkCommandBufferSubmitInfo cbInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
            cbInfo.commandBuffer = f.cmd;
            VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
            si.waitSemaphoreInfoCount = 1;
            si.pWaitSemaphoreInfos = &waitInfo;
            si.commandBufferInfoCount = 1;
            si.pCommandBufferInfos = &cbInfo;
            si.signalSemaphoreInfoCount = 2;
            si.pSignalSemaphoreInfos = signalInfos;
            device_.noteResult(vkQueueSubmit2(device_.graphicsQueue(), 1, &si, VK_NULL_HANDLE), "viewport frame submit");
            f.timelineValue = signalValue;
            lastValue = signalValue;
            if (sl) {
                sl->marker(gpu::PclMarker::RenderSubmitEnd);
                sl->marker(gpu::PclMarker::PresentStart);
            }
            const VkResult pres = sc.present(device_.graphicsQueue(), imageIndex);
            if (sl) sl->marker(gpu::PclMarker::PresentEnd);
            device_.noteResult(pres, "present");
            if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR || pres == VK_ERROR_SURFACE_LOST_KHR) {
                v.swapW = 0;
                std::lock_guard<std::mutex> lock(mutex_);
                v.dirty = true;
            } else if (pres != VK_SUCCESS) {
                base::logError("vkQueuePresentKHR failed (" + std::to_string(pres) + ")");
            }

            if (w.capture) {
                CapturedImage out;
                if (doCapture) {
                    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
                    wi.semaphoreCount = 1;
                    wi.pSemaphores = &renderTimeline_;
                    wi.pValues = &signalValue;
                    vkWaitSemaphores(device_.device(), &wi, 5'000'000'000ull);
                    vmaInvalidateAllocation(device_.allocator(), stagingAlloc, 0, VK_WHOLE_SIZE);
                    out.width = extent.width;
                    out.height = extent.height;
                    out.rgba.resize(static_cast<size_t>(extent.width) * extent.height * 4);
                    std::memcpy(out.rgba.data(), mapped, out.rgba.size());
                    if (sc.format() == VK_FORMAT_B8G8R8A8_UNORM || sc.format() == VK_FORMAT_B8G8R8A8_SRGB)
                        for (size_t i = 0; i < out.rgba.size(); i += 4) std::swap(out.rgba[i], out.rgba[i + 2]);
                }
                w.capture->set_value(std::move(out));
            }
            if (staging) vmaDestroyBuffer(device_.allocator(), staging, stagingAlloc);

            std::lock_guard<std::mutex> lock(mutex_);
            ++v.frames;
            totalFrames_.fetch_add(1);
        }
        if (active) {
            ring_->endFrame(lastValue);
            ringValue_[slot] = lastValue;
            std::lock_guard<std::mutex> lock(mutex_);
            info_.scene = scene_->stats();
            info_.texturesAssigned = textures_->assigned();
            info_.texturesLoaded = textures_->loaded();
            info_.texturesFailed = textures_->failed();
            info_.texturesPending = textures_->pending();
        }
    }
    // Leaving the loop: make sure nobody waits for a capture that will never come.
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& kv : viewports_) {
        if (kv.second->capture) kv.second->capture->set_value(CapturedImage{});
        kv.second->capture.reset();
    }
}

bool RenderThread::ensureSwapchain(Viewport& v) {
    uint32_t w = 0, h = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        w = v.width;
        h = v.height;
    }
    if (w == 0 || h == 0) return false;
    if (!v.gpuReady) {
        for (Frame& f : v.frame) {
            VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pci.queueFamilyIndex = device_.graphicsFamily();
            vkCreateCommandPool(device_.device(), &pci, nullptr, &f.pool);
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = f.pool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            vkAllocateCommandBuffers(device_.device(), &ai, &f.cmd);
            VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            vkCreateSemaphore(device_.device(), &sci, nullptr, &f.acquire);
        }
        v.gpuReady = true;
    }
    if (v.swapchain && v.swapchain->valid() && v.swapW == w && v.swapH == h) return true;

    if (!v.swapchain) v.swapchain = std::make_unique<gpu::Swapchain>(device_, v.surface);
    // Everything we submitted uses the old images: wait for our own queue (nothing else submits to it).
    vkQueueWaitIdle(device_.graphicsQueue());
    // Frames in flight are finished now, so their acquire semaphores may be signalled-but-unwaited: recreate them.
    for (Frame& f : v.frame) {
        vkDestroySemaphore(device_.device(), f.acquire, nullptr);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(device_.device(), &sci, nullptr, &f.acquire);
        f.timelineValue = 0;
    }
    if (!v.swapchain->create(w, h)) return false;
    v.swapW = w;
    v.swapH = h;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++v.recreations;
    }
    if (v.depthView) vkDestroyImageView(device_.device(), v.depthView, nullptr);
    if (v.depth) vmaDestroyImage(device_.allocator(), v.depth, v.depthAlloc);
    v.depthView = VK_NULL_HANDLE;
    v.depth = VK_NULL_HANDLE;
    {
        const VkExtent2D e = v.swapchain->extent();
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = kDepthFormat;
        ici.extent = {e.width, e.height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        aci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (vmaCreateImage(device_.allocator(), &ici, &aci, &v.depth, &v.depthAlloc, nullptr) != VK_SUCCESS) return false;
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = v.depth;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = kDepthFormat;
        vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device_.device(), &vci, nullptr, &v.depthView) != VK_SUCCESS) return false;
    }
    if (!pass_ || pass_->format() != v.swapchain->format()) {
        mesh_.reset();
        pass_ = std::make_unique<ScenePass>(device_, v.swapchain->format(), kDepthFormat);
        mesh_ = std::make_unique<MeshPass>(device_, v.swapchain->format(), kDepthFormat, textures_->layout(), lightmapView_->layout());
    }
    return pass_->valid() && mesh_->valid();
}

} // namespace native::render
