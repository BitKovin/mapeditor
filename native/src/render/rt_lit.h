// render/rt_lit -- the GPU side of the ray traced Lit viewport (R2): per-viewport history buffers and the three compute passes (trace + accumulate, a-trous filter,
// compose) that turn the rasterised G-buffer into the lit picture.
//
// Owns: RtLitTargets (per viewport: the G-buffer's normal pair and distance image, the history pair RGBA32F, the filter pair, the sample image, the converged-stop counters
// and their read-back, the descriptor sets of two frames in flight) and RtLit (the shared pipelines). The G-buffer itself is drawn by MeshPass (kMeshGBuffer) into
// DlssTargets::color (albedo) / RtLitTargets::normal / RtLitTargets::dist; the motion vectors come from DlssPass (the same depth -> mvec kernel).
// Signals: the ambient occlusion `o` (R2) and the direct light `D` (R3: sun, sky, local lights, rt_light.glsl), each a running mean per pixel with the shared sample count N;
// `L = (ambient + D) * aoFactor(o)`. Bounces join in R4 - R6 on the same buffers (12_RT_VIEWPORT.md §3, §7).
// Image layouts: the history pair stays GENERAL and the normal pair SHADER_READ_ONLY between frames; everything else is rewritten each frame (UNDEFINED at the start).
// Thread: the render thread. Spec: 12_RT_VIEWPORT.md §2, §3, §7, 08_MILESTONES.md R2.
#pragma once

#include <volk.h>

#include <vk_mem_alloc.h>

#include <cstdint>

#include "gpu/device.h"
#include "render/dlss_pass.h"
#include "render/math3d.h"
#include "render/rt_lighting.h"
#include "render/rt_lights.h"

namespace native::render {

struct RtLitOptions {
    uint32_t targetSamples = 1024;     // a pixel with this many samples under a still camera is not traced any more (the converged stop)
    uint32_t sampleCap = 4096;         // the running mean's N while the camera is still (a plain mean up to here)
    uint32_t sampleCapMoving = 32;     // N after reprojection while the camera moves (bounds the blur of resampling the history)
    uint32_t filterIterations = 4;     // a-trous iterations of the fallback denoiser
    bool reconstruction = true;        // use DLSS Ray Reconstruction when the viewport asks for DLSS (Quality / DLAA) and Streamline offers it
    bool filter = true;                // the fallback denoiser (it never runs while DLSS Ray Reconstruction does)
    float meanBlend = 32.0f;           // N at which the running mean has replaced the filtered value completely
    float meanBlendRr = 64.0f;         // the same for the input of DLSS Ray Reconstruction under a still camera
    float lightmapTexelsPerMeter = 16.0f;   // the lightmap look: shading at the centres of a planar texel grid of this density, bilinearly weighted (the game base density); 0 = smooth
    // Bounce light (R6): the SHaRC radiance cache of S = Db + I as the bake defines it. One cache per viewport; off without the device features (DeviceCaps::bounceCache).
    bool bounces = true;
    uint32_t giCacheLog2 = 21;         // entries of the cache: 2^n (80 MB at 21)
    float giSceneScale = 16.0f;        // voxel size = distance to the camera / this (SHaRC sceneScale)
    float giAlbedo = 0.7f;             // the bake's reflectance of every surface for bounces (LiveSettings::albedo)
    float giRange = 10000.0f;          // the gather range (the bake's window radius)
    uint32_t giPathLength = 4;         // vertices of one update path
    uint32_t giBlock = 5;              // the update traces one pixel of every giBlock x giBlock block per frame
    uint32_t giAccumFrames = 32;       // the cache's temporal window (frames)
    uint32_t giStaleFrames = 64;       // frames without a sample before an entry is dropped
};

struct RtLitTargets {
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };
    VkExtent2D render{};
    Image normal[2], dist, hist[2], fil[2], sample, radiance, specAlbedo;   // radiance, specAlbedo: only with reconstruction
    Image histD[2], filD[2], sampleD;  // the direct light: history (rgb mean, mean of luminance^2), filtered value (rgb, variance of the mean), this frame sample
    bool specAlbedoReady = false;      // specAlbedo was cleared to black and is in SHADER_READ_ONLY
    struct Cache {                     // the SHaRC buffers (R6)
        VkBuffer buffer[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};   // hash entries, accumulation, resolved
        VmaAllocation alloc[3] = {nullptr, nullptr, nullptr};
        VkDeviceAddress address[3] = {0, 0, 0};
        uint32_t capacity = 0;
        bool clear = true;             // must be zeroed before the next use
        bool havePrev = false;
        float camPrev[3] = {0, 0, 0};
        uint32_t frame = 0;
        bool valid() const { return capacity != 0; }
    } cache;
    VkBuffer counters = VK_NULL_HANDLE;
    VmaAllocation countersAlloc = nullptr;
    VkBuffer readback[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VmaAllocation readbackAlloc[2] = {nullptr, nullptr};
    uint32_t* readbackPtr[2] = {nullptr, nullptr};
    VkBuffer params[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VmaAllocation paramsAlloc[2] = {nullptr, nullptr};
    void* paramsPtr[2] = {nullptr, nullptr};
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet sets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE}, tlasSets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    // frame state
    bool initialized = false;          // layouts of the persistent images were set
    uint32_t normalCur = 0;            // the normal image this frame draws (the other one is the previous frame's)
    uint32_t latest = 0;               // the history image that holds the newest data
    bool readbackValid[2] = {false, false};
    bool valid() const { return dist.image != VK_NULL_HANDLE; }
};

// What one frame's compute passes need to know.
struct RtLitFrame {
    uint32_t slot = 0;                 // frame in flight: picks the descriptor sets and the read-back
    uint32_t frameNumber = 0;          // the random sequence's position
    bool reset = false;                // every history restarts
    bool still = false;                // the camera did not move
    bool skipTrace = false;            // converged: the history is shown as it is, nothing is traced
    bool reconstruction = false;       // the radiance goes to DLSS Ray Reconstruction (the compose output is its input, not the final picture)
    int mode = 1;                      // 1 = Lit, 2 = Lighting only
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    double eye[3] = {0, 0, 0}, forward[3] = {0, 0, -1};
    Mat4 jitteredViewProj = mat4Identity();      // the matrix the G-buffer was drawn with
    Mat4 prevViewProj = mat4Identity();          // the previous frame's unjittered matrix
    RtLighting lighting;
    RtLitOptions options;
    const RtLightsGpu* lights = nullptr;   // the level lights (required; an empty set is fine)
    VkBuffer records = VK_NULL_HANDLE;     // RtScene::recordBuffer (the objects a gather ray can hit)
    VkDescriptorSet textures = VK_NULL_HANDLE;   // the renderer's texture table (set 2 of the trace kernels)
};

class RtLit {
public:
    // `textureLayout`: the layout of the renderer's texture table (TextureTable::layout()), set 2 of the kernels that read the albedo of a gather hit.
    RtLit(const gpu::Device& device, VkDescriptorSetLayout textureLayout);
    ~RtLit();
    RtLit(const RtLit&) = delete;
    RtLit& operator=(const RtLit&) = delete;

