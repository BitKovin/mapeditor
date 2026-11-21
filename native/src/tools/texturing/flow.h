// tools/texturing/flow -- Flow (port of shell/src/model/flow.ts, REQUIREMENTS section 6 B5, the user's example: a curved road). The UVs of an already textured face continue
// across a shared edge onto its neighbour, as if the neighbour were unfolded flat into the first face's plane: nothing is stretched and the texture meets itself along every
// edge. The target face (and the faces in between on the shortest chain of neighbours) receive the same texture and an editable projection of their own (kind Free).
// Thread: any (pure). Spec: 07_EDITOR_PORT.md B, 08_MILESTONES.md M11 item 3.
#pragma once

#include <core/brush.h>

#include <optional>
#include <vector>

#include "tools/texturing/surface.h"

namespace native::tools::tex {

struct FlowResult {
    core::Brush brush;
    std::vector<uint32_t> flowed;   // the faces that received UVs (the targets and the faces between), in flow order
};

/// Continues the UVs of face `sourceId` onto `targetIds` of the same brush along the shortest chains of edge-sharing faces. Nullopt when a target cannot be reached, the source
/// does not exist or no target is given. The source face is unchanged; flowed faces take the source's texture and repeat override.
std::optional<FlowResult> flowFaces(const core::Brush& b, uint32_t sourceId, const std::vector<uint32_t>& targetIds, const InfoFn& info);

} // namespace native::tools::tex
