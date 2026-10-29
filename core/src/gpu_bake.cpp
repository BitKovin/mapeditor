#include "core/parallel.h"
#include "gpu_bake.h"
#include <core_shaders/lightmap_ao.spv.h>
#include <core_shaders/lightmap_direct.spv.h>
#include <core_shaders/lightmap_direct_rq.spv.h>
#include <core_shaders/lightmap_gather.spv.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX  // windows.h must not define min / max macros (std::min / std::max are used)
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace core::gpu {

namespace {

// ---- function tables (everything is resolved at run time; no link against the Vulkan loader) -----------------------------
#define GLOBAL_FNS(X) X(vkCreateInstance) X(vkEnumerateInstanceLayerProperties) X(vkEnumerateInstanceExtensionProperties)
#define INSTANCE_FNS(X)                                                                                                    \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkGetPhysicalDeviceFeatures2)              \
    X(vkGetPhysicalDeviceProperties2) X(vkEnumerateDeviceExtensionProperties)
#define DEBUG_FNS(X) X(vkCreateDebugUtilsMessengerEXT) X(vkDestroyDebugUtilsMessengerEXT)
#define DEVICE_FNS(X)                                                                                                      \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements)               \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkCreateShaderModule)          \
    X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout)         \
    X(vkDestroyPipelineLayout) X(vkCreateComputePipelines) X(vkDestroyPipeline) X(vkCreateDescriptorPool)                      \
    X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateCommandPool)                    \
    X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdBindPipeline)     \
    X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDispatch) X(vkCmdPipelineBarrier) X(vkQueueSubmit)                 \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkDeviceWaitIdle) X(vkResetFences) X(vkResetCommandBuffer)                    \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView)    \
    X(vkCreateSampler) X(vkDestroySampler) X(vkCmdCopyBufferToImage) X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool)    \
    X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults)
// only valid on a device created with the ray query extensions
#define RT_FNS(X)                                                                                                          \
    X(vkGetBufferDeviceAddress) X(vkCreateAccelerationStructureKHR) X(vkDestroyAccelerationStructureKHR)                       \
    X(vkGetAccelerationStructureBuildSizesKHR) X(vkCmdBuildAccelerationStructuresKHR) X(vkGetAccelerationStructureDeviceAddressKHR)

struct Fns {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
#define DECL(n) PFN_##n n = nullptr;
    GLOBAL_FNS(DECL) INSTANCE_FNS(DECL) DEBUG_FNS(DECL) DEVICE_FNS(DECL) RT_FNS(DECL)
#undef DECL
};

struct Library {
    void* handle = nullptr;
    ~Library() {
#ifdef _WIN32
        if (handle) FreeLibrary(static_cast<HMODULE>(handle));
#else
        if (handle) dlclose(handle);
#endif
    }
    bool open() {
#ifdef _WIN32
        handle = LoadLibraryA("vulkan-1.dll");
#else
        for (const char* n : {"libvulkan.so.1", "libvulkan.so"}) if ((handle = dlopen(n, RTLD_NOW | RTLD_LOCAL))) break;
#endif
        return handle != nullptr;
    }
    void* sym(const char* name) const {
#ifdef _WIN32
        return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
#else
        return dlsym(handle, name);
#endif
    }
};

[[noreturn]] void fail(const std::string& msg) { throw std::runtime_error(msg); }
void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) fail(std::string(what) + " failed (VkResult " + std::to_string(static_cast<int>(r)) + ")");
}

// Children before parents, even when an exception unwinds.
struct Cleanup {
    std::vector<std::function<void()>> stack;
    void add(std::function<void()> f) { stack.push_back(std::move(f)); }
    ~Cleanup() {
        for (auto it = stack.rbegin(); it != stack.rend(); ++it) (*it)();
    }
};

struct Collector {
    std::vector<std::string> messages;
};

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    auto* c = static_cast<Collector*>(user);
    // The Vulkan loader reports problems with the machine's installed layers (duplicate OBS hook manifests, a broken ReShade
    // DLL, ...) through this same callback. They are environment noise, not validation findings, so they are demoted to notes.
    const bool loader = data->pMessageIdName && std::string(data->pMessageIdName) == "Loader Message";
    const char* tag = loader ? "[note] " : (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "[error] " : "[warning] ");
    if (c->messages.size() < 50) c->messages.push_back(std::string(tag) + (data->pMessage ? data->pMessage : ""));
    return VK_FALSE;
}

// GPU light record (6 x vec4). The layout is documented in shaders/lightmap_direct.comp.
struct GpuLight {
    float v[32];  // 8 x vec4
};

GpuLight pack(const Light& L) {
    GpuLight g{};
    auto set = [&](int i, float a, float b, float c, float d) { g.v[i * 4] = a; g.v[i * 4 + 1] = b; g.v[i * 4 + 2] = c; g.v[i * 4 + 3] = d; };
    constexpr double kDeg = 3.14159265358979323846 / 180.0;
    set(0, static_cast<float>(L.position.x), static_cast<float>(L.position.y), static_cast<float>(L.position.z), static_cast<float>(L.radius));
    set(1, static_cast<float>(L.color.x * L.brightness), static_cast<float>(L.color.y * L.brightness), static_cast<float>(L.color.z * L.brightness),
        static_cast<float>(static_cast<int>(L.type)));
    set(2, static_cast<float>(L.forward.x), static_cast<float>(L.forward.y), static_cast<float>(L.forward.z), static_cast<float>(std::cos(L.innerAngle * kDeg)));
    set(3, static_cast<float>(L.right.x), static_cast<float>(L.right.y), static_cast<float>(L.right.z), static_cast<float>(std::cos(L.outerAngle * kDeg)));
    set(4, static_cast<float>(L.up.x), static_cast<float>(L.up.y), static_cast<float>(L.up.z), static_cast<float>(L.sourceSize));
    set(5, static_cast<float>(L.width), static_cast<float>(L.height), L.castShadows ? 1.f : 0.f, static_cast<float>(L.style));
    set(6, static_cast<float>(L.bounce), 0, 0, 0);
    return g;
}

// Records of every light, then the skybox tables of the first sky light that has them (lightmap_direct.comp `skyRadiance`): record 7 of each sky light
// using them holds where they start (in vec4) and 1.
constexpr size_t kSkyTableRecords = (SkyRadiance::kLon * SkyRadiance::kLat + SkyRadiance::kNLon * SkyRadiance::kNLat + 7) / 8;  // GpuLight-sized blocks
std::vector<GpuLight> packLights(const std::vector<Light>& lights) {
    std::vector<GpuLight> out;
    for (const auto& L : lights) out.push_back(pack(L));
    const SkyRadiance* sky = nullptr;
    for (const auto& L : lights) if (L.type == LightType::Sky && L.sky) { sky = L.sky.get(); break; }
    if (!sky) return out;
    const size_t base = out.size() * 8;  // vec4 index
    for (size_t k = 0; k < lights.size(); ++k)
        if (lights[k].type == LightType::Sky && lights[k].sky.get() == sky) { out[k].v[28] = static_cast<float>(base); out[k].v[29] = 1.f; }
    std::vector<float> table;
    for (const Vec3& c : sky->radiance) table.insert(table.end(), {static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.z), 0.f});
    for (const Vec3& c : sky->irradiance) table.insert(table.end(), {static_cast<float>(c.x), static_cast<float>(c.y), static_cast<float>(c.z), 0.f});
    table.resize(kSkyTableRecords * 32, 0.f);
    for (size_t r = 0; r < kSkyTableRecords; ++r) { GpuLight g{}; std::memcpy(g.v, &table[r * 32], sizeof(g.v)); out.push_back(g); }
    return out;
}

struct PushConstants {  // layout of `PC` in shaders/lightmap_direct.comp
    uint32_t base, count, lightCount, areaSamples;
    float ambient[4];
    uint32_t skySamples;
    float bias;
    float farDistance;
    uint32_t gi;  // 1: also write the bounce source
    uint32_t sampleOffset;
    uint32_t superSamples;  // sub-texel positions per texel (1 = off)
    uint32_t pad[2];
};

struct GatherPush {  // layout of `PC` in shaders/lightmap_gather.comp
    uint32_t base, count, giSamples, atlas;
    float albedo, bias, farDistance;
    uint32_t sampleOffset;
    uint32_t sAtlas;  // atlas size of the bounce source (differs from `atlas` on the fine GI grid)
    uint32_t flags;   // bit 0: do not write the bounce source; bit 1: the hit surface's albedo comes from its texture (bindless array)
};

struct AoPush {  // layout of `PC` in shaders/lightmap_ao.comp
    uint32_t base, count, rays, flags;  // flags bit 0: transparent surfaces exist
    float radius, bias, falloff;
    uint32_t pad;
};
// Bindings of the ambient occlusion shader: 0 pos, 1 nrm, 2 result, 3 tlas, 4 per triangle albedo record (raw ids), 5 transparency words.
constexpr uint32_t kAoBindings = 6;

// Bindings of the ray traced direct shader: 0..3 pos nrm lights result, 4 tlas, 5 bounce result, 6 texel index, 7 atlas0, 8 sub-texel positions.
constexpr uint32_t kDirectRtBindings = 12;  // 0..8, 9 per triangle albedo record (raw ids), 10 transparency words, 11 sky + rect share of the direct light
// Bindings of the gather shader: 0..7 buffers, 8 tlas, 9 per triangle albedo record, 10 sampler, 11 texture array.
constexpr uint32_t kGatherBindings = 13;  // ..., 11 transparency words, 12 the textures (variable count: last)

const char* const kRayQueryExtensions[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME,
                                           VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};

uint64_t alignUp(uint64_t v, uint64_t a) { return a ? (v + a - 1) / a * a : v; }

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;      // what was asked for
    VkDeviceSize capacity = 0;  // what the buffer really holds (>= size; pooled buffers are reused for anything that fits)
    VkDeviceAddress address = 0;
};

bool hasExtensions(const Fns& f, VkPhysicalDevice pd, const char* const* names, size_t n) {
    uint32_t count = 0;
    f.vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> props(count);
    f.vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, props.data());
    for (size_t i = 0; i < n; ++i) {
        bool found = false;
        for (const auto& p : props) if (std::string(p.extensionName) == names[i]) found = true;
        if (!found) return false;
    }
    return true;
}

// Hardware ray queries need Vulkan 1.2, the three KHR extensions and the matching feature bits.
bool supportsRayQuery(const Fns& f, VkPhysicalDevice pd, const VkPhysicalDeviceProperties& props) {
    if (props.apiVersion < VK_API_VERSION_1_2) return false;
    if (!hasExtensions(f, pd, kRayQueryExtensions, 3)) return false;
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    v12.pNext = &as;
    as.pNext = &rq;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &v12;
    f.vkGetPhysicalDeviceFeatures2(pd, &f2);
    // the bounce shader indexes one array of textures with a per-triangle id ("bindless"): descriptor indexing is part of Vulkan 1.2
    const bool bindless = v12.runtimeDescriptorArray && v12.shaderSampledImageArrayNonUniformIndexing && v12.descriptorBindingPartiallyBound &&
                          v12.descriptorBindingVariableDescriptorCount;
    return v12.bufferDeviceAddress && as.accelerationStructure && rq.rayQuery && bindless;
}


struct GpuTextures;
struct DirectPipe { VkShaderModule shader = VK_NULL_HANDLE; VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout pl = VK_NULL_HANDLE; VkPipeline pipe = VK_NULL_HANDLE; };
struct GatherPipe { VkShaderModule shader = VK_NULL_HANDLE; VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout pl = VK_NULL_HANDLE; VkPipeline pipe = VK_NULL_HANDLE; };

// Everything that does not depend on the scene: loader, instance, device, queue, compute pipelines. It lives across calls (one per
// combination of ray tracing / validation), so a live bake step no longer pays for device and pipeline creation.
// Members are destroyed in reverse order: the cleanup stack (device, instance ...) first, then the library, the collector last.
// Buffers of ended live sessions wait here for the next one (an edit rebuilds the session: about fifteen buffers, some of them an atlas page
// each, plus the acceleration structures' storage). Creating, allocating, mapping and freeing them again every time cost more than the light itself.
// Entries are matched by usage and memory kind and have to be big enough, without being much bigger.
struct BufferPool {
    struct Entry { Buffer b; VkBufferUsageFlags usage = 0; int kind = 0; bool address = false; };
    std::vector<Entry> free;
    std::mutex m;
    size_t hits = 0, misses = 0;
    Fns* f = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    static constexpr VkDeviceSize kMaxPooledBytes = 768ull << 20;  // beyond this the oldest go back to the driver

    void destroy(const Buffer& b) const {
        if (b.mapped) f->vkUnmapMemory(device, b.memory);
        f->vkDestroyBuffer(device, b.buffer, nullptr);
        f->vkFreeMemory(device, b.memory, nullptr);
    }
    VkDeviceSize pooledBytes() const {
        VkDeviceSize t = 0;
        for (const auto& e : free) t += e.b.capacity;
        return t;
    }
    void give(Entry e) {
        std::lock_guard<std::mutex> lock(m);
        free.push_back(std::move(e));
        while (free.size() > 1 && pooledBytes() > kMaxPooledBytes) {
            destroy(free.front().b);
            free.erase(free.begin());
        }
    }
    bool take(VkDeviceSize size, VkBufferUsageFlags usage, int kind, bool address, Buffer& out) {
        std::lock_guard<std::mutex> lock(m);
        int best = -1;
        for (size_t i = 0; i < free.size(); ++i) {
            const Entry& e = free[i];
            if (e.usage != usage || e.kind != kind || e.address != address || e.b.capacity < size || e.b.capacity > size * 2 + (1u << 20)) continue;
            if (best < 0 || e.b.capacity < free[static_cast<size_t>(best)].b.capacity) best = static_cast<int>(i);
        }
        if (best < 0) { ++misses; return false; }
        out = free[static_cast<size_t>(best)].b;
        free.erase(free.begin() + best);
        ++hits;
        return true;
    }
    ~BufferPool() {
        if (!f || !device) return;
        for (const auto& e : free) destroy(e.b);
    }
};

