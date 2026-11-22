// gpu/fidelityfx -- see fidelityfx.h.
#include "gpu/fidelityfx.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <vector>

#include "base/log.h"

#if defined(NATIVE_HAS_FIDELITYFX)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#undef near
#undef far

#include <ffx_api.h>
#include <ffx_api_types.h>
#include <vk/ffx_api_vk.h>
#include <ffx_upscale.h>
#endif

namespace native::gpu {

#if defined(NATIVE_HAS_FIDELITYFX)

namespace {

// The DLL names its versions "3.1.6" / "4.1.2"; the editor shows "FSR 3.1.6".
std::string fsrName(const char* name) {
    const std::string n = name ? name : "?";
    return n.rfind("FSR", 0) == 0 ? n : "FSR " + n;
}


std::wstring exeDirectory() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    std::wstring p(buf, n);
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : p.substr(0, slash);
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// The effect's own messages (errors and warnings of the runtime and of the effect); called from the thread that calls into the API.
void ffxMessage(uint32_t type, const wchar_t* message) {
    const std::string text = "fidelityfx: " + narrow(message);
    if (type == FFX_API_MESSAGE_TYPE_ERROR) base::logError(text);
    else base::logWarning(text);
}

const char* codeName(ffxReturnCode_t rc) {
    switch (rc) {
        case FFX_API_RETURN_OK: return "ok";
        case FFX_API_RETURN_ERROR: return "error";
        case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE: return "unknown descriptor type";
        case FFX_API_RETURN_ERROR_RUNTIME_ERROR: return "runtime error (Vulkan or the effect)";
        case FFX_API_RETURN_NO_PROVIDER: return "no provider";
        case FFX_API_RETURN_ERROR_MEMORY: return "out of memory";
        case FFX_API_RETURN_ERROR_PARAMETER: return "invalid parameter";
        case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE: return "the effect DLL is older than this program";
        default: return "unknown code";
    }
}

std::string failure(const char* what, ffxReturnCode_t rc) { return std::string(what) + " failed: " + codeName(rc) + " (" + std::to_string(rc) + ")"; }

struct Provider {
    uint64_t id = 0;
    std::string name;
};

// One barrier for one whole image (the upscaler's inputs: the layouts DlssPass left them in are not the ones the API reads them in).
void transition(VkCommandBuffer cmd, const DlssImage& img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to) {
    if (from == to) return;
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

FfxApiResource resourceOf(const DlssImage& img, bool depth, bool uav, uint32_t state) {
    FfxApiResource r{};
    r.resource = reinterpret_cast<void*>(img.image);
    r.state = state;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = ffxApiGetSurfaceFormatVK(img.format);
    r.description.width = img.width;
    r.description.height = img.height;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    r.description.usage = FFX_API_RESOURCE_USAGE_READ_ONLY;
    if (depth) r.description.usage |= FFX_API_RESOURCE_USAGE_DEPTHTARGET;
    if (uav) r.description.usage |= FFX_API_RESOURCE_USAGE_UAV;
    return r;
}

} // namespace

struct FidelityFx::Impl {
    HMODULE module = nullptr;
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    VkDeviceContext deviceContext{};
    std::vector<Provider> providers;   // newest first, as the DLL lists them

    struct Ctx {
        ffxContext context = nullptr;
        uint32_t renderW = 0, renderH = 0, outW = 0, outH = 0;
        std::string version;
        std::chrono::steady_clock::time_point last{};
        bool haveLast = false;
    };
    mutable std::mutex mutex;   // guards `viewports` for versionOf (the UI thread); the render thread is the only writer
    std::map<uint32_t, Ctx> viewports;

    FidelityFxStatus status;

    ~Impl() {
        for (auto& kv : viewports)
            if (kv.second.context) destroy(&kv.second.context, nullptr);
        viewports.clear();
        if (module) FreeLibrary(module);
    }

