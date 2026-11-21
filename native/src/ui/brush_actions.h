// ui/brush_actions -- the Radiant brush operation actions (T11.2): the clipper (X; Enter / Shift+Enter / Ctrl+Enter / Esc while it runs), Hollow, Merge, Snap to grid and
// Select inside / touching, in the "&Brush" menu. Registered into the same registry as the application's actions; the default keys are in assets/keymaps/default.json.
// Thread: UI thread. Spec: 10_UI_AND_INPUT.md section 3, 08_MILESTONES.md M11 item 2.
#pragma once

#include "app/actions/registry.h"

namespace native::ui {

void registerBrushActions(actions::Registry& registry);

} // namespace native::ui
