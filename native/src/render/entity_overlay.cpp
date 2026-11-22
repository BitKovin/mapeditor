// render/entity_overlay -- see entity_overlay.h.
#include "render/entity_overlay.h"

#include <deque>
#include <set>

#include "doc/document.h"
#include "scene/arrows.h"
#include "scene/light_gizmo.h"
#include "scene/model_entity.h"
#include "scene/point_box.h"

namespace native::render {

namespace {

const std::array<float, 3> kDefaultMarker = {1.0f, 0.85f, 0.25f};
const std::array<float, 3> kPicked = {1.0f, 0.62f, 0.15f};

void addLines(std::vector<LineVertex>& out, const std::vector<double>& flat, uint32_t color) {
    for (size_t i = 0; i + 2 < flat.size(); i += 3)
        out.push_back({{static_cast<float>(flat[i]), static_cast<float>(flat[i + 1]), static_cast<float>(flat[i + 2])}, color});
}

LineVertex vertex(const scene::V3& p, uint32_t color) { return {{static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])}, color}; }

uint32_t packRgb(const std::array<double, 3>& c, float a = 1.0f) {
    return packColor(static_cast<float>(c[0]), static_cast<float>(c[1]), static_cast<float>(c[2]), a);
}

} // namespace

EntityOverlay buildEntityOverlay(const scene::SceneSnapshot& snap, const std::vector<doc::SelRef>& selection, const core::EntityDefinitions* defs,
                                 const EntityOverlayOptions& opt) {
    EntityOverlay out;
    if (!snap.doc) return out;
    const doc::DocumentVersion& doc = *snap.doc;

    std::set<std::string> selectedEntities, selectedInstances;
    for (const doc::SelRef& s : selection) {
        if (!s.instancePath.empty()) selectedInstances.insert(s.instancePath.front());
        else if (s.brushId.empty() && !s.entityId.empty()) selectedEntities.insert(s.entityId);
    }

    // the document's entities; a brush entity gets a copy that carries its brushes (the nodes keep them apart)
    std::deque<core::Entity> copies;
    std::vector<const core::Entity*> docEntities;
    for (const auto& node : doc.entities) {
        if (node->brushes.empty() || node->typed.classname == "worldspawn") {
            docEntities.push_back(&node->typed);
            continue;
        }
        copies.push_back(node->typed);
        for (const auto& b : node->brushes) copies.back().brushes.push_back(b->brush);
        docEntities.push_back(&copies.back());
    }
    std::vector<core::ResolvedEntity> prefab;
    if (snap.prefabs) prefab = snap.prefabs->flat();
    const std::vector<scene::Placed> items = scene::placedEntities(docEntities, prefab);

    const auto isSelected = [&](const scene::Placed& p) {
        if (!p.ids.empty()) return selectedInstances.count(p.ids.front()) != 0;
        return selectedEntities.count(p.entity->id) != 0;
    };
    const auto classColor = [&](const std::string& classname) -> std::array<float, 3> {
        if (defs)
            if (const core::EntClass* c = defs->find(classname)) return c->color;
        return kDefaultMarker;
    };
    const scene::BoxOf boxOf = scene::classBoxes(defs);

    for (const scene::Placed& p : items) {
        const core::Entity& e = *p.entity;
        if (opt.hidden && !opt.hidden->empty() && opt.hidden->count(p.ids.empty() ? scene::ObjectKey{"", e.id, ""} : scene::ObjectKey{p.ids.front(), "", ""})) continue;
        if (e.isBrushEntity()) {
            if (opt.origins && p.ids.empty() && e.classname != "worldspawn" && e.showsCustomOriginGizmo()) {
                const uint32_t c = packColor(0.3f, 0.9f, 0.95f);
                const scene::V3 o{e.origin.x, e.origin.y, e.origin.z};
                const double r = 0.25;
                for (size_t a = 0; a < 3; ++a) {
                    scene::V3 lo = o, hi = o;
                    lo[a] -= r;
                    hi[a] += r;
                    out.lines.push_back(vertex(lo, c));
                    out.lines.push_back(vertex(hi, c));
                }
            }
            continue;
        }
        const bool picked = isSelected(p);
        const std::array<float, 3> col = picked ? kPicked : classColor(e.classname);
        const scene::OrientedBox ob = scene::pointBox(e, boxOf);
        if (opt.boxes && scene::drawsBox(e)) {
            // an entity that shows a model: the model is drawn, its box only outlines it while it is picked (a filled box would hide it)
            const bool showsModel = scene::entityModelBox(e).has_value();
            const auto corners = scene::worldCorners(ob);
            const uint32_t line = packColor(col[0], col[1], col[2], 1.0f);
            if (!showsModel || picked)
                for (size_t i = 0; i < scene::kBoxEdges.size(); i += 2) {
                    out.lines.push_back(vertex(corners[static_cast<size_t>(scene::kBoxEdges[i])], line));
                    out.lines.push_back(vertex(corners[static_cast<size_t>(scene::kBoxEdges[i + 1])], line));
                }
            const uint32_t fill = packColor(col[0], col[1], col[2], kEntityFillAlpha);
            static const int quads[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4}, {3, 2, 6, 7}, {0, 3, 7, 4}, {1, 2, 6, 5}};
            if (!showsModel)
                for (const auto& q : quads)
                    for (const int k : {0, 1, 2, 0, 2, 3}) out.fills.push_back(vertex(corners[static_cast<size_t>(q[k])], fill));
        }
        if (opt.boxes) {
            const scene::V3 centre = scene::toWorld(ob, {(ob.min[0] + ob.max[0]) / 2, (ob.min[1] + ob.max[1]) / 2, (ob.min[2] + ob.max[2]) / 2});
            out.labels.push_back({e.classname, centre, picked ? kLabelAlwaysRgba : kLabelNearRgba});
        }
        if (opt.lightGizmos && scene::isModernLightClass(e.classname)) {
            const core::EntClass* def = defs ? defs->find(e.classname) : nullptr;
            if (const auto params = scene::lightParams(e, def)) {
                const scene::V3 gc = scene::lightGizmoColor(*params);
                addLines(out.lines, scene::lightGizmo(e, *params, picked), packRgb(gc));
            }
        }
    }

    if (opt.directions || opt.links) {
        std::set<std::string> present;
        for (const scene::Placed& p : items)
            for (const auto& [k, v] : p.entity->refs) present.insert(k);
        scene::ArrowOptions ao;
        ao.boxOf = boxOf;
        ao.colorOf = [&](const std::string& c) -> std::optional<scene::Rgb> {
            if (!defs || !defs->find(c)) return std::nullopt;
            const auto cc = classColor(c);
            return scene::Rgb{cc[0], cc[1], cc[2]};
        };
        ao.isSelected = isSelected;
        ao.refKeys = [&](const std::string& classname) {
            std::set<std::string> keys = {"target", "killtarget"};
            if (defs)
                if (const core::EntClass* c = defs->find(classname))
                    for (const core::EntKey& k : c->keys)
                        if (k.type == "target_destination" || k.type == "target") keys.insert(k.key);
            keys.insert(present.begin(), present.end());
            return std::vector<std::string>(keys.begin(), keys.end());
        };
        ao.showDirections = opt.directions;
        ao.showAllLinks = opt.links;
        for (const scene::Overlay& o : scene::entityArrows(items, ao)) addLines(out.lines, o.lines, packRgb(o.color));
    }
    return out;
}

} // namespace native::render
