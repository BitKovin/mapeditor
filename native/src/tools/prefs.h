// tools/prefs -- the editor preferences (Settings > Editor): what a NEW viewport starts with. Kept as one JSON text in the QSettings ("prefs/editor");
// unknown / missing / bad fields fall back to the defaults, numbers are clamped to what the viewport accepts.
// Thread: any (pure).
// Spec: 08_MILESTONES.md T4.5 (shell/src/ui/settingsDialog.ts, Editor page).
#pragma once

#include <string>

#include "render/viewport_settings.h"

namespace native::tools {

struct EditorPrefs {
    double gridSize = 0.5;
    bool snapEnabled = true;
    double rotSnap = 15;
    double scaleSnap = 0.1;
    double flySpeed = 8;
    bool showGrid = true;

    bool operator==(const EditorPrefs&) const = default;
};

// Clamps to the ranges the viewports accept (grid > 0, speed within kFlySpeedMin..Max, snaps > 0); non-finite values give the defaults.
EditorPrefs sanitize(const EditorPrefs& p);
std::string prefsToJson(const EditorPrefs& p);
EditorPrefs prefsFromJson(const std::string& text);
// The prefs as the start of a viewport's own settings (camera mode, render mode and the other view flags stay as they are in `s`).
render::ViewportSettings applyPrefs(const EditorPrefs& p, render::ViewportSettings s);
// The prefs a viewport's settings stand for.
EditorPrefs prefsOf(const render::ViewportSettings& s);

} // namespace native::tools