struct Context {
    Collector collector;  // the debug messenger writes here; per call it is cleared first
    Library lib;
    Fns f;
    Cleanup cleanup;
    BufferPool bufPool;  // after `cleanup`: freed before the device goes
    std::string deviceName;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t chosenQueue = 0;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties mem{};
    uint64_t scratchAlign = 1, maxPrimitives = 0;
    bool useRT = false;
    uint32_t maxSampledImages = 1;
    uint32_t timestampBits = 0;  // valid bits of the chosen queue's timestamps (0: it cannot write them)
    std::shared_ptr<GpuTextures> textures;                         // the last uploaded texture array (sessions that use it keep it alive)
    std::vector<std::shared_ptr<const AlbedoImage>> textureKey;    // its images, to recognise the same set again
    std::unique_ptr<DirectPipe> direct;
    std::unique_ptr<GatherPipe> gather;
    std::unique_ptr<DirectPipe> ao;
    const DirectPipe& directPipe();
    const GatherPipe& gatherPipe();
    const DirectPipe& aoPipe();
};

void makeContext(Context& c, bool useRT, bool wantValidation) {
    Collector& collector = c.collector;
    c.useRT = useRT;
    Library& lib = c.lib;
    if (!lib.open()) fail("no Vulkan loader found (vulkan-1.dll / libvulkan.so.1)");
    Fns& f = c.f;
    f.vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(lib.sym("vkGetInstanceProcAddr"));
    if (!f.vkGetInstanceProcAddr) fail("vkGetInstanceProcAddr missing");
#define LOAD_G(n) f.n = reinterpret_cast<PFN_##n>(f.vkGetInstanceProcAddr(nullptr, #n));
    GLOBAL_FNS(LOAD_G)
#undef LOAD_G
    if (!f.vkCreateInstance) fail("loader does not expose vkCreateInstance");

    Cleanup& cleanup = c.cleanup;  // declared after `lib` in Context: destroyed before the library is unloaded

    // ---- instance (+ validation) ----
    std::vector<const char*> layers, exts;
    bool debugUtils = false, syncVal = false;
    if (wantValidation) {
        uint32_t n = 0;
        f.vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> props(n);
        f.vkEnumerateInstanceLayerProperties(&n, props.data());
        for (const auto& p : props)
            if (std::string(p.layerName) == "VK_LAYER_KHRONOS_validation") layers.push_back("VK_LAYER_KHRONOS_validation");
        if (layers.empty()) {
            collector.messages.push_back("[note] validation requested but VK_LAYER_KHRONOS_validation is not installed");
        } else {
            uint32_t ne = 0;
            f.vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &ne, nullptr);
            std::vector<VkExtensionProperties> ep(ne);
            f.vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", &ne, ep.data());
            for (const auto& e : ep) {
                if (std::string(e.extensionName) == VK_EXT_DEBUG_UTILS_EXTENSION_NAME) debugUtils = true;
                if (std::string(e.extensionName) == VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME) syncVal = true;
            }
            if (debugUtils) exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            if (syncVal) exts.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
        }
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "mapeditor_lightbake";
    app.apiVersion = VK_API_VERSION_1_2;
    VkDebugUtilsMessengerCreateInfoEXT dbgInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    dbgInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dbgInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    dbgInfo.pfnUserCallback = debugCallback;
    dbgInfo.pUserData = &collector;
    const VkValidationFeatureEnableEXT enables[] = {VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
    VkValidationFeaturesEXT features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    features.enabledValidationFeatureCount = 1;
    features.pEnabledValidationFeatures = enables;
    const void* chain = nullptr;
    VkDebugUtilsMessengerCreateInfoEXT instanceDbg = dbgInfo;  // chained into instance creation (covers create / destroy); carries the features
    instanceDbg.pNext = syncVal ? &features : nullptr;
    if (debugUtils) chain = &instanceDbg;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pNext = chain;
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    ici.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    VkInstance instance = VK_NULL_HANDLE;
    check(f.vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance");
#define LOAD_I(n) f.n = reinterpret_cast<PFN_##n>(f.vkGetInstanceProcAddr(instance, #n));
    INSTANCE_FNS(LOAD_I)
#undef LOAD_I
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if (debugUtils) {
        f.vkCreateDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(f.vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        f.vkDestroyDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(f.vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    }
    cleanup.add([&f, instance] { f.vkDestroyInstance(instance, nullptr); });
    if (debugUtils && f.vkCreateDebugUtilsMessengerEXT) {
        check(f.vkCreateDebugUtilsMessengerEXT(instance, &dbgInfo, nullptr, &messenger), "vkCreateDebugUtilsMessengerEXT");
        cleanup.add([&f, instance, messenger] { f.vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr); });  // dies before the instance
    }

    // ---- physical device (when ray tracing is needed, only devices with ray query qualify) ----
    uint32_t pc = 0;
    f.vkEnumeratePhysicalDevices(instance, &pc, nullptr);
    if (!pc) fail("no Vulkan physical device");
    std::vector<VkPhysicalDevice> pds(pc);
    f.vkEnumeratePhysicalDevices(instance, &pc, pds.data());
    int chosen = -1, chosenScore = -1;
    uint32_t chosenQueue = 0, chosenTimestampBits = 0;
    bool anyCompute = false;
    const char* forced = std::getenv("MAPEDITOR_VK_DEVICE");
    for (uint32_t i = 0; i < pc; ++i) {
        VkPhysicalDeviceProperties pr;
        f.vkGetPhysicalDeviceProperties(pds[i], &pr);
        uint32_t nq = 0;
        f.vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qs(nq);
        f.vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, qs.data());
        int q = -1;
        for (uint32_t k = 0; k < nq; ++k) if (qs[k].queueFlags & VK_QUEUE_COMPUTE_BIT) { q = static_cast<int>(k); break; }
        if (q < 0) continue;
        anyCompute = true;
        if (useRT && !supportsRayQuery(f, pds[i], pr)) continue;
        int score = pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 :
                    pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? 1 : 0;
        if (forced && static_cast<uint32_t>(std::atoi(forced)) == i) score = 100;
        if (score > chosenScore) { chosenScore = score; chosen = static_cast<int>(i); chosenQueue = static_cast<uint32_t>(q); chosenTimestampBits = qs[static_cast<size_t>(q)].timestampValidBits; }
    }
    if (chosen < 0) fail(anyCompute ? "no Vulkan device supports hardware ray queries (VK_KHR_ray_query + acceleration structures)"
                                    : "no Vulkan device with a compute queue");
    const VkPhysicalDevice pd = pds[static_cast<size_t>(chosen)];
    VkPhysicalDeviceProperties props;
    f.vkGetPhysicalDeviceProperties(pd, &props);
    c.deviceName = props.deviceName;
    VkPhysicalDeviceMemoryProperties mem;
    f.vkGetPhysicalDeviceMemoryProperties(pd, &mem);
    // alignment the acceleration structure scratch address must have (queried, never hardcoded)
    uint64_t scratchAlign = 1, maxPrimitives = 0;
    if (useRT) {
        VkPhysicalDeviceAccelerationStructurePropertiesKHR asp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &asp;
        f.vkGetPhysicalDeviceProperties2(pd, &p2);
        scratchAlign = std::max<uint64_t>(1, asp.minAccelerationStructureScratchOffsetAlignment);
        maxPrimitives = asp.maxPrimitiveCount;
    }

    // ---- device ----
    const float prio = 1.f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = chosenQueue;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR rqf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    v12.bufferDeviceAddress = VK_TRUE;
    v12.runtimeDescriptorArray = VK_TRUE;
    v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    v12.descriptorBindingPartiallyBound = VK_TRUE;
    v12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    asf.accelerationStructure = VK_TRUE;
    rqf.rayQuery = VK_TRUE;
    v12.pNext = &asf;
    asf.pNext = &rqf;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (useRT) {
        dci.pNext = &v12;
        dci.enabledExtensionCount = 3;
        dci.ppEnabledExtensionNames = kRayQueryExtensions;
    }
    VkDevice device = VK_NULL_HANDLE;
    check(f.vkCreateDevice(pd, &dci, nullptr, &device), "vkCreateDevice");
#define LOAD_D(n) f.n = reinterpret_cast<PFN_##n>(f.vkGetDeviceProcAddr(device, #n));
    DEVICE_FNS(LOAD_D)
    if (useRT) { RT_FNS(LOAD_D) }
#undef LOAD_D
    cleanup.add([&f, device] { f.vkDestroyDevice(device, nullptr); });
    cleanup.add([&f, device] { f.vkDeviceWaitIdle(device); });  // runs first
    if (useRT && !(f.vkCreateAccelerationStructureKHR && f.vkCmdBuildAccelerationStructuresKHR && f.vkGetBufferDeviceAddress)) fail("ray tracing entry points missing");
    VkQueue queue;
    f.vkGetDeviceQueue(device, chosenQueue, 0, &queue);
    c.device = device;
    c.bufPool.f = &c.f;
    c.bufPool.device = device;
    c.queue = queue;
    c.chosenQueue = chosenQueue;
    c.timestampBits = chosenTimestampBits;
    c.props = props;
    c.mem = mem;
    c.scratchAlign = scratchAlign;
    c.maxPrimitives = maxPrimitives;
    // how many textures the bounce shader can index: the per stage / per set sampled image limits (the array is not update-after-bind), capped
    c.maxSampledImages = std::max<uint32_t>(1, std::min<uint32_t>({props.limits.maxPerStageDescriptorSampledImages, props.limits.maxDescriptorSetSampledImages, 16384u}));
}

const DirectPipe& Context::directPipe() {
    if (direct) return *direct;
    const bool useRT = this->useRT;
    Fns& f = this->f;
    const VkDevice device = this->device;
    Cleanup& cleanup = this->cleanup;
    // ---- pipeline ----
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = useRT ? sizeof(core::shaders::k_lightmap_direct_rq_spv) : sizeof(core::shaders::k_lightmap_direct_spv);
    smi.pCode = useRT ? core::shaders::k_lightmap_direct_rq_spv : core::shaders::k_lightmap_direct_spv;
    VkShaderModule shader;
    check(f.vkCreateShaderModule(device, &smi, nullptr, &shader), "vkCreateShaderModule");
    cleanup.add([&f, device, shader] { f.vkDestroyShaderModule(device, shader, nullptr); });

    const uint32_t bindingCount = useRT ? kDirectRtBindings : 4;
    VkDescriptorSetLayoutBinding binds[kDirectRtBindings];
    for (uint32_t i = 0; i < 4; ++i) binds[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    binds[4] = {4, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    for (uint32_t i = 5; i < kDirectRtBindings; ++i) binds[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = bindingCount;
    dli.pBindings = binds;
    VkDescriptorSetLayout dsl;
    check(f.vkCreateDescriptorSetLayout(device, &dli, nullptr, &dsl), "vkCreateDescriptorSetLayout");
    cleanup.add([&f, device, dsl] { f.vkDestroyDescriptorSetLayout(device, dsl, nullptr); });
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VkPipelineLayout pl;
    check(f.vkCreatePipelineLayout(device, &pli, nullptr, &pl), "vkCreatePipelineLayout");
    cleanup.add([&f, device, pl] { f.vkDestroyPipelineLayout(device, pl, nullptr); });
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, shader, "main", nullptr};
    cpi.layout = pl;
    VkPipeline pipe;
    check(f.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe), "vkCreateComputePipelines");
    cleanup.add([&f, device, pipe] { f.vkDestroyPipeline(device, pipe, nullptr); });
    auto d = std::make_unique<DirectPipe>();
    d->shader = shader; d->dsl = dsl; d->pl = pl; d->pipe = pipe;
    direct = std::move(d);
    return *direct;
}

const GatherPipe& Context::gatherPipe() {
    if (gather) return *gather;
    Fns& f = this->f;
    const VkDevice device = this->device;
    Cleanup& cleanup = this->cleanup;
    VkPipelineLayout gpl = VK_NULL_HANDLE;
    VkPipeline gpipe = VK_NULL_HANDLE;
        VkShaderModuleCreateInfo gsmi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        gsmi.codeSize = sizeof(core::shaders::k_lightmap_gather_spv);
        gsmi.pCode = core::shaders::k_lightmap_gather_spv;
        VkShaderModule gshader;
        check(f.vkCreateShaderModule(device, &gsmi, nullptr, &gshader), "vkCreateShaderModule(gather)");
        cleanup.add([&f, device, gshader] { f.vkDestroyShaderModule(device, gshader, nullptr); });
        // 0..7 storage buffers, 8 acceleration structure, 9 per triangle albedo record, 10 sampler, 11 transparency words, 12 the textures: ONE array of
        // sampled images, indexed with the id in the hit triangle's record ("bindless"). Partially bound + variable count (so it is the last binding):
        // the set is allocated with exactly as many as exist.
        VkDescriptorSetLayoutBinding gb[kGatherBindings];
        for (uint32_t i = 0; i < 8; ++i) gb[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        gb[8] = {8, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        gb[9] = {9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        gb[10] = {10, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        gb[11] = {11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        gb[12] = {12, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, this->maxSampledImages, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorBindingFlags bflags[kGatherBindings] = {};
        bflags[12] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo gbf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        gbf.bindingCount = kGatherBindings;
        gbf.pBindingFlags = bflags;
        VkDescriptorSetLayoutCreateInfo gdli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        gdli.pNext = &gbf;
        gdli.bindingCount = kGatherBindings;
        gdli.pBindings = gb;
        VkDescriptorSetLayout gdsl;
        check(f.vkCreateDescriptorSetLayout(device, &gdli, nullptr, &gdsl), "vkCreateDescriptorSetLayout(gather)");
        cleanup.add([&f, device, gdsl] { f.vkDestroyDescriptorSetLayout(device, gdsl, nullptr); });
        VkPushConstantRange gpcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GatherPush)};
        VkPipelineLayoutCreateInfo gpli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        gpli.setLayoutCount = 1;
        gpli.pSetLayouts = &gdsl;
        gpli.pushConstantRangeCount = 1;
        gpli.pPushConstantRanges = &gpcr;
        check(f.vkCreatePipelineLayout(device, &gpli, nullptr, &gpl), "vkCreatePipelineLayout(gather)");
        const VkPipelineLayout gplCopy = gpl;
        cleanup.add([&f, device, gplCopy] { f.vkDestroyPipelineLayout(device, gplCopy, nullptr); });
        VkComputePipelineCreateInfo gcpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        gcpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, gshader, "main", nullptr};
        gcpi.layout = gpl;
        check(f.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &gcpi, nullptr, &gpipe), "vkCreateComputePipelines(gather)");
        const VkPipeline gpipeCopy = gpipe;
        cleanup.add([&f, device, gpipeCopy] { f.vkDestroyPipeline(device, gpipeCopy, nullptr); });
    auto g = std::make_unique<GatherPipe>();
    g->dsl = gdsl; g->pl = gpl; g->pipe = gpipe;
    gather = std::move(g);
    return *gather;
}

const DirectPipe& Context::aoPipe() {
    if (ao) return *ao;
    Fns& f = this->f;
    const VkDevice device = this->device;
    Cleanup& cleanup = this->cleanup;
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = sizeof(core::shaders::k_lightmap_ao_spv);
    smi.pCode = core::shaders::k_lightmap_ao_spv;
    VkShaderModule shader;
    check(f.vkCreateShaderModule(device, &smi, nullptr, &shader), "vkCreateShaderModule(ao)");
    cleanup.add([&f, device, shader] { f.vkDestroyShaderModule(device, shader, nullptr); });
    VkDescriptorSetLayoutBinding binds[kAoBindings];
    for (uint32_t i = 0; i < kAoBindings; ++i)
        binds[i] = {i, i == 3 ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = kAoBindings;
    dli.pBindings = binds;
    VkDescriptorSetLayout dsl;
    check(f.vkCreateDescriptorSetLayout(device, &dli, nullptr, &dsl), "vkCreateDescriptorSetLayout(ao)");
    cleanup.add([&f, device, dsl] { f.vkDestroyDescriptorSetLayout(device, dsl, nullptr); });
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(AoPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VkPipelineLayout pl;
    check(f.vkCreatePipelineLayout(device, &pli, nullptr, &pl), "vkCreatePipelineLayout(ao)");
    cleanup.add([&f, device, pl] { f.vkDestroyPipelineLayout(device, pl, nullptr); });
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, shader, "main", nullptr};
    cpi.layout = pl;
    VkPipeline pipe;
    check(f.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe), "vkCreateComputePipelines(ao)");
    cleanup.add([&f, device, pipe] { f.vkDestroyPipeline(device, pipe, nullptr); });
    auto d = std::make_unique<DirectPipe>();
    d->shader = shader; d->dsl = dsl; d->pl = pl; d->pipe = pipe;
    ao = std::move(d);
    return *ao;
}

enum class MemKind { Host, HostCached, DeviceHost, Device };
Buffer allocBuffer(Context& c, Cleanup& cleanup, VkDeviceSize size, VkBufferUsageFlags usage, MemKind kind, bool address, bool storageRange);

// ---- bindless albedo textures ---------------------------------------------------------------------------------------------------
// Every texture of an AlbedoSet as a sampled image (RGBA8 sRGB: the hardware decodes to linear before filtering, like AlbedoImage::sample does on the
// CPU), full mip chain, in one or few memory blocks (a level may have thousands of textures; allocations are limited to a few thousand). One sampler.
// Built once per set and kept by the context, so a geometry edit that re-creates a session does not upload them again.
struct GpuTextures {
    Context* ctx = nullptr;  // not owning: the context owns the last set, sessions hold the context too (declared before this)
    VkSampler sampler = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkDeviceMemory> memory;
    uint32_t used = 0;  // textures that carry real data: ids >= used (over the device limit) are drawn white
    ~GpuTextures() {
        if (!ctx || !ctx->device) return;
        Fns& f = ctx->f;
        const VkDevice d = ctx->device;
        if (f.vkDeviceWaitIdle) f.vkDeviceWaitIdle(d);
        for (VkImageView v : views) if (v) f.vkDestroyImageView(d, v, nullptr);
        for (VkImage i : images) if (i) f.vkDestroyImage(d, i, nullptr);
        for (VkDeviceMemory m : memory) if (m) f.vkFreeMemory(d, m, nullptr);
        if (sampler) f.vkDestroySampler(d, sampler, nullptr);
    }
};

std::shared_ptr<GpuTextures> uploadTextures(Context& c, const AlbedoSet* set) {
    std::vector<std::shared_ptr<const AlbedoImage>> key;
    if (set) key = set->images;
    if (c.textures && c.textureKey == key) return c.textures;
    Fns& f = c.f;
    const VkDevice device = c.device;
    auto gt = std::make_shared<GpuTextures>();
    gt->ctx = &c;
    // the 1x1 white texture stands in when nothing loaded (the descriptor array needs at least one entry)
    static const AlbedoImage kWhite = [] { AlbedoImage w; w.width = w.height = 1; w.mips.push_back({255, 255, 255, 255}); return w; }();
    std::vector<const AlbedoImage*> src;
    if (set) for (const auto& im : set->images) { if (src.size() >= c.maxSampledImages) break; src.push_back(im.get()); }
    gt->used = static_cast<uint32_t>(src.size());
    if (src.empty()) src.push_back(&kWhite);

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = 16.f;
    check(f.vkCreateSampler(device, &sci, nullptr, &gt->sampler), "vkCreateSampler");

    // images + memory (sub allocated in blocks)
    constexpr VkDeviceSize kBlock = 256ull << 20;
    struct Place { uint32_t block; VkDeviceSize offset; };
    std::vector<Place> place(src.size());
    std::vector<VkMemoryRequirements> reqs(src.size());
    struct Block { VkDeviceSize size = 0; uint32_t typeBits = 0xFFFFFFFFu; };
    std::vector<Block> blocks;
    for (size_t i = 0; i < src.size(); ++i) {
        const AlbedoImage& im = *src[i];
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_SRGB;
        ii.extent = {static_cast<uint32_t>(im.width), static_cast<uint32_t>(im.height), 1};
        ii.mipLevels = static_cast<uint32_t>(im.mipCount());
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage img = VK_NULL_HANDLE;
        check(f.vkCreateImage(device, &ii, nullptr, &img), "vkCreateImage");
        gt->images.push_back(img);
        f.vkGetImageMemoryRequirements(device, img, &reqs[i]);
        const uint32_t bits = reqs[i].memoryTypeBits;
        const VkDeviceSize a = std::max<VkDeviceSize>(1, reqs[i].alignment);
        if (blocks.empty() || (blocks.back().typeBits & bits) == 0 || alignUp(blocks.back().size, a) + reqs[i].size > kBlock) blocks.push_back({});
        Block& b = blocks.back();
        b.size = alignUp(b.size, a);
        place[i] = {static_cast<uint32_t>(blocks.size() - 1), b.size};
        b.size += reqs[i].size;
        b.typeBits &= bits;
    }
    for (const Block& b : blocks) {
        int type = -1;
        for (int pass = 0; pass < 2 && type < 0; ++pass)
            for (uint32_t t = 0; t < c.mem.memoryTypeCount; ++t) {
                if (!(b.typeBits & (1u << t))) continue;
                if (pass == 0 && !(c.mem.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) continue;
                type = static_cast<int>(t);
                break;
            }
        if (type < 0) fail("no memory type for the albedo textures");
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = b.size;
        ai.memoryTypeIndex = static_cast<uint32_t>(type);
        VkDeviceMemory mem = VK_NULL_HANDLE;
        check(f.vkAllocateMemory(device, &ai, nullptr, &mem), "vkAllocateMemory(textures)");
        gt->memory.push_back(mem);
    }
    for (size_t i = 0; i < src.size(); ++i) check(f.vkBindImageMemory(device, gt->images[i], gt->memory[place[i].block], place[i].offset), "vkBindImageMemory");
    for (size_t i = 0; i < src.size(); ++i) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = gt->images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R8G8B8A8_SRGB;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, static_cast<uint32_t>(src[i]->mipCount()), 0, 1};
        VkImageView v = VK_NULL_HANDLE;
        check(f.vkCreateImageView(device, &vi, nullptr, &v), "vkCreateImageView");
        gt->views.push_back(v);
    }

    // staging buffer with every level, one submission with the copies
    Cleanup tmp;
    VkDeviceSize total = 0;
    std::vector<std::vector<VkDeviceSize>> levelOffset(src.size());
    for (size_t i = 0; i < src.size(); ++i)
        for (const auto& m : src[i]->mips) { levelOffset[i].push_back(total); total += alignUp(m.size(), 4); }
    Buffer staging = allocBuffer(c, tmp, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemKind::Host, false, false);
    for (size_t i = 0; i < src.size(); ++i)
        for (size_t l = 0; l < src[i]->mips.size(); ++l) std::memcpy(static_cast<char*>(staging.mapped) + levelOffset[i][l], src[i]->mips[l].data(), src[i]->mips[l].size());
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = c.chosenQueue;
    VkCommandPool pool;
    check(f.vkCreateCommandPool(device, &cpci, nullptr, &pool), "vkCreateCommandPool(textures)");
    tmp.add([&f, device, pool] { f.vkDestroyCommandPool(device, pool, nullptr); });
    VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbi.commandPool = pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    VkCommandBuffer cmd;
    check(f.vkAllocateCommandBuffers(device, &cbi, &cmd), "vkAllocateCommandBuffers(textures)");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    check(f.vkCreateFence(device, &fi, nullptr, &fence), "vkCreateFence(textures)");
    tmp.add([&f, device, fence] { f.vkDestroyFence(device, fence, nullptr); });
    tmp.add([&f, device] { f.vkDeviceWaitIdle(device); });  // runs first
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(f.vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer(textures)");
    std::vector<VkImageMemoryBarrier> toDst(src.size()), toRead(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, static_cast<uint32_t>(src[i]->mipCount()), 0, 1};
        toDst[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, gt->images[i], range};
        toRead[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, gt->images[i], range};
    }
    f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, static_cast<uint32_t>(toDst.size()), toDst.data());
    for (size_t i = 0; i < src.size(); ++i) {
        std::vector<VkBufferImageCopy> regions;
        for (size_t l = 0; l < src[i]->mips.size(); ++l) {
            VkBufferImageCopy r{};
            r.bufferOffset = levelOffset[i][l];
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, static_cast<uint32_t>(l), 0, 1};
            r.imageExtent = {static_cast<uint32_t>(src[i]->mipWidth(static_cast<int>(l))), static_cast<uint32_t>(src[i]->mipHeight(static_cast<int>(l))), 1};
            regions.push_back(r);
        }
        f.vkCmdCopyBufferToImage(cmd, staging.buffer, gt->images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(regions.size()), regions.data());
    }
    f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, static_cast<uint32_t>(toRead.size()), toRead.data());
    check(f.vkEndCommandBuffer(cmd), "vkEndCommandBuffer(textures)");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    check(f.vkQueueSubmit(c.queue, 1, &si, fence), "vkQueueSubmit(textures)");
    check(f.vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences(textures)");
    c.textures = gt;
    c.textureKey = std::move(key);
    return gt;
}

// Writes bindings 10 (sampler) and 11 (textures) of a gather descriptor set.
void writeTextureDescriptors(Fns& f, VkDevice device, VkDescriptorSet set, const GpuTextures& t) {
    VkDescriptorImageInfo sampler{t.sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    std::vector<VkDescriptorImageInfo> imgs(t.views.size());
    for (size_t i = 0; i < imgs.size(); ++i) imgs[i] = {VK_NULL_HANDLE, t.views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w[2];
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 10, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &sampler, nullptr, nullptr};
    w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 12, 0, static_cast<uint32_t>(imgs.size()), VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, imgs.data(), nullptr, nullptr};
    f.vkUpdateDescriptorSets(device, 2, w, 0, nullptr);
}

// Per triangle albedo record for the shader: the mesh's albedoMeta with texture ids the device cannot index (over its limit) turned into "white".
// Transparency buffers (REQUIREMENTS §23): the raw per triangle albedo records (ids index the transparency headers, not the device's textures) and
// the packed words. Tiny dummies when the mesh has no transparency (the descriptors must still be valid).
struct TransparencyBuffers { std::vector<float> meta; std::vector<uint32_t> words; bool on = false; };
TransparencyBuffers transparencyOf(const OccluderMesh& mesh) {
    TransparencyBuffers t;
    t.on = mesh.hasTransparency();
    if (t.on) {
        // 3 x vec4 per triangle for the direct shader: the albedo record (raw ids) and the winding normal (translucent: count where the ray enters)
        const size_t n = mesh.triangleCount();
        t.meta.assign(n * 12, 0.f);
        for (size_t i = 0; i < n; ++i) {
            std::memcpy(&t.meta[i * 12], &mesh.albedoMeta[i * 8], 8 * sizeof(float));
            const float* v = &mesh.tris[i * 9];
            const float e1[3] = {v[3] - v[0], v[4] - v[1], v[5] - v[2]}, e2[3] = {v[6] - v[0], v[7] - v[1], v[8] - v[2]};
            t.meta[i * 12 + 8] = e1[1] * e2[2] - e1[2] * e2[1];
            t.meta[i * 12 + 9] = e1[2] * e2[0] - e1[0] * e2[2];
            t.meta[i * 12 + 10] = e1[0] * e2[1] - e1[1] * e2[0];
        }
        t.words = mesh.alphaWords;
    } else { t.meta.assign(12, -1.f); t.words.assign(8, 0u); }
    return t;
}

void packAlbedoMeta(const OccluderMesh& mesh, uint32_t used, float* dst) {
    std::memcpy(dst, mesh.albedoMeta.data(), mesh.albedoMeta.size() * sizeof(float));
    for (size_t t = 0; t < mesh.triangleCount(); ++t) {
        float& id = dst[t * 8 + 6];
        if (id >= static_cast<float>(used)) id = -1.f;
    }
}

std::mutex g_mutex;
std::map<int, std::shared_ptr<Context>> g_contexts;  // shared: a live session keeps its device alive

std::shared_ptr<Context> contextFor(bool useRT, bool wantValidation) {
    const int key = (useRT ? 1 : 0) | (wantValidation ? 2 : 0);
    auto it = g_contexts.find(key);
    if (it != g_contexts.end()) return it->second;
    auto c = std::make_shared<Context>();
    makeContext(*c, useRT, wantValidation);  // throws: nothing is cached, the half built context unwinds
    return g_contexts.emplace(key, std::move(c)).first->second;
}

void run(const TexelSamples& samples, const LightScene& scene, const OccluderMesh* occluders, const BakeOptions& options, Result& result,
         std::vector<float>& out, std::vector<float>* dbOut, std::vector<float>* indirectOut, const GiSamples* gi, std::vector<float>* giOut) {
    const bool wantValidation = options.gpuValidation;
    const bool useRT = needsRayTracing(scene, occluders, options);
    const bool useGi = useRT && options.bounces > 0;
    const bool wantDb = dbOut != nullptr;
    if (wantDb && !useRT) fail("the bounce-weighted direct light needs the ray traced shader variant");
    if (useGi && !occluders->hasLightmap()) fail("bounces need occluders with lightmap coordinates");

    const std::shared_ptr<Context> ctxHold = contextFor(useRT, wantValidation);
    Context& ctx = *ctxHold;
    ctx.collector.messages.clear();
    result.device = ctx.deviceName;
    Fns& f = ctx.f;
    const VkDevice device = ctx.device;
    const VkQueue queue = ctx.queue;
    const uint32_t chosenQueue = ctx.chosenQueue;
    const VkPhysicalDeviceProperties& props = ctx.props;
    const VkPhysicalDeviceMemoryProperties& mem = ctx.mem;
    const uint64_t scratchAlign = ctx.scratchAlign, maxPrimitives = ctx.maxPrimitives;
    Cleanup cleanup;  // per-call objects (buffers, acceleration structures, descriptors, command pool, fence)
    cleanup.add([&f, device] { f.vkDeviceWaitIdle(device); });  // runs first

    // ---- buffers ----
    // host: host visible + coherent, persistently mapped (inputs and results); otherwise device local when possible.
    auto makeBuffer = [&](VkDeviceSize size, VkBufferUsageFlags usage, bool host, bool address, bool storageRange) {
        if (storageRange && size > props.limits.maxStorageBufferRange) fail("buffer of " + std::to_string(size) + " bytes exceeds maxStorageBufferRange");
        Buffer b;
        b.size = size;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size;
        bi.usage = usage | (address ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(f.vkCreateBuffer(device, &bi, nullptr, &b.buffer), "vkCreateBuffer");
        VkBuffer buf = b.buffer;
        cleanup.add([&f, device, buf] { f.vkDestroyBuffer(device, buf, nullptr); });
        VkMemoryRequirements mr;
        f.vkGetBufferMemoryRequirements(device, b.buffer, &mr);
        int type = -1;
        const VkMemoryPropertyFlags hostFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (int pass = 0; pass < 2 && type < 0; ++pass)
            for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
                if (!(mr.memoryTypeBits & (1u << i))) continue;
                const VkMemoryPropertyFlags fl = mem.memoryTypes[i].propertyFlags;
                const bool ok = host ? (fl & hostFlags) == hostFlags : (pass == 0 ? (fl & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 : true);
                if (ok) { type = static_cast<int>(i); break; }
            }
        if (type < 0) fail(host ? "no host visible + coherent memory type" : "no suitable memory type");
        VkMemoryAllocateFlagsInfo flagsInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.pNext = address ? &flagsInfo : nullptr;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = static_cast<uint32_t>(type);
        check(f.vkAllocateMemory(device, &ai, nullptr, &b.memory), "vkAllocateMemory");
        VkDeviceMemory m = b.memory;
        cleanup.add([&f, device, m] { f.vkFreeMemory(device, m, nullptr); });
        check(f.vkBindBufferMemory(device, b.buffer, b.memory, 0), "vkBindBufferMemory");
        if (host) {
            check(f.vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped), "vkMapMemory");
            cleanup.add([&f, device, m] { f.vkUnmapMemory(device, m); });
        }
        if (address) {
            VkBufferDeviceAddressInfo dai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            dai.buffer = b.buffer;
            b.address = f.vkGetBufferDeviceAddress(device, &dai);
        }
        return b;
    };
    const size_t n = samples.count();
    std::vector<GpuLight> lights;
    lights = packLights(scene.lights);
    if (lights.empty()) lights.push_back(GpuLight{});  // zero-sized buffers are invalid

    const VkDeviceSize sampleBytes = std::max<size_t>(n, 1) * 16;
    // Fine GI grid: its entries are appended behind the base samples in the same buffers (positions, normals, texel index, indirect result); the
    // direct pass only covers [0, n), the last gather covers [n, n + m) and does not write the bounce source.
    const bool fine = useGi && gi && gi->scale > 1 && gi->count() > 0;
    const size_t m = fine ? gi->count() : 0;
    const VkDeviceSize totalBytes = std::max<size_t>(n + m, 1) * 16;
    const VkBufferUsageFlags kStorage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    Buffer bPos = makeBuffer(totalBytes, kStorage, true, false, true), bNrm = makeBuffer(totalBytes, kStorage, true, false, true),
           bLights = makeBuffer(lights.size() * sizeof(GpuLight), kStorage, true, false, true), bOut = makeBuffer(sampleBytes, kStorage, true, false, true);
    std::memset(bPos.mapped, 0, static_cast<size_t>(totalBytes));
    std::memset(bNrm.mapped, 0, static_cast<size_t>(totalBytes));
    if (m) {
        std::memcpy(static_cast<float*>(bPos.mapped) + n * 4, gi->s.pos.data(), m * 16);
        std::memcpy(static_cast<float*>(bNrm.mapped) + n * 4, gi->s.nrm.data(), m * 16);
    }
    if (n) {
        // w carries the sample's random-number id as raw bits (TexelSamples::seedOf), so a chunk keeps the draws of a full bake
        float* dst = static_cast<float*>(bPos.mapped);
        for (size_t i = 0; i < n; ++i) {
            dst[i * 4 + 0] = samples.pos[i * 4 + 0];
            dst[i * 4 + 1] = samples.pos[i * 4 + 1];
            dst[i * 4 + 2] = samples.pos[i * 4 + 2];
            const uint32_t id = samples.seedOf(i);
            std::memcpy(&dst[i * 4 + 3], &id, sizeof(id));
        }
        float* dn = static_cast<float*>(bNrm.mapped);  // w carries the sample's pass offset as raw bits
        for (size_t i = 0; i < n; ++i) {
            dn[i * 4 + 0] = samples.nrm[i * 4 + 0];
            dn[i * 4 + 1] = samples.nrm[i * 4 + 1];
            dn[i * 4 + 2] = samples.nrm[i * 4 + 2];
            const uint32_t off = samples.offsetOf(i);
            std::memcpy(&dn[i * 4 + 3], &off, sizeof(off));
        }
    }
    std::memcpy(bLights.mapped, lights.data(), lights.size() * sizeof(GpuLight));
    std::memset(bOut.mapped, 0, static_cast<size_t>(sampleBytes));

    // ---- bounce buffers (tiny dummies when bounces are off: the descriptors must still be valid) ----
    const size_t atlasTexels = static_cast<size_t>(samples.pageCount) * static_cast<size_t>(samples.atlas) * static_cast<size_t>(samples.atlas);
    const VkDeviceSize atlasBytes = (useGi ? std::max<size_t>(atlasTexels, 1) : 1) * 16;
    const VkDeviceSize perSample = useGi ? totalBytes : 16;
    Buffer bDb = makeBuffer(useGi || wantDb ? sampleBytes : 16, kStorage, true, false, true), bInd = makeBuffer(perSample, kStorage, true, false, true),
           bIdx = makeBuffer(useGi ? std::max<size_t>(n + m, 1) * 4 : 16, kStorage, true, false, true),
           bAtlasA = makeBuffer(atlasBytes, kStorage, true, false, true), bAtlasB = makeBuffer(atlasBytes, kStorage, true, false, true),
           bMeta = makeBuffer(useGi ? occluders->lightmap.size() * sizeof(float) : 16, kStorage, true, false, true);
    const bool textured = useGi && occluders->hasAlbedo();  // bounces pick up the colour of the surface they hit
    Buffer bAlb = makeBuffer(textured ? occluders->albedoMeta.size() * sizeof(float) : 16, kStorage, true, false, true);
    std::memset(bAlb.mapped, 0, static_cast<size_t>(bAlb.size));
    std::shared_ptr<GpuTextures> gtex;
    if (useGi) gtex = uploadTextures(ctx, textured ? occluders->albedo.get() : nullptr);
    if (textured) packAlbedoMeta(*occluders, gtex->used, static_cast<float*>(bAlb.mapped));
    const TransparencyBuffers tb = transparencyOf(*occluders);
    const bool transparent = useRT && tb.on;
    Buffer bTMeta = makeBuffer(tb.meta.size() * sizeof(float), kStorage, true, false, true), bTWords = makeBuffer(tb.words.size() * 4, kStorage, true, false, true);
    std::memcpy(bTMeta.mapped, tb.meta.data(), tb.meta.size() * sizeof(float));
    std::memcpy(bTWords.mapped, tb.words.data(), tb.words.size() * 4);
    // sub-texel positions (3 x vec4 per sample) for the super sampled direct light; a dummy when it is off
    const uint32_t superSamples = useRT && samples.hasSub() ? static_cast<uint32_t>(std::clamp(options.superSamples, 1, kMaxSuperSamples)) : 1u;
    const size_t subBytes = superSamples > 1 ? n * 48 : 16;
    Buffer bSub = makeBuffer(subBytes, kStorage, true, false, true);
    std::memset(bSub.mapped, 0, subBytes);
    if (superSamples > 1) std::memcpy(bSub.mapped, samples.sub.data(), n * 48);
    std::memset(bDb.mapped, 0, static_cast<size_t>(useGi || wantDb ? sampleBytes : 16));
    std::memset(bInd.mapped, 0, static_cast<size_t>(perSample));
    std::memset(bAtlasA.mapped, 0, static_cast<size_t>(atlasBytes));
    std::memset(bAtlasB.mapped, 0, static_cast<size_t>(atlasBytes));
    std::memset(bIdx.mapped, 0, useGi ? std::max<size_t>(n + m, 1) * 4 : 16);
    std::memset(bMeta.mapped, 0, useGi ? occluders->lightmap.size() * sizeof(float) : 16);
    if (useGi) {
        if (n) std::memcpy(bIdx.mapped, samples.texel.data(), n * 4);
        if (m) std::memcpy(static_cast<uint32_t*>(bIdx.mapped) + n, gi->s.texel.data(), m * 4);
        std::memcpy(bMeta.mapped, occluders->lightmap.data(), occluders->lightmap.size() * sizeof(float));
    }

    // ---- acceleration structures: one BLAS of all occluder triangles, one TLAS with a single identity instance ----
    VkAccelerationStructureKHR blas = VK_NULL_HANDLE, tlas = VK_NULL_HANDLE;
    VkAccelerationStructureBuildGeometryInfoKHR blasBuild{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    VkAccelerationStructureBuildGeometryInfoKHR tlasBuild{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    VkAccelerationStructureGeometryKHR blasGeom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    VkAccelerationStructureGeometryKHR tlasGeom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    VkAccelerationStructureBuildRangeInfoKHR blasRange{}, tlasRange{};
    if (useRT) {
        const uint32_t triangles = static_cast<uint32_t>(occluders->triangleCount());
        if (maxPrimitives && triangles > maxPrimitives) fail("more occluder triangles than the device supports");
        const VkBufferUsageFlags kBuildInput = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        Buffer vb = makeBuffer(occluders->tris.size() * sizeof(float), kBuildInput, true, true, false);
        std::memcpy(vb.mapped, occluders->tris.data(), occluders->tris.size() * sizeof(float));

        blasGeom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        // with transparent surfaces the shaders decide per hit (surfaceAt); each triangle once per ray, so translucent layers multiply exactly once
        blasGeom.flags = transparent ? VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR : VK_GEOMETRY_OPAQUE_BIT_KHR;
        blasGeom.geometry.triangles = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
        blasGeom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        blasGeom.geometry.triangles.vertexData.deviceAddress = vb.address;
        blasGeom.geometry.triangles.vertexStride = 3 * sizeof(float);
        blasGeom.geometry.triangles.maxVertex = triangles * 3 - 1;
        blasGeom.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
        blasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        blasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        blasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        blasBuild.geometryCount = 1;
        blasBuild.pGeometries = &blasGeom;
        blasRange.primitiveCount = triangles;

        auto makeAS = [&](VkAccelerationStructureBuildGeometryInfoKHR& build, VkAccelerationStructureTypeKHR type, uint32_t primitives,
                          VkAccelerationStructureKHR& handle) {
            VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            f.vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &primitives, &sizes);
            Buffer storage = makeBuffer(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, true, false);
            VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
            ci.buffer = storage.buffer;
            ci.size = sizes.accelerationStructureSize;
            ci.type = type;
            check(f.vkCreateAccelerationStructureKHR(device, &ci, nullptr, &handle), "vkCreateAccelerationStructureKHR");
            VkAccelerationStructureKHR h = handle;
            cleanup.add([&f, device, h] { f.vkDestroyAccelerationStructureKHR(device, h, nullptr); });  // before its backing buffer
            // each build gets its own scratch buffer, over-allocated so the address can be aligned up
            Buffer scratch = makeBuffer(sizes.buildScratchSize + scratchAlign, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, true, false);
            build.dstAccelerationStructure = handle;
            build.scratchData.deviceAddress = alignUp(scratch.address, scratchAlign);
        };
        makeAS(blasBuild, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, triangles, blas);

        VkAccelerationStructureDeviceAddressInfoKHR bai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        bai.accelerationStructure = blas;
        VkAccelerationStructureInstanceKHR inst{};
        inst.transform.matrix[0][0] = inst.transform.matrix[1][1] = inst.transform.matrix[2][2] = 1.f;
        inst.mask = 0xFF;
        inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;  // double sided
        inst.accelerationStructureReference = f.vkGetAccelerationStructureDeviceAddressKHR(device, &bai);
        Buffer ib = makeBuffer(sizeof(inst), kBuildInput, true, true, false);
        std::memcpy(ib.mapped, &inst, sizeof(inst));

        tlasGeom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        tlasGeom.flags = transparent ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;
        tlasGeom.geometry.instances = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
        tlasGeom.geometry.instances.arrayOfPointers = VK_FALSE;
        tlasGeom.geometry.instances.data.deviceAddress = ib.address;
        tlasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        tlasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        tlasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        tlasBuild.geometryCount = 1;
        tlasBuild.pGeometries = &tlasGeom;
        tlasRange.primitiveCount = 1;
        makeAS(tlasBuild, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, 1, tlas);
    }

    // ---- pipeline (cached in the context) ----
    const DirectPipe& dp = ctx.directPipe();
    const VkDescriptorSetLayout dsl = dp.dsl;
    const VkPipelineLayout pl = dp.pl;
    const VkPipeline pipe = dp.pipe;
    const uint32_t bindingCount = useRT ? kDirectRtBindings : 4;

    const uint32_t texCount = gtex ? static_cast<uint32_t>(gtex->views.size()) : 1u;
    VkDescriptorPoolSize ps[4] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 40}, {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 3},
                                  {VK_DESCRIPTOR_TYPE_SAMPLER, 2}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * texCount}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = useGi ? 3 : 1;
    dpi.poolSizeCount = useGi ? 4 : (useRT ? 2 : 1);
    dpi.pPoolSizes = ps;
    VkDescriptorPool pool;
    check(f.vkCreateDescriptorPool(device, &dpi, nullptr, &pool), "vkCreateDescriptorPool");
    cleanup.add([&f, device, pool] { f.vkDestroyDescriptorPool(device, pool, nullptr); });
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = pool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &dsl;
    VkDescriptorSet set;
    check(f.vkAllocateDescriptorSets(device, &dai, &set), "vkAllocateDescriptorSets");
    const Buffer* all[4] = {&bPos, &bNrm, &bLights, &bOut};
    VkDescriptorBufferInfo dbi[4];
    VkWriteDescriptorSet w[kDirectRtBindings];
    for (uint32_t i = 0; i < 4; ++i) {
        dbi[i] = {all[i]->buffer, 0, VK_WHOLE_SIZE};
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, i, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &dbi[i], nullptr};
    }
    VkWriteDescriptorSetAccelerationStructureKHR asw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    asw.accelerationStructureCount = 1;
    asw.pAccelerationStructures = &tlas;
    w[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &asw, set, 4, 0, 1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, nullptr, nullptr, nullptr};
    const Buffer* extra[4] = {&bDb, &bIdx, &bAtlasA, &bSub};  // bindings 5, 6, 7, 8 of the direct shader
    VkDescriptorBufferInfo ebi[4];
    for (uint32_t i = 0; i < 4; ++i) {
        ebi[i] = {extra[i]->buffer, 0, VK_WHOLE_SIZE};
        w[5 + i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 5 + i, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &ebi[i], nullptr};
    }
    VkDescriptorBufferInfo tbi[2] = {{bTMeta.buffer, 0, VK_WHOLE_SIZE}, {bTWords.buffer, 0, VK_WHOLE_SIZE}};  // bindings 9, 10: transparency
    w[9] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 9, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &tbi[0], nullptr};
    w[10] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 10, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &tbi[1], nullptr};
    VkDescriptorBufferInfo xbi{bDb.buffer, 0, VK_WHOLE_SIZE};  // binding 11: only written with push.gi bit 4, which a one-shot evaluation never sets
    w[11] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 11, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &xbi, nullptr};
    f.vkUpdateDescriptorSets(device, bindingCount, w, 0, nullptr);

    // ---- gather pipeline (one bounce) and its two ping-pong descriptor sets ----
    VkPipelineLayout gpl = VK_NULL_HANDLE;
    VkPipeline gpipe = VK_NULL_HANDLE;
    VkDescriptorSet gsets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    if (useGi) {
        const GatherPipe& gp0 = ctx.gatherPipe();
        const VkDescriptorSetLayout gdsl = gp0.dsl;
        gpl = gp0.pl;
        gpipe = gp0.pipe;
        VkDescriptorSetLayout both[2] = {gdsl, gdsl};
        const uint32_t counts[2] = {texCount, texCount};  // the texture array of binding 11 is variable sized
        VkDescriptorSetVariableDescriptorCountAllocateInfo vc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
        vc.descriptorSetCount = 2;
        vc.pDescriptorCounts = counts;
        VkDescriptorSetAllocateInfo gdai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        gdai.pNext = &vc;
        gdai.descriptorPool = pool;
        gdai.descriptorSetCount = 2;
        gdai.pSetLayouts = both;
        check(f.vkAllocateDescriptorSets(device, &gdai, gsets), "vkAllocateDescriptorSets(gather)");
        for (int parity = 0; parity < 2; ++parity) {  // parity 0: A -> B, parity 1: B -> A
            const Buffer* gbufs[9] = {&bPos, &bNrm, &bDb, &bIdx, parity ? &bAtlasB : &bAtlasA, parity ? &bAtlasA : &bAtlasB, &bInd, &bMeta, &bAlb};
            VkDescriptorBufferInfo gbi[9];
            VkWriteDescriptorSet gw[11];
            for (uint32_t i = 0; i < 8; ++i) {
                gbi[i] = {gbufs[i]->buffer, 0, VK_WHOLE_SIZE};
                gw[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, gsets[parity], i, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &gbi[i], nullptr};
            }
            gw[8] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &asw, gsets[parity], 8, 0, 1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, nullptr, nullptr, nullptr};
            gbi[8] = {gbufs[8]->buffer, 0, VK_WHOLE_SIZE};
            gw[9] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, gsets[parity], 9, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &gbi[8], nullptr};
            VkDescriptorBufferInfo twi{bTWords.buffer, 0, VK_WHOLE_SIZE};
            gw[10] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, gsets[parity], 11, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &twi, nullptr};
            f.vkUpdateDescriptorSets(device, 11, gw, 0, nullptr);
            writeTextureDescriptors(f, device, gsets[parity], *gtex);
        }
    }

    // ---- record + run ----
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = chosenQueue;
    VkCommandPool cmdPool;
    check(f.vkCreateCommandPool(device, &cpci, nullptr, &cmdPool), "vkCreateCommandPool");
    cleanup.add([&f, device, cmdPool] { f.vkDestroyCommandPool(device, cmdPool, nullptr); });
    VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbi.commandPool = cmdPool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    VkCommandBuffer cmd;
    check(f.vkAllocateCommandBuffers(device, &cbi, &cmd), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(f.vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
    if (useRT) {
        // Barriers (NOT verifiable with the validation layer on lavapipe, reviewed by hand, see VULKAN_RT_SANDBOX_GUIDE.md §5/§6):
        //   1. build BLAS            2. BLAS write -> TLAS build read      3. build TLAS
        //   4. TLAS write -> ray query reads in the compute shader        5. dispatch    6. shader write -> host read
        const VkAccelerationStructureBuildRangeInfoKHR* ranges[1];
        ranges[0] = &blasRange;
        f.vkCmdBuildAccelerationStructuresKHR(cmd, 1, &blasBuild, ranges);
        VkMemoryBarrier b1{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b1.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        b1.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &b1, 0, nullptr, 0, nullptr);
        ranges[0] = &tlasRange;
        f.vkCmdBuildAccelerationStructuresKHR(cmd, 1, &tlasBuild, ranges);
        VkMemoryBarrier b2{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b2.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        b2.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b2, 0, nullptr, 0, nullptr);
    }
    f.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    f.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, nullptr);
    const uint32_t maxGroups = std::max<uint32_t>(1, std::min<uint32_t>(props.limits.maxComputeWorkGroupCount[0], 65535));
    PushConstants push{};
    push.count = static_cast<uint32_t>(n);
    push.lightCount = static_cast<uint32_t>(scene.lights.size());
    push.areaSamples = static_cast<uint32_t>(std::max(1, options.areaSamples));
    push.skySamples = static_cast<uint32_t>(std::max(1, options.skySamples));
    push.bias = static_cast<float>(kShadowBias);
    push.farDistance = static_cast<float>(kShadowFar);
    push.ambient[0] = static_cast<float>(scene.ambient.x);
    push.ambient[1] = static_cast<float>(scene.ambient.y);
    push.ambient[2] = static_cast<float>(scene.ambient.z);
    push.gi = (useGi ? 1u : 0u) | (options.shadows ? 2u : 0u) | (wantDb && !useGi ? 4u : 0u) | (transparent ? 8u : 0u);
    push.sampleOffset = options.sampleOffset;
    push.superSamples = superSamples;
    for (uint64_t base = 0; base < n; base += static_cast<uint64_t>(maxGroups) * 64) {
        push.base = static_cast<uint32_t>(base);
        const uint64_t groups = std::min<uint64_t>(maxGroups, (n - base + 63) / 64);
        f.vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        f.vkCmdDispatch(cmd, static_cast<uint32_t>(groups), 1, 1);
    }
    if (useGi) {
        // direct pass -> gather 0 -> gather 1 ...: each reads what the previous wrote (ping-pong atlas buffers A <-> B)
        VkMemoryBarrier cb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        cb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        cb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        f.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gpipe);
        GatherPush gp{};
        gp.count = static_cast<uint32_t>(n);
        gp.giSamples = static_cast<uint32_t>(std::max(1, options.giSamples));
        gp.atlas = static_cast<uint32_t>(samples.atlas);
        gp.albedo = static_cast<float>(options.albedo);
        gp.bias = static_cast<float>(kShadowBias);
        gp.farDistance = static_cast<float>(kShadowFar);
        gp.sampleOffset = options.sampleOffset;
        gp.sAtlas = static_cast<uint32_t>(samples.atlas);
        const int baseBounces = fine ? options.bounces - 1 : options.bounces;
        for (int it = 0; it < options.bounces; ++it) {
            const bool last = fine && it == options.bounces - 1;  // the last bounce runs on the fine grid: entries [n, n + m), no bounce source written
            const uint64_t first = last ? n : 0, end = last ? n + m : n;
            f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &cb, 0, nullptr, 0, nullptr);
            f.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gpl, 0, 1, &gsets[it & 1], 0, nullptr);
            gp.count = static_cast<uint32_t>(end);
            gp.atlas = last ? static_cast<uint32_t>(gi->s.atlas) : static_cast<uint32_t>(samples.atlas);
            gp.flags = (last ? 1u : 0u) | (textured ? 2u : 0u) | (transparent ? 4u : 0u);
            for (uint64_t base = first; base < end; base += static_cast<uint64_t>(maxGroups) * 64) {
                gp.base = static_cast<uint32_t>(base);
                const uint64_t groups = std::min<uint64_t>(maxGroups, (end - base + 63) / 64);
                f.vkCmdPushConstants(cmd, gpl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(gp), &gp);
                f.vkCmdDispatch(cmd, static_cast<uint32_t>(groups), 1, 1);
            }
        }
        (void)baseBounces;
    }
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};  // shader writes -> host reads
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    check(f.vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    check(f.vkCreateFence(device, &fi, nullptr, &fence), "vkCreateFence");
    cleanup.add([&f, device, fence] { f.vkDestroyFence(device, fence, nullptr); });
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    check(f.vkQueueSubmit(queue, 1, &si, fence), "vkQueueSubmit");
    check(f.vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    out.resize(n * 4);
    if (n) std::memcpy(out.data(), bOut.mapped, n * 16);
    if (indirectOut) indirectOut->assign(n * 4, 0.f);
    if (giOut) giOut->clear();
    if (fine && n) {  // the last bounce was gathered on the fine grid: hand that out raw and average it per texel
        const float* grid = static_cast<const float*>(bInd.mapped) + n * 4;
        std::vector<float> g(grid, grid + m * 4);
        const std::vector<float> ind = resolveGi(*gi, g);
        for (size_t i = 0; i < n * 4; ++i) out[i] += ind[i];
        if (indirectOut) *indirectOut = ind;
        if (giOut) *giOut = std::move(g);
    } else if (useGi && n) {  // total = direct + the last bounce's indirect light
        const float* ind = static_cast<const float*>(bInd.mapped);
        for (size_t i = 0; i < n * 4; ++i) out[i] += ind[i];
        if (indirectOut) std::memcpy(indirectOut->data(), ind, n * 16);
    }
    if (wantDb) {
        dbOut->resize(n * 4);
        if (n) std::memcpy(dbOut->data(), bDb.mapped, n * 16);
    }
    result.ok = true;
}


// ---- live session -----------------------------------------------------------------------------------------------------------
// Where a buffer lives. Inputs the CPU writes and the GPU reads go to device local, host visible memory (BAR) when the driver has it,
// results the CPU reads back to cached host memory (uncached write-combined memory reads at a fraction of the speed).
Buffer allocBuffer(Context& c, Cleanup& cleanup, VkDeviceSize size, VkBufferUsageFlags usage, MemKind kind, bool address, bool storageRange) {
    Fns& f = c.f;
    const VkDevice device = c.device;
    if (storageRange && size > c.props.limits.maxStorageBufferRange) fail("buffer of " + std::to_string(size) + " bytes exceeds maxStorageBufferRange");
    // The buffer goes back to the pool when the session (the cleanup stack it is registered on) ends; a pooled one that fits is reused as is.
    auto giveBack = [&c, usage, kind, address](const Buffer& buf) { c.bufPool.give({buf, usage, static_cast<int>(kind), address}); };
    Buffer b;
    if (c.bufPool.take(size, usage, static_cast<int>(kind), address, b)) {
        b.size = size;
        const Buffer held = b;
        cleanup.add([giveBack, held] { giveBack(held); });
        return b;
    }
    // a little headroom, so that an edit that adds a few texels still finds this buffer in the pool (never beyond what a storage binding may span)
    VkDeviceSize capacity = size >= 65536 ? alignUp(size + size / 8, 4096) : size;
    if (storageRange) capacity = std::max(size, std::min<VkDeviceSize>(capacity, c.props.limits.maxStorageBufferRange));
    b.size = size;
    b.capacity = capacity;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = capacity;
    bi.usage = usage | (address ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(f.vkCreateBuffer(device, &bi, nullptr, &b.buffer), "vkCreateBuffer");
    VkBuffer buf = b.buffer;
    bool owned = false;  // true once the buffer is on the cleanup stack (it frees itself from there)
    struct Guard {  // a failure below must not leak the VkBuffer
        Fns& f; VkDevice device; VkBuffer buf; bool& owned;
        ~Guard() { if (!owned) f.vkDestroyBuffer(device, buf, nullptr); }
    } guard{f, device, buf, owned};
    VkMemoryRequirements mr;
    f.vkGetBufferMemoryRequirements(device, b.buffer, &mr);
    const VkMemoryPropertyFlags kHost = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    std::vector<uint32_t> candidates;  // memory types in order of preference; allocation falls through to the next one on failure
    auto add = [&](auto pred) {
        for (uint32_t i = 0; i < c.mem.memoryTypeCount; ++i) {
            if (!(mr.memoryTypeBits & (1u << i)) || !pred(c.mem.memoryTypes[i].propertyFlags)) continue;
            if (std::find(candidates.begin(), candidates.end(), i) == candidates.end()) candidates.push_back(i);
        }
    };
    const auto isHost = [&](VkMemoryPropertyFlags fl) { return (fl & kHost) == kHost; };
    switch (kind) {
        case MemKind::Host: add(isHost); break;
        case MemKind::HostCached:
            add([&](VkMemoryPropertyFlags fl) { return isHost(fl) && (fl & VK_MEMORY_PROPERTY_HOST_CACHED_BIT); });
            add(isHost);
            break;
        case MemKind::DeviceHost:
            add([&](VkMemoryPropertyFlags fl) { return isHost(fl) && (fl & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); });
            add(isHost);
            break;
        case MemKind::Device:
            add([&](VkMemoryPropertyFlags fl) { return (fl & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && !(fl & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT); });
            add([&](VkMemoryPropertyFlags fl) { return (fl & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0; });
            add([&](VkMemoryPropertyFlags) { return true; });
            break;
    }
    if (candidates.empty()) fail("no suitable memory type");
    VkMemoryAllocateFlagsInfo flagsInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkResult last = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    for (uint32_t type : candidates) {
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.pNext = address ? &flagsInfo : nullptr;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = type;
        last = f.vkAllocateMemory(device, &ai, nullptr, &b.memory);
        if (last == VK_SUCCESS) break;
        b.memory = VK_NULL_HANDLE;
    }
    check(last, "vkAllocateMemory");
    const VkDeviceMemory m = b.memory;
    struct MemGuard { Fns& f; VkDevice device; VkDeviceMemory m; bool& owned; ~MemGuard() { if (!owned) f.vkFreeMemory(device, m, nullptr); } } memGuard{f, device, m, owned};
    check(f.vkBindBufferMemory(device, b.buffer, b.memory, 0), "vkBindBufferMemory");
    if (kind != MemKind::Device) check(f.vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped), "vkMapMemory");
    if (address) {
        VkBufferDeviceAddressInfo dai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        dai.buffer = b.buffer;
        b.address = f.vkGetBufferDeviceAddress(device, &dai);
    }
    owned = true;
    const Buffer held = b;
    cleanup.add([giveBack, held] { giveBack(held); });
    return b;
}

class VkLive final : public LiveSession {
public:
    std::shared_ptr<Context> ctx;  // before `cleanup`: members die in reverse order, so the device outlives everything below
    Cleanup cleanup;
    LiveSessionConfig cfg;
    std::string err, dev;
    std::vector<std::string> val;
    double gpuSec = 0;
    size_t n = 0, m = 0, atlasTexels = 0, lightCap = 0;  // n base samples, m fine GI grid entries behind them
    uint32_t atlasSize = 0, lightCount = 0;
    float ambient[3] = {0, 0, 0};
    Buffer bPos, bNrm, bLights, bOut, bDb, bDx, bInd, bIdx, bAtlas, bScratch, bDummy, bMeta, bSub, bAlb, bTMeta, bTWords, bOcc;
    bool transparent = false;          // the mesh has transparent surfaces (direct bit 3, gather bit 2; the geometry is not opaque)
    std::shared_ptr<GpuTextures> tex;  // after `ctx`: released first
    bool textured = false;             // bounces use the albedo textures (flag bit 1 of the gather)
    uint32_t superSamples = 1;  // sub-texel positions per texel (1 = off)
    VkDescriptorSet dset = VK_NULL_HANDLE, gset = VK_NULL_HANDLE, aset = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    static constexpr size_t kMaxParts = 8;  // submissions a runParts() step is split into at most
    VkCommandBuffer partCmd[kMaxParts] = {};
    VkFence partFence[kMaxParts] = {};
    VkQueryPool queries = VK_NULL_HANDLE;   // start / end timestamps of a runParts() step (none when the queue cannot write them)

    ~VkLive() override {
        if (ctx && ctx->device && ctx->f.vkDeviceWaitIdle) ctx->f.vkDeviceWaitIdle(ctx->device);
    }

    int key() const { return 1 | (cfg.validation ? 2 : 0); }

    void init(const TexelSamples& S, const OccluderMesh& mesh, const LightScene& L, const std::vector<float>& atlasInit) {
        ctx = contextFor(true, cfg.validation);
        Context& c = *ctx;
        c.collector.messages.clear();
        dev = c.deviceName;
        Fns& f = c.f;
        const VkDevice device = c.device;
        m = cfg.giEntries;
        if (S.count() < m + 1) fail("no samples");
        n = S.count() - m;
        if (mesh.empty()) fail("no occluders");
        if (cfg.gather && !mesh.hasLightmap()) fail("bounces need occluders with lightmap coordinates");
        const uint32_t triangles = static_cast<uint32_t>(mesh.triangleCount());
        if (c.maxPrimitives && triangles > c.maxPrimitives) fail("more occluder triangles than the device supports");
        atlasSize = static_cast<uint32_t>(S.atlas);
        atlasTexels = static_cast<size_t>(S.pageCount) * static_cast<size_t>(S.atlas) * static_cast<size_t>(S.atlas);
        lightCap = std::max<size_t>(32, L.lights.size() * 2);

        const VkBufferUsageFlags kStorage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        auto mk = [&](VkDeviceSize size, MemKind kind) { return allocBuffer(c, cleanup, std::max<VkDeviceSize>(size, 16), kStorage, kind, false, true); };
        const VkDeviceSize sampleBytes = n * 16, totalBytes = (n + m) * 16;
        bPos = mk(totalBytes, MemKind::DeviceHost);
        bNrm = mk(totalBytes, MemKind::DeviceHost);
        bIdx = mk((n + m) * 4, MemKind::DeviceHost);
        bLights = mk((lightCap + kSkyTableRecords) * sizeof(GpuLight), MemKind::DeviceHost);
        bOut = mk(sampleBytes, MemKind::HostCached);
        bOcc = mk(n * 4, MemKind::HostCached);                     // ambient occlusion, one float per base sample
        bDb = mk(sampleBytes, MemKind::HostCached);
        bDx = mk(sampleBytes, MemKind::HostCached);                // the sky + rect share of the direct light (what a client cannot predict)
        bInd = mk(cfg.gather ? totalBytes : 16, MemKind::HostCached);
        bDummy = mk(sampleBytes, MemKind::Device);                 // gather's db input: only feeds its (unused) atlas write
        bAtlas = mk(atlasTexels * 16, MemKind::DeviceHost);        // resident bounce source, written by the CPU between steps
        bScratch = mk(atlasTexels * 16, MemKind::Device);          // the shaders' own atlas output, never read
        bMeta = mk(cfg.gather ? mesh.lightmap.size() * sizeof(float) : 16, MemKind::DeviceHost);
        textured = cfg.gather && mesh.hasAlbedo();
        bAlb = mk(textured ? mesh.albedoMeta.size() * sizeof(float) : 16, MemKind::DeviceHost);
        std::memset(bAlb.mapped, 0, static_cast<size_t>(bAlb.size));
        tex = uploadTextures(c, textured ? mesh.albedo.get() : nullptr);  // (a 1x1 white placeholder without bounces: the descriptor needs an image)
        if (textured) packAlbedoMeta(mesh, tex->used, static_cast<float*>(bAlb.mapped));
        {
            const TransparencyBuffers tb = transparencyOf(mesh);
            transparent = tb.on;
            bTMeta = mk(tb.meta.size() * sizeof(float), MemKind::DeviceHost);
            bTWords = mk(tb.words.size() * 4, MemKind::DeviceHost);
            std::memcpy(bTMeta.mapped, tb.meta.data(), tb.meta.size() * sizeof(float));
            std::memcpy(bTWords.mapped, tb.words.data(), tb.words.size() * 4);
        }
        superSamples = S.hasSub() ? static_cast<uint32_t>(std::clamp(cfg.superSamples, 1, kMaxSuperSamples)) : 1u;
        bSub = mk(superSamples > 1 ? n * 48 : 16, MemKind::DeviceHost);
        if (superSamples > 1) std::memcpy(bSub.mapped, S.sub.data(), n * 48);
        else std::memset(bSub.mapped, 0, 16);
        float* dp = static_cast<float*>(bPos.mapped);
        float* dn = static_cast<float*>(bNrm.mapped);
        // (tens of millions of entries on a big level: on all cores)
        parallelFor(n, 8192, [&](size_t b0, size_t e0) {
            for (size_t i = b0; i < e0; ++i) {
                dp[i * 4 + 0] = S.pos[i * 4 + 0];
                dp[i * 4 + 1] = S.pos[i * 4 + 1];
                dp[i * 4 + 2] = S.pos[i * 4 + 2];
                const uint32_t id = S.seedOf(i);  // the random-number id travels in the bits of w (as in evaluateDirect)
                std::memcpy(&dp[i * 4 + 3], &id, sizeof(id));
                dn[i * 4 + 0] = S.nrm[i * 4 + 0];
                dn[i * 4 + 1] = S.nrm[i * 4 + 1];
                dn[i * 4 + 2] = S.nrm[i * 4 + 2];
                dn[i * 4 + 3] = 0.f;  // pass offsets are push constants here, never per sample
            }
        });
        // grid entries: position, normal and the texel of the fine atlas (the gather's direction lattice); their w stays 0
        parallelFor(m, 8192, [&](size_t b0, size_t e0) {
            for (size_t e = b0; e < e0; ++e) {
                const size_t i = n + e;
                for (size_t c = 0; c < 3; ++c) { dp[i * 4 + c] = S.pos[i * 4 + c]; dn[i * 4 + c] = S.nrm[i * 4 + c]; }
                dp[i * 4 + 3] = 0.f;
                dn[i * 4 + 3] = 0.f;
            }
        });
        std::memcpy(bIdx.mapped, S.texel.data(), (n + m) * 4);
        if (atlasInit.size() == atlasTexels * 4) std::memcpy(bAtlas.mapped, atlasInit.data(), atlasTexels * 16);
        else std::memset(bAtlas.mapped, 0, atlasTexels * 16);
        if (cfg.gather) std::memcpy(bMeta.mapped, mesh.lightmap.data(), mesh.lightmap.size() * sizeof(float));
        setLightsImpl(L);

        // ---- command pool / buffer / fence ----
        VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpci.queueFamilyIndex = c.chosenQueue;
        VkCommandPool pool;
        check(f.vkCreateCommandPool(device, &cpci, nullptr, &pool), "vkCreateCommandPool");
        cleanup.add([&f, device, pool] { f.vkDestroyCommandPool(device, pool, nullptr); });
        VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbi.commandPool = pool;
        cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbi.commandBufferCount = 1;
        check(f.vkAllocateCommandBuffers(device, &cbi, &cmd), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(f.vkCreateFence(device, &fi, nullptr, &fence), "vkCreateFence");
        const VkFence fenceCopy = fence;
        cleanup.add([&f, device, fenceCopy] { f.vkDestroyFence(device, fenceCopy, nullptr); });
        // the parts of a runParts() step: one command buffer and fence each, and two timestamps for the step's GPU time
        cbi.commandBufferCount = static_cast<uint32_t>(kMaxParts);
        check(f.vkAllocateCommandBuffers(device, &cbi, partCmd), "vkAllocateCommandBuffers(parts)");
        for (VkFence& pf : partFence) {
            check(f.vkCreateFence(device, &fi, nullptr, &pf), "vkCreateFence(parts)");
            const VkFence copy = pf;
            cleanup.add([&f, device, copy] { f.vkDestroyFence(device, copy, nullptr); });
        }
        if (c.timestampBits > 0 && c.props.limits.timestampPeriod > 0.f) {
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 2;
            check(f.vkCreateQueryPool(device, &qi, nullptr, &queries), "vkCreateQueryPool");
            const VkQueryPool copy = queries;
            cleanup.add([&f, device, copy] { f.vkDestroyQueryPool(device, copy, nullptr); });
        }

        // ---- acceleration structures: one BLAS of every occluder triangle, one TLAS with one identity instance (built once) ----
        const VkBufferUsageFlags kBuildInput = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        Buffer vb = allocBuffer(c, cleanup, mesh.tris.size() * sizeof(float), kBuildInput, MemKind::Host, true, false);
        std::memcpy(vb.mapped, mesh.tris.data(), mesh.tris.size() * sizeof(float));
        VkAccelerationStructureGeometryKHR blasGeom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        blasGeom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        // with transparent surfaces the shaders decide per hit (surfaceAt); each triangle once per ray, so translucent layers multiply exactly once
        blasGeom.flags = transparent ? VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR : VK_GEOMETRY_OPAQUE_BIT_KHR;
        blasGeom.geometry.triangles = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
        blasGeom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        blasGeom.geometry.triangles.vertexData.deviceAddress = vb.address;
        blasGeom.geometry.triangles.vertexStride = 3 * sizeof(float);
        blasGeom.geometry.triangles.maxVertex = triangles * 3 - 1;
        blasGeom.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
        VkAccelerationStructureBuildGeometryInfoKHR blasBuild{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        blasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        blasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        blasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        blasBuild.geometryCount = 1;
        blasBuild.pGeometries = &blasGeom;
        VkAccelerationStructureBuildRangeInfoKHR blasRange{}, tlasRange{};
        blasRange.primitiveCount = triangles;
        tlasRange.primitiveCount = 1;
        VkAccelerationStructureKHR blas = VK_NULL_HANDLE;
        auto makeAS = [&](VkAccelerationStructureBuildGeometryInfoKHR& build, VkAccelerationStructureTypeKHR type, uint32_t primitives, VkAccelerationStructureKHR& handle) {
            VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
            f.vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &primitives, &sizes);
            Buffer storage = allocBuffer(c, cleanup, sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, MemKind::Device, true, false);
            VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
            ci.buffer = storage.buffer;
            ci.size = sizes.accelerationStructureSize;
            ci.type = type;
            check(f.vkCreateAccelerationStructureKHR(device, &ci, nullptr, &handle), "vkCreateAccelerationStructureKHR");
            VkAccelerationStructureKHR h = handle;
            cleanup.add([&f, device, h] { f.vkDestroyAccelerationStructureKHR(device, h, nullptr); });  // before its backing buffer
            Buffer scratch = allocBuffer(c, cleanup, sizes.buildScratchSize + c.scratchAlign, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MemKind::Device, true, false);
            build.dstAccelerationStructure = handle;
            build.scratchData.deviceAddress = alignUp(scratch.address, c.scratchAlign);
        };
        makeAS(blasBuild, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, triangles, blas);
        VkAccelerationStructureDeviceAddressInfoKHR bai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        bai.accelerationStructure = blas;
        VkAccelerationStructureInstanceKHR inst{};
        inst.transform.matrix[0][0] = inst.transform.matrix[1][1] = inst.transform.matrix[2][2] = 1.f;
        inst.mask = 0xFF;
        inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;  // double sided
        inst.accelerationStructureReference = f.vkGetAccelerationStructureDeviceAddressKHR(device, &bai);
        Buffer ib = allocBuffer(c, cleanup, sizeof(inst), kBuildInput, MemKind::Host, true, false);
        std::memcpy(ib.mapped, &inst, sizeof(inst));
        VkAccelerationStructureGeometryKHR tlasGeom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        tlasGeom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        tlasGeom.flags = transparent ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;
        tlasGeom.geometry.instances = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
        tlasGeom.geometry.instances.arrayOfPointers = VK_FALSE;
        tlasGeom.geometry.instances.data.deviceAddress = ib.address;
        VkAccelerationStructureBuildGeometryInfoKHR tlasBuild{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        tlasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        tlasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        tlasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        tlasBuild.geometryCount = 1;
        tlasBuild.pGeometries = &tlasGeom;
        VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
        makeAS(tlasBuild, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, 1, tlas);

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(f.vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
        const VkAccelerationStructureBuildRangeInfoKHR* ranges[1] = {&blasRange};
        f.vkCmdBuildAccelerationStructuresKHR(cmd, 1, &blasBuild, ranges);
        VkMemoryBarrier b1{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b1.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        b1.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &b1, 0, nullptr, 0, nullptr);
        ranges[0] = &tlasRange;
        f.vkCmdBuildAccelerationStructuresKHR(cmd, 1, &tlasBuild, ranges);
        VkMemoryBarrier b2{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b2.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        b2.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b2, 0, nullptr, 0, nullptr);
        check(f.vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
        submitAndWait();

        // ---- descriptors (written once: every buffer has a fixed size for the life of the session) ----
        const DirectPipe& dp0 = c.directPipe();
        const GatherPipe& gp0 = c.gatherPipe();
        const DirectPipe& ap0 = c.aoPipe();
        const uint32_t texCount = tex ? static_cast<uint32_t>(tex->views.size()) : 1u;
        VkDescriptorPoolSize ps[4] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 29}, {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 3},
                                      {VK_DESCRIPTOR_TYPE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, texCount}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 3;
        dpi.poolSizeCount = 4;
        dpi.pPoolSizes = ps;
        VkDescriptorPool dpool;
        check(f.vkCreateDescriptorPool(device, &dpi, nullptr, &dpool), "vkCreateDescriptorPool");
        cleanup.add([&f, device, dpool] { f.vkDestroyDescriptorPool(device, dpool, nullptr); });
        {   // the direct set has fixed bindings, the gather set a variable sized texture array: two allocations
            VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            dai.descriptorPool = dpool;
            dai.descriptorSetCount = 1;
            dai.pSetLayouts = &dp0.dsl;
            check(f.vkAllocateDescriptorSets(device, &dai, &dset), "vkAllocateDescriptorSets");
            dai.pSetLayouts = &ap0.dsl;
            check(f.vkAllocateDescriptorSets(device, &dai, &aset), "vkAllocateDescriptorSets(ao)");
            VkDescriptorSetVariableDescriptorCountAllocateInfo vc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
            vc.descriptorSetCount = 1;
            vc.pDescriptorCounts = &texCount;
            dai.pNext = &vc;
            dai.pSetLayouts = &gp0.dsl;
            check(f.vkAllocateDescriptorSets(device, &dai, &gset), "vkAllocateDescriptorSets(gather)");
        }
        VkWriteDescriptorSetAccelerationStructureKHR asw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        asw.accelerationStructureCount = 1;
        asw.pAccelerationStructures = &tlas;
        // direct shader (RAY_QUERY): pos nrm lights result | tlas | bounce result, texel index, atlas0 (unused: gi bit 0 stays clear)
        const Buffer* dbufs[kDirectRtBindings] = {&bPos, &bNrm, &bLights, &bOut, nullptr, &bDb, &bIdx, &bScratch, &bSub, &bTMeta, &bTWords, &bDx};
        // gather: pos nrm db(dummy) texel index | atlas in, atlas out(scratch), indirect, triangle meta | tlas
        const Buffer* gbufs[10] = {&bPos, &bNrm, &bDummy, &bIdx, &bAtlas, &bScratch, &bInd, &bMeta, nullptr, &bAlb};
        VkDescriptorBufferInfo infos[kDirectRtBindings + 11 + kAoBindings];
        VkWriteDescriptorSet w[kDirectRtBindings + 11 + kAoBindings];
        uint32_t k = 0;
        for (int set = 0; set < 2; ++set) {
            const Buffer* const* bufs = set == 0 ? dbufs : gbufs;
            const uint32_t count = set == 0 ? kDirectRtBindings : 10;
            const uint32_t asBinding = set == 0 ? 4 : 8;
            for (uint32_t i = 0; i < count; ++i, ++k) {
                if (i == asBinding) {
                    w[k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &asw, set == 0 ? dset : gset, i, 0, 1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, nullptr, nullptr, nullptr};
                } else {
                    infos[k] = {bufs[i]->buffer, 0, VK_WHOLE_SIZE};
                    w[k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set == 0 ? dset : gset, i, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &infos[k], nullptr};
                }
            }
        }
        infos[k] = {bTWords.buffer, 0, VK_WHOLE_SIZE};  // gather binding 11: transparency words
        w[k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, gset, 11, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &infos[k], nullptr};
        ++k;
        // ambient occlusion: pos nrm result | tlas | triangle records, transparency words
        const Buffer* abufs[kAoBindings] = {&bPos, &bNrm, &bOcc, nullptr, &bTMeta, &bTWords};
        for (uint32_t i = 0; i < kAoBindings; ++i, ++k) {
            if (i == 3) {
                w[k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &asw, aset, i, 0, 1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, nullptr, nullptr, nullptr};
            } else {
                infos[k] = {abufs[i]->buffer, 0, VK_WHOLE_SIZE};
                w[k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, aset, i, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &infos[k], nullptr};
            }
        }
        f.vkUpdateDescriptorSets(device, k, w, 0, nullptr);
        writeTextureDescriptors(f, device, gset, *tex);
    }

    void submitAndWait() {
        Fns& f = ctx->f;
        const VkDevice device = ctx->device;
        check(f.vkResetFences(device, 1, &fence), "vkResetFences");
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        const auto t0 = std::chrono::steady_clock::now();
        check(f.vkQueueSubmit(ctx->queue, 1, &si, fence), "vkQueueSubmit");
        check(f.vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        gpuSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }

    bool setLightsImpl(const LightScene& L) {
        if (L.lights.size() > lightCap) return false;
        const std::vector<GpuLight> g = packLights(L.lights);
        if (!g.empty()) std::memcpy(bLights.mapped, g.data(), g.size() * sizeof(GpuLight));
        lightCount = static_cast<uint32_t>(L.lights.size());
        ambient[0] = static_cast<float>(L.ambient.x);
        ambient[1] = static_cast<float>(L.ambient.y);
        ambient[2] = static_cast<float>(L.ambient.z);
        return true;
    }

    // ---- LiveSession ----
    bool setLights(const LightScene& L) override {
        std::lock_guard<std::mutex> lock(g_mutex);
        return setLightsImpl(L);  // the GPU is idle between steps (run() waits for its fence)
    }
    float* atlas() override { return static_cast<float*>(bAtlas.mapped); }
    float* samplePositions() override { return static_cast<float*>(bPos.mapped); }
    float* sampleNormals() override { return static_cast<float*>(bNrm.mapped); }
    uint32_t* sampleTexels() override { return static_cast<uint32_t*>(bIdx.mapped); }
    size_t sampleCount() const override { return n; }
    const std::string& error() const override { return err; }
    const std::string& device() const override { return dev; }
    const std::vector<std::string>& validation() const override { return val; }
    double lastGpuSeconds() const override { return gpuSec; }

    bool run(const std::vector<Job>& jobs, std::vector<float>& d, std::vector<float>& db, std::vector<float>* ind) override {
        std::lock_guard<std::mutex> lock(g_mutex);
        try {
            runImpl(jobs, d, db, ind);
            val = ctx->collector.messages;
            return true;
        } catch (const std::exception& e) {
            err = e.what();
            g_contexts.erase(key());  // a lost device stays lost: the next session starts from a fresh one
            return false;
        }
    }

    bool occlusion(const std::vector<Job>& jobs, double radius, int rays, double falloff, std::vector<float>& occ) override {
        std::lock_guard<std::mutex> lock(g_mutex);
        try {
            occlusionImpl(jobs, radius, rays, falloff, occ);
            val = ctx->collector.messages;
            return true;
        } catch (const std::exception& e) {
            err = e.what();
            g_contexts.erase(key());
            return false;
        }
    }

    void occlusionImpl(const std::vector<Job>& jobs, double radius, int rays, double falloff, std::vector<float>& occ) {
        Fns& f = ctx->f;
        size_t total = 0;
        for (const Job& j : jobs) {
            if (static_cast<size_t>(j.first) + j.count > n) fail("job outside the resident samples");
            total += j.count;
        }
        occ.assign(total, 0.f);
        if (!total) return;
        std::vector<Job> order = jobs;
        std::sort(order.begin(), order.end(), [](const Job& a, const Job& b) { return a.first < b.first; });
        std::vector<Job> merged;  // adjacent ranges become one dispatch
        for (const Job& j : order) {
            if (!j.count) continue;
            if (!merged.empty() && merged.back().first + merged.back().count == j.first) merged.back().count += j.count;
            else merged.push_back(j);
        }
        const DirectPipe& ap = ctx->aoPipe();
        const uint32_t maxGroups = std::max<uint32_t>(1, std::min<uint32_t>(ctx->props.limits.maxComputeWorkGroupCount[0], 65535));
        check(f.vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(f.vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
        AoPush push{};
        push.rays = static_cast<uint32_t>(std::max(1, rays));
        push.flags = transparent ? 1u : 0u;
        push.radius = static_cast<float>(radius);
        push.bias = static_cast<float>(kAoBias);
        push.falloff = static_cast<float>(falloff);
        f.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ap.pipe);
        f.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ap.pl, 0, 1, &aset, 0, nullptr);
        for (const Job& j : merged)
            for (uint64_t base = j.first; base < static_cast<uint64_t>(j.first) + j.count; base += static_cast<uint64_t>(maxGroups) * 64) {
                push.base = static_cast<uint32_t>(base);
                push.count = j.first + j.count;
                const uint64_t groups = std::min<uint64_t>(maxGroups, (push.count - base + 63) / 64);
                f.vkCmdPushConstants(cmd, ap.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
                f.vkCmdDispatch(cmd, static_cast<uint32_t>(groups), 1, 1);
            }
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};  // shader writes -> host reads
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        f.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        check(f.vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
        submitAndWait();
        size_t at = 0;
        for (const Job& j : jobs) {
            std::memcpy(occ.data() + at, static_cast<const float*>(bOcc.mapped) + j.first, static_cast<size_t>(j.count) * 4);
            at += j.count;
        }
    }

    // Totals of the jobs (base samples, grid entries); throws on a job outside the resident samples.
    std::pair<size_t, size_t> checkJobs(const std::vector<Job>& jobs) const {
        size_t total = 0, giTotal = 0;
        for (const Job& j : jobs) {
            if (static_cast<size_t>(j.first) + j.count > n) fail("job outside the resident samples");
            if (m && j.giCount && (static_cast<size_t>(j.giFirst) + j.giCount > n + m || j.giFirst < n)) fail("job outside the resident grid entries");
            total += j.count;
            giTotal += j.giCount;
        }
        return {total, m ? giTotal : 0};
    }

    // Records the direct light and (with cfg.gather) one gather pass of the jobs into `cb`, then the barrier that makes the results visible to the host.
    void recordJobs(VkCommandBuffer cb, const std::vector<Job>& jobs) {
        Fns& f = ctx->f;
        // adjacent ranges with the same draw offset become one dispatch (no barriers between dispatches: they overlap on the GPU)
        std::vector<Job> order = jobs;
        std::sort(order.begin(), order.end(), [](const Job& a, const Job& b) { return a.first < b.first; });
        std::vector<Job> merged;
        for (const Job& j : order) {
            if (!j.count) continue;
            if (!merged.empty() && merged.back().first + merged.back().count == j.first && merged.back().offset == j.offset) merged.back().count += j.count;
            else merged.push_back(j);
        }
        // the same for the grid entries (their own offsets are the jobs' pass offsets)
        std::vector<Job> mergedGi;
        if (m) {
            std::vector<Job> og = jobs;
            std::sort(og.begin(), og.end(), [](const Job& a, const Job& b) { return a.giFirst < b.giFirst; });
            for (const Job& j : og) {
                if (!j.giCount) continue;
                if (!mergedGi.empty() && mergedGi.back().giFirst + mergedGi.back().giCount == j.giFirst && mergedGi.back().offset == j.offset) mergedGi.back().giCount += j.giCount;
                else mergedGi.push_back(j);
            }
        }
        const DirectPipe& dp = ctx->directPipe();
        const GatherPipe& gp = ctx->gatherPipe();
        const uint32_t maxGroups = std::max<uint32_t>(1, std::min<uint32_t>(ctx->props.limits.maxComputeWorkGroupCount[0], 65535));
        PushConstants push{};
        push.lightCount = lightCount;
        push.areaSamples = static_cast<uint32_t>(std::max(1, cfg.areaSamples));
        push.skySamples = static_cast<uint32_t>(std::max(1, cfg.skySamples));
        push.bias = static_cast<float>(kShadowBias);
        push.farDistance = static_cast<float>(kShadowFar);
        std::memcpy(push.ambient, ambient, sizeof(ambient));
        push.gi = 2u | 4u | 16u | (transparent ? 8u : 0u);  // ray traced shadows, write the bounce-weighted direct light only (and the sky + rect share); transparency
        push.superSamples = superSamples;
        f.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, dp.pipe);
        f.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, dp.pl, 0, 1, &dset, 0, nullptr);
        for (const Job& j : merged) {
            push.sampleOffset = j.offset;
            for (uint64_t base = j.first; base < static_cast<uint64_t>(j.first) + j.count; base += static_cast<uint64_t>(maxGroups) * 64) {
                push.base = static_cast<uint32_t>(base);
                push.count = j.first + j.count;
                const uint64_t groups = std::min<uint64_t>(maxGroups, (push.count - base + 63) / 64);
                f.vkCmdPushConstants(cb, dp.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
                f.vkCmdDispatch(cb, static_cast<uint32_t>(groups), 1, 1);
            }
        }
        if (cfg.gather) {
            f.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gp.pipe);
            f.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, gp.pl, 0, 1, &gset, 0, nullptr);
            GatherPush g{};
            g.giSamples = static_cast<uint32_t>(std::max(1, cfg.giSamples));
            g.albedo = static_cast<float>(cfg.albedo);
            g.bias = static_cast<float>(kShadowBias);
            g.farDistance = static_cast<float>(kShadowFar);
            g.sAtlas = atlasSize;
            // fine grid: gather over the grid entries (fine atlas for the directions, the bounce source is not written); else over the samples
            g.atlas = m ? static_cast<uint32_t>(cfg.giAtlas) : atlasSize;
            g.flags = (m ? 1u : 0u) | (textured ? 2u : 0u) | (transparent ? 4u : 0u);
            for (const Job& j : (m ? mergedGi : merged)) {
                const uint32_t first = m ? j.giFirst : j.first, cnt = m ? j.giCount : j.count;
                g.sampleOffset = j.offset;
                for (uint64_t base = first; base < static_cast<uint64_t>(first) + cnt; base += static_cast<uint64_t>(maxGroups) * 64) {
                    g.base = static_cast<uint32_t>(base);
                    g.count = first + cnt;
                    const uint64_t groups = std::min<uint64_t>(maxGroups, (g.count - base + 63) / 64);
                    f.vkCmdPushConstants(cb, gp.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(g), &g);
                    f.vkCmdDispatch(cb, static_cast<uint32_t>(groups), 1, 1);
                }
            }
        }
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};  // shader writes -> host reads
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        f.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    }

    void runImpl(const std::vector<Job>& jobs, std::vector<float>& d, std::vector<float>& db, std::vector<float>* ind) {
        Fns& f = ctx->f;
        const auto [total, giTotal] = checkJobs(jobs);
        d.assign(total * 4, 0.f);
        db.assign(total * 4, 0.f);
        if (ind) ind->assign((m ? giTotal : total) * 4, 0.f);
        if (!total) return;
        check(f.vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(f.vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
        recordJobs(cmd, jobs);
        check(f.vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
        submitAndWait();
        size_t at = 0, gat = 0;
        for (const Job& j : jobs) {
            const size_t bytes = static_cast<size_t>(j.count) * 16, src = static_cast<size_t>(j.first) * 4;
            std::memcpy(d.data() + at * 4, static_cast<const float*>(bOut.mapped) + src, bytes);
            std::memcpy(db.data() + at * 4, static_cast<const float*>(bDb.mapped) + src, bytes);
            if (ind && !m) std::memcpy(ind->data() + at * 4, static_cast<const float*>(bInd.mapped) + src, bytes);
            if (ind && m && j.giCount) {
                std::memcpy(ind->data() + gat * 4, static_cast<const float*>(bInd.mapped) + static_cast<size_t>(j.giFirst) * 4, static_cast<size_t>(j.giCount) * 16);
                gat += j.giCount;
            }
            at += j.count;
        }
    }

    bool runParts(const std::vector<Job>& jobs, Indirect indirect, size_t maxParts, const PartDone& done) override {
        std::lock_guard<std::mutex> lock(g_mutex);
        try {
            runPartsImpl(jobs, indirect, maxParts, done);
            val = ctx->collector.messages;
            return true;
        } catch (const std::exception& e) {
            err = e.what();
            g_contexts.erase(key());
            return false;
        }
    }

    // One submission per part, each with its own fence: while the caller merges part k, the GPU works on the parts behind it. The parts are
    // independent (disjoint result ranges; every gather reads the bounce source as it was before the call), so no barriers separate them.
    void runPartsImpl(const std::vector<Job>& jobs, Indirect indirect, size_t maxParts, const PartDone& done) {
        Fns& f = ctx->f;
        const VkDevice device = ctx->device;
        const auto [total, giTotal] = checkJobs(jobs);
        gpuSec = 0;
        if (!total) return;
        // contiguous groups of jobs with about the same work (base samples + grid entries)
        const size_t parts = std::clamp<size_t>(maxParts, 1, std::min(kMaxParts, jobs.size()));
        const size_t per = (total + giTotal + parts - 1) / parts;
        std::vector<size_t> cut{0};
        size_t acc = 0;
        for (size_t i = 0; i + 1 < jobs.size() && cut.size() < parts; ++i) {
            acc += jobs[i].count + jobs[i].giCount;
            if (acc >= per * cut.size()) cut.push_back(i + 1);
        }
        cut.push_back(jobs.size());
        const size_t np = cut.size() - 1;
        const bool timed = queries != VK_NULL_HANDLE;
        const auto t0 = std::chrono::steady_clock::now();
        // every submitted part is waited for before this returns, also when a later one fails or the caller's merge throws (the buffers are reused)
        size_t submitted = 0, waited = 0;
        struct Drain {
            Fns& f; VkDevice device; VkFence* fences; size_t& submitted; size_t& waited;
            ~Drain() { for (size_t k = waited; k < submitted; ++k) f.vkWaitForFences(device, 1, &fences[k], VK_TRUE, UINT64_MAX); }
        } drain{f, device, partFence, submitted, waited};
        for (size_t k = 0; k < np; ++k) {
            const std::vector<Job> part(jobs.begin() + static_cast<std::ptrdiff_t>(cut[k]), jobs.begin() + static_cast<std::ptrdiff_t>(cut[k + 1]));
            VkCommandBuffer cb = partCmd[k];
            check(f.vkResetCommandBuffer(cb, 0), "vkResetCommandBuffer");
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check(f.vkBeginCommandBuffer(cb, &bi), "vkBeginCommandBuffer");
            if (timed && k == 0) {
                f.vkCmdResetQueryPool(cb, queries, 0, 2);
                f.vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
            }
            recordJobs(cb, part);
            if (timed && k + 1 == np) f.vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
            check(f.vkEndCommandBuffer(cb), "vkEndCommandBuffer");
            check(f.vkResetFences(device, 1, &partFence[k]), "vkResetFences");
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cb;
            check(f.vkQueueSubmit(ctx->queue, 1, &si, partFence[k]), "vkQueueSubmit");
            ++submitted;
        }
        std::vector<JobResult> r;
        for (size_t k = 0; k < np; ++k) {
            check(f.vkWaitForFences(device, 1, &partFence[k], VK_TRUE, UINT64_MAX), "vkWaitForFences");
            waited = k + 1;
            if (k + 1 == np) gpuSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();  // (also holds the merges before it)
            r.assign(cut[k + 1] - cut[k], JobResult{});
            for (size_t i = cut[k]; i < cut[k + 1]; ++i) {
                const Job& j = jobs[i];
                JobResult& x = r[i - cut[k]];
                x.d = static_cast<const float*>(bOut.mapped) + static_cast<size_t>(j.first) * 4;
                x.db = static_cast<const float*>(bDb.mapped) + static_cast<size_t>(j.first) * 4;
                x.dx = static_cast<const float*>(bDx.mapped) + static_cast<size_t>(j.first) * 4;
                if (indirect == Indirect::PerSample) x.ind = static_cast<const float*>(bInd.mapped) + static_cast<size_t>(j.first) * 4;
                else if (indirect == Indirect::PerEntry) x.ind = static_cast<const float*>(bInd.mapped) + static_cast<size_t>(j.giFirst) * 4;
            }
            done(cut[k], cut[k + 1] - cut[k], r);
        }
        // the GPU's own time for the step: from the first part's start to the last part's end
        uint64_t ts[2] = {0, 0};
        if (timed && f.vkGetQueryPoolResults(device, queries, 0, 2, sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
            const uint64_t mask = ctx->timestampBits >= 64 ? ~0ull : ((1ull << ctx->timestampBits) - 1);
            gpuSec = static_cast<double>((ts[1] - ts[0]) & mask) * static_cast<double>(ctx->props.limits.timestampPeriod) * 1e-9;
        }
    }
};

}  // namespace

bool needsRayTracing(const LightScene& lights, const OccluderMesh* occluders, const BakeOptions& options) {
    if (!occluders || occluders->empty()) return false;
    if (options.bounces > 0) return true;  // gather rays need the acceleration structure whether or not lights cast shadows
    if (!options.shadows) return false;
    for (const auto& l : lights.lights) if (l.castShadows) return true;
    return false;
}

Result evaluateDirect(const TexelSamples& samples, const LightScene& lights, const OccluderMesh* occluders, const BakeOptions& options,
                      std::vector<float>& out, std::vector<float>* db, std::vector<float>* indirectOut, const GiSamples* gi, std::vector<float>* giOut) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Result r;
    const bool useRT = needsRayTracing(lights, occluders, options);
    const int key = (useRT ? 1 : 0) | (options.gpuValidation ? 2 : 0);
    try {
        run(samples, lights, occluders, options, r, out, db, indirectOut, gi, giOut);  // per-call objects are destroyed before we return
    } catch (const std::exception& e) {
        r.ok = false;
        r.error = e.what();
        g_contexts.erase(key);  // the next call starts from a fresh device (a lost device would otherwise stay broken)
        return r;
    }
    const auto it = g_contexts.find(key);
    if (it != g_contexts.end()) r.validation = it->second->collector.messages;
    return r;
}

BufferPoolStats liveBufferPoolStats() {
    std::lock_guard<std::mutex> lock(g_mutex);
    BufferPoolStats st;
    for (auto& kv : g_contexts) {
        BufferPool& p = kv.second->bufPool;
        std::lock_guard<std::mutex> pl(p.m);
        st.hits += p.hits;
        st.misses += p.misses;
        st.pooledBuffers += p.free.size();
        st.pooledBytes += static_cast<size_t>(p.pooledBytes());
    }
    return st;
}

std::unique_ptr<LiveSession> createLiveSession(const TexelSamples& resident, const OccluderMesh& occluders, const LightScene& lights,
                                               const LiveSessionConfig& config, const std::vector<float>& atlasInit, std::string& error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto session = std::make_unique<VkLive>();
    session->cfg = config;
    try {
        session->init(resident, occluders, lights, atlasInit);
    } catch (const std::exception& e) {
        error = e.what();
        const int key = session->key();
        session.reset();        // releases its resources and its hold on the context ...
        g_contexts.erase(key);  // ... then the next attempt starts from a fresh device
        return nullptr;
    }
    return session;
}

void releaseContexts() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_contexts.clear();
}

}  // namespace core::gpu
