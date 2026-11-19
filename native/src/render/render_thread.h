// render/render_thread -- the render thread: owns the graphics queue use, every viewport's swapchain and the frame loop.
//
// Owns: a thread that renders a viewport only when its dirty flag is set (05_RENDERER.md §3 redraw policy: nothing changed =
// nothing presented, the GPU stays free for the bake), the renderTimeline semaphore (01_ARCHITECTURE.md §5), per-viewport
// frame resources, resize handling and frame capture for tests / screenshots.
// Thread: every public method may be called from any thread (UI thread in practice); GPU work happens on the render thread.
// Spec: 05_RENDERER.md §3, 01_ARCHITECTURE.md §2 and §5, 08_MILESTONES.md T1.3.
#pragma once

#include <volk.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "gpu/device.h"
#include "render/view_state.h"

namespace native::render {

class ScenePass;

using ViewportId = uint32_t;

struct CapturedImage {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;           // width * height * 4, row 0 at the top
    bool ok() const { return width > 0 && height > 0 && rgba.size() == static_cast<size_t>(width) * height * 4; }
};

class RenderThread {
public:
    explicit RenderThread(gpu::Device& device);
    ~RenderThread();                      // stops the loop, waits for the queue, releases every viewport
    RenderThread(const RenderThread&) = delete;
    RenderThread& operator=(const RenderThread&) = delete;

    // The surface must stay valid until removeViewport() returns. The first frame is requested immediately.
    ViewportId addViewport(VkSurfaceKHR surface, uint32_t width, uint32_t height);
    // Blocks until the render thread has destroyed this viewport's swapchain (so the surface may be destroyed after).
    void removeViewport(ViewportId id);
    // Size in pixels; the swapchain is recreated by the render thread before its next frame, which is requested.
    void resizeViewport(ViewportId id, uint32_t width, uint32_t height);
    void markDirty(ViewportId id);
    void markAllDirty();
    void setClearColor(ViewportId id, float r, float g, float b);
    // The camera view the next frames use (view-projection, grid); marks the viewport dirty.
    void setView(ViewportId id, const ViewState& view);

    // Renders the next frame of this viewport and returns its pixels. Blocks the caller (not the render thread's other
    // viewports) up to timeoutMs; returns an empty image on timeout or when the viewport has no surface area.
    CapturedImage capture(ViewportId id, int timeoutMs = 4000);

    uint64_t framesRendered(ViewportId id) const;
    uint64_t totalFrames() const { return totalFrames_.load(); }
    uint64_t swapchainRecreations(ViewportId id) const;
    // Number of frames the GPU has finished (renderTimeline value).
    uint64_t completedFrames() const;

private:
    struct Viewport;
    struct Frame;
    void loop();
    bool ensureSwapchain(Viewport& v);
    void releaseViewport(Viewport& v);

    gpu::Device& device_;
    VkSemaphore renderTimeline_ = VK_NULL_HANDLE;
    std::atomic<uint64_t> nextTimelineValue_{0};
    std::atomic<uint64_t> totalFrames_{0};
    std::unique_ptr<ScenePass> pass_;     // render thread only

    mutable std::mutex mutex_;
    std::condition_variable wake_;        // new work for the render thread
    std::condition_variable removed_;     // a viewport finished releasing
    std::map<ViewportId, std::shared_ptr<Viewport>> viewports_;
    ViewportId nextId_ = 1;
    bool stop_ = false;
    std::thread thread_;
};

} // namespace native::render
