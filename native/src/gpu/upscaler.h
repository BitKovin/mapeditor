// gpu/upscaler -- the seam between the render thread and the temporal upscalers: NVIDIA DLSS Super Resolution (through Streamline, gpu/streamline.h) and AMD FidelityFX Super
// Resolution (gpu/fidelityfx.h, runs on any Vulkan GPU). Both take the same frame (`DlssFrame`: jittered colour, depth and motion vectors at the render size, the camera, a reset flag)
// and write the output size image, so the render thread draws the scene the same way for both and only asks `UpscaleBackend` for the render size and the upscale.
//
// The preset numbers are the DlssMode values of render/viewport_settings.h (1 Quality, 2 DLAA = native resolution, 3 Ultra Performance, 4 Performance, 5 Balanced); the FSR ratios
// below are AMD's per-dimension upscale ratios for the same names (Quality 1.5, Balanced 1.7, Performance 2.0, Ultra Performance 3.0, native 1.0).
// Owns: nothing (the backends own their contexts). Thread: the render thread, except the pure functions (any). Spec: 11_NVIDIA_TECH.md section 5, 08_MILESTONES.md T15.1.
#pragma once

#include <volk.h>
#undef near
#undef far

#include <cstdint>
#include <memory>
#include <string>

#include "gpu/streamline.h"

namespace native::gpu {

enum class UpscalerKind { None, Dlss, Fsr };
// What the person (or --upscaler) asked for.
enum class UpscalerPref { Auto, Dlss, Fsr, Off };

const char* upscalerName(UpscalerKind k);       // "none", "DLSS", "FSR"
const char* upscalerPrefName(UpscalerPref p);   // "auto", "dlss", "fsr", "off"
// "auto" / "dlss" / "fsr" / "off" (any case); *ok false and Auto for anything else.
UpscalerPref upscalerPrefFromName(const std::string& name, bool* ok = nullptr);
// Auto: DLSS when it can run, else FSR, else none. A named backend that cannot run gives way to the other one; Off is none.
UpscalerKind chooseUpscaler(UpscalerPref pref, bool dlssAvailable, bool fsrAvailable);

// FSR: the per-dimension upscale ratio of a preset (1.0 for DLAA / native), the render size for an output size (AMD truncates: floor(out / ratio), at least 1), and the number of
// jitter phases AMD asks for (8 * ratio^2 rounded, at least 8; the same rule as NVIDIA's).
double fsrRatio(int mode);
void fsrRenderSize(int mode, uint32_t outW, uint32_t outH, uint32_t* renderW, uint32_t* renderH);

class UpscaleBackend {
public:
    virtual ~UpscaleBackend() = default;
    virtual UpscalerKind kind() const = 0;
    // The render size the backend wants for an output size and preset; false when the preset is not offered.
    virtual bool renderSize(int mode, uint32_t outW, uint32_t outH, uint32_t* renderW, uint32_t* renderH) = 0;
    // Records the upscale of `frame` into `cmd` (outside a render pass; the command buffer's bound state is undefined afterwards). False with *err set when the backend refuses;
    // the output image then has no valid content.
    virtual bool evaluate(VkCommandBuffer cmd, const DlssFrame& frame, std::string* err) = 0;
    // The viewport's context and history go (the GPU must be idle).
    virtual void release(uint32_t viewport) = 0;
};

// DLSS Super Resolution through Streamline as an UpscaleBackend (`sl` outlives it).
std::unique_ptr<UpscaleBackend> makeStreamlineBackend(Streamline& sl);

} // namespace native::gpu
