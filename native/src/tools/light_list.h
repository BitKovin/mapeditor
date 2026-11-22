// tools/light_list -- the lights of a document as the Lighting dock lists them (10_UI_AND_INPUT.md U12): the entities of a light class (no brushes, a `light_` class) with the
// name, the class, the brightness, the colour and the style, in document order. Prefab instances' lights are not listed (they follow their instance).
// Thread: any (pure). Spec: 08_MILESTONES.md M11 item 11.
#pragma once

#include <string>
#include <vector>

#include "doc/document.h"

namespace native::tools {

struct LightRow {
    std::string id, name, classname;   // name = targetname, or the id
    double brightness = 1;
    std::string color;                 // "#rrggbb" of the `color` key (white when unset)
    int style = 0;
};

std::vector<LightRow> listLights(const doc::DocumentVersion& doc);

} // namespace native::tools
