// render/render_thread -- see render_thread.h.
#include "render/render_thread.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "base/log.h"
#include "base/trace.h"
#include "gpu/swapchain.h"
#include "render/scene_pass.h"

namespace native::render {

namespace {

constexpr int kFramesInFlight = 2;

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage,
                  VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
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
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
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
    std::shared_ptr<std::promise<CapturedImage>> capture;     // pending capture request
    uint64_t frames = 0, recreations = 0;
    // Render thread only.
    std::unique_ptr<gpu::Swapchain> swapchain;
    Frame frame[kFramesInFlight];
    uint32_t frameCounter = 0;
    uint32_t swapW = 0, swapH = 0;        // size the swapchain was created for (0 = must be recreated)
    bool gpuReady = false;
};

RenderThread::RenderThread(gpu::Device& device) : device_(device) {
    VkSemaphoreTypeCreateInfo tci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    tci.initialValue = 0;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &tci;
    vkCreateSemaphore(device_.device(), &sci, nullptr, &renderTimeline_);
    thread_ = std::thread([this] { loop(); });
}

RenderThread::~RenderThread() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    vkQueueWaitIdle(device_.graphicsQueue());
    vkDestroySemaphore(device_.device(), renderTimeline_, nullptr);
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
    v.swapchain.reset();
    v.gpuReady = false;
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
        };
        std::vector<Work> work;
        std::vector<std::shared_ptr<Viewport>> toRelease;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] {
                if (stop_) return true;
                for (auto& kv : viewports_)
                    if (kv.second->removing || kv.second->dirty || kv.second->capture) return true;
                return false;
            });
            if (stop_) {
                for (auto& kv : viewports_) toRelease.push_back(kv.second);
            } else {
                for (auto& kv : viewports_) {
                    Viewport& v = *kv.second;
                    if (v.removing) { toRelease.push_back(kv.second); continue; }
                    if (!v.dirty && !v.capture) continue;
                    work.push_back({kv.second, v.width, v.height, {v.clear[0], v.clear[1], v.clear[2]}, v.view, std::move(v.capture)});
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
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, extent};
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &att;
            vkCmdBeginRendering(f.cmd, &ri);
            if (pass_) pass_->record(f.cmd, extent, w.view);
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
            vkQueueSubmit2(device_.graphicsQueue(), 1, &si, VK_NULL_HANDLE);
            f.timelineValue = signalValue;

            const VkResult pres = sc.present(device_.graphicsQueue(), imageIndex);
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
    if (!pass_ || pass_->format() != v.swapchain->format()) pass_ = std::make_unique<ScenePass>(device_, v.swapchain->format());
    return pass_->valid();
}

} // namespace native::render
