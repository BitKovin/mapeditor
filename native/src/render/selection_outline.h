// render/selection_outline -- the lines that show what is selected: the edges of the selected brushes (orange), the selected face (yellow),
// the box of a point entity or of a prefab instance.
//
// Owns: one pure function from a scene snapshot and a selection to a world-space line list (two vertices a line) for MeshPass; the lines are
// drawn depth-tested with a bias so the part behind a wall is hidden and the edges on a surface win against it.
// Thread: any (pure).
// Spec: 05_RENDERER.md section 6 (outlines), 08_MILESTONES.md T3.5.
#pragma once

#include <vector>

#include "doc/selection.h"
#include "render/line_vertex.h"
#include "scene/snapshot.h"

namespace native::render {

std::vector<LineVertex> selectionOutline(const scene::SceneSnapshot& snapshot, const std::vector<doc::SelRef>& items);

} // namespace native::render
