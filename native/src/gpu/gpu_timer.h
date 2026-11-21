// gpu/gpu_timer -- GPU time of the passes of a frame: timestamp queries, one set per frame in flight.
//
// Owns: `slots` query pools of kMaxStamps timestamps. The thread that records a frame calls begin(slot) (host reset of that slot's queries, so the slot's previous GPU work must have
// finished), then stamp(cmd, slot, name) between the passes: the interval that ENDS at a stamp is named by it, the first stamp ("start") opens the first interval. When the GPU finished the
// frame, collect(slot) gives the intervals in milliseconds. Cost: one vkCmdWriteTimestamp2 per stamp.
// Thread: the recording thread only (no locking). Spec: 08_MILESTONES.md R8, 12_RT_VIEWPORT.md section 9 (the target split of the frame).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gpu/device.h"

namespace native::gpu {

class GpuTimer {
public:
    static constexpr uint32_t kMaxStamps = 32;
    struct Interval {
        std::string name;
        double ms = 0;
    };

    GpuTimer(Device& device, uint32_t slots);
    ~GpuTimer();
    GpuTimer(const GpuTimer&) = delete;
    GpuTimer& operator=(const GpuTimer&) = delete;

    bool ok() const { return !pools_.empty(); }
    // Starts the slot's recording: forgets the earlier stamps. The GPU must be done with the slot.
    void begin(uint32_t slot);
    // Writes a timestamp after everything recorded so far; `name` labels the interval that ends here ("start" for the first one).
    void stamp(VkCommandBuffer cmd, uint32_t slot, const char* name);
    // The intervals of the slot's last recorded frame, once the GPU finished it. False when nothing was recorded or the results are not there.
    bool collect(uint32_t slot, std::vector<Interval>& out);

private:
    Device& device_;
    std::vector<VkQueryPool> pools_;   // one pool per slot: the validation layer treats a pool as in use by every pending command buffer that wrote any of its queries
    uint32_t slots_ = 0;
    double nsPerTick_ = 1.0;
    uint32_t validBits_ = 64;
    std::vector<std::vector<std::string>> names_;   // per slot, one per written stamp
};

} // namespace native::gpu
