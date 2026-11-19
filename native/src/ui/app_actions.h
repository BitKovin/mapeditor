// ui/app_actions -- the actions of the application shell: file, edit, view / layout, the cameras of a viewport.
//
// Owns: what the registry knows about the window (file.exit, edit.undo / redo, view.layout, camera.mode, view.stats.toggle) and the
// camera input of a viewport (camera.look / orbit / pan2d / pan / dolly / fly.*), which used to be hard-coded in ViewportWindow. The
// default keys are in assets/keymaps/default.json. Later modules register their own actions the same way.
// Thread: UI thread.
// Spec: 10_UI_AND_INPUT.md §2 - §4, 08_MILESTONES.md T1.6.
#pragma once

#include "app/actions/registry.h"

namespace native::render { class ViewportWindow; }

namespace native::ui {

class MainWindow;

// What ActionEnv::app points to for the actions below.
struct UiEnv {
    MainWindow* window = nullptr;
    render::ViewportWindow* viewport = nullptr;   // the viewport the input belongs to (null for menu actions without one)
};

// Registers every action above into `registry` (once per registry).
void registerAppActions(actions::Registry& registry);

// Contexts a viewport adds to the stack: "area:viewport", "view:perspective" | "view:ortho" and the mode's own ("view:fly",
// "view:orbit", "view:ortho").
std::vector<std::string> viewportContexts(const render::ViewportWindow& viewport);

} // namespace native::ui
