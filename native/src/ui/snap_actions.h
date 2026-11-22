// ui/snap_actions -- the Snapping menu (M11 item 11, U5): snap on / off, grid size, rotation snap and scale snap of the active viewport as actions with
// variants (View > Snapping), also offered by a click on the grid field of the status bar. The snap values are the viewport's settings (render/viewport_settings).
// Thread: UI thread. Spec: 10_UI_AND_INPUT.md section 7 (U5), 08_MILESTONES.md M11 item 11.
#pragma once

#include "app/actions/registry.h"

namespace native::ui {

void registerSnapActions(actions::Registry& registry);

} // namespace native::ui
