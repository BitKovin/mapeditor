// render/render_thread -- the render thread: owns the graphics queue use, every viewport's swapchain and the frame loop.
//
// Owns: a thread that renders a viewport only when its dirty flag is set (05_RENDERER.md §3 redraw policy: nothing changed =
// nothing presented, the GPU stays free for the bake), the renderTimeline semaphore (01_ARCHITECTURE.md §5), per-viewport
// frame resources, resize handling and frame capture for tests / screenshots.
// Thread: every public method may be called from any thread (UI thread in practice); GPU work happens on the render thread.
// Spec: 05_RENDERER.md §3, 01_ARCHITECTURE.md §2 and §5, 08_MILESTONES.md T1.3.
#pragma once

#include "scene/snapshot.h"   // first: core headers use `near` as a name, which windows.h (via volk) defines as a macro

#include <volk.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <unordered_set>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "base/jobs.h"
#include "gpu/device.h"
#include "light/light_link.h"
#include "light/mailbox.h"
#include "light/page_images.h"
#include "render/gpu_scene.h"
#include "render/entity_overlay.h"
#include "render/dlss_pass.h"
#include "render/glyph_atlas.h"
#include "render/line_vertex.h"
#include "render/mesh_pass.h"
#include "render/view_state.h"

namespace native::render {

class ScenePass;
class MeshPass;
class TextureTable;
class LightmapView;

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

    // The scene the viewports draw (published by the scene builder; any thread). Marks every viewport dirty when the render thread has taken it.
    void setSnapshot(std::shared_ptr<const scene::SceneSnapshot> snapshot);
    // The objects of the snapshot that are not drawn (Hide); null = none.
    void setHiddenObjects(std::shared_ptr<const std::unordered_set<scene::ObjectKey, scene::ObjectKey::Hash>> hidden);
    // Where textures are read from and which pool decodes them.
    void setTextureSource(const std::filesystem::path& assetsRoot, base::JobPool* pool);
    // The selection outline drawn in every viewport (world space line list, two vertices a line; empty = none).
    void setSelectionLines(std::shared_ptr<const std::vector<LineVertex>> lines);
    // The rubber band of a box select in a viewport, in pixels of the viewport (x0, y0, x1, y1); `active` false hides it.
    void setRubberBand(ViewportId id, bool active, float x0 = 0, float y0 = 0, float x1 = 0, float y1 = 0);
    // What the viewports draw for entities (boxes, arrows, gizmos, class names); null = nothing.
    void setEntityOverlay(std::shared_ptr<const EntityOverlay> overlay);
    // The glyph atlas of the overlay texts (made on the UI thread for the window's pixel ratio); texts are not drawn without one.
    void setGlyphAtlas(std::shared_ptr<const GlyphAtlas> atlas);
    // The size labels of the selection in a viewport (world anchors; the UI thread works out the placement from the camera).
    void setSizeLabels(ViewportId id, std::shared_ptr<const std::vector<TextItem>> labels);
    // The transform gizmo of a viewport (world space line list, two vertices a line, drawn over everything; null = none).
    void setGizmoLines(ViewportId id, std::shared_ptr<const std::vector<LineVertex>> lines);
    // The bake's side of the lit modes: the lit meshes, the mailboxes and the "a viewport wants light" flag (set while some viewport is in a lit mode). The renderer
    // scatters the newest mailbox into its display pages before every frame. Call it before the lighting service starts, and with null before the link goes.
    void setLightLink(std::shared_ptr<light::LightLink> link);
    // The skybox ("env/<name> cube") drawn behind the scene in perspective views; "" = none.
    void setSkyTexture(const std::string& name);
    // What the scene holds on the GPU and how the textures stand (refreshed every loop iteration).
    struct SceneInfo {
        SceneStats scene;
        uint32_t texturesAssigned = 0, texturesLoaded = 0, texturesFailed = 0, texturesPending = 0;
        uint32_t visibleObjects = 0;   // of the last frame drawn
    };
    SceneInfo sceneInfo() const;

