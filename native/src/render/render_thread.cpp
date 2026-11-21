// render/render_thread -- see render_thread.h.
#include "render/render_thread.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <cstring>

#include "base/log.h"
#include "base/trace.h"
#include "gpu/swapchain.h"
#include "render/dlss_math.h"
#include "render/lightmap_view.h"
#include "render/mesh_pass.h"
#include "render/scene_pass.h"
#include "render/texture_table.h"

namespace native::render {

namespace {

// The clock of the light styles (they play at 10 Hz from the start of the program).
std::chrono::steady_clock::time_point styleEpoch() {
    static const auto epoch = std::chrono::steady_clock::now();
    return epoch;
}
double styleSeconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - styleEpoch()).count(); }

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
    // DLSS (render thread only, except dlssInfo which is guarded by RenderThread::mutex_).
    DlssTargets dlss;
    int dlssMode = 0;                     // the mode the targets were made for
    int dlssFailedMode = 0;               // Streamline refused this mode: draw normally until the mode changes
    bool dlssReset = false;               // the next upscaled frame starts a new history
    uint32_t dlssFrame = 0;               // jitter sequence position
    uint32_t dlssSettle = 0;              // frames still to render after the last change so the history converges (the loop is demand driven)
    Mat4 prevViewProj = mat4Identity();
    bool havePrev = false;
    unsigned lastCut = 0;
    int lastRenderMode = 0;
    uint64_t lastChanges = 0;
    DlssInfo dlssInfo;
    // The ray traced Lit path (R2). It shares `dlss` (the render-size colour / depth / mvec and the output image) and the previous-frame fields above; dlssOwner says which path made them.
    int dlssOwner = 0;                    // 0 none, 1 DLSS Super Resolution, 2 the ray traced Lit path
    RtLitTargets rt;
    bool rtFailed = false;                // the targets could not be made: this viewport stays on the chunked path until ray traced Lit is switched off
    uint32_t rtStill = 0;                 // consecutive frames of a still camera so far
    uint32_t rtFrameNumber = 0;           // the random sequence's position
    bool rtMore = false;                  // the last ray traced frame still traced (it asks for another one)
    uint32_t rtDeferred = 0;              // iterations this viewport's ray traced frame was held back by the budget (R8)
    bool rtOutputReady = false;           // the output image holds a finished ray traced frame (a converged repaint reuses it)
    float rtOutStyleW[4] = {1.f, 1.f, 1.f, 1.f};   // the style values that frame was made with
    std::array<uint64_t, 5> rtOutKey{};   // scene, acceleration structure, textures, sky, lights it was made with
    uint32_t rtOutSame = 0;               // full frames in a row whose inputs equalled their predecessor's
    bool rtRr = false;                    // the ray traced Lit targets were made for DLSS Ray Reconstruction
    int rtRrFailedMode = 0;               // Streamline refused Ray Reconstruction in this DLSS mode: the filter is used until the mode changes
    RtLighting rtLighting;
    uint64_t lastLightSerial = 0;         // the lights the accumulation was made with
    uint64_t rtSlotFrame[2] = {0, 0};
    RtLitInfo rtInfo;
    std::unique_ptr<gpu::GpuTimer> timer;   // R8: render thread only
    std::vector<PassTime> gpuPasses;        // guarded by RenderThread::mutex_: the smoothed pass times
    uint64_t gpuPassFrames = 0;
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
    models_ = std::make_unique<ModelRenderer>(device_, *textures_);
    // the display pages of the lit modes: one empty layer until the bake publishes its layout (the shader needs something to sample)
    lightmapView_ = std::make_unique<LightmapView>(device_);
    display_ = std::make_unique<light::PageImages>(device_, 1024, VK_FORMAT_R16G16B16A16_SFLOAT, true);
    display_->ensureLayers(1);
    lightmapView_->update(display_->view());
    scatter_ = std::make_unique<light::Scatter>(device_);
    textures_->setGpuIdleWait([this] {
        device_.waitTimeline(renderTimeline_, nextTimelineValue_.load(), 5'000'000'000ull, "texture wait");
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
    meshHdr_.reset();
    dlssPass_.reset();
    mesh_.reset();
    pass_.reset();
    scatter_.reset();
    display_.reset();
    lightmapView_.reset();
    rtScene_.reset();
    models_.reset();
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
        if (it->second->view.viewProj != view.viewProj || it->second->view.cut != view.cut) activeViewport_ = id;   // the viewport whose camera moves is the one being worked in
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

void RenderThread::setModels(std::shared_ptr<const ModelScene> models) {
    models_->setScene(std::move(models));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        workPending_ = true;   // the next iteration takes the scene (and draws the viewports that need it)
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

void RenderThread::setRayTracing(bool on, const RtOptions& options) {
    std::lock_guard<std::mutex> lock(mutex_);
    rtOptions_ = options;
    rtEnabled_.store(on && device_.caps().lit());
    workPending_ = true;
    wake_.notify_all();
}

void RenderThread::setRtLit(bool on, const RtLitOptions& options) {
    std::lock_guard<std::mutex> lock(mutex_);
    rtLitOptions_ = options;
    const bool enable = on && device_.caps().lit();
    rtLitEnabled_.store(enable);
    if (!enable)
        for (auto& kv : viewports_) kv.second->rtFailed = false;
    workPending_ = true;
    for (auto& kv : viewports_) kv.second->dirty = true;
    wake_.notify_all();
}

void RenderThread::setObjectRules(std::shared_ptr<const ObjectRules> rules) {
    scene_->setRules(std::move(rules));
    std::lock_guard<std::mutex> lock(mutex_);
    workPending_ = true;
    for (auto& kv : viewports_) kv.second->dirty = true;
    wake_.notify_all();
}

void RenderThread::setRtLights(std::shared_ptr<const RtLightData> lights) {
    std::lock_guard<std::mutex> lock(mutex_);
    rtLightData_ = std::move(lights);
    ++rtLightSerial_;
    if (rtLitEnabled_.load()) {
        for (auto& kv : viewports_) kv.second->dirty = true;
        wake_.notify_all();
    }
}

void RenderThread::setStyleClock(double seconds) {
    styleClock_.store(seconds);
    std::lock_guard<std::mutex> lock(mutex_);
    if (rtLitEnabled_.load()) {
        for (auto& kv : viewports_) kv.second->dirty = true;
        wake_.notify_all();
    }
}

double RenderThread::styleNow() const {
    const double c = styleClock_.load();
    return c >= 0.0 ? c : styleSeconds();
}

void RenderThread::setRtLighting(const RtLighting& lighting) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (rtLighting_ == lighting) return;
    rtLighting_ = lighting;
    if (rtLitEnabled_.load()) {
        for (auto& kv : viewports_) kv.second->dirty = true;
        wake_.notify_all();
    }
}

RenderThread::RtLitInfo RenderThread::rtLitInfo(ViewportId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = viewports_.find(id);
    return it == viewports_.end() ? RtLitInfo{} : it->second->rtInfo;
}

std::vector<RenderThread::PassTime> RenderThread::gpuPassTimes(ViewportId id, uint64_t* frames) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = viewports_.find(id);
    if (it == viewports_.end()) return {};
    if (frames) *frames = it->second->gpuPassFrames;
    return it->second->gpuPasses;
}

RenderThread::RtInfo RenderThread::rtInfo() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RtInfo i = rtInfo_;
    i.enabled = rtEnabled_.load() || rtLitEnabled_.load();
    return i;
}

RenderThread::DlssInfo RenderThread::dlssInfo(ViewportId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = viewports_.find(id);
    return it == viewports_.end() ? DlssInfo{} : it->second->dlssInfo;
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
    releaseDlss(v);
    if (v.depthView) vkDestroyImageView(device_.device(), v.depthView, nullptr);
    if (v.depth) vmaDestroyImage(device_.allocator(), v.depth, v.depthAlloc);
    v.depthView = VK_NULL_HANDLE;
    v.depth = VK_NULL_HANDLE;
    v.depthAlloc = nullptr;
    v.swapchain.reset();
    v.gpuReady = false;
}

void RenderThread::releaseDlss(Viewport& v) {
    if (v.rt.valid() && rtLit_) {
        vkQueueWaitIdle(device_.graphicsQueue());
        rtLit_->destroyTargets(v.rt);
    }
    if (v.dlss.valid() && dlssPass_) {
        vkQueueWaitIdle(device_.graphicsQueue());
        if (gpu::Streamline* sl = device_.streamline()) sl->dlssRelease(v.id);
        dlssPass_->destroyTargets(v.dlss);
    }
    v.dlssOwner = 0;
    v.rtStill = 0;
    v.rtMore = false;
    v.rtOutputReady = false;
    v.dlssMode = 0;
    v.havePrev = false;
    v.dlssSettle = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    v.dlssInfo.active = false;
}

namespace {

// The camera part of a DLSS frame (Super Resolution and Ray Reconstruction): output size, projection matrices, the basis, the planes.
void fillDlssCamera(gpu::DlssFrame& df, const ViewState& view, VkExtent2D extent, const Mat4& clipToPrev, const Mat4& prevToClip) {
    const auto toFloat = [](const Mat4& m, float* out) {
        for (size_t i = 0; i < 16; ++i) out[i] = static_cast<float>(m[i]);
    };
    df.outW = extent.width;
    df.outH = extent.height;
    toFloat(view.proj, df.viewToClip);
    toFloat(mat4Invert(view.proj), df.clipToView);
    toFloat(clipToPrev, df.clipToPrevClip);
    toFloat(prevToClip, df.prevClipToClip);
    for (int i = 0; i < 3; ++i) {
        df.position[i] = static_cast<float>(view.eye[i]);
        df.up[i] = static_cast<float>(view.up[i]);
        df.right[i] = static_cast<float>(view.right[i]);
        df.forward[i] = static_cast<float>(view.forward[i]);
    }
    df.nearPlane = static_cast<float>(view.nearPlane);
    df.farPlane = static_cast<float>(view.farPlane);
    df.fovY = static_cast<float>(view.fovY);
    df.aspect = static_cast<float>(extent.width) / static_cast<float>(std::max(1u, extent.height));
    // the view matrix (world -> camera, the camera looks down -z) and its inverse, column-major
    Mat4 V = mat4Identity();
    for (int c = 0; c < 3; ++c) {
        V[static_cast<size_t>(c * 4 + 0)] = view.right[c];
        V[static_cast<size_t>(c * 4 + 1)] = view.up[c];
        V[static_cast<size_t>(c * 4 + 2)] = -view.forward[c];
    }
    double dr = 0, du = 0, df2 = 0;
    for (int c = 0; c < 3; ++c) {
        dr += view.right[c] * view.eye[c];
        du += view.up[c] * view.eye[c];
        df2 += view.forward[c] * view.eye[c];
    }
    V[12] = -dr;
    V[13] = -du;
    V[14] = df2;
    toFloat(V, df.worldToView);
    toFloat(mat4Invert(V), df.viewToWorld);
}

} // namespace

bool RenderThread::recordDlss(Viewport& v, VkCommandBuffer cmd, const ViewState& view, VkExtent2D extent, const DrawLists& lists, const float clear[3], LitParams lit,
                              gpu::Streamline& sl) {
    const int mode = view.dlss;
    if (!dlssPass_ || dlssPass_->swapchainFormat() != v.swapchain->format()) {
        vkQueueWaitIdle(device_.graphicsQueue());
        dlssPass_ = std::make_unique<DlssPass>(device_, v.swapchain->format(), kDepthFormat);
    }
    if (!meshHdr_)
        meshHdr_ = std::make_unique<MeshPass>(device_, VK_FORMAT_R16G16B16A16_SFLOAT, kDepthFormat, textures_->layout(), lightmapView_->layout());
    if (!dlssPass_->valid() || !meshHdr_->valid()) {
        v.dlssFailedMode = mode;
        base::logWarning("DLSS: the pipelines could not be made; this viewport is drawn without it");
        return false;
    }
    DlssTargets& t = v.dlss;
    if (!t.valid() || t.out.width != extent.width || t.out.height != extent.height || v.dlssMode != mode || v.dlssOwner != 1) {
        uint32_t rw = 0, rh = 0;
        if (!sl.dlssRenderSize(mode, extent.width, extent.height, &rw, &rh)) {
            v.dlssFailedMode = mode;
            base::logWarning("DLSS: no render size for " + std::to_string(extent.width) + "x" + std::to_string(extent.height) + "; this viewport is drawn without it");
            return false;
        }
        vkQueueWaitIdle(device_.graphicsQueue());
        sl.dlssRelease(v.id);
        if (v.rt.valid() && rtLit_) rtLit_->destroyTargets(v.rt);
        if (!dlssPass_->createTargets(t, {rw, rh}, extent)) {
            v.dlssFailedMode = mode;
            return false;
        }
        base::logInfo("DLSS " + std::string(dlssModeName(static_cast<DlssMode>(mode))) + ": viewport " + std::to_string(v.id) + " renders " + std::to_string(rw) + "x" +
                      std::to_string(rh) + " for " + std::to_string(extent.width) + "x" + std::to_string(extent.height));
        v.dlssMode = mode;
        v.dlssOwner = 1;
        v.dlssReset = true;
        v.dlssFrame = 0;
        v.havePrev = false;
    }
    const uint32_t rw = t.render.width, rh = t.render.height;
    const uint32_t phases = dlssPhaseCount(rw, extent.width);
    double jx = 0, jy = 0;
    dlssJitter(v.dlssFrame++, phases, &jx, &jy);
    const Mat4 jittered = jitteredViewProj(view.viewProj, jx, jy, rw, rh);
    const uint64_t changes = scene_->changeSerial();
    const bool moved = !v.havePrev || view.viewProj != v.prevViewProj;
    const bool reset = v.dlssReset || !v.havePrev || view.cut != v.lastCut || view.renderMode != v.lastRenderMode || changes != v.lastChanges;
    const bool changed = moved || reset;
    const Mat4 prevVP = v.havePrev ? v.prevViewProj : view.viewProj;
    const Mat4 clipToPrev = mat4Multiply(prevVP, mat4Invert(view.viewProj));
    const Mat4 prevToClip = mat4Multiply(view.viewProj, mat4Invert(prevVP));
    const Mat4 reproject = mat4Multiply(prevVP, mat4Invert(jittered));
    v.prevViewProj = view.viewProj;
    v.havePrev = true;
    v.lastCut = view.cut;
    v.lastRenderMode = view.renderMode;
    v.lastChanges = changes;
    v.dlssReset = false;
    if (changed) v.dlssSettle = phases + 6;

    // 1. the scene at the render size, jittered
    dlssPass_->beginScene(cmd, t);
    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = t.color.view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue.color = {{clear[0], clear[1], clear[2], 1.0f}};
    VkRenderingAttachmentInfo datt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    datt.imageView = t.depth.view;
    datt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    datt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    datt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    datt.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, t.render};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    ri.pDepthAttachment = &datt;
    vkCmdBeginRendering(cmd, &ri);
    {
        const VkViewport viewport{0.0f, 0.0f, static_cast<float>(rw), static_cast<float>(rh), 0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, t.render};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
    }
    ViewState sceneView = view;
    sceneView.viewProj = jittered;
    lit.lodBias = dlssMipBias(rw, extent.width);
    FrameOverlays none;
    meshHdr_->record(cmd, sceneView, *scene_, lists, textures_->set(), skyIndex_, true, none, lightmapView_->set(), lit, kMeshScene);
    vkCmdEndRendering(cmd);

    // 2. camera motion vectors from the depth, 3. the upscale
    float rep[16];
    for (int i = 0; i < 16; ++i) rep[i] = static_cast<float>(reproject[static_cast<size_t>(i)]);
    dlssPass_->endSceneAndMotion(cmd, t, rep, static_cast<float>(jx), static_cast<float>(jy));
    dlssPass_->beforeUpscale(cmd, t);
    gpu::DlssFrame df;
    df.viewport = v.id;
    df.mode = mode;
    df.renderW = rw;
    df.renderH = rh;
    df.jitter[0] = static_cast<float>(jx);
    df.jitter[1] = static_cast<float>(jy);
    fillDlssCamera(df, view, extent, clipToPrev, prevToClip);
    df.reset = reset;
    df.color = DlssPass::color(t);
    df.depth = DlssPass::depth(t);
    df.mvec = DlssPass::mvec(t);
    df.out = DlssPass::output(t);
    std::string err;
    const bool ok = sl.dlssEvaluate(cmd, df, &err);
    if (!ok) {
        v.dlssFailedMode = mode;
        base::logError("DLSS: " + err + "; this viewport is drawn without it");
    }
    dlssPass_->afterUpscale(cmd, t);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        v.dlssInfo.active = ok;
        v.dlssInfo.failed = !ok;
        v.dlssInfo.mode = mode;
        v.dlssInfo.renderW = rw;
        v.dlssInfo.renderH = rh;
        v.dlssInfo.outW = extent.width;
        v.dlssInfo.outH = extent.height;
        if (reset) ++v.dlssInfo.resets;
        if (ok) ++v.dlssInfo.frames;
    }
    return ok;
}

