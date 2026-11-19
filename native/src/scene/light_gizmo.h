// scene/light_gizmo -- the preview gizmo of the modern light entities (port of the viewport half of shell/src/model/lights.ts).
//
// Owns: a light's parameters read like the baker reads them (same keys and clamps, the game's .ent giving the defaults of keys that are not set), the
// gizmo colour and the gizmo as line endpoints: every light gets a compact icon (what it is, where it looks, its real rectangle / cone shape); a selected
// light also shows its reach (attenuation radius, cones out to the radius, source size).
// Thread: any (pure).
// Spec: 05_RENDERER.md section 5, 08_MILESTONES.md T3.6.
#pragma once

#include <optional>
#include <vector>

#include "core/document.h"
#include "core/entdef.h"
#include "scene/point_box.h"

namespace native::scene {

enum class LightKind { Point, Spot, Directional, Rect, Sky };

struct LightParams {
    LightKind kind = LightKind::Point;
    V3 color{1, 1, 1};
    double brightness = 1;   // any sign: a negative brightness is a dark light that subtracts light
    double radius = 10, bounce = 1;
    bool castShadows = true;
    int style = 0;
    double sourceSize = 0;   // sphere radius in metres (point, spot) or angular diameter in degrees (directional); 0 = the live bake's default softness
    double innerAngle = 20, outerAngle = 35;
    double width = 1, height = 1;
};

// nullopt for a class that is not a modern light. `def` = the class in the entity definitions (may be null).
std::optional<LightParams> lightParams(const core::Entity& e, const core::EntClass* def);
bool isDarkLight(const LightParams& p);
V3 lightGizmoColor(const LightParams& p);
// The gizmo's lines: x, y, z, x, y, z, ... (two points a line).
std::vector<double> lightGizmo(const core::Entity& e, const LightParams& p, bool selected);

} // namespace native::scene
