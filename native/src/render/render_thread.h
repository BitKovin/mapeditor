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
#include "gpu/upscaler.h"
#include "light/light_link.h"
#include "light/mailbox.h"
#include "light/page_images.h"
#include "render/gpu_scene.h"
#include "render/entity_overlay.h"
#include "render/dlss_pass.h"
#include "render/rt_scene.h"
#include "render/rt_lit.h"
#include "render/rt_lighting.h"
#include "render/glyph_atlas.h"
#include "render/line_vertex.h"
#include "render/mesh_pass.h"
#include "render/model_renderer.h"
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
    // Reads a texture again (its image and its sidecar: transparency) after it changed on disk; every viewport draws again once it is in. Any thread.
    void reloadTexture(const std::string& name);
    // Looks for textures whose files changed on disk (image or sidecar) and reads those again; the viewports draw again once they are in. Any thread.
    void refreshTextures();
    // The models of entities the viewports draw (render/model_scene); null = none. Marks every viewport dirty. Animated models keep the viewports redrawing.
    void setModels(std::shared_ptr<const ModelScene> models);
    // What the scene holds on the GPU and how the textures stand (refreshed every loop iteration).
    struct SceneInfo {
        SceneStats scene;
        uint32_t texturesAssigned = 0, texturesLoaded = 0, texturesFailed = 0, texturesPending = 0;
        uint32_t visibleObjects = 0;   // of the last frame drawn
    };
    SceneInfo sceneInfo() const;

    // Which temporal upscaler the viewports use: DLSS Super Resolution (Streamline) or AMD FSR (FidelityFX, any GPU). `Auto` (the default) takes DLSS where it can run and FSR otherwise;
    // a named one that cannot run gives way to the other; `Off` turns upscaling off. Viewports that ask for upscaling start again on the new backend. Any thread.
    void setUpscaler(gpu::UpscalerPref pref);
    gpu::UpscalerPref upscalerPref() const;
    // The backend the preference and the device give right now (None: neither can run).
    gpu::UpscalerKind upscalerKind() const;

    // What the upscaler path does in a viewport (Stats panel, tests): `active` = the last frame was upscaled; `failed` = the backend refused and the viewport draws normally.
    struct DlssInfo {
        bool active = false, failed = false;
        gpu::UpscalerKind backend = gpu::UpscalerKind::None;   // who upscaled the last frame
        std::string version;                   // the FSR provider ("FSR 3.1.6"); empty for DLSS
        int mode = 0;                          // DlssMode as int of the last frame's request
        uint32_t renderW = 0, renderH = 0, outW = 0, outH = 0;
        uint64_t resets = 0;                   // frames that told DLSS to drop its history
        uint64_t frames = 0;                   // frames upscaled so far
    };
    DlssInfo dlssInfo(ViewportId id) const;

    // The ray tracing scene (R1): off by default; while on, the render thread keeps one BLAS per drawn solid object and a TLAS in step with the scene. Needs a device
    // with ray queries (DeviceCaps::lit()); without one it stays off.
    void setRayTracing(bool on, const RtOptions& options = {});
    struct RtInfo {
        bool enabled = false, valid = false;
        uint64_t generation = 0;                    // +1 whenever `tlas` changed
        VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;   // only to be used while the render thread is idle (tests); null until the first object
        VkBuffer records = VK_NULL_HANDLE;
        RtScene::Stats stats;
    };
    RtInfo rtInfo() const;

    // The ray traced Lit viewport (R2, 12_RT_VIEWPORT.md): off by default. While on, a perspective viewport in Lit / Lighting only shows the per-pixel ray traced light (ambient
    // occlusion at this step) instead of the chunked bake's pages; it needs the ray tracing scene (switched on with it) and a device with ray queries. `lighting` = the level's
    // settings (render/rt_lighting.h), fed by the editor from every snapshot. Marks every viewport dirty.
    void setRtLit(bool on, const RtLitOptions& options = {});
    void setRtLighting(const RtLighting& lighting);
    // The clock of the light styles in seconds (tests): a fixed value freezes the animation; negative = the real clock (the default).
    void setStyleClock(double seconds);
    // The level lights (R3, render/rt_lights.h), fed by the editor from every snapshot (RtLightsProducer). Restarts the accumulation of every ray traced viewport; null = no lights.
    void setRtLights(std::shared_ptr<const RtLightData> lights);
    // Which objects receive baked light (shadows show on them) and which block light: render/rt_lights.h makeRtRules. Null = all objects do both.
    void setObjectRules(std::shared_ptr<const ObjectRules> rules);
    struct RtLitInfo {
        bool active = false;                   // the last frame of the viewport was drawn by the ray traced path
        bool reconstruction = false;           // ... with DLSS Ray Reconstruction
        uint32_t renderW = 0, renderH = 0;
        uint64_t frames = 0;                   // frames drawn by the ray traced path
        uint64_t resets = 0;                   // frames that restarted every history
        uint32_t stillFrames = 0;              // consecutive frames of a still camera (the converged stop is at RtLitOptions::targetSamples + 1)
        bool tracingSkipped = false;           // the last frame did not trace (converged)
        uint32_t tracedPixels = 0, surfacePixels = 0;   // the counters of the newest frame whose read-back came in (a frame or two behind)
        uint64_t counterFrame = 0;             // `frames` of that frame
    };
    RtLitInfo rtLitInfo(ViewportId id) const;

    // R8: GPU time per pass of this viewport's frames (timestamp queries, smoothed over the last frames; "raster", "rt setup", "rt cache", "rt trace", "rt filter", "rt compose", "upscale",
    // "display"; a frame that is not ray traced has only "display" = the whole frame). `frames` counts the frames folded in so far. Empty before the first frame came back.
    struct PassTime {
        std::string name;
        double ms = 0;
    };
    std::vector<PassTime> gpuPassTimes(ViewportId id, uint64_t* frames = nullptr) const;

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
                    gpu::UpscaleBackend& up);
    void releaseDlss(Viewport& v);
    // The backend that upscales now (see setUpscaler); null when none can run. Render thread.
    gpu::UpscaleBackend* upscaler();
    // The viewport's context in every backend goes (the GPU must be idle).
    void releaseUpscalers(uint32_t viewportId);
    // R2: the ray traced Lit frame of one viewport: G-buffer at the render size, the trace / accumulate / filter / compose passes, and (with DLSS Ray Reconstruction) the
    // upscale into DlssTargets::output. False when the frame has to be drawn the normal way (the ray tracing scene is not ready, a failure).
    bool recordRtLit(Viewport& v, VkCommandBuffer cmd, uint32_t slot, const ViewState& view, VkExtent2D extent, const DrawLists& lists, const float clear[3], gpu::Streamline* sl);
    bool syncRayTracing(VkCommandBuffer cmd);

    gpu::Device& device_;
    VkSemaphore renderTimeline_ = VK_NULL_HANDLE;
    std::atomic<uint64_t> nextTimelineValue_{0};
    std::atomic<uint64_t> totalFrames_{0};
    std::unique_ptr<gpu::UploadRing> ring_;
    std::unique_ptr<TextureTable> textures_;
    std::unique_ptr<GpuScene> scene_;
    std::chrono::steady_clock::time_point modelNext_{};   // the next frame of animated models
    std::unique_ptr<ModelRenderer> models_;   // render thread (setModels from any thread); after textures_, so it goes first
    std::unique_ptr<RtScene> rtScene_;    // render thread only; made while ray tracing is on (declared after scene_: it goes first)
    std::atomic<bool> rtEnabled_{false};
    std::atomic<bool> rtLitEnabled_{false};
    std::atomic<double> styleClock_{-1.0};
    double styleNow() const;
    RtLitOptions rtLitOptions_;           // guarded by mutex_
    std::atomic<ViewportId> activeViewport_{0};   // the viewport whose camera moved last (the ray budget serves it first)
    RtLighting rtLighting_;               // guarded by mutex_
    std::shared_ptr<const RtLightData> rtLightData_;   // guarded by mutex_
    uint64_t rtLightSerial_ = 1;          // guarded by mutex_; grows with every setRtLights
    std::unique_ptr<RtLightsGpu> rtLightsGpu_;         // render thread only
    uint64_t rtLightsUploaded_ = 0;       // the serial rtLightsGpu_ holds
    std::unique_ptr<RtLit> rtLit_;        // render thread only
    RtOptions rtOptions_;                 // guarded by mutex_; read when the RT scene is made
    RtInfo rtInfo_;                       // guarded by mutex_
    std::unique_ptr<ScenePass> pass_;     // render thread only
    std::unique_ptr<MeshPass> mesh_;      // render thread only
    std::unique_ptr<MeshPass> meshHdr_;   // the scene at the DLSS render size (RGBA16F), made on first use
    std::unique_ptr<DlssPass> dlssPass_;  // render thread only
    std::atomic<int> upscalerPref_{0};    // a gpu::UpscalerPref
    std::atomic<uint64_t> upscalerSerial_{1};   // grows with every setUpscaler: a viewport forgets that its last mode failed
    std::unique_ptr<gpu::UpscaleBackend> dlssBackend_;   // DLSS through Streamline, made on first use; render thread only
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
    std::vector<std::string> reloadNames_;   // guarded by mutex_: textures to read again
    bool refreshTextures_ = false;           // guarded by mutex_: look for changed texture files
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