bool RenderThread::recordRtLit(Viewport& v, VkCommandBuffer cmd, uint32_t slot, const ViewState& view, VkExtent2D extent, const DrawLists& lists, const float clear[3],
                               gpu::Streamline* sl) {
    if (!rtScene_ || !rtScene_->valid() || !mesh_) return false;
    const VkAccelerationStructureKHR tlas = rtScene_->tlas();
    if (tlas == VK_NULL_HANDLE) return false;   // the ray tracing scene has no triangle yet: the normal frame
    if (!rtLit_) rtLit_ = std::make_unique<RtLit>(device_, textures_->layout());
    if (!dlssPass_ || dlssPass_->swapchainFormat() != v.swapchain->format()) {
        vkQueueWaitIdle(device_.graphicsQueue());
        dlssPass_ = std::make_unique<DlssPass>(device_, v.swapchain->format(), kDepthFormat);
    }
    if (!meshHdr_)
        meshHdr_ = std::make_unique<MeshPass>(device_, VK_FORMAT_R16G16B16A16_SFLOAT, kDepthFormat, textures_->layout(), lightmapView_->layout());
    if (!rtLit_->valid() || !dlssPass_->valid() || !meshHdr_->valid()) {
        base::logWarning("ray traced Lit: the pipelines could not be made; this viewport is drawn with the chunked bake");
        v.rtFailed = true;
        return false;
    }
    RtLitOptions options;
    RtLighting lighting;
    std::shared_ptr<const RtLightData> lightData;
    uint64_t lightSerial = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        options = rtLitOptions_;
        lighting = rtLighting_;
        lightData = rtLightData_;
        lightSerial = rtLightSerial_;
    }
    if (!rtLightsGpu_) rtLightsGpu_ = std::make_unique<RtLightsGpu>(device_);
    if (lightSerial != rtLightsUploaded_) {   // the buffers are replaced: nothing in flight may still read the old ones
        waitForFrames();
        RtLightData none;
        none.records.assign(64, 0.f);
        none.cells = {0u, 0u};
        none.index = {0u};
        if (rtLightsGpu_->upload(lightData ? *lightData : none)) rtLightsUploaded_ = lightSerial;
        else if (device_.lost()) return false;
    }
    // DLSS Ray Reconstruction denoises and upscales when the viewport asks for DLSS (Quality / DLAA); otherwise the a-trous filter runs at the output size.
    const int rrMode = view.dlss;
    if (rrMode == 0) v.rtRrFailedMode = 0;
    bool rr = options.reconstruction && sl && sl->rrAvailable() && rrMode != 0 && v.rtRrFailedMode != rrMode;
    VkExtent2D render = extent;
    if (rr) {
        uint32_t rw = 0, rh = 0;
        if (sl->dlssRenderSize(rrMode, extent.width, extent.height, &rw, &rh, true)) render = {rw, rh};
        else {
            v.rtRrFailedMode = rrMode;
            base::logWarning("Ray Reconstruction: no render size for " + std::to_string(extent.width) + "x" + std::to_string(extent.height) + "; this viewport uses the filter");
            rr = false;
        }
    }
    DlssTargets& d = v.dlss;
    bool created = false;
    if (!d.valid() || !v.rt.valid() || v.dlssOwner != 2 || v.rtRr != rr || (rr && v.dlssMode != rrMode) || d.out.width != extent.width || d.out.height != extent.height ||
        d.render.width != render.width || d.render.height != render.height) {
        vkQueueWaitIdle(device_.graphicsQueue());
        if (sl) sl->dlssRelease(v.id);
        rtLit_->destroyTargets(v.rt);
        if (!dlssPass_->createTargets(d, render, extent) || !rtLit_->createTargets(v.rt, render, rr)) {
            dlssPass_->destroyTargets(d);
            rtLit_->destroyTargets(v.rt);
            v.dlssOwner = 0;
            v.rtFailed = true;
            base::logWarning("ray traced Lit: the targets could not be made; this viewport is drawn with the chunked bake");
            return false;
        }
        v.dlssOwner = 2;
        v.dlssMode = rr ? rrMode : 0;
        v.rtRr = rr;
        v.dlssFrame = 0;
        v.havePrev = false;
        created = true;
        base::logInfo("ray traced Lit: viewport " + std::to_string(v.id) + " renders " + std::to_string(render.width) + "x" + std::to_string(render.height) +
                      (rr ? std::string(", DLSS Ray Reconstruction ") + dlssModeName(static_cast<DlssMode>(rrMode)) + " to " + std::to_string(extent.width) + "x" + std::to_string(extent.height) : std::string(", filtered")));
    }
    {   // the counters of the frame that used this slot last (it has finished)
        RtLit::Counters c;
        if (rtLit_->readCounters(v.rt, slot, &c)) {
            std::lock_guard<std::mutex> lock(mutex_);
            v.rtInfo.tracedPixels = c.traced;
            v.rtInfo.surfacePixels = c.surface;
            v.rtInfo.counterFrame = v.rtSlotFrame[slot & 1u];
        }
    }

    // ---- history rules: what restarts it, what keeps it
    const uint64_t changes = scene_->lightingSerial();   // not changeSerial(): an edit that leaves every shape as it was (a texture) keeps the accumulated light
    const bool moved = !v.havePrev || view.viewProj != v.prevViewProj;
    const bool reset = created || !v.havePrev || view.cut != v.lastCut || view.renderMode != v.lastRenderMode || changes != v.lastChanges || !lighting.sameTrace(v.rtLighting) ||
                         rtLightsUploaded_ != v.lastLightSerial;
    const bool still = !moved && !reset;
    const bool skip = still && v.rtStill >= options.targetSamples + 1;
    // A repaint of a converged picture (an overlay changed, the window was exposed) costs nothing: the output of the last full frame is still right when nothing that shapes it changed
    // (camera, scene, lights, the animated style values) and the upscaler had its settling frames. G-rt-still (12 §9: no more than Unlit + 0.5 ms).
    float styleNowW[4] = {1.f, 1.f, 1.f, 1.f};
    if (lightData) lightData->layerWeights(styleNow(), styleNowW);
    // everything the picture depends on besides the camera and the traced light: the shapes and materials, the textures that arrived, the sky, the acceleration structure, the display
    // settings of the compose pass (exposure), the style values
    const std::array<uint64_t, 5> outKey = {scene_->changeSerial(), rtScene_->generation(), (static_cast<uint64_t>(textures_->assigned()) << 32) | skyIndex_,
                                            textures_->pending() == 0 ? 1ull : 0ull, rtLightsUploaded_};
    const bool sameInputs = outKey == v.rtOutKey && lighting == v.rtLighting && std::equal(styleNowW, styleNowW + 4, v.rtOutStyleW);
    const uint32_t settledAfter = options.targetSamples + 1 + (rr ? dlssPhaseCount(render.width, extent.width) + 6 : 1);
    if (still && !created && v.rtOutputReady && v.rtStill >= settledAfter && sameInputs && v.rtOutSame >= 2 && outKey[3] == 1) {
        ++v.rtStill;
        v.rtMore = false;
        std::lock_guard<std::mutex> lock(mutex_);
        v.rtInfo.active = true;
        v.rtInfo.stillFrames = v.rtStill;
        v.rtInfo.tracingSkipped = true;
        return true;
    }
    const Mat4 prevVP = v.havePrev ? v.prevViewProj : view.viewProj;
    double jx = 0, jy = 0;
    uint32_t phases = 1;
    if (rr) {
        phases = dlssPhaseCount(render.width, extent.width);
        dlssJitter(v.dlssFrame++, phases, &jx, &jy);
        if (moved || reset || !(lighting == v.rtLighting)) v.dlssSettle = phases + 6;   // the upscaler needs a few jittered frames after any change
    }
    const Mat4 jittered = rr ? jitteredViewProj(view.viewProj, jx, jy, render.width, render.height) : view.viewProj;
    const Mat4 reproject = mat4Multiply(prevVP, mat4Invert(jittered));
    const Mat4 clipToPrev = mat4Multiply(prevVP, mat4Invert(view.viewProj));
    const Mat4 prevToClip = mat4Multiply(view.viewProj, mat4Invert(prevVP));
    v.rtStill = still ? v.rtStill + 1 : 0;
    v.rtMore = !skip;
    v.prevViewProj = view.viewProj;
    v.havePrev = true;
    v.lastCut = view.cut;
    v.lastRenderMode = view.renderMode;
    v.lastChanges = changes;
    v.rtLighting = lighting;
    v.lastLightSerial = rtLightsUploaded_;

    // ---- 1. the G-buffer at the render size: albedo (the colour target), normal, distance
    if (rr) rtLit_->prepareReconstruction(cmd, v.rt);
    dlssPass_->beginScene(cmd, d);
    rtLit_->beginGBuffer(cmd, v.rt);
    VkRenderingAttachmentInfo atts[3]{{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}};
    const VkImageView views[3] = {d.color.view, RtLit::normalView(v.rt), RtLit::distView(v.rt)};
    for (int i = 0; i < 3; ++i) {
        atts[i].imageView = views[i];
        atts[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        atts[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        atts[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    atts[0].clearValue.color = {{clear[0], clear[1], clear[2], 1.0f}};
    VkRenderingAttachmentInfo datt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    datt.imageView = d.depth.view;
    datt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    datt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    datt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    datt.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, render};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 3;
    ri.pColorAttachments = atts;
    ri.pDepthAttachment = &datt;
    vkCmdBeginRendering(cmd, &ri);
    {
        const VkViewport viewport{0.0f, 0.0f, static_cast<float>(render.width), static_cast<float>(render.height), 0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, render};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
    }
    FrameOverlays none;
    ViewState sceneView = view;
    sceneView.viewProj = jittered;
    LitParams gl;
    if (rr) gl.lodBias = dlssMipBias(render.width, extent.width);
    meshHdr_->record(cmd, sceneView, *scene_, lists, textures_->set(), skyIndex_, false, none, lightmapView_->set(), gl, kMeshGBuffer);
    vkCmdEndRendering(cmd);
    float rep[16];
    for (int i = 0; i < 16; ++i) rep[i] = static_cast<float>(reproject[static_cast<size_t>(i)]);
    dlssPass_->endSceneAndMotion(cmd, d, rep, static_cast<float>(jx), static_cast<float>(jy));
    rtLit_->endGBuffer(cmd, v.rt);
    if (v.timer) v.timer->stamp(cmd, slot, "raster");

    // ---- 2. the light of every pixel
    dlssPass_->beforeUpscale(cmd, d);
    RtLitFrame fr;
    fr.timer = v.timer.get();
    fr.timerSlot = slot;
    fr.slot = slot;
    fr.frameNumber = v.rtFrameNumber++;
    fr.reset = reset;
    fr.still = still;
    fr.skipTrace = skip;
    fr.reconstruction = rr;
    fr.mode = view.renderMode;
    fr.tlas = tlas;
    for (int i = 0; i < 3; ++i) {
        fr.eye[i] = view.eye[i];
        fr.forward[i] = view.forward[i];
    }
    fr.jitteredViewProj = jittered;
    fr.prevViewProj = prevVP;
    fr.lighting = lighting;
    fr.options = options;
    fr.lights = rtLightsGpu_.get();
    if (lightData) lightData->layerWeights(styleNow(), fr.styleW);   // light styles: the layers' animated values of this frame
    fr.records = rtScene_->recordBuffer();
    fr.textures = textures_->set();
    bool ok = rtLit_->record(cmd, v.rt, d, fr);
    if (ok && rr) {   // 3. Ray Reconstruction: the radiance of the compose pass + the guides in, the denoised, upscaled picture out (d.output)
        gpu::DlssFrame df;
        df.viewport = v.id;
        df.mode = rrMode;
        df.renderW = render.width;
        df.renderH = render.height;
        df.jitter[0] = static_cast<float>(jx);
        df.jitter[1] = static_cast<float>(jy);
        fillDlssCamera(df, view, extent, clipToPrev, prevToClip);
        df.reset = reset;
        df.reconstruction = true;
        df.color = RtLit::radiance(v.rt);
        df.depth = DlssPass::depth(d);
        df.mvec = DlssPass::mvec(d);
        df.out = DlssPass::output(d);
        df.albedo = DlssPass::color(d);
        df.specAlbedo = RtLit::specularAlbedo(v.rt);
        df.normal = RtLit::normalForUpscaler(v.rt);
        std::string err;
        if (!sl->dlssEvaluate(cmd, df, &err)) {
            v.rtRrFailedMode = rrMode;
            base::logError("Ray Reconstruction: " + err + "; this viewport uses the filter");
            ok = false;
            std::lock_guard<std::mutex> lock(mutex_);
            v.dirty = true;
        }
    }
    dlssPass_->afterUpscale(cmd, d);
    if (v.timer) v.timer->stamp(cmd, slot, "upscale");
    v.rtOutputReady = ok;
    v.rtOutSame = sameInputs ? v.rtOutSame + 1 : 0;   // the upscaler / filter needs a second frame on unchanged inputs before the picture is settled
    v.rtOutKey = outKey;
    std::copy(fr.styleW, fr.styleW + 4, v.rtOutStyleW);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        v.rtSlotFrame[slot & 1u] = v.rtInfo.frames + 1;
        v.rtInfo.active = ok;
        v.rtInfo.reconstruction = ok && rr;
        v.rtInfo.renderW = render.width;
        v.rtInfo.renderH = render.height;
        ++v.rtInfo.frames;
        if (reset) ++v.rtInfo.resets;
        v.rtInfo.stillFrames = v.rtStill;
        v.rtInfo.tracingSkipped = skip;
    }
    return ok;
}

// Waits until every frame and upload submitted so far finished (the display pages may change, the descriptor set may be rewritten).
void RenderThread::waitForFrames() {
    const uint64_t value = nextTimelineValue_.load();
    if (value == 0) return;
    device_.waitTimeline(renderTimeline_, value, 5'000'000'000ull, "frame wait");
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
            device_.waitTimeline(mailbox.renderTimeline(), renderValue - 1, 5'000'000'000ull, "mailbox wait");
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

// R1: keeps the ray tracing scene in step with the GpuScene while ray tracing is on (render thread, in the upload command buffer after the scene's own uploads).
bool RenderThread::syncRayTracing(VkCommandBuffer cmd) {
    if (!rtEnabled_.load() && !rtLitEnabled_.load()) {
        if (rtScene_) {
            vkQueueWaitIdle(device_.graphicsQueue());
            rtScene_.reset();
            std::lock_guard<std::mutex> lock(mutex_);
            rtInfo_ = RtInfo{};
        }
        return false;
    }
    if (!rtScene_) {
        RtOptions options;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            options = rtOptions_;
        }
        rtScene_ = std::make_unique<RtScene>(device_, *scene_, options);
        if (!rtScene_->valid()) {
            base::logWarning("ray tracing scene: could not be created, ray tracing stays off");
            rtScene_.reset();
            rtEnabled_.store(false);
            rtLitEnabled_.store(false);
            return false;
        }
    }
    const bool more = rtScene_->update(cmd, completedFrames(), nextTimelineValue_.load());
    std::lock_guard<std::mutex> lock(mutex_);
    rtInfo_.valid = true;
    rtInfo_.generation = rtScene_->generation();
    rtInfo_.tlas = rtScene_->tlas();
    rtInfo_.records = rtScene_->recordBuffer();
    rtInfo_.stats = rtScene_->stats();
    return more;
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
        bool rtDue = false;
        std::string skyName;
        std::shared_ptr<const EntityOverlay> overlay;
        std::shared_ptr<const GlyphAtlas> atlas;
        std::shared_ptr<light::LightLink> link;
        std::vector<std::shared_ptr<Viewport>> toRelease;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            const auto ready = [&] {
                if (stop_ || workPending_) return true;
                for (auto& kv : viewports_)
                    if (kv.second->removing || kv.second->dirty || kv.second->capture) return true;
                return false;
            };
            // the ray tracing scene's idle upgrade is due at a time of its own
            const auto rtDeadline = rtScene_ && !device_.lost() ? rtScene_->nextWake() : std::chrono::steady_clock::time_point::max();
            // light styles play at 10 Hz: while the level has an animated style the ray traced views draw a frame every style step (the compose pass applies the new values)
            const bool styleTick = rtLitEnabled_.load() && styleClock_.load() < 0.0 && rtLightData_ && rtLightData_->animated();
            auto styleDeadline = std::chrono::steady_clock::time_point::max();
            if (styleTick) {
                const double step = 1.0 / core::kStyleHz;
                styleDeadline = styleEpoch() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>((std::floor(styleSeconds() / step) + 1.0) * step));
            }
            // animated models play at about 30 frames a second
            const bool modelTick = models_ && models_->animated();
            auto modelDeadline = std::chrono::steady_clock::time_point::max();
            if (modelTick) {
                if (modelNext_ < std::chrono::steady_clock::now() - std::chrono::milliseconds(200)) modelNext_ = std::chrono::steady_clock::now();
                modelDeadline = modelNext_;
            }
            const auto deadline = std::min(std::min(rtDeadline, styleDeadline), modelDeadline);
            if (deadline == std::chrono::steady_clock::time_point::max()) wake_.wait(lock, ready);
            else wake_.wait_until(lock, deadline, ready);
            rtDue = rtDeadline != std::chrono::steady_clock::time_point::max() && std::chrono::steady_clock::now() >= rtDeadline;
            if (styleTick && std::chrono::steady_clock::now() >= styleDeadline)
                for (auto& kv : viewports_)
                    if (kv.second->view.renderMode > 0) kv.second->dirty = true;
            if (modelTick && std::chrono::steady_clock::now() >= modelDeadline) {
                for (auto& kv : viewports_) kv.second->dirty = true;
                modelNext_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(33);
            }
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
                // R8 ray budget: with several ray traced viewports in one iteration the active one draws first and the others are held back one iteration when the GPU time of
                // the iteration would exceed the budget (a held-back viewport is drawn at the next iteration whatever the budget says: nothing starves)
                if (rtLitEnabled_.load() && rtLitOptions_.frameBudgetMs > 0.0f && work.size() > 1) {
                    const auto isRt = [&](const Work& w) { return w.view.perspective && (w.view.renderMode == 1 || w.view.renderMode == 2) && !w.v->rtFailed && !w.capture; };
                    const auto cost = [&](const Work& w) {
                        double ms = 0;
                        for (const PassTime& t : w.v->gpuPasses) ms += t.ms;
                        return ms > 0 ? ms : 2.0;   // no measurement yet
                    };
                    const ViewportId activeId = activeViewport_.load();
                    std::stable_sort(work.begin(), work.end(), [&](const Work& a, const Work& b) { return (a.v->id == activeId) > (b.v->id == activeId); });
                    double spent = 0;
                    bool any = false;
                    for (size_t i = 0; i < work.size();) {
                        Work& w = work[i];
                        if (!isRt(w)) { ++i; continue; }
                        const double c = cost(w);
                        if (any && w.v->rtDeferred < 1 && spent + c > rtLitOptions_.frameBudgetMs) {
                            ++w.v->rtDeferred;
                            w.v->dirty = true;
                            work.erase(work.begin() + static_cast<std::ptrdiff_t>(i));
                            continue;
                        }
                        w.v->rtDeferred = 0;
                        spent += c;
                        any = true;
                        ++i;
                    }
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
        if (device_.lost()) {   // the GPU is gone: nothing is submitted or waited for any more, requests are answered empty; removals and the stop still work
            for (Work& w : work)
                if (w.capture) w.capture->set_value(CapturedImage{});
            continue;
        }

        // the iteration's upload ring slice and upload commands (scene geometry, textures), then every viewport's frame
        uint64_t lastValue = 0;
        LightSubmit lightSubmit;
        const uint32_t slot = static_cast<uint32_t>(iteration_++ % 2);
        const bool active = !work.empty() || doUpload || rtDue;
        if (active) {
            if (ringValue_[slot] > 0) device_.waitTimeline(renderTimeline_, ringValue_[slot], 5'000'000'000ull, "upload ring wait");
            bool ringReady = true;
            while (!(ringReady = ring_->beginFrame(completedFrames())) && !device_.lost())
                device_.waitTimeline(renderTimeline_, nextTimelineValue_.load(), 5'000'000'000ull, "upload ring wait");
            if (!ringReady) {   // the GPU is gone: nothing of this iteration can run, the requests are answered empty
                for (Work& w : work)
                    if (w.capture) w.capture->set_value(CapturedImage{});
                continue;
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
            models_->sync(*ring_, completedFrames(), nextTimelineValue_.load(), styleSeconds());   // before the textures' upload: it asks for the models' images
            more = textures_->upload(uploadCmd_[slot], *ring_) || more;
            more = syncRayTracing(uploadCmd_[slot]) || more;
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
            if (f.timelineValue > 0) device_.waitTimeline(renderTimeline_, f.timelineValue, 5'000'000'000ull, "frame wait");
            if (device_.lost()) {   // the wait above (or the last frame) found the device lost: give the request back empty
                if (w.capture) w.capture->set_value(CapturedImage{});
                continue;
            }
            gpu::Swapchain& sc = *v.swapchain;
            const uint32_t timerSlot = static_cast<uint32_t>((v.frameCounter - 1) % kFramesInFlight);
            static const bool timing = [] {
                char* value = nullptr;
                size_t len = 0;
                const bool off = _dupenv_s(&value, &len, "NATIVE_NO_GPU_TIMER") == 0 && value != nullptr;
                std::free(value);
                return !off;
            }();
            if (timing && !v.timer) v.timer = std::make_unique<gpu::GpuTimer>(device_, kFramesInFlight);
            if (v.timer) {   // the GPU time of the frame this slot drew last time, now that it finished
                std::vector<gpu::GpuTimer::Interval> iv;
                if (f.timelineValue > 0 && v.timer->collect(timerSlot, iv)) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    for (const auto& i : iv) {
                        auto p = std::find_if(v.gpuPasses.begin(), v.gpuPasses.end(), [&](const PassTime& t) { return t.name == i.name; });
                        if (p == v.gpuPasses.end()) v.gpuPasses.push_back({i.name, i.ms});
                        else p->ms += (i.ms - p->ms) * 0.1;
                    }
                    // a pass that was not in this frame decays towards zero (a frame without ray tracing has only "display")
                    for (auto& t : v.gpuPasses)
                        if (std::none_of(iv.begin(), iv.end(), [&](const gpu::GpuTimer::Interval& i) { return i.name == t.name; })) t.ms *= 0.9;
                    ++v.gpuPassFrames;
                }
            }
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
            device_.noteResult(acq, "acquire");
            if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
                if (!device_.lost()) base::logError("vkAcquireNextImageKHR failed (" + std::to_string(acq) + ")" + (acq == VK_TIMEOUT ? ": no image came back in 2 s" : ""));
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
            if (v.timer) v.timer->begin(timerSlot);   // after the command buffer that used the slot's queries was reset
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(f.cmd, &bi);
            if (v.timer) v.timer->stamp(f.cmd, timerSlot, "start");
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
            models_->buildDraws(w.view.viewProj, *ring_, lists.models);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                info_.visibleObjects = lists.visible;
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
            LitParams lit;
            lit.mode = w.view.renderMode;
            if (litSeen_) {
                lit.exposure = litSeen_->exposure;
                lit.brightness = litSeen_->brightness;
            }
            // DLSS Super Resolution (T6.8): the scene at the render size and the upscale come first, outside the display pass; the display pass then copies
            // the result to the swapchain image and draws the overlays at full size. Any trouble falls back to the normal frame below.
            // The ray traced Lit viewport (R2) draws the scene itself (G-buffer, compute passes) and replaces both.
            bool useRt = false;
            if (rtLitEnabled_.load() && !v.rtFailed && w.view.perspective && (w.view.renderMode == 1 || w.view.renderMode == 2) && v.swapchain)
                useRt = recordRtLit(v, f.cmd, static_cast<uint32_t>((v.frameCounter - 1) % kFramesInFlight), w.view, extent, lists, w.clear, sl);
            if (!useRt && v.dlssOwner == 2) releaseDlss(v);
            bool useDlss = false;
            if (!useRt && w.view.dlss != 0 && w.view.perspective && sl && sl->dlssAvailable() && v.dlssFailedMode != w.view.dlss && mesh_)
                useDlss = recordDlss(v, f.cmd, w.view, extent, lists, w.clear, lit, *sl);
            if (!useRt && !useDlss && v.dlss.valid() && (w.view.dlss == 0 || !w.view.perspective)) releaseDlss(v);
            if (!useDlss && !useRt) v.havePrev = false;
            if (!useRt && v.rtInfo.active) {
                std::lock_guard<std::mutex> lock(mutex_);
                v.rtInfo.active = false;
            }
            if (w.view.dlss == 0) v.dlssFailedMode = 0;
            if (!useDlss && v.dlssInfo.active) {
                std::lock_guard<std::mutex> lock(mutex_);
                v.dlssInfo.active = false;
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
            if (useDlss || useRt) dlssPass_->resolve(f.cmd, v.dlss);
            if (mesh_) {
                GlassSets glass;   // the ray traced view lights translucent surfaces per fragment
                if (useRt && rtLit_ && mesh_->enableGlass(rtLit_->litSetLayout(), rtLit_->tlasSetLayout())) {
                    const uint32_t rtSlot = static_cast<uint32_t>((v.frameCounter - 1) % kFramesInFlight);
                    glass.lit = RtLit::litSet(v.rt, rtSlot);
                    glass.tlas = RtLit::tlasSet(v.rt, rtSlot);
                }
                mesh_->record(f.cmd, w.view, *scene_, lists, textures_->set(), skyIndex_, true, ov, lightmapView_->set(), lit,
                              useRt ? (kMeshOverlays | kMeshBlended) : (useDlss ? kMeshOverlays : kMeshAll), glass);
            }
            if (pass_) pass_->record(f.cmd, extent, w.view, !scene_->hasObjects());
            vkCmdEndRendering(f.cmd);
            if (v.timer) v.timer->stamp(f.cmd, timerSlot, "display");

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
                    if (device_.waitTimeline(renderTimeline_, signalValue, 5'000'000'000ull, "capture wait") != VK_SUCCESS) {
                        w.capture->set_value(CapturedImage{});
                        if (staging) vmaDestroyBuffer(device_.allocator(), staging, stagingAlloc);
                        continue;
                    }
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
            if ((useDlss || useRt) && v.dlssSettle > 0) {   // the loop only draws what changed: a still camera needs a few more jittered frames
                --v.dlssSettle;
                v.dirty = true;
                wake_.notify_all();
            }
            if (useRt && v.rtMore) {             // the accumulation goes on until the converged stop
                v.dirty = true;
                wake_.notify_all();
            }
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
