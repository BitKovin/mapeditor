#pragma once
#include <string>

namespace core {

// Deliberately tiny and hard-coded. Natural future home of an FGD-like
// entity-definition system. Two related-but-different predicates:
//
//   isStructural(): worldspawn / func_detail / func_group. Static world:
//     lightmapped, and never shows a custom-origin gizmo.
//   requiresConvexBrushes(): worldspawn / func_group ONLY. These generate
//     vis portals at export, so their brushes must be convex closed solids.
//     func_detail is structural but free-form (DESIGN.md §5.1).
//
// Everything else is a "dynamic" brush entity: custom origin, and lit from
// light volumes instead of lightmaps.
class EntityClassRegistry {
public:
    static bool isStructural(const std::string& classname);
    static bool requiresConvexBrushes(const std::string& classname);
    // Editor-only volumes (classname prefix "editor_", e.g. editor_playable_area): authored with brushes but never part of the level. They have
    // no surfaces, no texture, do not carve, block light or collide, and nothing of them is exported; the lighting reads them as bake inputs.
    static bool isEditorOnly(const std::string& classname) { return classname.rfind("editor_", 0) == 0; }
    static bool isPlayableArea(const std::string& classname) { return classname == "editor_playable_area"; }
    static bool showsCustomOriginGizmo(const std::string& classname) { return !isStructural(classname); }
};

}  // namespace core
