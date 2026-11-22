// ui/theme -- the look of the application (10_UI_AND_INPUT.md U15): a dark or light theme (the Fusion style with a palette) or the system's own, and a UI scale that
// multiplies the application font (independent of the Windows DPI scale). Kept in the user settings ("ui/theme", "ui/scale"); applied at start and from Settings > General.
// Thread: UI thread only.
// Spec: 10_UI_AND_INPUT.md section 7 (U15), 08_MILESTONES.md M11 item 11.
#pragma once

#include <QColor>
#include <QPalette>
#include <QString>

#include <string>

class QSettings;

namespace native::ui {

struct Appearance {
    std::string theme = "dark";     // "dark" (the default) or "light" (the Fusion style with the editor's colours), "system" (what Qt starts with)
    int scalePercent = 100;         // 70..200

    bool operator==(const Appearance&) const = default;
};

inline constexpr int kScaleMin = 70, kScaleMax = 200;

// The stored appearance; unknown themes and wild scales fall back to the defaults / the nearest limit. A null `settings` gives the defaults.
Appearance readAppearance(QSettings* settings);
void writeAppearance(QSettings* settings, const Appearance& a);
Appearance sanitize(const Appearance& a);

// The colours of the editor's chrome (14_UI_REDESIGN.md section 9.1; the Shell's design tokens, shell/src/style.css). Surfaces go from the darkest (bg0: menu bar, tab strip, status
// bar) to the lightest (bg4: pressed buttons); `line` separates panels, `accent` marks what is active or selected.
struct Tokens {
    QColor bg0, bg1, bg2, bg3, bg4;
    QColor line, line2, lineHover;
    QColor text, textStrong, dim;
    QColor accent, accentBg;
    QColor ok, warn, bad;
};
Tokens darkTokens();
Tokens lightTokens();
// The tokens of the theme that is applied now (dark until applyAppearance says otherwise).
const Tokens& tokens();

// The style sheet built from the tokens: the docking chrome (tabs, caption bars, splitters, the auto-hide bar) always, the rest of the application (menus, buttons,
// inputs, scroll bars, tool bars, tool tips) when `full`. Pure text, so a test can read it without a window.
QString dockStyleSheet(const Tokens& t);
QString appStyleSheet(const Tokens& t, bool full);

// The dark palette (the light one is the Fusion style's own).
QPalette darkPalette();

// Applies the appearance to the application. The style, palette and font the application started with are remembered the first time, so "system" and 100 % bring them back.
void applyAppearance(const Appearance& a);

} // namespace native::ui