    bool valid() const { return aoPipeline_ != VK_NULL_HANDLE && filterPipeline_ != VK_NULL_HANDLE && composePipeline_ != VK_NULL_HANDLE; }

    // The GPU must be idle (or the old targets unused). `reconstruction` also makes the radiance image DLSS Ray Reconstruction reads.
    bool createTargets(RtLitTargets& t, VkExtent2D render, bool reconstruction) const;
    void destroyTargets(RtLitTargets& t) const;

    // Before the G-buffer pass: the persistent images get their first layouts, the normal and distance images become attachments. The caller attaches
    // normalView() / distView() as colour attachments 1 and 2.
    void beginGBuffer(VkCommandBuffer cmd, RtLitTargets& t) const;
    static VkImageView normalView(const RtLitTargets& t) { return t.normal[t.normalCur].view; }
    static VkImageView distView(const RtLitTargets& t) { return t.dist.view; }
    // After the G-buffer pass (and DlssPass::endSceneAndMotion): the new normals and the distances are readable.
    void endGBuffer(VkCommandBuffer cmd, const RtLitTargets& t) const;

    // The trace + accumulate, filter and compose passes. `d.output` (GENERAL, DlssPass::beforeUpscale) receives the picture, or `t.radiance` when `f.reconstruction`.
    // Returns false when the descriptors could not be written.
    bool record(VkCommandBuffer cmd, RtLitTargets& t, const DlssTargets& d, const RtLitFrame& f) const;

    // The counters of the last frame that used `slot` (call before recording the slot again, once its frame finished): false until there is one.
    struct Counters {
        uint32_t traced = 0, surface = 0;
    };
    bool readCounters(RtLitTargets& t, uint32_t slot, Counters* out) const;

    // DLSS Ray Reconstruction: once per target set, before the first evaluate (outside a render pass): the black specular albedo guide.
    void prepareReconstruction(VkCommandBuffer cmd, RtLitTargets& t) const;

    // The images as Ray Reconstruction sees them at the evaluate call: radiance GENERAL (left by record), normal / specular albedo SHADER_READ_ONLY.
    static gpu::DlssImage radiance(const RtLitTargets& t);
    static gpu::DlssImage normalForUpscaler(const RtLitTargets& t);
    static gpu::DlssImage specularAlbedo(const RtLitTargets& t);

private:
    const gpu::Device& device_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE, tlasLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout aoLayout_ = VK_NULL_HANDLE, filterLayout_ = VK_NULL_HANDLE, resolveLayout_ = VK_NULL_HANDLE;
    VkPipeline aoPipeline_ = VK_NULL_HANDLE, filterPipeline_ = VK_NULL_HANDLE, composePipeline_ = VK_NULL_HANDLE, updatePipeline_ = VK_NULL_HANDLE, resolvePipeline_ = VK_NULL_HANDLE;
    bool ensureCache(RtLitTargets& t, const RtLitOptions& o) const;
    void destroyCache(RtLitTargets& t) const;
};

} // namespace native::render
