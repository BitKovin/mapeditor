// scene/arrows -- entity arrows (port of shell/src/model/arrows.ts, REQUIREMENTS section 17 items 1 and 2): the direction arrow of every point entity and the
// link arrows between an entity and the entities its reference keys (`target`, `killtarget`, ...) name.
//
// Pure: everything returns flat line endpoints (x, y, z, x, y, z, ...) grouped by colour. References are resolved exactly as the picker stores them: a
// scope-relative path of ids (`core::Entity::refs`), plus the older plain-name form (`keyvalues[key]` = a targetname).
// Thread: any (pure).
// Spec: 05_RENDERER.md section 5, 08_MILESTONES.md T3.6.
#pragma once

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/document.h"
#include "core/prefab.h"
#include "scene/point_box.h"

namespace native::scene {

// An entity as the viewport sees it: the document's own (`ids` empty) or prefab content (`ids` = instance ids from the root, without "root").
struct Placed {
    const core::Entity* entity = nullptr;
    std::vector<std::string> ids;
    std::string sourceId;
    std::map<std::string, size_t> refScopes;   // key -> how many leading ids the key's paths are relative to (prefab content)
};
using Rgb = std::array<double, 3>;
struct Overlay {
    Rgb color;
    std::vector<double> lines;
};
struct Link {
    size_t from = 0, to = 0;   // indices into the items
    std::string key;
};

// The document's own entities followed by the prefab content. The pointers must outlive the result.
std::vector<Placed> placedEntities(const std::vector<const core::Entity*>& docEntities, const std::vector<core::ResolvedEntity>& prefab);
// Root-relative path of an entity: the key references resolve to.
std::string pathOfPlaced(const Placed& p);

// Shaft from `from` to `tip` plus a four-barbed head (reads as a cone from any side) at `tip`.
std::vector<double> arrowLines(const V3& from, const V3& tip, double headLen);
// Axis-aligned box of an entity in root space: the bounds of a brush entity's brushes, else the class box turned with its angles.
Box boundsOf(const core::Entity& e, const BoxOf& boxOf);
// Where the direction arrow of a point entity runs: from the centre of its (turned) box along forward = R(angles) * (0, 0, 1), out through the box.
struct DirectionSpan {
    V3 from, dir;
    double len = 0;
};
DirectionSpan directionSpan(const core::Entity& e, const BoxOf& boxOf);
std::vector<double> directionArrow(const core::Entity& e, const BoxOf& boxOf);
// Centre to centre, trimmed to the surfaces of both boxes (the head touches the target's box); empty for one point.
std::vector<double> linkArrow(const Box& from, const Box& to);

// Every link between entities: `refKeys(classname)` names the reference keys of a class. `resolvedName` (default: the targetname) is what a plain-name key matches.
using RefKeys = std::function<std::vector<std::string>(const std::string& classname)>;
using ResolvedName = std::function<std::string(const Placed&)>;
std::vector<Link> resolveLinks(const std::vector<Placed>& items, const RefKeys& refKeys, const ResolvedName& resolvedName = {});

struct ArrowOptions {
    BoxOf boxOf;
    std::function<std::optional<Rgb>(const std::string& classname)> colorOf;
    std::function<bool(const Placed&)> isSelected;
    RefKeys refKeys;
    ResolvedName resolvedName;
    bool showDirections = true;
    bool showAllLinks = true;   // false = only the links that touch a selected entity
};
extern const Rgb kLinkColor, kLinkColorSelected;
// Everything the viewport draws, one Overlay per colour.
std::vector<Overlay> entityArrows(const std::vector<Placed>& items, const ArrowOptions& o);

} // namespace native::scene
