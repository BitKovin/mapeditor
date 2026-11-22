// ui/icons -- the editor's own monochrome icon set (14_UI_REDESIGN.md section 9.3): simple 24 x 24 stroke drawings (SVG text kept in the program, drawn in the Lucide style),
// painted in the colours of the theme tokens: normal text for the idle state, the strong text for hover and checked, the dim colour for disabled. Used by the tool bar and
// the viewport headers; a name that is unknown gives a null icon.
// Thread: UI thread only.
// Spec: 14_UI_REDESIGN.md section 9.3, 08_MILESTONES.md T14.2.
#pragma once

#include <QIcon>

#include <string>
#include <vector>

#include "ui/theme.h"

namespace native::ui {

// Every icon name ("save", "undo", "select", "box", "clipper", "move" ...).
const std::vector<std::string>& iconNames();
bool hasIcon(const std::string& name);

// The icon in the colours of `t` (default: the theme that is applied now). Cached per name and colour set.
QIcon icon(const std::string& name);
QIcon icon(const std::string& name, const Tokens& t);

// The SVG text of one icon in one colour (#rrggbb); empty for an unknown name. Pure text, so a test can read it.
QString iconSvg(const std::string& name, const QColor& stroke);

} // namespace native::ui
