// ui/select_actions -- Select all (Ctrl+A), Invert selection (Ctrl+I), Select none (Alt+A, Esc) and the Radiant arrow nudge (arrows, PageUp / PageDown) of the object
// tool (M11 item 10, the Shell's keys). Registered into the same registry as the application's actions; the default keys are in assets/keymaps/default.json.
// Thread: UI thread. Spec: 10_UI_AND_INPUT.md section 3, 08_MILESTONES.md M11 item 10.
#pragma once

#include "app/actions/registry.h"

namespace native::ui {

void registerSelectActions(actions::Registry& registry);

} // namespace native::ui
