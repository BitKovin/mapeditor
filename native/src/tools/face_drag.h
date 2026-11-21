// tools/face_drag -- Radiant-style face drag (port of shell/src/model/faceDrag.ts): the face's PLANE moves along its normal, every other face keeps its plane (no rotation),
// so the dragged face grows or shrinks; Alt-drag "extend face" grows a new brush out of a face.
// Pure functions of core::Brush. Thread: any. Spec: 08_MILESTONES.md M11 item 2 (T11.2), 07_EDITOR_PORT.md B.
#pragma once

#include <core/brush.h>

#include <string>

namespace native::tools {

struct DragResult {
    core::Brush brush;
    std::string error;   // empty = fine
    bool ok() const { return error.empty(); }
};

// Moves face `faceId` by `dist` along its outward normal. Each vertex of the face slides along the edge where its two neighbouring planes meet; a corner where three or more
// planes meet (a pyramid apex) is solved by re-intersecting the planes of a convex brush. Faces keep their ids and textures, uvs follow the old mapping. Refuses a collapsed,
// inside-out or invalid result with a message.
DragResult dragFace(const core::Brush& b, uint32_t faceId, double dist);
// Alt-drag "extend face": a NEW brush grows out of the face by `dist` (> 0) along its normal, as a prism over the face's outline; `b` is untouched. The side walls continue the
// texture mapping of the face they grow from. The result keeps op and carve; face ids: 0 = end cap, 1 = base, 2.. = side walls in outline order.
DragResult extendFace(const core::Brush& b, uint32_t faceId, double dist, const std::string& newId);

} // namespace native::tools
