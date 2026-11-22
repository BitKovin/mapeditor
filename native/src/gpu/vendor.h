// gpu/vendor -- who made the graphics adapter: PCI vendor ids, display names, and the list of the adapters Windows knows about (DXGI), available BEFORE any Vulkan call.
//
// Why before Vulkan: the NVIDIA Streamline interposer (DLSS, Reflex) has to take over the Vulkan loader before the instance exists, so the program has to know whether an NVIDIA
// adapter is present at all; on an AMD or Intel only machine it is left out and the vendor neutral upscaler (AMD FidelityFX Super Resolution, gpu/fidelityfx.h) is used.
// Owns: nothing. Thread: any. Spec: 11_NVIDIA_TECH.md section 5 (other vendors), 08_MILESTONES.md T15.1.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace native::gpu {

enum class Vendor { Unknown, Nvidia, Amd, Intel, Other };

// The PCI vendor id of a Vulkan / DXGI adapter: 0x10DE NVIDIA, 0x1002 (and 0x1022) AMD, 0x8086 Intel; anything else Other, 0 Unknown.
Vendor vendorFromId(uint32_t pciVendorId);
// "NVIDIA", "AMD", "Intel", "other", "unknown".
const char* vendorName(Vendor v);

struct AdapterInfo {
    uint32_t vendorId = 0;
    std::string name;
    bool software = false;   // the Microsoft Basic Render Driver (WARP) and the like
};

// The display adapters Windows reports, software adapters last. Empty when DXGI cannot be loaded (the caller then assumes nothing).
std::vector<AdapterInfo> listAdapters();

// True when one of `adapters` (default: the adapters of this machine) is from `v`. Software adapters do not count.
bool hasAdapterFrom(Vendor v);
bool hasAdapterFrom(const std::vector<AdapterInfo>& adapters, Vendor v);

} // namespace native::gpu
