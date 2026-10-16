#include "core/denoise.h"
#include <OpenImageDenoise/oidn.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace core::denoise {

namespace {

// The entry points the denoiser needs. Types come from the vendored header, so a signature that drifts is a compile error here.
struct Api {
    decltype(&oidnNewDevice) newDevice = nullptr;
    decltype(&oidnCommitDevice) commitDevice = nullptr;
    decltype(&oidnReleaseDevice) releaseDevice = nullptr;
    decltype(&oidnGetDeviceError) getDeviceError = nullptr;
    decltype(&oidnNewBuffer) newBuffer = nullptr;
    decltype(&oidnReleaseBuffer) releaseBuffer = nullptr;
    decltype(&oidnWriteBuffer) writeBuffer = nullptr;
    decltype(&oidnReadBuffer) readBuffer = nullptr;
    decltype(&oidnNewFilter) newFilter = nullptr;
    decltype(&oidnReleaseFilter) releaseFilter = nullptr;
    decltype(&oidnSetFilterImage) setFilterImage = nullptr;
    decltype(&oidnSetFilterBool) setFilterBool = nullptr;
    decltype(&oidnSetFilterInt) setFilterInt = nullptr;
    decltype(&oidnCommitFilter) commitFilter = nullptr;
    decltype(&oidnExecuteFilter) executeFilter = nullptr;
};

struct State {
    std::mutex mutex;
    bool probed = false;
    void* lib = nullptr;
    Api api;
    OIDNDevice device = nullptr;
    bool deviceIsCpu = false;
    Info info;
};

State& state() {
    static State s;  // never destroyed on purpose: unloading the library during static destruction can deadlock its thread pool
    return s;
}

void* openLibrary(const std::string& path) {
#ifdef _WIN32
    return LoadLibraryExA(path.c_str(), nullptr, path.find_first_of("\\/") != std::string::npos ? LOAD_WITH_ALTERED_SEARCH_PATH : 0);
#else
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* symbol(void* lib, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

std::filesystem::path exeDir() {
    namespace fs = std::filesystem;
    std::error_code ec;
#ifdef _WIN32
    char buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof buf));
    if (n > 0 && n < sizeof buf) return fs::path(std::string(buf, n)).parent_path();
#else
    const fs::path p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return p.parent_path();
#endif
    return {};
}

// Candidate file names / paths, most specific first.
std::vector<std::string> candidates() {
    std::vector<std::string> out;
    if (const char* env = std::getenv("MAPEDITOR_OIDN"); env && *env) out.push_back(env);
#ifdef _WIN32
    const char* const names[] = {"OpenImageDenoise.dll"};
#elif defined(__APPLE__)
    const char* const names[] = {"libOpenImageDenoise.2.dylib", "libOpenImageDenoise.dylib"};
#else
    const char* const names[] = {"libOpenImageDenoise.so.2", "libOpenImageDenoise.so"};
#endif
    const std::filesystem::path dir = exeDir();
    if (!dir.empty())
        for (const char* n : names) out.push_back((dir / n).string());
    for (const char* n : names) out.push_back(n);
    return out;
}

template <class F>
bool bind(void* lib, F& slot, const char* name, std::string& missing) {
    slot = reinterpret_cast<F>(symbol(lib, name));
    if (!slot) missing = name;
    return slot != nullptr;
}

bool loadApi(State& s, std::string& error) {
    std::string tried;
    for (const std::string& c : candidates()) {
        s.lib = openLibrary(c);
        if (s.lib) break;
        tried += (tried.empty() ? "" : ", ") + c;
    }
    if (!s.lib) {
        error = "OpenImageDenoise library not found (tried " + tried + "); put its DLLs / shared libraries next to the executable or set MAPEDITOR_OIDN";
        return false;
    }
    std::string missing;
    Api& a = s.api;
    const bool ok = bind(s.lib, a.newDevice, "oidnNewDevice", missing) && bind(s.lib, a.commitDevice, "oidnCommitDevice", missing) &&
                    bind(s.lib, a.releaseDevice, "oidnReleaseDevice", missing) && bind(s.lib, a.getDeviceError, "oidnGetDeviceError", missing) &&
                    bind(s.lib, a.newBuffer, "oidnNewBuffer", missing) && bind(s.lib, a.releaseBuffer, "oidnReleaseBuffer", missing) &&
                    bind(s.lib, a.writeBuffer, "oidnWriteBuffer", missing) && bind(s.lib, a.readBuffer, "oidnReadBuffer", missing) &&
                    bind(s.lib, a.newFilter, "oidnNewFilter", missing) && bind(s.lib, a.releaseFilter, "oidnReleaseFilter", missing) &&
                    bind(s.lib, a.setFilterImage, "oidnSetFilterImage", missing) && bind(s.lib, a.setFilterBool, "oidnSetFilterBool", missing) &&
                    bind(s.lib, a.setFilterInt, "oidnSetFilterInt", missing) && bind(s.lib, a.commitFilter, "oidnCommitFilter", missing) &&
                    bind(s.lib, a.executeFilter, "oidnExecuteFilter", missing);
    if (!ok) {
        error = "OpenImageDenoise library is too old or not OIDN 2.x: missing " + missing;
        return false;
    }
    return true;
}

std::string deviceError(State& s, OIDNDevice dev) {
    const char* msg = nullptr;
    if (s.api.getDeviceError(dev, &msg) == OIDN_ERROR_NONE) return {};
    return msg ? msg : "unknown OIDN error";
}

// Creates the shared device. `cpu`: force the CPU device (the fallback when the automatically chosen one fails).
bool makeDevice(State& s, bool cpu, std::string& error) {
    if (s.device) { s.api.releaseDevice(s.device); s.device = nullptr; }
    s.device = s.api.newDevice(cpu ? OIDN_DEVICE_TYPE_CPU : OIDN_DEVICE_TYPE_DEFAULT);
    if (!s.device) {
        const std::string e = deviceError(s, nullptr);
        error = "could not create an OIDN device" + (e.empty() ? std::string() : ": " + e);
        return false;
    }
    s.api.commitDevice(s.device);
    const std::string e = deviceError(s, s.device);
    if (!e.empty()) {
        s.api.releaseDevice(s.device);
        s.device = nullptr;
        error = "OIDN device: " + e;
        return false;
    }
    s.deviceIsCpu = cpu;
    return true;
}

// Requires the state's mutex.
void probeLocked(State& s) {
    if (s.probed) return;
    s.probed = true;
    std::string error;
    if (loadApi(s, error) && !makeDevice(s, false, error)) {
        std::string cpuError;
        if (makeDevice(s, true, cpuError)) error.clear();  // the automatically chosen device failed, the CPU one works
    }
    s.info.available = s.device != nullptr;
    s.info.version = OIDN_VERSION_STRING;
    s.info.error = s.info.available ? std::string() : error;
}

// Runs the RTLightmap filter on one image on `s.device`. Returns an error message, empty on success.
std::string runFilter(State& s, std::vector<float>& rgb, int w, int h, Quality quality) {
    const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 3 * sizeof(float);
    OIDNBuffer buf = s.api.newBuffer(s.device, bytes);
    if (!buf) return "OIDN buffer allocation failed: " + deviceError(s, s.device);
    OIDNFilter filter = s.api.newFilter(s.device, "RTLightmap");
    std::string err;
    if (!filter) {
        err = "OIDN RTLightmap filter unavailable: " + deviceError(s, s.device);
    } else {
        s.api.writeBuffer(buf, 0, bytes, rgb.data());
        s.api.setFilterImage(filter, "color", buf, OIDN_FORMAT_FLOAT3, static_cast<size_t>(w), static_cast<size_t>(h), 0, 0, 0);
        s.api.setFilterImage(filter, "output", buf, OIDN_FORMAT_FLOAT3, static_cast<size_t>(w), static_cast<size_t>(h), 0, 0, 0);  // in place
        s.api.setFilterBool(filter, "directional", false);
        const OIDNQuality q = quality == Quality::Fast ? OIDN_QUALITY_FAST : quality == Quality::Balanced ? OIDN_QUALITY_BALANCED : OIDN_QUALITY_HIGH;
        s.api.setFilterInt(filter, "quality", q);
        s.api.commitFilter(filter);
        err = deviceError(s, s.device);
        if (err.empty()) {
            s.api.executeFilter(filter);
            err = deviceError(s, s.device);
        }
        if (err.empty()) s.api.readBuffer(buf, 0, bytes, rgb.data());
        s.api.releaseFilter(filter);
    }
    s.api.releaseBuffer(buf);
    return err;
}

}  // namespace

