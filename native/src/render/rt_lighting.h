// render/rt_lighting -- the level settings the ray traced Lit path shows: ambient, ambient occlusion, exposure and brightness of the worldspawn.
//
// Owns: a plain value (RtLighting) and the function that reads it from a document version (the same keys and rules as the bake: core::levelSettingsOf, `_ambient`).
// Thread: any (pure functions). Spec: 12_RT_VIEWPORT.md §2, 06_LIGHTING.md §6.
#pragma once

#include <cstdint>

#include "doc/document.h"

namespace native::render {

struct RtLighting {
    float ambient[3] = {0, 0, 0};
    bool aoActive = false;                       // radius > 0, intensity > 0 and strength > 0
    float aoRadius = 0, aoIntensity = 1, aoStrength = 1, aoPower = 1, aoFalloff = 1;
    float exposure = 1, brightness = 0;          // the display: clamp(colour * exposure + brightness)

    // What the traced occlusion depends on: a change of these restarts the accumulation (the rest only changes how the mean is shown).
    bool sameTrace(const RtLighting& o) const { return aoActive == o.aoActive && aoRadius == o.aoRadius && aoFalloff == o.aoFalloff; }
    bool operator==(const RtLighting& o) const;
    bool operator!=(const RtLighting& o) const { return !(*this == o); }

    // The shown factor of an occlusion `o` in 0..1 (core/lights.h): 1 - intensity * (1 - (1 - min(1, strength * o))^power); 1 when the occlusion is not active.
    float aoFactor(float occlusion) const;
};

// The worldspawn's settings of a document version (defaults without a worldspawn).
RtLighting rtLightingOf(const doc::DocumentVersion& version);

} // namespace native::render