    // What the DLSS path does in a viewport (Stats panel, tests): `active` = the last frame was upscaled; `failed` = Streamline refused and the viewport draws normally.
    struct DlssInfo {
        bool active = false, failed = false;
        int mode = 0;                          // DlssMode as int of the last frame's request
        uint32_t renderW = 0, renderH = 0, outW = 0, outH = 0;
        uint64_t resets = 0;                   // frames that told DLSS to drop its history
        uint64_t frames = 0;                   // frames upscaled so far
    };
    DlssInfo dlssInfo(ViewportId id) const;

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
    // T6.8: records the scene at the render size, the motion vectors and the DLSS upscale of one viewport frame into `cmd` (outside a render pass). False
    // when the frame has to be drawn the normal way (no DLSS here, a failure; the viewport's DLSS state says why).
    bool recordDlss(Viewport& v, VkCommandBuffer cmd, const ViewState& view, VkExtent2D extent, const DrawLists& lists, const float clear[3], LitParams lit,
                    gpu::Streamline& sl);
    void releaseDlss(Viewport& v);

    gpu::Device& device_;
    VkSemaphore renderTimeline_ = VK_NULL_HANDLE;
    std::atomic<uint64_t> nextTimelineValue_{0};
    std::atomic<uint64_t> totalFrames_{0};
    std::unique_ptr<gpu::UploadRing> ring_;
    std::unique_ptr<TextureTable> textures_;
    std::unique_ptr<GpuScene> scene_;
    std::unique_ptr<ScenePass> pass_;     // render thread only
    std::unique_ptr<MeshPass> mesh_;      // render thread only
    std::unique_ptr<MeshPass> meshHdr_;   // the scene at the DLSS render size (RGBA16F), made on first use
    std::unique_ptr<DlssPass> dlssPass_;  // render thread only
    // Lit modes (render thread only, except link_)
    struct LightSubmit {
        bool wait = false, signal = false;
        uint64_t waitValue = 0, signalValue = 0;
    };
    LightSubmit syncLighting(light::LightLink& link, VkCommandBuffer cmd);
    void waitForFrames();
    std::shared_ptr<light::LightLink> link_;                  // guarded by mutex_
    std::unique_ptr<LightmapView> lightmapView_;
    std::unique_ptr<light::PageImages> display_;              // the display pages the lit shader samples
    std::unique_ptr<light::Scatter> scatter_;
    std::shared_ptr<const light::LitSet> litSeen_;
    uint64_t mailboxRenderValue_ = 0;
    uint32_t scatterUses_ = 0;
    VkCommandPool uploadPool_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkCommandBuffer uploadCmd_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    uint64_t ringValue_[2] = {0, 0};      // timeline value that frees each ring slice
    uint64_t iteration_ = 0;
    uint32_t skyIndex_ = 0;
    std::string skyName_;                 // guarded by mutex_
    std::shared_ptr<const std::vector<LineVertex>> selectionLines_;   // guarded by mutex_
    std::shared_ptr<const EntityOverlay> entityOverlay_;              // guarded by mutex_
    std::shared_ptr<const GlyphAtlas> glyphAtlas_;                    // guarded by mutex_
    const GlyphAtlas* atlasSeen_ = nullptr;                           // render thread: the atlas whose image is in the table
    uint32_t atlasIndex_ = 0;
    uint32_t atlasSerial_ = 0;
    bool workPending_ = false;            // guarded by mutex_: a snapshot or a decoded texture is waiting
    SceneInfo info_;                      // guarded by mutex_

    mutable std::mutex mutex_;
    std::condition_variable wake_;        // new work for the render thread
    std::condition_variable removed_;     // a viewport finished releasing
    std::map<ViewportId, std::shared_ptr<Viewport>> viewports_;
    ViewportId nextId_ = 1;
    bool stop_ = false;
    std::thread thread_;
};

} // namespace native::render
