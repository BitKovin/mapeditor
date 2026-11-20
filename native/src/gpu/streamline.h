// gpu/streamline -- NVIDIA Streamline (SL) loaded at run time: the interposer, Reflex low latency and PCL markers.
//
// Owns: loading sl.interposer.dll (never linked, so a missing DLL or a missing SDK only means "no Streamline"), slInit /
// slShutdown, the Vulkan entry points the interposer provides (volk is pointed at them so SL sees instance, device, swapchain
// and present calls), the Reflex options and the per-frame sleep / marker calls. T6.8 adds DLSS through the same object.
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

struct StreamlineStatus {
    bool loaded = false;               // slInit succeeded
    bool reflexSupported = false;      // slIsFeatureSupported(Reflex) on the chosen adapter
    bool reflexActive = false;         // low-latency mode is on and the driver reports it available
    bool pclSupported = false;
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

    StreamlineStatus status() const;

private:
    Streamline();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace native::gpu
