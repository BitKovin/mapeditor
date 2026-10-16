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
    static bool showsCustomOriginGizmo(const std::string& classname) { return !isStructural(classname); }
};

}  // namespace core