    // The providers the DLL has for the upscaler on this device, newest first.
    bool queryProviders(std::string* why) {
        uint64_t count = 0;
        ffxQueryDescGetVersions q{};
        q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        q.device = &deviceContext;
        q.outputCount = &count;
        ffxReturnCode_t rc = query(nullptr, &q.header);
        if (rc != FFX_API_RETURN_OK) {
            *why = failure("the version query", rc);
            return false;
        }
        if (count == 0) {
            *why = "no FidelityFX upscaler version supports this GPU";
            return false;
        }
        std::vector<uint64_t> ids(count);
        std::vector<const char*> names(count, nullptr);
        q.outputCount = &count;
        q.versionIds = ids.data();
        q.versionNames = names.data();
        rc = query(nullptr, &q.header);
        if (rc != FFX_API_RETURN_OK) {
            *why = failure("the version list", rc);
            return false;
        }
        for (uint64_t i = 0; i < count; ++i) providers.push_back({ids[i], fsrName(names[i])});
        return true;
    }

    // A context for the sizes, with the default provider first and every other one after it when that fails.
    bool makeContext(Ctx& c, uint32_t viewport, uint32_t renderW, uint32_t renderH, uint32_t outW, uint32_t outH, std::string* err) {
        ffxCreateBackendVKDesc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        backend.deviceContext = deviceContext;
        ffxCreateContextDescUpscaleVersion apiVersion{};
        apiVersion.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
        apiVersion.version = FFX_UPSCALER_VERSION;
        apiVersion.header.pNext = &backend.header;
        ffxCreateContextDescUpscale desc{};
        desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        desc.header.pNext = &apiVersion.header;
        // HDR colour in, the exposure from the picture itself (the editor has no tone mapper in front of the upscaler), depth 0 near .. 1 far, motion vectors at the render size
        // with the jitter already out of them (the shader that makes them uses the unjittered previous matrix).
        desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
        desc.maxRenderSize = {renderW, renderH};
        desc.maxUpscaleSize = {outW, outH};
        desc.fpMessage = &ffxMessage;

        ffxReturnCode_t rc = create(&c.context, &desc.header, nullptr);
        size_t next = 0;
        ffxOverrideVersion ov{};
        while (rc != FFX_API_RETURN_OK && next < providers.size()) {   // the default provider failed: try the others, newest first
            base::logWarning("fidelityfx: viewport " + std::to_string(viewport) + ": the default provider " + failure("context", rc) + "; trying " + providers[next].name);
            ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
            ov.versionId = providers[next].id;
            ov.header.pNext = desc.header.pNext;
            desc.header.pNext = &ov.header;
            c.context = nullptr;
            rc = create(&c.context, &desc.header, nullptr);
            desc.header.pNext = ov.header.pNext;
            ++next;
        }
        if (rc != FFX_API_RETURN_OK) {
            *err = failure("ffxCreateContext", rc);
            c.context = nullptr;
            return false;
        }
        ffxQueryGetProviderVersion pv{};
        pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (query(&c.context, &pv.header) == FFX_API_RETURN_OK && pv.versionName) c.version = fsrName(pv.versionName);
        c.renderW = renderW;
        c.renderH = renderH;
        c.outW = outW;
        c.outH = outH;
        return true;
    }
};

std::unique_ptr<FidelityFx> FidelityFx::load(const std::string& dir, VkDevice device, VkPhysicalDevice physical, std::string* why) {
    std::string sink;
    if (!why) why = &sink;
    std::unique_ptr<FidelityFx> fx(new FidelityFx());
    fx->impl_ = std::make_unique<Impl>();
    Impl& m = *fx->impl_;
    const std::wstring folder = dir.empty() ? exeDirectory() : widen(dir);
    const std::wstring path = folder + L"\\amd_fidelityfx_loader_vk.dll";
    m.module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);   // the effect DLL is looked up next to the loader
    if (!m.module) {
        *why = "amd_fidelityfx_loader_vk.dll not found next to the program";
        return nullptr;
    }
    PfnFfxCreateContext fnCreateContext = reinterpret_cast<PfnFfxCreateContext>(reinterpret_cast<void*>(GetProcAddress(m.module, "ffxCreateContext")));
    PfnFfxDestroyContext fnDestroyContext = reinterpret_cast<PfnFfxDestroyContext>(reinterpret_cast<void*>(GetProcAddress(m.module, "ffxDestroyContext")));
    PfnFfxQuery fnQuery = reinterpret_cast<PfnFfxQuery>(reinterpret_cast<void*>(GetProcAddress(m.module, "ffxQuery")));
    PfnFfxDispatch fnDispatch = reinterpret_cast<PfnFfxDispatch>(reinterpret_cast<void*>(GetProcAddress(m.module, "ffxDispatch")));
    if (!fnCreateContext || !fnDestroyContext || !fnQuery || !fnDispatch) {
        *why = "amd_fidelityfx_loader_vk.dll does not export the FidelityFX API";
        return nullptr;
    }
    m.create = fnCreateContext;
    m.destroy = fnDestroyContext;
    m.query = fnQuery;
    m.dispatch = fnDispatch;
    m.deviceContext.vkDevice = device;
    m.deviceContext.vkPhysicalDevice = physical;
    m.deviceContext.vkDeviceProcAddr = vkGetDeviceProcAddr;
    m.status.loaded = true;
    if (!m.queryProviders(why)) {
        m.status.note = *why;
        return nullptr;
    }
    m.status.supported = true;
    m.status.version = m.providers.front().name;
    std::string all;
    for (const Provider& p : m.providers) all += (all.empty() ? "" : ", ") + p.name;
    base::logInfo("fidelityfx: upscaler versions for this GPU: " + all);
    return fx;
}

FidelityFx::FidelityFx() = default;
FidelityFx::~FidelityFx() = default;

FidelityFxStatus FidelityFx::status() const { return impl_->status; }

std::string FidelityFx::versionOf(uint32_t viewport) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->viewports.find(viewport);
    return it == impl_->viewports.end() ? std::string() : it->second.version;
}

