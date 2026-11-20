// light/results -- GPU storage of the bake's per-sample results (06_LIGHTING.md §6).
//
// Owns: device buffers indexed by the sample SLOT of light/samples.h: the running mean of the direct light per class (rgb + pass count), the scratch value of the
// pass being merged, the ambient occlusion. All of them are plain buffers (gpu::Arena used as a buffer holder): the slots come from the SampleStore's arena.
// Thread: the bake thread (compute queue). reset() only records commands; the caller orders them (a barrier before the next pass reads or writes).
// Spec: 06_LIGHTING.md §6, 08 T6.4. Deviation kept for T7: the bounce-weighted direct light Db, the indirect light I, the GI grid entries and the stats come
// with the gather pass.
#pragma once

#include <cstdint>

#include "gpu/arena.h"

namespace native::light {

enum class LightClass : uint32_t { Local = 0, Sun = 1, Sky = 2 };   // point + spot + rect, directional, sky
constexpr uint32_t kLightClasses = 3;
constexpr uint32_t classBit(LightClass c) { return 1u << static_cast<uint32_t>(c); }

class Results {
public:
    // Buffers for `slotCapacity` slots, cleared (direct light 0 with pass count 0, occlusion NaN = not computed). Blocks on the compute queue once.
    Results(gpu::Device& device, uint32_t slotCapacity);
    ~Results();
    Results(const Results&) = delete;
    Results& operator=(const Results&) = delete;

    bool valid() const { return valid_; }
    uint32_t slotCapacity() const { return slotCapacity_; }
    VkBuffer direct(LightClass c) const { return direct_[static_cast<size_t>(c)].buffer(); }   // vec4 per slot: running mean rgb, pass count
    VkBuffer scratch() const { return scratch_.buffer(); }                                      // vec4 per slot: the value of the pass being merged
    VkBuffer occlusion() const { return occlusion_.buffer(); }                                  // float per slot: occluded fraction, NaN = not computed

    // Records fills that make the slots [first, first + count) "never baked": direct light 0 / pass count 0, occlusion NaN. Needed when a chart gets new slots.
    void reset(VkCommandBuffer cmd, uint32_t first, uint32_t count);

private:
    uint32_t slotCapacity_;
    gpu::Arena direct_[kLightClasses];
    gpu::Arena scratch_;
    gpu::Arena occlusion_;
    bool valid_ = false;
};

} // namespace native::light
