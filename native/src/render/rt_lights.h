// render/rt_lights -- the lights of the ray traced Lit path (R3): the packed light records of the bake (light/light_pack), a dense uniform grid of the local lights' ranges, and
// the buffers that hold them on the GPU.
//
// Owns: RtLightData (CPU, immutable once made: the records, the list of lights without a position (sun, sky), the grid cells and their light lists), makeRtLightData,
// RtLightsProducer (an async job that turns a scene snapshot into RtLightData; the newest request wins) and RtLightsGpu (device-local copies of one RtLightData).
// Index buffer layout: [the `globalCount` directional / sky light indices][the lists of the cells]; cell c = (offset into that buffer, count).
// Thread: makeRtLightData / RtLightsProducer: any; RtLightsGpu: the render thread. Spec: 12_RT_VIEWPORT.md §4, 08_MILESTONES.md R3.
#pragma once

#include "base/win_clean.h"   // before any core header: windows.h `near` / `far` macros

#include <volk.h>

#include <vk_mem_alloc.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/light_styles.h"
#include "core/lighting_classes.h"
#include "core/lighting_inputs.h"
#include "gpu/device.h"
#include "light/light_filter.h"
#include "render/gpu_scene.h"
#include "scene/snapshot.h"

namespace native::render {

struct RtLightData {
    std::vector<float> records;          // light::packLights(...): 8 vec4 per light, then the sky tables
    uint32_t lightCount = 0;
    uint32_t globalCount = 0;            // lights without a position of layer 0: directional and sky (the first entries of `index`)
    std::vector<uint32_t> index;         // [globals of layer 0][globals of layer 1]...[the cell lists of every layer]
    std::vector<uint32_t> cells;         // (offset, count) per cell, x fastest, layer after layer (cellCount cells per layer)
    // Light styles (12 section 4): the direct light is accumulated per style layer, the compose pass multiplies the layers by the animated style values, so a flicker
    // never smears the accumulation. Layer 0 = style 0 (always 1); layers 1..3 = the first distinct styles of the level; when there are more styles, the last layer holds
    // all the rest as constant light (every light scaled by the mean of its pattern: layerStyle = -1).
    uint32_t layerCount = 1;
    int layerStyle[4] = {0, -1, -1, -1};
    uint32_t layerGlobalOffset[4] = {0, 0, 0, 0}, layerGlobalCount[4] = {0, 0, 0, 0};
    uint32_t cellCount = 1;              // grid cells of one layer
    core::LightStyleTable styles;        // the patterns of the layers' styles
    // The multiplier of every layer at `seconds` (layer 0: 1).
    void layerWeights(double seconds, float out[4]) const;
    bool animated() const;               // some layer has a pattern that changes over time
    float origin[3] = {0, 0, 0};
    float cell = 1;                      // edge of a cell in metres
    uint32_t dim[3] = {1, 1, 1};
    bool operator==(const RtLightData& o) const;
};

// All lights, grouped into style layers (RtLightData); the local lights (point, spot, rect) are entered into the grid cells their range touches, per layer. `styles`:
// the patterns (null = the engine's defaults).
std::shared_ptr<const RtLightData> makeRtLightData(const std::vector<core::Light>& lights, const core::LightStyleTable* styles = nullptr);

// The lighting rules of the snapshot's objects (the game profile's classes): receives = the class has lightmaps, casts = the class blocks light and the object has a surface that
// does. Objects whose surfaces are not built yet are left out (they have both until they are). The same rules as the bake's accel (light/accel.cpp) and layout.
std::shared_ptr<const ObjectRules> makeRtRules(const scene::SceneSnapshot& snapshot, const core::LightingClasses& classes);

// Turns scene snapshots into RtLightData off the UI thread: the newest request is the one that gets built; `done` receives only data that differs from the previous result
// (an edit that does not change a light does not restart the accumulation).
class RtLightsProducer {
public:
    struct Request {
        std::shared_ptr<const scene::SceneSnapshot> snapshot;
        std::shared_ptr<const core::GameProfile> profile;
        std::filesystem::path assetsRoot;
        std::shared_ptr<const light::LightFilter> filter;   // muted / soloed lights (null = all)
    };
    // `done` is called on the producer's thread.
    explicit RtLightsProducer(std::function<void(std::shared_ptr<const RtLightData>)> done);
    ~RtLightsProducer();
    RtLightsProducer(const RtLightsProducer&) = delete;
    RtLightsProducer& operator=(const RtLightsProducer&) = delete;

    void request(Request r);
    // Waits until the newest request has been built and reported (tests).
    void waitIdle();
    // The sizes the bake gives lights authored without a source size (light/lighting_service.h Options).
    static constexpr double kDefaultSourceSize = 0.25, kDefaultSunSize = 0.5;

private:
    void run();
    std::function<void(std::shared_ptr<const RtLightData>)> done_;
    core::SkyTableCache skyCache_;
    std::mutex mutex_;
    std::condition_variable wake_, idle_;
    Request pending_;
    bool hasPending_ = false, busy_ = false, stop_ = false;
    std::shared_ptr<const RtLightData> last_;
    std::thread thread_;
};

// The GPU copies. All buffers are always valid (an empty set holds one zero record) so descriptors can be written without a check.
class RtLightsGpu {
public:
    explicit RtLightsGpu(const gpu::Device& device);
    ~RtLightsGpu();
    RtLightsGpu(const RtLightsGpu&) = delete;
    RtLightsGpu& operator=(const RtLightsGpu&) = delete;

    // Replaces the contents; the GPU must not be using the old buffers. Synchronous (a one-time submit). False when the upload failed (the old contents stay).
    bool upload(const RtLightData& data);

    VkBuffer records() const { return records_.buffer; }
    VkBuffer cells() const { return cells_.buffer; }
    VkBuffer index() const { return index_.buffer; }
    VkDeviceSize recordsSize() const { return records_.size; }
    VkDeviceSize cellsSize() const { return cells_.size; }
    VkDeviceSize indexSize() const { return index_.size; }
    // The numbers the shaders need.
    const RtLightData& info() const { return info_; }   // records / index / cells are not kept here (empty); only the counts and the grid
    uint32_t serial() const { return serial_; }         // grows with every upload

private:
    struct Buf {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation alloc = nullptr;
        VkDeviceSize size = 0;
    };
    bool make(Buf& b, VkDeviceSize bytes, bool host) const;
    void destroy(Buf& b) const;
    const gpu::Device& device_;
    Buf records_, cells_, index_;
    RtLightData info_;
    uint32_t serial_ = 0;
};

} // namespace native::render
