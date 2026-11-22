// light/light_filter -- which lights the live lighting uses (the Lighting dock, 10_UI_AND_INPUT.md U12): lights can be muted, or one light soloed (all others off). Editor
// state only: the document is not touched and the export never sees it. Both live light paths (the Lit bake, the ray traced Lit view) apply it to the light list they collect.
// Thread: any (immutable once shared).
// Spec: 10_UI_AND_INPUT.md section 7 (U12), 08_MILESTONES.md M11 item 11.
#pragma once

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include <core/lights.h>

namespace native::light {

struct LightFilter {
    std::set<std::string> muted;   // keys of lights (keyOf) that give no light
    std::string solo;              // a key: only this light gives light (the muted ones stay off); "" = none soloed

    bool active() const { return !muted.empty() || !solo.empty(); }
    /// The key of a light: its entity id, with the prefab instance path in front when it is inside one ("inst1/e3").
    static std::string keyOf(const std::string& instance, const std::string& id) { return instance.empty() ? id : instance + "/" + id; }
    static std::string keyOf(const core::Light& l) { return keyOf(l.instance, l.id); }
    bool keeps(const core::Light& l) const {
        const std::string k = keyOf(l);
        if (muted.count(k)) return false;
        return solo.empty() || solo == k;
    }
    /// Removes the lights that give no light now.
    void apply(std::vector<core::Light>& lights) const {
        if (!active()) return;
        lights.erase(std::remove_if(lights.begin(), lights.end(), [&](const core::Light& l) { return !keeps(l); }), lights.end());
    }
    bool operator==(const LightFilter&) const = default;
};

} // namespace native::light
