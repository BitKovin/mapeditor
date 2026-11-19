// light -- owns the live lighting bake: layout, texel samples, scheduler, passes, denoise, light volume.
//
// Threads: bake thread; the job pool prepares inputs.
// Spec: 06_LIGHTING.md; tasks M5-M8, M10
//
// This header only identifies the module (used by the skeleton test that checks every module library links).
#pragma once

namespace native::light {

const char* moduleName();

} // namespace native::light
