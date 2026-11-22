// gpu/device -- see device.h.
#include "gpu/device.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <set>
#include <string>

#include "base/log.h"

namespace native::gpu {

struct Device::MessageCounts {
    std::atomic<uint64_t> errors{0}, warnings{0};
};

namespace {

using base::logError;
using base::logInfo;
using base::logWarning;

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    auto* counts = static_cast<Device::MessageCounts*>(user);
    const std::string msg = data && data->pMessage ? data->pMessage : "(no message)";
    if (!(types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT)) {
        // Loader chatter about other software's implicit layers (OBS hooks, ReShade, ...): logged, never counted.
        logInfo("vulkan loader: " + msg);
        return VK_FALSE;
    }
    const std::string text = "vulkan validation: " + msg;
    // NVIDIA's NGX creates its own images inside slEvaluateFeature (named "nv.ngx.*") and the synchronization validation reports a hazard between two of its own
    // commands there (DLSS Ray Reconstruction, first evaluate). That is not ours to fix: counted as a warning, still logged.
    const bool ngxInternal = msg.find("[nv.ngx.") != std::string::npos;
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) && !ngxInternal) {
        ++counts->errors;
        logError(text);
    } else if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT))) {
        ++counts->warnings;
        logWarning(text);
    }
    return VK_FALSE;
}

std::string versionString(uint32_t v) {
    return std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) + "." +
           std::to_string(VK_API_VERSION_PATCH(v));
}

std::string decodeDriver(const VkPhysicalDeviceProperties& p, const VkPhysicalDeviceDriverProperties& d) {
    const uint32_t v = p.driverVersion;
    if (p.vendorID == 0x10DE)   // NVIDIA: 10 / 8 / 8 / 6 bits
        return "NVIDIA " + std::to_string((v >> 22) & 0x3ff) + "." + std::to_string((v >> 14) & 0xff);
    std::string s = d.driverName;
    if (d.driverInfo[0]) s += std::string(" ") + d.driverInfo;
    if (s.empty()) s = versionString(v);
    return s;
}

