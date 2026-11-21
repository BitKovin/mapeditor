// gpu/streamline -- NVIDIA Streamline (SL) loaded at run time: the interposer, Reflex low latency and PCL markers.
//
// Owns: loading sl.interposer.dll (never linked, so a missing DLL or a missing SDK only means "no Streamline"), slInit /
// slShutdown, the Vulkan entry points the interposer provides (volk is pointed at them so SL sees instance, device, swapchain
// and present calls), the Reflex options, the per-frame sleep / marker calls, and (T6.8) DLSS Super Resolution: per viewport the options,
// the constants, the resource tags and the evaluate call recorded into the frame's command buffer.
// Thread: load / deviceCreated on the UI thread before the render thread exists; beginFrame / sleep / marker on the render
// thread only; status() from any thread.
// Spec: 11_NVIDIA_TECH.md §1, §2.1; 08_MILESTONES.md T6.7. The SDK itself lives in third_party/streamline (fetched by
// tools/fetch_streamline.ps1, git-ignored); without it this file compiles to a stub whose load() always fails with a reason.
#pragma once

#include <volk.h>
#undef near
#undef far

#include <cstdint>
#include <memory>
#include <string>

namespace native::gpu {

// The PCL markers the render thread sets (a subset of sl::PCLMarker, so callers need no SL header).
enum class PclMarker : uint32_t {
    SimulationStart,
    SimulationEnd,
    RenderSubmitStart,
    RenderSubmitEnd,
    PresentStart,
    PresentEnd,
};

// One image handed to DLSS (the SL header is not visible to callers).
struct DlssImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;           // the view the feature reads / writes (depth: depth aspect only)
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags usage = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;   // the layout the image has on the command buffer at the evaluate call
    uint32_t width = 0, height = 0;
};

// Everything one DLSS evaluate needs. Matrices are the column-major arrays of render/math3d.h copied unchanged (Streamline reads them as the
// row-major, row-vector form of the same matrix) and carry no jitter.
struct DlssFrame {
    uint32_t viewport = 0;                       // the viewport id: one DLSS instance and history per viewport
    int mode = 1;                                // 1 = Quality (MaxQuality), 2 = DLAA
    uint32_t renderW = 0, renderH = 0, outW = 0, outH = 0;
    float jitter[2] = {0, 0};                    // pixels of the render image, as applied to the projection
    float viewToClip[16] = {}, clipToView[16] = {}, clipToPrevClip[16] = {}, prevClipToClip[16] = {};
    float position[3] = {}, up[3] = {}, right[3] = {}, forward[3] = {};
    float nearPlane = 0.05f, farPlane = 2000.0f, fovY = 0.9f, aspect = 1.0f;
    bool reset = false;                          // the history is not valid for this frame (camera cut, resize, mode switch, big change)
    DlssImage color, depth, mvec, out;           // colour in (render size), depth (render size), motion vectors in pixels (render size), colour out (output size)
    // DLSS Ray Reconstruction (R2): `reconstruction` evaluates the denoiser + upscaler instead of Super Resolution. `color` is then the noisy radiance (render size), and
    // albedo (diffuse albedo), specAlbedo (black), normal (world normal, roughness 1 in w) are the guides; worldToView / viewToWorld are the camera's view matrices (same
    // column-major convention as the others).
    bool reconstruction = false;
    DlssImage albedo, specAlbedo, normal;
    float worldToView[16] = {}, viewToWorld[16] = {};
};

struct StreamlineStatus {
    bool loaded = false;               // slInit succeeded
    bool reflexSupported = false;      // slIsFeatureSupported(Reflex) on the chosen adapter
    bool reflexActive = false;         // low-latency mode is on and the driver reports it available
    bool pclSupported = false;
    bool dlssSupported = false;        // the DLSS Super Resolution plugin is usable on this adapter (RTX GPU, nvngx_dlss.dll present)
    bool rrSupported = false;          // the DLSS Ray Reconstruction plugin is usable on this adapter (sl.dlss_d.dll, nvngx_dlssd.dll present)
    std::string version;               // "2.14.16"
    std::string note;                  // why something is off ("sl.interposer.dll not found", ...); empty when all is well
    double latencyMs = 0;              // average of the last frame reports: simulation start -> GPU render end
    uint64_t frames = 0;               // frames that went through beginFrame()
};

class Streamline {
public:
    // Loads <dir>/sl.interposer.dll (dir empty: the folder of the exe) and calls slInit. Returns null with *why set when the SDK
    // was not built in, the DLL is missing, or slInit fails. Must run before any Vulkan call of the process.
    static std::unique_ptr<Streamline> load(const std::string& dir, std::string* why);
    ~Streamline();   // slShutdown (if shutdown() was not called); the DLL stays loaded until the process ends
    Streamline(const Streamline&) = delete;
    Streamline& operator=(const Streamline&) = delete;

    // The interposer's vkGetInstanceProcAddr: give it to volkInitializeCustom instead of the system loader's.
    PFN_vkGetInstanceProcAddr instanceProcAddr() const;

    // After the device exists: checks which features this adapter supports and turns Reflex on. UI thread, before any render thread.
    void deviceCreated(VkInstance instance, VkPhysicalDevice physical, VkDevice device);
    // Before vkDestroyDevice (SL asks to be shut down while the device still exists).
    void shutdown();

    // Render thread. beginFrame takes a new frame token; sleep is Reflex's pacing point; markers belong to the current frame.
    void beginFrame();
    void sleep();
    void marker(PclMarker m);

    // DLSS Super Resolution (render thread only; SL's DLSS calls are not thread safe). mode 1 = Quality, 2 = DLAA.
    bool dlssAvailable() const;
    // The render size DLSS wants for an output size and mode; false when DLSS is not available.
    bool dlssRenderSize(int mode, uint32_t outW, uint32_t outH, uint32_t* renderW, uint32_t* renderH, bool reconstruction = false);
    // DLSS Ray Reconstruction (render thread only): the same calls with DlssFrame::reconstruction. mode 1 = Quality, 2 = DLAA.
    bool rrAvailable() const;
    // Sets the options (when they changed), the constants and the tags of the viewport and records the upscale into `cmd` (outside a render pass).
    // The command buffer's bound state is undefined afterwards. False with *err set when SL refuses (the output image then has no valid content).
    bool dlssEvaluate(VkCommandBuffer cmd, const DlssFrame& frame, std::string* err);
    // The viewport's DLSS resources (Super Resolution and Ray Reconstruction) go (the GPU must be idle).
    void dlssRelease(uint32_t viewport);

    StreamlineStatus status() const;

private:
    Streamline();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace native::gpu
