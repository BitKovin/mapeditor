// tools/status_text -- the texts of the permanent status bar fields: tool, grid and snap of the active viewport, what is selected, the game folder and the map.
// The window only puts them into labels. Thread: any (pure).
// Spec: 08_MILESTONES.md T4.5.
#pragma once

#include <string>

#include "doc/document.h"
#include "doc/selection.h"
#include "render/viewport_settings.h"

namespace native::tools {

// "Grid 0.5 m, snap on" / "Grid 1 m, snap off"; the rotation step is added when snapping ("rot 15°").
std::string gridText(const render::ViewportSettings& s);
// "Nothing selected" / "1 brush" / "3 brushes" / "light_point" (one entity: its class) / "2 entities, 1 brush" / "1 face" / "1 prefab instance".
// `doc` may be null (then only the counts are told).
std::string selectionText(const doc::Selection& selection, const doc::DocumentVersion* doc);
// The last folder of the assets root ("GameData"), or "No game folder" when empty.
std::string folderText(const std::string& assetsRoot);
// "untitled" / the map's path, with " *" while it has unsaved changes; "No map" without a document.
std::string mapText(const doc::DocumentVersion* doc, bool dirty);
// The number as short text: 0.5, 2, 22.5 (no trailing zeros).
std::string shortNumber(double v);

} // namespace native::tools