Info probe() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    probeLocked(s);
    return s.info;
}

Result filterRGB(std::vector<float>& rgb, int width, int height, Quality quality) {
    Result r;
    if (width <= 0 || height <= 0 || rgb.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 3) {
        r.error = "denoise: image size does not match its data";
        return r;
    }
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    probeLocked(s);
    if (!s.info.available) { r.error = s.info.error; return r; }
    const auto t0 = std::chrono::steady_clock::now();
    for (float& v : rgb) if (!std::isfinite(v) || v < 0.f) v = 0.f;  // the network is trained on HDR values in [0, inf)
    const std::vector<float> original = rgb;
    std::string err = runFilter(s, rgb, width, height, quality);
    if (!err.empty() && !s.deviceIsCpu) {  // an automatically chosen (GPU) device failed at run time: retry once on the CPU
        std::string cpuError;
        if (makeDevice(s, true, cpuError)) {
            rgb = original;
            err = runFilter(s, rgb, width, height, quality);
        }
    }
    if (!err.empty()) { rgb = original; r.error = err; return r; }
    for (float& v : rgb) if (!std::isfinite(v) || v < 0.f) v = 0.f;
    r.ok = true;
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

Result denoiseSamples(const TexelSamples& samples, std::vector<float>& values4, Quality quality, int fillRadius, const std::vector<int>* pages) {
    Result total;
    if (values4.size() != samples.count() * 4) { total.error = "denoise: values do not match the samples"; return total; }
    const Info info = probe();
    if (!info.available) { total.error = info.error; return total; }
    if (samples.count() == 0) { total.ok = true; return total; }

    const size_t atlas = static_cast<size_t>(samples.atlas), perPage = atlas * atlas;
    std::vector<std::vector<uint32_t>> byPage(static_cast<size_t>(samples.pageCount));
    for (size_t i = 0; i < samples.count(); ++i) byPage[samples.texel[i] / perPage].push_back(static_cast<uint32_t>(i));
    std::vector<char> wanted(byPage.size(), pages ? 0 : 1);
    if (pages) for (int p : *pages) if (p >= 0 && static_cast<size_t>(p) < wanted.size()) wanted[static_cast<size_t>(p)] = 1;

    const auto t0 = std::chrono::steady_clock::now();
    for (size_t page = 0; page < byPage.size(); ++page) {
        if (!wanted[page] || byPage[page].empty()) continue;
        std::vector<float> img(perPage * 3, 0.f);
        std::vector<uint8_t> have(perPage, 0);
        for (uint32_t i : byPage[page]) {
            const size_t t = samples.texel[i] % perPage;
            for (size_t c = 0; c < 3; ++c) img[t * 3 + c] = values4[static_cast<size_t>(i) * 4 + c];
            have[t] = 1;
        }
        // grow the baked texels into the empty space: nearest-first, ring by ring
        std::vector<uint8_t> filled = have;
        for (int ring = 0; ring < fillRadius; ++ring) {
            const std::vector<uint8_t> before = filled;
            bool any = false;
            for (size_t y = 0; y < atlas; ++y)
                for (size_t x = 0; x < atlas; ++x) {
                    const size_t idx = y * atlas + x;
                    if (before[idx]) continue;
                    float acc[3] = {0, 0, 0};
                    int n = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            if (!dx && !dy) continue;
                            const long nx = static_cast<long>(x) + dx, ny = static_cast<long>(y) + dy;
                            if (nx < 0 || ny < 0 || nx >= static_cast<long>(atlas) || ny >= static_cast<long>(atlas)) continue;
                            const size_t j = static_cast<size_t>(ny) * atlas + static_cast<size_t>(nx);
                            if (!before[j]) continue;
                            for (size_t c = 0; c < 3; ++c) acc[c] += img[j * 3 + c];
                            ++n;
                        }
                    if (!n) continue;
                    for (size_t c = 0; c < 3; ++c) img[idx * 3 + c] = acc[c] / static_cast<float>(n);
                    filled[idx] = 1;
                    any = true;
                }
            if (!any) break;
        }
        const Result r = filterRGB(img, samples.atlas, samples.atlas, quality);
        if (!r.ok) { total.error = r.error; return total; }
        for (uint32_t i : byPage[page]) {
            const size_t t = samples.texel[i] % perPage;
            for (size_t c = 0; c < 3; ++c) values4[static_cast<size_t>(i) * 4 + c] = img[t * 3 + c];
        }
    }
    total.ok = true;
    total.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return total;
}

}  // namespace core::denoise
