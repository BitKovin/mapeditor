#include "core/entity_class.h"
#include <unordered_set>

namespace core {

bool EntityClassRegistry::isStructural(const std::string& classname) {
    static const std::unordered_set<std::string> kStructural = {"worldspawn", "func_detail", "func_group"};
    return kStructural.count(classname) > 0;
}

bool EntityClassRegistry::requiresConvexBrushes(const std::string& classname) {
    static const std::unordered_set<std::string> kConvex = {"worldspawn", "func_group"};
    return kConvex.count(classname) > 0;
}

}  // namespace core