bool hasExt(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (const auto& e : list)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

// One candidate physical device with everything we learned about it.
struct Candidate {
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceDriverProperties driver{};
    std::vector<VkExtensionProperties> exts;
    bool meetsRequired = false;
    std::string whyNot;
    bool rt = false;
    uint64_t score = 0;
};

// The feature chain, used both to query and to enable. All optional structs are always part of the chain; an extension
// that is not present just leaves its struct untouched (the driver ignores unknown pNext members only when it does not
// know the type, so the chain is built from the structs of enabled-or-supported extensions only).
struct FeatureChain {
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accel{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT reorder{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_EXT};
    VkPhysicalDeviceOpacityMicromapFeaturesEXT omm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT};
    VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR posFetch{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR};
    VkPhysicalDeviceHostImageCopyFeatures hostCopy{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES};
    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT atomicFloat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT};
    VkPhysicalDevicePartitionedAccelerationStructureFeaturesNV partitioned{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PARTITIONED_ACCELERATION_STRUCTURE_FEATURES_NV};
    VkPhysicalDeviceClusterAccelerationStructureFeaturesNV cluster{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_ACCELERATION_STRUCTURE_FEATURES_NV};
    VkPhysicalDeviceDescriptorHeapFeaturesEXT heap{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT};
    VkPhysicalDeviceDiagnosticsConfigFeaturesNV diagnostics{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DIAGNOSTICS_CONFIG_FEATURES_NV};
    VkPhysicalDevicePresentIdFeaturesKHR presentId{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};
    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR derivatives{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR};

    // Links the structs of the extensions in `present` (all of them for a query, the enabled ones for creation).
    void link(const std::set<std::string>& present, bool includeHeap) {
        void* head = nullptr;
        auto add = [&](auto& s, const char* ext) {
            if (ext && !present.count(ext)) return;
            s.pNext = head;
            head = &s;
        };
        add(f11, nullptr);
        add(f12, nullptr);
        add(f13, nullptr);
        add(accel, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        add(rayQuery, VK_KHR_RAY_QUERY_EXTENSION_NAME);
        add(rtPipeline, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
        add(reorder, VK_EXT_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME);
        add(omm, VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME);
        add(posFetch, VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME);
        add(hostCopy, VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME);
        add(atomicFloat, VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME);
        add(partitioned, VK_NV_PARTITIONED_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        add(cluster, VK_NV_CLUSTER_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        if (includeHeap) add(heap, VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME);
        add(diagnostics, VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME);
        add(presentId, VK_KHR_PRESENT_ID_EXTENSION_NAME);
        add(derivatives, VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
        f2.pNext = head;
    }
};

// Returns the missing required feature's name, or empty when all are there.
const char* missingRequired(const FeatureChain& c) {
    if (!c.f13.dynamicRendering) return "dynamicRendering";
    if (!c.f13.synchronization2) return "synchronization2";
    if (!c.f12.timelineSemaphore) return "timelineSemaphore";
    if (!c.f12.bufferDeviceAddress) return "bufferDeviceAddress";
    if (!c.f12.runtimeDescriptorArray) return "runtimeDescriptorArray";
    if (!c.f12.descriptorBindingPartiallyBound) return "descriptorBindingPartiallyBound";
    if (!c.f12.descriptorBindingSampledImageUpdateAfterBind) return "descriptorBindingSampledImageUpdateAfterBind";
    if (!c.f12.descriptorBindingStorageBufferUpdateAfterBind) return "descriptorBindingStorageBufferUpdateAfterBind";
    if (!c.f12.shaderSampledImageArrayNonUniformIndexing) return "shaderSampledImageArrayNonUniformIndexing";
    if (!c.f12.scalarBlockLayout) return "scalarBlockLayout";
    if (!c.f12.storageBuffer8BitAccess) return "storageBuffer8BitAccess";
    if (!c.f11.storageBuffer16BitAccess) return "storageBuffer16BitAccess";
    if (!c.f2.features.multiDrawIndirect) return "multiDrawIndirect";
    if (!c.f2.features.drawIndirectFirstInstance) return "drawIndirectFirstInstance";
    if (!c.f2.features.independentBlend) return "independentBlend";
    return "";
}

} // namespace

std::vector<std::pair<std::string, std::string>> DeviceCaps::rows() const {
    auto onOff = [](bool b) { return std::string(b ? "on" : "off"); };
    return {
        {"device", deviceName},
        {"driver", driver},
        {"Vulkan API", apiVersion},
        {"vendor", vendorName(vendor)},
        {"type", discrete ? "discrete GPU" : "integrated / other"},
        {"acceleration structure", onOff(accelerationStructure)},
        {"ray query", onOff(rayQuery)},
        {"bounce cache (SHaRC)", onOff(bounceCache)},
        {"ray tracing pipeline", onOff(rayTracingPipeline)},
        {"invocation reorder (SER)", onOff(invocationReorder)},
        {"opacity micromap", onOff(opacityMicromap)},
        {"position fetch", onOff(positionFetch)},
        {"global priority", globalPriorityHigh ? "on" : globalPriority ? "partial (graphics HIGH not allowed)" : "off"},
        {"host image copy", onOff(hostImageCopy)},
        {"external memory (win32)", onOff(externalMemoryWin32)},
        {"shader atomic float", onOff(shaderAtomicFloat)},
        {"partitioned acceleration structure (NV)", onOff(partitionedAccelerationStructure)},
        {"cluster acceleration structure (NV)", onOff(clusterAccelerationStructure)},
        {"descriptor heap", descriptorHeap ? "available, not used in v1" : "off"},
        {"dedicated compute queue", onOff(dedicatedComputeQueue)},
        {"FSR upscaler (AMD FidelityFX)", fsr ? "on (" + fsrVersion + ")" : fsrNote.empty() ? "off" : "off: " + fsrNote},
        {"Streamline", streamline ? "on (interposer)" : streamlineNote.empty() ? "off" : "off: " + streamlineNote},
        {"Reflex low latency", onOff(reflex)},
        {"DLSS Super Resolution", onOff(dlss)},
        {"GPU crash dumps", diagnostics ? "on (--gpu-crash-dumps)" : "off"},
        {"Lit mode", lit() ? "available" : "unavailable (no ray query device)"},
    };
}

std::unique_ptr<Device> Device::create(const DeviceOptions& options, std::string* error) {
    std::string note;
    bool trySl = options.streamline;
    if (trySl) {   // the Streamline interposer takes the Vulkan loader over, which only pays off when an NVIDIA GPU is there to use it (DLSS, Reflex)
        const std::vector<AdapterInfo> adapters = listAdapters();
        for (const AdapterInfo& a : adapters) logInfo("graphics adapter: " + a.name + " (" + vendorName(vendorFromId(a.vendorId)) + (a.software ? ", software" : "") + ")");
        if (!adapters.empty() && !hasAdapterFrom(adapters, Vendor::Nvidia)) {
            trySl = false;
            note = "no NVIDIA graphics adapter in this machine: DLSS and Reflex are off, FSR is the upscaler";
            logInfo(note);
        }
    }
    if (trySl) {
        // First try: instance and device through the Streamline interposer. A missing DLL is not an error (init notes it and goes on
        // with the system loader); a failure after the interposer took over is, so start again from scratch without it.
        std::unique_ptr<Device> d(new Device());
        std::string err;
        if (d->init(options, true, &err)) return d;
        note = "Vulkan start-up through Streamline failed (" + err + ")";
        logWarning(note + "; using the system Vulkan loader");
    }
    std::unique_ptr<Device> d(new Device());
    std::string err;
    if (!d->init(options, false, &err)) {
        if (error) *error = err;
        logError("Vulkan device: " + err);
        return nullptr;
    }
    if (!note.empty()) d->caps_.streamlineNote = note;
    return d;
}

bool Device::init(const DeviceOptions& options, bool useStreamline, std::string* error) {
    counts_ = std::make_unique<MessageCounts>();
    crashDumpDir_ = options.crashDumpDir;

    // ---- loader and instance ----
    // Streamline first: slInit must run before any Vulkan call, and volk then takes its entry points from the interposer.
    if (useStreamline) {
        std::string why;
        streamline_ = Streamline::load(options.streamlineDir, &why);
        if (!streamline_) {
            caps_.streamlineNote = why;
            logInfo("Streamline is not used: " + why);
        }
    }
    if (streamline_) {
        volkInitializeCustom(streamline_->instanceProcAddr());
    } else if (volkInitialize() != VK_SUCCESS) {
        *error = "the Vulkan loader (vulkan-1.dll) was not found; install a current graphics driver";
        return false;
    }
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&loaderVersion);
    if (loaderVersion < VK_API_VERSION_1_3) {
        *error = "the Vulkan loader is older than 1.3 (" + versionString(loaderVersion) + ")";
        return false;
    }

    std::vector<const char*> instExts = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    std::vector<const char*> layers;
    bool wantMessenger = false;
    bool syncValidation = false;
    if (options.validation) {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> lp(n);
        vkEnumerateInstanceLayerProperties(&n, lp.data());
        const char* kLayer = "VK_LAYER_KHRONOS_validation";
        const bool have = std::any_of(lp.begin(), lp.end(), [&](const VkLayerProperties& p) { return std::strcmp(p.layerName, kLayer) == 0; });
        if (have) {
            layers.push_back(kLayer);
            uint32_t en = 0;
            vkEnumerateInstanceExtensionProperties(nullptr, &en, nullptr);
            std::vector<VkExtensionProperties> ie(en);
            vkEnumerateInstanceExtensionProperties(nullptr, &en, ie.data());
            if (hasExt(ie, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) { instExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME); wantMessenger = true; }
            uint32_t ln = 0;
            vkEnumerateInstanceExtensionProperties(kLayer, &ln, nullptr);
            std::vector<VkExtensionProperties> le(ln);
            vkEnumerateInstanceExtensionProperties(kLayer, &ln, le.data());
            if (hasExt(le, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)) {
                if (streamline_) {
                    // The interposer checks every requested instance extension against the loader's own list, which does not include
                    // the ones a layer provides: ask the layer for synchronization validation through its environment variable instead.
                    _putenv_s("VK_LAYER_ENABLES", "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT");
                } else {
                    instExts.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
                    syncValidation = true;
                }
            }
        } else {
            logWarning("--validation: VK_LAYER_KHRONOS_validation is not installed (install the Vulkan SDK); running without");
        }
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "mapeditor_native";
    app.applicationVersion = 1;
    app.pEngineName = "mapeditor_native";
    app.apiVersion = VK_API_VERSION_1_3;

    const VkValidationFeatureEnableEXT enables[] = {VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
    VkValidationFeaturesEXT vf{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    vf.enabledValidationFeatureCount = 1;
    vf.pEnabledValidationFeatures = enables;

    VkDebugUtilsMessengerCreateInfoEXT mci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    mci.pfnUserCallback = debugCallback;
    mci.pUserData = counts_.get();

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<uint32_t>(instExts.size());
    ici.ppEnabledExtensionNames = instExts.data();
    ici.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    void* chain = nullptr;
    if (wantMessenger) { mci.pNext = chain; chain = &mci; }             // also reports messages during instance creation
    if (syncValidation) { vf.pNext = chain; chain = &vf; }
    ici.pNext = chain;
    if (VkResult r = vkCreateInstance(&ici, nullptr, &instance_); r != VK_SUCCESS) {
        *error = "vkCreateInstance failed (" + std::to_string(r) + ")";
        return false;
    }
    volkLoadInstance(instance_);
    if (wantMessenger) vkCreateDebugUtilsMessengerEXT(instance_, &mci, nullptr, &messenger_);

    // ---- physical device ----
    uint32_t pdCount = 0;
    vkEnumeratePhysicalDevices(instance_, &pdCount, nullptr);
    std::vector<VkPhysicalDevice> pds(pdCount);
    vkEnumeratePhysicalDevices(instance_, &pdCount, pds.data());
    if (pds.empty()) { *error = "no Vulkan physical device"; return false; }

    std::vector<Candidate> cands;
    for (uint32_t i = 0; i < pds.size(); ++i) {
        Candidate c;
        c.pd = pds[i];
        c.driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &c.driver;
        vkGetPhysicalDeviceProperties2(c.pd, &p2);
        c.props = p2.properties;
        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(c.pd, nullptr, &en, nullptr);
        c.exts.resize(en);
        vkEnumerateDeviceExtensionProperties(c.pd, nullptr, &en, c.exts.data());

        if (c.props.apiVersion < VK_API_VERSION_1_3) c.whyNot = "Vulkan " + versionString(c.props.apiVersion) + " < 1.3";
        else if (!hasExt(c.exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) c.whyNot = "no VK_KHR_swapchain";
        else {
            std::set<std::string> all;
            for (const auto& e : c.exts) all.insert(e.extensionName);
            FeatureChain fc;
            fc.link(all, true);
            vkGetPhysicalDeviceFeatures2(c.pd, &fc.f2);
            const char* missing = missingRequired(fc);
            if (*missing) c.whyNot = std::string("missing feature ") + missing;
            else {
                c.meetsRequired = true;
                c.rt = hasExt(c.exts, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) && fc.accel.accelerationStructure &&
                       hasExt(c.exts, VK_KHR_RAY_QUERY_EXTENSION_NAME) && fc.rayQuery.rayQuery;
            }
        }
        c.score = (c.rt ? 100000ull : 0) + (c.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 10000ull : 0);
        cands.push_back(std::move(c));
        logInfo(std::string("Vulkan device ") + std::to_string(i) + ": " + cands.back().props.deviceName +
                (cands.back().meetsRequired ? (cands.back().rt ? " (ray tracing)" : " (no ray query)")
                                            : " (unusable: " + cands.back().whyNot + ")"));
    }
    const Candidate* best = nullptr;
    if (options.forceDeviceIndex >= 0 && options.forceDeviceIndex < static_cast<int>(cands.size())) {
        if (cands[options.forceDeviceIndex].meetsRequired) best = &cands[options.forceDeviceIndex];
    } else {
        for (const auto& c : cands)
            if (c.meetsRequired && (!best || c.score > best->score)) best = &c;
    }
    if (!best) {
        std::string msg = "no suitable Vulkan 1.3 device:";
        for (const auto& c : cands) msg += std::string(" [") + c.props.deviceName + ": " + (c.whyNot.empty() ? "not chosen" : c.whyNot) + "]";
        *error = msg;
        return false;
    }
    if (options.requireRayTracing && !best->rt) {
        *error = std::string("the device ") + best->props.deviceName + " has no ray query support";
        return false;
    }
    physical_ = best->pd;

    // ---- queue families ----
    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qfCount, qf.data());
    int gfx = -1;
    for (uint32_t i = 0; i < qfCount; ++i)
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
            vkGetPhysicalDeviceWin32PresentationSupportKHR(physical_, i)) { gfx = static_cast<int>(i); break; }
    if (gfx < 0) { *error = "no graphics queue family that can present"; return false; }
    int comp = -1;
    for (uint32_t i = 0; i < qfCount; ++i)
        if (static_cast<int>(i) != gfx && (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { comp = static_cast<int>(i); break; }
    graphicsFamily_ = static_cast<uint32_t>(gfx);
    const bool dedicated = comp >= 0;
    const bool secondOnSameFamily = !dedicated && qf[gfx].queueCount > 1;
    computeFamily_ = dedicated ? static_cast<uint32_t>(comp) : graphicsFamily_;

    // ---- enabled extensions and features ----
    std::set<std::string> avail;
    for (const auto& e : best->exts) avail.insert(e.extensionName);
    std::set<std::string> enabled = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    FeatureChain supported;
    supported.link(avail, true);
    vkGetPhysicalDeviceFeatures2(physical_, &supported.f2);

    auto enableIf = [&](const char* ext, bool featureBit, std::initializer_list<const char*> needs = {}) {
        if (!avail.count(ext) || !featureBit) return false;
        for (const char* n : needs)
            if (!avail.count(n) && !enabled.count(n)) return false;
        enabled.insert(ext);
        return true;
    };
    caps_.accelerationStructure = avail.count(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) &&
                                  enableIf(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, supported.accel.accelerationStructure);
    if (caps_.accelerationStructure) enabled.insert(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    caps_.rayQuery = caps_.accelerationStructure && enableIf(VK_KHR_RAY_QUERY_EXTENSION_NAME, supported.rayQuery.rayQuery);
    caps_.bounceCache = caps_.rayQuery && supported.f12.shaderBufferInt64Atomics && supported.f12.shaderFloat16 && supported.f2.features.shaderInt64;
    caps_.rayTracingPipeline = caps_.accelerationStructure && enableIf(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, supported.rtPipeline.rayTracingPipeline);
    caps_.invocationReorder = caps_.rayTracingPipeline && enableIf(VK_EXT_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME, supported.reorder.rayTracingInvocationReorder);
    caps_.opacityMicromap = caps_.accelerationStructure && enableIf(VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME, supported.omm.micromap);
    caps_.positionFetch = caps_.accelerationStructure && enableIf(VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME, supported.posFetch.rayTracingPositionFetch);
    caps_.hostImageCopy = enableIf(VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME, supported.hostCopy.hostImageCopy);
    caps_.externalMemoryWin32 = enableIf(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, true);
    caps_.shaderAtomicFloat = enableIf(VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME, supported.atomicFloat.shaderBufferFloat32AtomicAdd);
    caps_.partitionedAccelerationStructure = caps_.accelerationStructure && enableIf(VK_NV_PARTITIONED_ACCELERATION_STRUCTURE_EXTENSION_NAME, supported.partitioned.partitionedAccelerationStructure);
    caps_.clusterAccelerationStructure = caps_.accelerationStructure && enableIf(VK_NV_CLUSTER_ACCELERATION_STRUCTURE_EXTENSION_NAME, supported.cluster.clusterAccelerationStructure);
    caps_.descriptorHeap = avail.count(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME) && supported.heap.descriptorHeap;   // reported, not enabled
    caps_.diagnostics = options.gpuCrashDumps && avail.count(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME) &&
                        avail.count(VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME) && supported.diagnostics.diagnosticsConfig;
    // Reflex (VK_NV_low_latency2, which Streamline adds itself) needs VK_KHR_present_id next to it; Streamline does not add that one.
    const bool presentId = streamline_ && avail.count(VK_KHR_PRESENT_ID_EXTENSION_NAME) && supported.presentId.presentId;
    if (presentId) enabled.insert(VK_KHR_PRESENT_ID_EXTENSION_NAME);
    // FidelityFX's compute shaders (FSR) declare SPV_KHR_compute_shader_derivatives; core in no version yet, so the extension and its two feature bits are switched on whenever the GPU has them
    const bool derivatives = avail.count(VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME) && (supported.derivatives.computeDerivativeGroupLinear || supported.derivatives.computeDerivativeGroupQuads);
    if (derivatives) enabled.insert(VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
    if (caps_.diagnostics) {
        enabled.insert(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
        enabled.insert(VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME);
    } else if (options.gpuCrashDumps) {
        logWarning("--gpu-crash-dumps: this device has no VK_NV_device_diagnostics_config / diagnostic checkpoints");
    }
    const bool priorityExt = avail.count(VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME) > 0;
    if (avail.count(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) enabled.insert(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);

    // Features to enable: exactly the required ones, the optional ones whose extension is on, plus a few cheap core features.
    FeatureChain on;
    on.f2.features.multiDrawIndirect = VK_TRUE;
    on.f2.features.drawIndirectFirstInstance = VK_TRUE;
    on.f2.features.independentBlend = VK_TRUE;
    auto copyIf = [](VkBool32 have, VkBool32& want) { if (have) want = VK_TRUE; };
    copyIf(supported.f2.features.samplerAnisotropy, on.f2.features.samplerAnisotropy);
    copyIf(supported.f2.features.shaderInt64, on.f2.features.shaderInt64);
    copyIf(supported.f2.features.shaderInt16, on.f2.features.shaderInt16);
    copyIf(supported.f2.features.fillModeNonSolid, on.f2.features.fillModeNonSolid);
    copyIf(supported.f2.features.wideLines, on.f2.features.wideLines);
    copyIf(supported.f2.features.fragmentStoresAndAtomics, on.f2.features.fragmentStoresAndAtomics);
    copyIf(supported.f2.features.shaderStorageImageReadWithoutFormat, on.f2.features.shaderStorageImageReadWithoutFormat);
    copyIf(supported.f2.features.shaderStorageImageWriteWithoutFormat, on.f2.features.shaderStorageImageWriteWithoutFormat);
    copyIf(supported.f2.features.textureCompressionBC, on.f2.features.textureCompressionBC);
    on.f11.storageBuffer16BitAccess = VK_TRUE;
    copyIf(supported.f11.uniformAndStorageBuffer16BitAccess, on.f11.uniformAndStorageBuffer16BitAccess);
    copyIf(supported.f11.shaderDrawParameters, on.f11.shaderDrawParameters);
    on.f12.timelineSemaphore = VK_TRUE;
    on.f12.bufferDeviceAddress = VK_TRUE;
    on.f12.runtimeDescriptorArray = VK_TRUE;
    on.f12.descriptorIndexing = VK_TRUE;
    on.f12.descriptorBindingPartiallyBound = VK_TRUE;
    on.f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    on.f12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
    on.f12.descriptorBindingStorageImageUpdateAfterBind = supported.f12.descriptorBindingStorageImageUpdateAfterBind;
    on.f12.descriptorBindingUpdateUnusedWhilePending = supported.f12.descriptorBindingUpdateUnusedWhilePending;
    on.f12.descriptorBindingVariableDescriptorCount = supported.f12.descriptorBindingVariableDescriptorCount;
    on.f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    on.f12.shaderStorageBufferArrayNonUniformIndexing = supported.f12.shaderStorageBufferArrayNonUniformIndexing;
    on.f12.scalarBlockLayout = VK_TRUE;
    on.f12.storageBuffer8BitAccess = VK_TRUE;
    on.f12.shaderInt8 = supported.f12.shaderInt8;
    on.f12.shaderFloat16 = supported.f12.shaderFloat16;
    on.f12.shaderBufferInt64Atomics = supported.f12.shaderBufferInt64Atomics;
    on.f12.hostQueryReset = supported.f12.hostQueryReset;
    on.f12.samplerFilterMinmax = supported.f12.samplerFilterMinmax;
    on.f13.dynamicRendering = VK_TRUE;
    on.f13.synchronization2 = VK_TRUE;
    on.f13.maintenance4 = supported.f13.maintenance4;
    on.f13.privateData = supported.f13.privateData;   // Streamline creates private data slots
    on.f13.shaderDemoteToHelperInvocation = supported.f13.shaderDemoteToHelperInvocation;
    on.accel.accelerationStructure = caps_.accelerationStructure;
    on.rayQuery.rayQuery = caps_.rayQuery;
    on.rtPipeline.rayTracingPipeline = caps_.rayTracingPipeline;
    on.reorder.rayTracingInvocationReorder = caps_.invocationReorder;
    on.omm.micromap = caps_.opacityMicromap;
    on.posFetch.rayTracingPositionFetch = caps_.positionFetch;
    on.hostCopy.hostImageCopy = caps_.hostImageCopy;
    on.atomicFloat.shaderBufferFloat32AtomicAdd = caps_.shaderAtomicFloat;
    on.partitioned.partitionedAccelerationStructure = caps_.partitionedAccelerationStructure;
    on.cluster.clusterAccelerationStructure = caps_.clusterAccelerationStructure;
    on.diagnostics.diagnosticsConfig = caps_.diagnostics;
    on.presentId.presentId = presentId;
    on.derivatives.computeDerivativeGroupLinear = derivatives && supported.derivatives.computeDerivativeGroupLinear;
    on.derivatives.computeDerivativeGroupQuads = derivatives && supported.derivatives.computeDerivativeGroupQuads;
    on.link(enabled, false);

    // ---- create the device (global priority first, then without when the driver refuses it) ----
    std::vector<const char*> devExts;
    for (const auto& e : enabled) devExts.push_back(e.c_str());
    if (priorityExt) devExts.push_back(VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME);

    // Ask the driver which global priorities each queue family accepts and request only those: asking for one that is not
    // allowed (HIGH needs elevated rights on some drivers) crashed the NVIDIA driver instead of failing (seen with 610.88).
    bool gfxHigh = false, compMedium = false;
    if (priorityExt) {
        uint32_t n = qfCount;
        std::vector<VkQueueFamilyGlobalPriorityPropertiesKHR> gp(n, {VK_STRUCTURE_TYPE_QUEUE_FAMILY_GLOBAL_PRIORITY_PROPERTIES_KHR});
        std::vector<VkQueueFamilyProperties2> p2(n, {VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2});
        for (uint32_t i = 0; i < n; ++i) p2[i].pNext = &gp[i];
        vkGetPhysicalDeviceQueueFamilyProperties2(physical_, &n, p2.data());
        auto supports = [&](uint32_t family, VkQueueGlobalPriorityKHR level) {
            for (uint32_t k = 0; k < gp[family].priorityCount; ++k)
                if (gp[family].priorities[k] == level) return true;
            return false;
        };
        gfxHigh = supports(graphicsFamily_, VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR);
        compMedium = dedicated && supports(computeFamily_, VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR);
        logInfo(std::string("global queue priority: graphics HIGH ") + (gfxHigh ? "allowed" : "not allowed") +
                (dedicated ? std::string(", compute MEDIUM ") + (compMedium ? "allowed" : "not allowed") : std::string()));
    }

    auto tryCreate = [&](bool withPriority) -> VkResult {
        const float gp = 1.0f;
        const float two[2] = {1.0f, 0.5f};
        VkDeviceQueueGlobalPriorityCreateInfoKHR prioGfx{VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR};
        prioGfx.globalPriority = VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR;
        VkDeviceQueueGlobalPriorityCreateInfoKHR prioComp{VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR};
        prioComp.globalPriority = VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR;
        std::vector<VkDeviceQueueCreateInfo> qci;
        VkDeviceQueueCreateInfo a{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        a.queueFamilyIndex = graphicsFamily_;
        a.queueCount = secondOnSameFamily ? 2u : 1u;
        a.pQueuePriorities = secondOnSameFamily ? two : &gp;
        if (withPriority && gfxHigh && !secondOnSameFamily) a.pNext = &prioGfx;   // two queues of one family share one priority: skip it then
        qci.push_back(a);
        if (dedicated) {
            VkDeviceQueueCreateInfo b{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            b.queueFamilyIndex = computeFamily_;
            b.queueCount = 1;
            b.pQueuePriorities = &gp;
            if (withPriority && compMedium) b.pNext = &prioComp;
            qci.push_back(b);
        }
        std::vector<const char*> exts = devExts;
        if (!withPriority && priorityExt) exts.pop_back();
        VkDeviceDiagnosticsConfigCreateInfoNV diag{VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV};
        diag.flags = VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_RESOURCE_TRACKING_BIT_NV | VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_AUTOMATIC_CHECKPOINTS_BIT_NV;
        diag.pNext = &on.f2;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.pNext = caps_.diagnostics ? static_cast<const void*>(&diag) : static_cast<const void*>(&on.f2);
        dci.queueCreateInfoCount = static_cast<uint32_t>(qci.size());
        dci.pQueueCreateInfos = qci.data();
        dci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
        dci.ppEnabledExtensionNames = exts.data();
        return vkCreateDevice(physical_, &dci, nullptr, &device_);
    };
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    caps_.globalPriority = false;
    bool created = false;
    if (priorityExt && (gfxHigh || compMedium)) {
        r = tryCreate(true);
        created = r == VK_SUCCESS;
        if (created) {
            caps_.globalPriority = true;
            caps_.globalPriorityHigh = gfxHigh && !secondOnSameFamily;
        }
        else logInfo("global queue priority refused by the driver (" + std::to_string(r) + "); using normal priority");
    }
    if (!created) r = tryCreate(false);
    if (r != VK_SUCCESS) {
        *error = "vkCreateDevice failed (" + std::to_string(r) + ")";
        return false;
    }
    volkLoadDevice(device_);

    vkGetDeviceQueue(device_, graphicsFamily_, 0, &graphicsQueue_);
    if (dedicated) vkGetDeviceQueue(device_, computeFamily_, 0, &computeQueue_);
    else if (secondOnSameFamily) vkGetDeviceQueue(device_, graphicsFamily_, 1, &computeQueue_);
    else computeQueue_ = graphicsQueue_;
    caps_.dedicatedComputeQueue = dedicated;

    if (streamline_) {
        streamline_->deviceCreated(instance_, physical_, device_);
        caps_.streamline = true;
        caps_.reflex = streamline_->status().reflexActive;
        caps_.dlss = streamline_->dlssAvailable();
    }

    // ---- AMD FidelityFX Super Resolution: the upscaler for every vendor ----
    if (options.fidelityFx) {
        std::string why;
        fidelityFx_ = FidelityFx::load(options.fidelityFxDir, device_, physical_, &why);
        if (fidelityFx_) {
            caps_.fsr = true;
            caps_.fsrVersion = fidelityFx_->status().version;
        } else {
            caps_.fsrNote = why;
            logInfo("FSR (FidelityFX) is not used: " + why);
        }
    }

    // ---- caps text ----
    caps_.vendorId = best->props.vendorID;
    caps_.vendor = vendorFromId(best->props.vendorID);
    caps_.deviceName = best->props.deviceName;
    caps_.driver = decodeDriver(best->props, best->driver);
    caps_.apiVersion = versionString(best->props.apiVersion);
    caps_.discrete = best->props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;

    // ---- VMA ----
    VmaVulkanFunctions fn{};
    fn.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    fn.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo aci{};
    aci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    if (enabled.count(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) aci.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    aci.physicalDevice = physical_;
    aci.device = device_;
    aci.instance = instance_;
    aci.vulkanApiVersion = VK_API_VERSION_1_3;
    aci.pVulkanFunctions = &fn;
    if (vmaCreateAllocator(&aci, &allocator_) != VK_SUCCESS) {
        *error = "vmaCreateAllocator failed";
        return false;
    }

    logInfo("Vulkan device: " + caps_.deviceName + ", driver " + caps_.driver + ", API " + caps_.apiVersion +
            (caps_.lit() ? ", Lit available" : ", Lit unavailable") + (validationActive() ? ", validation on" : ""));
    return true;
}

Device::~Device() {
    if (device_) vkDeviceWaitIdle(device_);
    if (allocator_) vmaDestroyAllocator(allocator_);
    fidelityFx_.reset();   // before the device: its contexts own Vulkan objects
    if (streamline_) streamline_->shutdown();   // SL wants to go while the device still exists
    if (device_) vkDestroyDevice(device_, nullptr);
    if (messenger_) vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
}

std::string Device::writeCrashDump(const std::string& where) const {
    if (!caps_.diagnostics || !device_) return {};
    if (crashDumpWritten_.exchange(true)) return {};
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    std::strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &local);
    const std::string path = (crashDumpDir_.empty() ? std::string(".") : crashDumpDir_) + "/gpu_crash_" + stamp + ".txt";
    std::ofstream out(path);
    if (!out) return {};
    out << "GPU crash report (VK_NV_device_diagnostic_checkpoints)\n";
    out << "device: " << caps_.deviceName << ", driver " << caps_.driver << ", Vulkan " << caps_.apiVersion << "\n";
    out << "where: " << where << "\n";
    auto dumpQueue = [&](const char* name, VkQueue queue) {
        uint32_t n = 0;
        if (!vkGetQueueCheckpointDataNV) return;
        vkGetQueueCheckpointDataNV(queue, &n, nullptr);
        std::vector<VkCheckpointDataNV> data(n, VkCheckpointDataNV{VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV});
        if (n) vkGetQueueCheckpointDataNV(queue, &n, data.data());
        out << "queue " << name << ": " << n << " checkpoint(s)\n";
        for (const VkCheckpointDataNV& c : data) {
            char line[96];
            std::snprintf(line, sizeof line, "  stage 0x%08x marker %p\n", static_cast<unsigned>(c.stage), c.pCheckpointMarker);
            out << line;
        }
    };
    dumpQueue("graphics", graphicsQueue_);
    if (computeQueue_ != graphicsQueue_) dumpQueue("compute", computeQueue_);
    return path;
}

void Device::noteResult(VkResult result, const char* where) const {
    if (result == VK_ERROR_DEVICE_LOST) markLost(std::string("VK_ERROR_DEVICE_LOST at ") + where);
}

void Device::markLost(const std::string& why) const {
    if (lost_.exchange(true)) return;   // the first one reports
    logError("GPU device lost: " + why);
    const std::string path = writeCrashDump(why);
    if (!path.empty()) logError("GPU crash report written to " + path);
    std::lock_guard<std::mutex> lock(lostMutex_);
    lostReason_ = why;
    if (lostHandler_) lostHandler_(why);
}

std::string Device::lostReason() const {
    std::lock_guard<std::mutex> lock(lostMutex_);
    return lostReason_;
}

void Device::setLostHandler(std::function<void(const std::string&)> handler) {
    std::lock_guard<std::mutex> lock(lostMutex_);
    lostHandler_ = std::move(handler);
}

bool Device::submitAndWait(VkQueue queue, const VkSubmitInfo2& si, const char* where) const {
    if (lost_.load()) return false;
    const VkResult sr = vkQueueSubmit2(queue, 1, &si, VK_NULL_HANDLE);
    noteResult(sr, where);
    if (sr != VK_SUCCESS) return false;
    const VkResult wr = vkQueueWaitIdle(queue);
    noteResult(wr, where);
    return wr == VK_SUCCESS;
}

VkResult Device::waitTimeline(VkSemaphore semaphore, uint64_t value, uint64_t timeoutNs, const char* where) const {
    if (lost_.load()) return VK_ERROR_DEVICE_LOST;
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &semaphore;
    wi.pValues = &value;
    const VkResult r = vkWaitSemaphores(device_, &wi, timeoutNs);
    noteResult(r, where);
    return r;
}

uint64_t Device::validationErrors() const { return counts_ ? counts_->errors.load() : 0; }
uint64_t Device::validationWarnings() const { return counts_ ? counts_->warnings.load() : 0; }

AllocationStats Device::allocationStats() const {
    VmaTotalStatistics st{};
    vmaCalculateStatistics(allocator_, &st);
    AllocationStats out;
    out.allocationCount = st.total.statistics.allocationCount;
    out.blockCount = st.total.statistics.blockCount;
    out.allocationBytes = st.total.statistics.allocationBytes;
    return out;
}

} // namespace native::gpu
