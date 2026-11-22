// gpu/fidelityfx -- AMD FidelityFX Super Resolution (FSR) as the vendor neutral upscaler: it runs on any Vulkan GPU (AMD, Intel, NVIDIA), so an editor on a Radeon has the same
// "render smaller, upscale" option NVIDIA owners get from DLSS. It is the second UpscaleBackend (gpu/upscaler.h) next to DLSS through Streamline.
//
// How: AMD's FidelityFX API (ffx_api.h) is a small C interface in `amd_fidelityfx_loader_vk.dll`; the effects live in `amd_fidelityfx_upscaler_vk.dll` next to it. Both are loaded at
// run time (never linked), so a missing SDK or DLL only means the upscaler is off (`FidelityFx::load` returns null with the reason). One context per viewport (the history lives in it),
// made for the render and output size and made again when either changes. The DLL picks the best provider for the GPU itself (FSR 4 on the AMD GPUs that run it, FSR 3.1 on the
// rest); the version in use is logged and shown in the Stats panel.
// Inputs are the ones DLSS takes (`DlssFrame`): jittered HDR colour, depth, motion vectors in pixels (current -> previous), the jitter, the camera, a reset flag. The images must have
// SAMPLED usage (colour, depth, motion vectors) and STORAGE usage (the output) and are moved to the layouts the API wants (and left there) by `evaluate`.
// Owns: the DLL handles, one FidelityFX context per viewport. Thread: the render thread only. Spec: 11_NVIDIA_TECH.md section 5, 08_MILESTONES.md T15.1.
#pragma once

#include <volk.h>
#undef near
#undef far

#include <memory>
#include <string>

#include "gpu/upscaler.h"

namespace native::gpu {

struct FidelityFxStatus {
    bool loaded = false;      // the loader DLL and its functions are there
    bool supported = false;   // a provider exists for this device (the version query returned at least one)
    std::string version;      // "FSR 3.1.6" style: the newest provider the DLL offers for this device
    std::string note;         // why it is off ("amd_fidelityfx_loader_vk.dll not found", ...); empty when all is well
};

class FidelityFx final : public UpscaleBackend {
public:
    // Loads <dir>/amd_fidelityfx_loader_vk.dll (dir empty: the folder of the exe) and asks which upscaler versions it has for the device. Null with *why set when the SDK was not
    // built in, the DLL is missing, or no version supports the device. After vkCreateDevice.
    static std::unique_ptr<FidelityFx> load(const std::string& dir, VkDevice device, VkPhysicalDevice physical, std::string* why);
    ~FidelityFx() override;
    FidelityFx(const FidelityFx&) = delete;
    FidelityFx& operator=(const FidelityFx&) = delete;

    UpscalerKind kind() const override { return UpscalerKind::Fsr; }
    bool renderSize(int mode, uint32_t outW, uint32_t outH, uint32_t* renderW, uint32_t* renderH) override;
    bool evaluate(VkCommandBuffer cmd, const DlssFrame& frame, std::string* err) override;
    void release(uint32_t viewport) override;

    FidelityFxStatus status() const;
    // The provider version a viewport's context runs ("" before its first evaluate).
    std::string versionOf(uint32_t viewport) const;

private:
    FidelityFx();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace native::gpu
