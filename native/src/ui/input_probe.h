// ui/input_probe -- asks the operating system whether a key or mouse button is physically down (the router's triggers: "btn:2", "key:W").
// Used only to repair the router when a release never arrived (a popup took the mouse, a window stole the keyboard mid-hold); events stay the normal path.
// Thread: UI thread.
// Spec: 10_UI_AND_INPUT.md section 3 (holds), 08_MILESTONES.md T1.6.
#pragma once

#include <functional>
#include <string>

namespace native::ui {

// True / false = what the system says. `known` is false for triggers this probe cannot map (the caller then assumes they are still down).
bool hardwareTriggerDown(const std::string& trigger, bool& known);

// --log-input: every key event the window routes, and every repair the probe makes, goes to the log (to find out what a stuck input did).
void setInputLogging(bool on);
bool inputLogging();

// The probe a MainWindow uses: down(trigger, known).
using InputProbe = std::function<bool(const std::string& trigger, bool& known)>;

} // namespace native::ui
