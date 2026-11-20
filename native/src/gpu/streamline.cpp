// gpu/streamline -- see streamline.h.
#include "gpu/streamline.h"

#include <atomic>
#include <cstring>
#include <string>

#include "base/log.h"

#if defined(NATIVE_HAS_STREAMLINE)
#include <windows.h>
#undef near
#undef far

#include <sl.h>
#include <sl_consts.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#endif

namespace native::gpu {

#if defined(NATIVE_HAS_STREAMLINE)

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring exeDirectory() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    std::wstring p(buf, n);
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : p.substr(0, slash);
}

// SL calls this from any thread, also from its own plugins.
void slLogCallback(sl::LogType type, const char* msg) {
    const std::string text = std::string("streamline: ") + (msg ? msg : "");
    switch (type) {
        case sl::LogType::eError: base::logError(text); break;
        case sl::LogType::eWarn: base::logWarning(text); break;
        default: base::logInfo(text); break;
    }
}

} // namespace

struct Streamline::Impl {
    HMODULE module = nullptr;
    PFun_slInit* init = nullptr;
    PFun_slShutdown* shutdown = nullptr;
    PFun_slIsFeatureSupported* isSupported = nullptr;
    PFun_slGetFeatureFunction* getFunction = nullptr;
    PFun_slGetNewFrameToken* newFrame = nullptr;
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;

    PFun_slReflexSetOptions* reflexSetOptions = nullptr;
    PFun_slReflexGetState* reflexGetState = nullptr;
    PFun_slReflexSleep* reflexSleep = nullptr;
    PFun_slPCLSetMarker* pclSetMarker = nullptr;

    sl::FrameToken* token = nullptr;
    bool running = false;          // slInit done, slShutdown not yet
    sl::ReflexState state{};       // 64 reports: kept here, not on the stack

    std::atomic<bool> loaded{false}, reflexSupported{false}, reflexActive{false}, pclSupported{false};
    std::atomic<uint32_t> latencyUs{0};
    std::atomic<uint64_t> frames{0};
    std::string note;              // written before the render thread starts
};

std::unique_ptr<Streamline> Streamline::load(const std::string& dir, std::string* why) {
    std::unique_ptr<Streamline> s(new Streamline());
    s->impl_ = std::make_unique<Impl>();
    Impl& m = *s->impl_;

    const std::wstring folder = dir.empty() ? exeDirectory() : widen(dir);
    const std::wstring dll = folder + L"\\sl.interposer.dll";
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (why) *why = "sl.interposer.dll not found next to the program (run native/tools/fetch_streamline.ps1)";
        return nullptr;
    }
    m.module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m.module) {
        if (why) *why = "sl.interposer.dll cannot be loaded (error " + std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    auto sym = [&](const char* name) { return reinterpret_cast<void*>(GetProcAddress(m.module, name)); };
    m.init = reinterpret_cast<PFun_slInit*>(sym("slInit"));
    m.shutdown = reinterpret_cast<PFun_slShutdown*>(sym("slShutdown"));
    m.isSupported = reinterpret_cast<PFun_slIsFeatureSupported*>(sym("slIsFeatureSupported"));
    m.getFunction = reinterpret_cast<PFun_slGetFeatureFunction*>(sym("slGetFeatureFunction"));
    m.newFrame = reinterpret_cast<PFun_slGetNewFrameToken*>(sym("slGetNewFrameToken"));
    m.vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(sym("vkGetInstanceProcAddr"));
    if (!m.init || !m.shutdown || !m.isSupported || !m.getFunction || !m.newFrame || !m.vkGetInstanceProcAddr) {
        if (why) *why = "sl.interposer.dll does not export the expected functions";
        return nullptr;   // the module stays loaded (the process is about to continue without it)
    }

    const wchar_t* paths[1] = {folder.c_str()};
    const sl::Feature features[] = {sl::kFeatureReflex, sl::kFeaturePCL};
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = sl::LogLevel::eDefault;
    pref.pathsToPlugins = paths;
    pref.numPathsToPlugins = 1;
    pref.pathToLogsAndData = nullptr;                 // our own log gets the messages (callback below)
    pref.logMessageCallback = &slLogCallback;
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking;   // no over-the-air downloads of plugins
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = 2;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "native-1";
    pref.projectId = "7d2f6f0a-3b1e-4c58-9a7e-5f3c2b8d1e44";
    pref.renderAPI = sl::RenderAPI::eVulkan;
    const sl::Result r = m.init(pref, sl::kSDKVersion);
    if (r != sl::Result::eOk) {
        if (why) *why = "slInit failed (" + std::to_string(static_cast<int>(r)) + ")";
        return nullptr;
    }
    m.running = true;
    m.loaded = true;
    base::logInfo("streamline: interposer loaded from " + std::string(dir.empty() ? "the program folder" : dir));
    return s;
}

Streamline::Streamline() = default;

Streamline::~Streamline() {
    shutdown();   // the interposer DLL is left loaded on purpose: Vulkan function pointers into it may still be called during teardown
}

PFN_vkGetInstanceProcAddr Streamline::instanceProcAddr() const { return impl_->vkGetInstanceProcAddr; }

