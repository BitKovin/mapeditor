// ui/mesh_actions -- the mesh mode actions (T11.1f): the mode and element kind, the instant tools (connect, merge, fill, dissolve, delete, flip, recalc, bridge, smooth,
// subdivide), the selection tools (all, none, invert, grow, shrink) and the click / box pick. Registered into the same registry as the application's actions; the default
// keys are in assets/keymaps/default.json (contexts "mode:mesh", "mode:mesh.vertex" | "edge" | "face").
// Thread: UI thread.
// Spec: 10_UI_AND_INPUT.md section 3, 08_MILESTONES.md M11 item 1.
#pragma once

#include "app/actions/registry.h"

namespace native::ui {

void registerMeshActions(actions::Registry& registry);

} // namespace native::ui
