// light/mailbox -- how the bake hands finished display texels to the renderer without sharing memory (06_LIGHTING.md §8).
//
// Owns: a small ring of device buffers (the mailboxes, lists of 16-byte entries {x | y << 16, page, rg, ba} with half floats), two timeline semaphores (the bake
// timeline: signalled by the compose submit with the mailbox's value; the render timeline: signalled by the frame that scattered a mailbox) and the bookkeeping
// of which mailbox is free / being composed / committed / consumed. `Scatter` is the render thread's pass that copies a mailbox into the display page images.
// Threads: the bake thread acquires, composes (Passes::recordCompose into buffer(slot)), submits signalling the bake timeline and commits; the render thread
// takes the newest committed mailbox whose value the bake timeline reached, records the scatter, submits (waiting for that bake value, signalling the render
// value) and marks it consumed. Both sides lock a mutex only for the bookkeeping.
// Rules: a mailbox holds the FULL current values of every chunk dirty since the last consumed one, so the render thread may skip older ones; the bake thread keeps
// a chunk dirty until `consumedBakeValue()` reaches the value of a mailbox that contained it. A slot is reused only after the render timeline reached the value of
// the frame that consumed it; when no slot is free the bake skips composing and keeps the chunks dirty.
// Spec: 06_LIGHTING.md §8, 08 T6.4.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "gpu/arena.h"
#include "gpu/device.h"

namespace native::light {

struct MailboxEntry {
    uint32_t xy;     // x | y << 16
    uint32_t page;
    uint32_t rg;     // two half floats
    uint32_t ba;
};
static_assert(sizeof(MailboxEntry) == 16);

class Mailbox {
public:
    // `slots` buffers of `entriesPerSlot` entries each.
    Mailbox(gpu::Device& device, uint32_t entriesPerSlot, uint32_t slots = 3);
    ~Mailbox();
    Mailbox(const Mailbox&) = delete;
    Mailbox& operator=(const Mailbox&) = delete;

    bool valid() const { return valid_; }
    uint32_t entriesPerSlot() const { return entriesPerSlot_; }
    uint32_t slotCount() const { return static_cast<uint32_t>(slots_.size()); }
    VkBuffer buffer(int slot) const { return slots_[static_cast<size_t>(slot)].buffer->buffer(); }
    VkSemaphore bakeTimeline() const { return bakeTimeline_; }
    VkSemaphore renderTimeline() const { return renderTimeline_; }
    uint64_t completedBake() const;       // the bake timeline's current value
    uint64_t completedRender() const;

    // ---- bake thread
    // A free slot (not in use, and the frame that consumed its last content finished), or -1.
    int acquire();
    // The slot holds `entries` entries and its compose submit will signal `bakeValue` (> every earlier value) on the bake timeline.
    void commit(int slot, uint32_t entries, uint64_t bakeValue);
    // The slot was acquired but nothing was composed.
    void abandon(int slot);
    // The bake value of the newest mailbox the render thread consumed (0 = none): chunks sent in mailboxes up to it are no longer dirty.
    uint64_t consumedBakeValue() const;

    // ---- render thread
    struct Taken {
        int slot = -1;
        uint32_t entries = 0;
        uint64_t bakeValue = 0;   // the frame's submit must wait for it on the bake timeline
    };
    // The newest committed mailbox whose compose finished; older committed ones are dropped (their content is in the newer). slot < 0: nothing new.
    Taken takeNewest();
    // The scatter of `slot` was recorded in a frame that will signal `renderValue` on the render timeline.
    void consumed(int slot, uint64_t renderValue);

private:
    enum class State { Free, Composing, Committed, Consumed };
    struct Slot {
        std::unique_ptr<gpu::Arena> buffer;
        State state = State::Free;
        uint32_t entries = 0;
        uint64_t bakeValue = 0, renderValue = 0;
    };
    gpu::Device& device_;
    uint32_t entriesPerSlot_;
    std::vector<Slot> slots_;
    VkSemaphore bakeTimeline_ = VK_NULL_HANDLE, renderTimeline_ = VK_NULL_HANDLE;
    uint64_t consumedBake_ = 0;
    bool valid_ = false;
    mutable std::mutex mutex_;
};

// The render thread's scatter pass: mailbox entries -> display page images (RGBA16F 2D array in GENERAL layout).
class Scatter {
public:
    explicit Scatter(gpu::Device& device);
    ~Scatter();
    Scatter(const Scatter&) = delete;
    Scatter& operator=(const Scatter&) = delete;
    bool valid() const { return valid_; }
    // Frees the descriptor sets of earlier records (their frames finished).
    void resetDescriptors();
    // Records the copy of `entries` entries from `mailbox` into `display` (a view of the whole array) and a barrier that makes the writes visible to fragment
    // shader reads and to later scatters.
    bool record(VkCommandBuffer cmd, VkBuffer mailbox, uint32_t entries, VkImageView display);

private:
    gpu::Device& device_;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule module_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    bool valid_ = false;
};

} // namespace native::light