void Streamline::deviceCreated(VkInstance, VkPhysicalDevice physical, VkDevice) {
    Impl& m = *impl_;
    sl::AdapterInfo adapter{};
    adapter.vkPhysicalDevice = physical;
    m.reflexSupported = m.isSupported(sl::kFeatureReflex, adapter) == sl::Result::eOk;
    m.pclSupported = m.isSupported(sl::kFeaturePCL, adapter) == sl::Result::eOk;
    auto fn = [&](sl::Feature feature, const char* name, auto*& out) {
        void* p = nullptr;
        if (m.getFunction(feature, name, p) != sl::Result::eOk || !p) return false;
        out = reinterpret_cast<std::remove_reference_t<decltype(out)>>(p);
        return true;
    };
    if (m.pclSupported) fn(sl::kFeaturePCL, "slPCLSetMarker", m.pclSetMarker);
    if (m.reflexSupported &&
        fn(sl::kFeatureReflex, "slReflexSetOptions", m.reflexSetOptions) && fn(sl::kFeatureReflex, "slReflexSleep", m.reflexSleep) &&
        fn(sl::kFeatureReflex, "slReflexGetState", m.reflexGetState)) {
        sl::ReflexOptions options{};
        options.mode = sl::ReflexMode::eLowLatency;
        if (m.reflexSetOptions(options) == sl::Result::eOk) {
            sl::ReflexState st{};
            m.reflexActive = m.reflexGetState(st) == sl::Result::eOk && st.lowLatencyAvailable;
        }
    }
    if (!m.reflexSupported) m.note = "Reflex is not supported by this adapter or driver";
    else if (!m.reflexActive) m.note = "Reflex low latency is not available";
    base::logInfo(std::string("streamline: Reflex ") + (m.reflexActive ? "on" : "off") + ", PCL markers " + (m.pclSupported ? "on" : "off"));
}

void Streamline::shutdown() {
    Impl& m = *impl_;
    if (!m.running) return;
    m.running = false;
    m.reflexActive = false;
    m.shutdown();
}

void Streamline::beginFrame() {
    Impl& m = *impl_;
    if (!m.running) return;
    m.newFrame(m.token, nullptr);
    const uint64_t n = ++m.frames;
    if (m.reflexActive && m.reflexGetState && n % 30 == 0) {
        if (m.reflexGetState(m.state) == sl::Result::eOk && m.state.latencyReportAvailable) {
            uint64_t sum = 0, count = 0;
            for (const sl::ReflexReport& r : m.state.frameReport) {
                if (r.simStartTime == 0 || r.gpuRenderEndTime <= r.simStartTime) continue;
                const uint64_t d = r.gpuRenderEndTime - r.simStartTime;
                if (d > 1'000'000) continue;   // an old or broken entry
                sum += d;
                ++count;
            }
            if (count) m.latencyUs = static_cast<uint32_t>(sum / count);
        }
    }
}

void Streamline::sleep() {
    Impl& m = *impl_;
    if (m.running && m.reflexActive && m.reflexSleep && m.token) m.reflexSleep(*m.token);
}

void Streamline::marker(PclMarker marker) {
    Impl& m = *impl_;
    if (!m.running || !m.pclSupported || !m.pclSetMarker || !m.token) return;
    sl::PCLMarker mm = sl::PCLMarker::eSimulationStart;
    switch (marker) {
        case PclMarker::SimulationStart: mm = sl::PCLMarker::eSimulationStart; break;
        case PclMarker::SimulationEnd: mm = sl::PCLMarker::eSimulationEnd; break;
        case PclMarker::RenderSubmitStart: mm = sl::PCLMarker::eRenderSubmitStart; break;
        case PclMarker::RenderSubmitEnd: mm = sl::PCLMarker::eRenderSubmitEnd; break;
        case PclMarker::PresentStart: mm = sl::PCLMarker::ePresentStart; break;
        case PclMarker::PresentEnd: mm = sl::PCLMarker::ePresentEnd; break;
    }
    m.pclSetMarker(mm, *m.token);
}

StreamlineStatus Streamline::status() const {
    const Impl& m = *impl_;
    StreamlineStatus s;
    s.loaded = m.loaded;
    s.reflexSupported = m.reflexSupported;
    s.reflexActive = m.reflexActive;
    s.pclSupported = m.pclSupported;
    s.version = std::to_string(SL_VERSION_MAJOR) + "." + std::to_string(SL_VERSION_MINOR) + "." + std::to_string(SL_VERSION_PATCH);
    s.note = m.note;
    s.latencyMs = m.latencyUs.load() / 1000.0;
    s.frames = m.frames;
    return s;
}

#else   // built without the SDK

struct Streamline::Impl {};

std::unique_ptr<Streamline> Streamline::load(const std::string&, std::string* why) {
    if (why) *why = "built without the Streamline SDK (run native/tools/fetch_streamline.ps1 and rebuild)";
    return nullptr;
}
Streamline::Streamline() = default;
Streamline::~Streamline() = default;
PFN_vkGetInstanceProcAddr Streamline::instanceProcAddr() const { return nullptr; }
void Streamline::deviceCreated(VkInstance, VkPhysicalDevice, VkDevice) {}
void Streamline::shutdown() {}
void Streamline::beginFrame() {}
void Streamline::sleep() {}
void Streamline::marker(PclMarker) {}
StreamlineStatus Streamline::status() const { return {}; }

#endif

} // namespace native::gpu
