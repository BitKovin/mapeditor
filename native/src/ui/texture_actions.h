// ui/texture_actions -- the Radiant texturing actions (T11.3d): apply the active texture, the texture keys (Shift+arrows shift, Ctrl+Shift+arrows scale, Shift+PageUp / PageDown
// rotate; Alt = fine), justify, fit, reset, project, flow, copy / paste UV, hotspot and lightmap density, in the "&Texture" menu. Registered into the same registry as the application's actions; the default keys are in assets/keymaps/default.json.
// Thread: UI thread. Spec: 10_UI_AND_INPUT.md section 3, 08_MILESTONES.md M11 item 3.
#pragma once

#include "app/actions/registry.h"

namespace native::ui {

void registerTextureActions(actions::Registry& registry);

} // namespace native::ui
