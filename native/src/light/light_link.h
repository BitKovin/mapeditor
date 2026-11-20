// light/light_link -- what the bake publishes to the renderer: the mailbox ring and the lit meshes of the current lighting epoch.
//
// Owns: the Mailbox (light/mailbox.h) and the latest LitSet. An epoch starts whenever the bake builds a new layout (every scene snapshot, for now): the LitSet holds
// the meshes with lightmap coordinates of that layout, the page count and atlas size the display images need, the display exposure, and the bake value of the
// epoch's first mailbox (mailboxes composed before it belong to the previous layout and are dropped by the renderer).
// Thread: the bake thread publishes; the render thread reads current() once per frame and takes mailboxes. `wanted` is set by the UI / renderer when some viewport
// shows a lit mode; the bake does nothing while it is false.
// Spec: 06_LIGHTING.md §8, 05_RENDERER.md §4, 08_MILESTONES.md T6.4.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <core/lightmap_uv.h>

#include "gpu/device.h"
#include "light/mailbox.h"
#include "scene/render_mesh.h"
#include "scene/snapshot.h"

namespace native::light {

struct LitObject {
    std::shared_ptr<const scene::RenderMesh> source;   // the mesh the lit copy was made from (what the snapshot's object held)
    std::shared_ptr<const scene::RenderMesh> lit;
};

struct LitSet {
    uint64_t epoch = 0;
    uint64_t firstBakeValue = 0;     // bake timeline value of the first mailbox composed for this epoch
    int pageCount = 0, atlasSize = 1024;
    float exposure = 1, brightness = 0;
    std::shared_ptr<const core::LightmapLayout> layout;   // the layout the meshes and the display pages follow
    std::unordered_map<scene::ObjectKey, LitObject, scene::ObjectKey::Hash> objects;
};

class LightLink {
public:
    explicit LightLink(gpu::Device& device, uint32_t mailboxEntries = 1u << 20, uint32_t mailboxSlots = 3);
    LightLink(const LightLink&) = delete;
    LightLink& operator=(const LightLink&) = delete;

    bool valid() const { return mailbox_.valid(); }
    Mailbox& mailbox() { return mailbox_; }

    void publish(std::shared_ptr<const LitSet> set);
    std::shared_ptr<const LitSet> current() const;

    // Called by the bake thread after it committed a mailbox or published a new LitSet (the renderer wakes up and draws).
    void setNotify(std::function<void()> fn);
    void notify();

    void setWanted(bool wanted) { wanted_.store(wanted); }
    bool wanted() const { return wanted_.load(); }

    // Progress, written by the bake thread for the status line / Stats panel.
    struct Progress {
        std::atomic<uint64_t> epoch{0};
        std::atomic<uint32_t> chunks{0}, convergedChunks{0}, pages{0};
        std::atomic<uint64_t> samples{0}, passesDone{0};
        std::atomic<bool> building{false};
    };
    Progress& progress() { return progress_; }
    const Progress& progress() const { return progress_; }

private:
    Mailbox mailbox_;
    mutable std::mutex mutex_;
    std::shared_ptr<const LitSet> current_;
    std::function<void()> notify_;
    std::atomic<bool> wanted_{false};
    Progress progress_;
};

} // namespace native::light
