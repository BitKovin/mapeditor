// light/light_pack -- the lights of a style layer as the records shaders/direct.comp reads.
//
// Owns: nothing (a pure function). A port of packLights in core/src/gpu_bake.cpp: 8 vec4 per light (position / radius, colour x brightness / type, forward /
// cos inner, right / cos outer, up / source size, width / height / casts shadows / style, bounce power, sky table base), then the skybox tables of the first
// sky light that has them (radiance cells, then irradiance samples), four floats per cell.
// Thread: any. Spec: 06_LIGHTING.md §10, 08 T6.4.
#pragma once

#include <vector>

#include "core/lights.h"

namespace native::light {

constexpr size_t kLightRecordFloats = 32;   // 8 vec4

// Records of `lights` (a light's brightness already includes the level's _light_scale), then the sky tables. Empty for no lights.
std::vector<float> packLights(const std::vector<core::Light>& lights);

} // namespace native::light
