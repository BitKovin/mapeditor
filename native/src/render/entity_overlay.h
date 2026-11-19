// render/entity_overlay -- what the viewports draw for entities on top of the scene: the box of every point entity (outline, translucent fill, class name),
// the direction and link arrows, the light gizmos.
//
// Owns: one pure function from a scene snapshot, the selection, the entity definitions and the view options to coloured line / triangle lists and text
// items, built from scene/point_box, scene/arrows and scene/light_gizmo (the Shell's renderer.ts / viewport.ts overlay code). The lists are world space and
// the same for every viewport; the selection size labels are per view (render/size_overlay).
// Thread: any (pure); built on the UI thread when the snapshot, the selection or an option changes.
// Spec: 05_RENDERER.md section 5, 08_MILESTONES.md T3.6.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "core/entdef.h"
#include "doc/selection.h"
#include "render/line_vertex.h"
#include "scene/snapshot.h"

namespace native::render {

// A text drawn at a world point (the glyph quads are placed on the render thread).
struct TextItem {
    std::string text;
    std::array<double, 3> at{0, 0, 0};
    uint32_t rgba = 0xFFFFFFFFu;
};

struct EntityOverlay {
    std::vector<LineVertex> lines;   // two vertices a line, depth tested
    std::vector<LineVertex> fills;   // three vertices a triangle, translucent, depth tested without writing
    std::vector<TextItem> labels;    // class names at the box centres
};

struct EntityOverlayOptions {
    bool boxes = true, directions = true, links = true, lightGizmos = true, origins = true;
};

constexpr float kEntityFillAlpha = 0.35f;   // GHOST_ALPHA * 0.7 of the Shell

EntityOverlay buildEntityOverlay(const scene::SceneSnapshot& snapshot, const std::vector<doc::SelRef>& selection, const core::EntityDefinitions* defs,
                                 const EntityOverlayOptions& options = {});

} // namespace native::render
