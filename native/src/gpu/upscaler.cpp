// gpu/upscaler -- see upscaler.h.
#include "gpu/upscaler.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace native::gpu {

const char* upscalerName(UpscalerKind k) {
    switch (k) {
        case UpscalerKind::Dlss: return "DLSS";
        case UpscalerKind::Fsr: return "FSR";
        case UpscalerKind::None: break;
    }
    return "none";
}

const char* upscalerPrefName(UpscalerPref p) {
    switch (p) {
        case UpscalerPref::Dlss: return "dlss";
        case UpscalerPref::Fsr: return "fsr";
        case UpscalerPref::Off: return "off";
        case UpscalerPref::Auto: break;
    }
    return "auto";
}

UpscalerPref upscalerPrefFromName(const std::string& name, bool* ok) {
    std::string s;
    for (const char c : name) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ok) *ok = true;
    if (s == "auto") return UpscalerPref::Auto;
    if (s == "dlss") return UpscalerPref::Dlss;
    if (s == "fsr") return UpscalerPref::Fsr;
    if (s == "off" || s == "none") return UpscalerPref::Off;
    if (ok) *ok = false;
    return UpscalerPref::Auto;
}

UpscalerKind chooseUpscaler(UpscalerPref pref, bool dlss, bool fsr) {
    if (pref == UpscalerPref::Off) return UpscalerKind::None;
    if (pref == UpscalerPref::Fsr && fsr) return UpscalerKind::Fsr;
    if (dlss) return UpscalerKind::Dlss;
    if (fsr) return UpscalerKind::Fsr;
    return UpscalerKind::None;
}

double fsrRatio(int mode) {
    switch (mode) {
        case 1: return 1.5;   // Quality
        case 3: return 3.0;   // Ultra Performance
        case 4: return 2.0;   // Performance
        case 5: return 1.7;   // Balanced
        default: return 1.0;  // 2 = DLAA: native resolution, only the anti-aliasing
    }
}

void fsrRenderSize(int mode, uint32_t outW, uint32_t outH, uint32_t* renderW, uint32_t* renderH) {
    const float ratio = static_cast<float>(fsrRatio(mode));
    *renderW = std::max<uint32_t>(1, static_cast<uint32_t>(static_cast<float>(outW) / ratio));
    *renderH = std::max<uint32_t>(1, static_cast<uint32_t>(static_cast<float>(outH) / ratio));
}

namespace {

class StreamlineBackend final : public UpscaleBackend {
public:
    explicit StreamlineBackend(Streamline& sl) : sl_(sl) {}
    UpscalerKind kind() const override { return UpscalerKind::Dlss; }
    bool renderSize(int mode, uint32_t outW, uint32_t outH, uint32_t* rw, uint32_t* rh) override { return sl_.dlssRenderSize(mode, outW, outH, rw, rh); }
    bool evaluate(VkCommandBuffer cmd, const DlssFrame& frame, std::string* err) override { return sl_.dlssEvaluate(cmd, frame, err); }
    void release(uint32_t viewport) override { sl_.dlssRelease(viewport); }

private:
    Streamline& sl_;
};

} // namespace

std::unique_ptr<UpscaleBackend> makeStreamlineBackend(Streamline& sl) { return std::make_unique<StreamlineBackend>(sl); }

} // namespace native::gpu