bool FidelityFx::renderSize(int mode, uint32_t outW, uint32_t outH, uint32_t* renderW, uint32_t* renderH) {
    if (mode < 1 || mode > 5 || outW == 0 || outH == 0) return false;
    fsrRenderSize(mode, outW, outH, renderW, renderH);   // AMD's own query returns the same sizes; this needs no context
    return true;
}

void FidelityFx::release(uint32_t viewport) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->viewports.find(viewport);
    if (it == impl_->viewports.end()) return;
    if (it->second.context) impl_->destroy(&it->second.context, nullptr);
    impl_->viewports.erase(it);
}

bool FidelityFx::evaluate(VkCommandBuffer cmd, const DlssFrame& f, std::string* err) {
    std::string sink;
    if (!err) err = &sink;
    Impl& m = *impl_;
    if (f.renderW == 0 || f.renderH == 0 || f.outW == 0 || f.outH == 0 || !f.color.image || !f.depth.image || !f.mvec.image || !f.out.image) {
        *err = "the upscaler was given an empty frame";
        return false;
    }
    Impl::Ctx* c = nullptr;
    {
        std::lock_guard<std::mutex> lock(m.mutex);
        c = &m.viewports[f.viewport];
    }
    if (c->context && (c->renderW != f.renderW || c->renderH != f.renderH || c->outW != f.outW || c->outH != f.outH)) {
        m.destroy(&c->context, nullptr);   // the sizes changed: the history is gone anyway (the render thread waited for the GPU to be idle)
        c->context = nullptr;
    }
    if (!c->context) {
        Impl::Ctx fresh;
        if (!m.makeContext(fresh, f.viewport, f.renderW, f.renderH, f.outW, f.outH, err)) return false;
        base::logInfo("fidelityfx: viewport " + std::to_string(f.viewport) + ": " + fresh.version + " " + std::to_string(f.renderW) + "x" + std::to_string(f.renderH) + " -> " +
                      std::to_string(f.outW) + "x" + std::to_string(f.outH));
        std::lock_guard<std::mutex> lock(m.mutex);
        *c = fresh;
    }

    // The inputs are read in SHADER_READ_ONLY (the depth too), the output is written in GENERAL; the barriers go before the dispatch.
    constexpr VkImageLayout kRead = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    transition(cmd, f.color, VK_IMAGE_ASPECT_COLOR_BIT, f.color.layout, kRead);
    transition(cmd, f.depth, VK_IMAGE_ASPECT_DEPTH_BIT, f.depth.layout, kRead);
    transition(cmd, f.mvec, VK_IMAGE_ASPECT_COLOR_BIT, f.mvec.layout, kRead);
    transition(cmd, f.out, VK_IMAGE_ASPECT_COLOR_BIT, f.out.layout, VK_IMAGE_LAYOUT_GENERAL);

    const auto now = std::chrono::steady_clock::now();
    float deltaMs = 1000.0f / 60.0f;
    if (c->haveLast) deltaMs = std::clamp(std::chrono::duration<float, std::milli>(now - c->last).count(), 0.1f, 250.0f);
    c->last = now;
    c->haveLast = true;

    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cmd;
    d.color = resourceOf(f.color, false, false, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.depth = resourceOf(f.depth, true, false, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors = resourceOf(f.mvec, false, false, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.output = resourceOf(f.out, false, true, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = {f.jitter[0], f.jitter[1]};   // pixels of the render image, +x right, +y down: the scene is drawn shifted by the same amount
    d.motionVectorScale = {1.0f, 1.0f};            // the motion vectors are already in pixels
    d.renderSize = {f.renderW, f.renderH};
    d.upscaleSize = {f.outW, f.outH};
    d.enableSharpening = false;
    d.sharpness = 0.0f;
    d.frameTimeDelta = deltaMs;
    d.preExposure = 1.0f;
    d.reset = f.reset;
    d.cameraNear = f.nearPlane;
    d.cameraFar = f.farPlane;
    d.cameraFovAngleVertical = f.fovY;
    d.viewSpaceToMetersFactor = 1.0f;   // the world is in metres
    d.flags = 0;
    const ffxReturnCode_t rc = m.dispatch(&c->context, &d.header);
    if (rc != FFX_API_RETURN_OK) {
        *err = failure("ffxDispatch", rc);
        return false;
    }
    // The caller's passes after the upscale (and its barriers on the next frame) expect the layouts it described, so the inputs go back to them.
    transition(cmd, f.color, VK_IMAGE_ASPECT_COLOR_BIT, kRead, f.color.layout);
    transition(cmd, f.depth, VK_IMAGE_ASPECT_DEPTH_BIT, kRead, f.depth.layout);
    transition(cmd, f.mvec, VK_IMAGE_ASPECT_COLOR_BIT, kRead, f.mvec.layout);
    return true;
}

#else   // ---- built without the FidelityFX SDK: the upscaler is simply not there ----

struct FidelityFx::Impl {};

std::unique_ptr<FidelityFx> FidelityFx::load(const std::string&, VkDevice, VkPhysicalDevice, std::string* why) {
    if (why) *why = "this build has no AMD FidelityFX SDK (run native/tools/fetch_fidelityfx.ps1)";
    return nullptr;
}
FidelityFx::FidelityFx() = default;
FidelityFx::~FidelityFx() = default;
FidelityFxStatus FidelityFx::status() const { return {}; }
std::string FidelityFx::versionOf(uint32_t) const { return {}; }
bool FidelityFx::renderSize(int, uint32_t, uint32_t, uint32_t*, uint32_t*) { return false; }
bool FidelityFx::evaluate(VkCommandBuffer, const DlssFrame&, std::string* err) {
    if (err) *err = "no FidelityFX SDK in this build";
    return false;
}
void FidelityFx::release(uint32_t) {}

#endif

} // namespace native::gpu
