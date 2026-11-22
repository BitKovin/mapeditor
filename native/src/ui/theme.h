// ui/theme -- the look of the application (10_UI_AND_INPUT.md U15): a dark or light theme (the Fusion style with a palette) or the system's own, and a UI scale that
// multiplies the application font (independent of the Windows DPI scale). Kept in the user settings ("ui/theme", "ui/scale"); applied at start and from Settings > General.
// Thread: UI thread only.
// Spec: 10_UI_AND_INPUT.md section 7 (U15), 08_MILESTONES.md M11 item 11.
#pragma once

#include <QPalette>

#include <string>

class QSettings;

namespace native::ui {

struct Appearance {
    std::string theme = "system";   // "system" (what Qt starts with), "dark" or "light" (the Fusion style)
    int scalePercent = 100;         // 70..200

    bool operator==(const Appearance&) const = default;
};

inline constexpr int kScaleMin = 70, kScaleMax = 200;

// The stored appearance; unknown themes and wild scales fall back to the defaults / the nearest limit. A null `settings` gives the defaults.
Appearance readAppearance(QSettings* settings);
void writeAppearance(QSettings* settings, const Appearance& a);
Appearance sanitize(const Appearance& a);

// The dark palette (the light one is the Fusion style's own).
QPalette darkPalette();

// Applies the appearance to the application. The style, palette and font the application started with are remembered the first time, so "system" and 100 % bring them back.
void applyAppearance(const Appearance& a);

} // namespace native::ui
