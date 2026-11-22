// gpu/vendor -- see vendor.h.
#include "gpu/vendor.h"

#include <algorithm>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// base/win_clean.h (forced into every file) removed windows.h's empty `near` / `far` macros, which the COM headers behind dxgi.h still write (`FAR *`): give them back for that include
#define near
#define far
#include <dxgi.h>
#undef near
#undef far

namespace native::gpu {

Vendor vendorFromId(uint32_t id) {
    switch (id) {
        case 0: return Vendor::Unknown;
        case 0x10DE: return Vendor::Nvidia;
        case 0x1002:
        case 0x1022: return Vendor::Amd;
        case 0x8086: return Vendor::Intel;
        default: return Vendor::Other;
    }
}

const char* vendorName(Vendor v) {
    switch (v) {
        case Vendor::Nvidia: return "NVIDIA";
        case Vendor::Amd: return "AMD";
        case Vendor::Intel: return "Intel";
        case Vendor::Other: return "other";
        case Vendor::Unknown: break;
    }
    return "unknown";
}

std::vector<AdapterInfo> listAdapters() {
    std::vector<AdapterInfo> out;
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");   // loaded by name, not linked: a machine without it simply reports nothing
    if (!dxgi) return out;
    using CreateFactory1 = HRESULT(WINAPI*)(REFIID, void**);
    const auto create = reinterpret_cast<CreateFactory1>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
    IDXGIFactory1* factory = nullptr;
    if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory) {
        for (UINT i = 0;; ++i) {
            IDXGIAdapter1* adapter = nullptr;
            if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND || !adapter) break;
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc))) {
                AdapterInfo a;
                a.vendorId = desc.VendorId;
                const int n = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
                if (n > 1) {
                    a.name.resize(static_cast<size_t>(n - 1));
                    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, a.name.data(), n, nullptr, nullptr);
                }
                a.software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || desc.VendorId == 0x1414;   // 0x1414: Microsoft (Basic Render Driver)
                out.push_back(std::move(a));
            }
            adapter->Release();
        }
        factory->Release();
    }
    std::stable_sort(out.begin(), out.end(), [](const AdapterInfo& a, const AdapterInfo& b) { return !a.software && b.software; });
    return out;   // dxgi.dll stays loaded: the process uses it for the window system anyway
}

bool hasAdapterFrom(const std::vector<AdapterInfo>& adapters, Vendor v) {
    for (const AdapterInfo& a : adapters)
        if (!a.software && vendorFromId(a.vendorId) == v) return true;
    return false;
}

bool hasAdapterFrom(Vendor v) { return hasAdapterFrom(listAdapters(), v); }

} // namespace native::gpu
