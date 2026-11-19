// ui -- owns the Qt panels and windows (the only module besides render/viewport_window that includes Qt).
//
// Threads: UI thread.
// Spec: 07_EDITOR_PORT.md group C, 10_UI_AND_INPUT.md; tasks M1, M4, M11
//
// This header only identifies the module (used by the skeleton test that checks every module library links).
#pragma once

namespace native::ui {

const char* moduleName();

} // namespace native::ui
