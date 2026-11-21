// render/model_scene -- see model_scene.h.
#include "render/model_scene.h"

#include <algorithm>
#include <deque>

#include "scene/arrows.h"
#include "scene/model_entity.h"

namespace native::render {

std::shared_ptr<const ModelScene> buildModelScene(const scene::SceneSnapshot& snap, scene::ModelStore& store, const std::unordered_set<scene::ObjectKey, scene::ObjectKey::Hash>* hidden) {
    if (!snap.doc) return nullptr;
    const doc::DocumentVersion& doc = *snap.doc;
    // the entities as the viewport sees them: the document's own, then the prefab content (brush entities are skipped below, so they need no brushes)
    std::vector<const core::Entity*> docEntities;
    for (const auto& node : doc.entities)
        if (node->brushes.empty() && node->typed.classname != "worldspawn") docEntities.push_back(&node->typed);
    std::vector<core::ResolvedEntity> prefab;
    if (snap.prefabs) prefab = snap.prefabs->flat();
    const std::vector<scene::Placed> items = scene::placedEntities(docEntities, prefab);

    auto out = std::make_shared<ModelScene>();
    for (const scene::Placed& p : items) {
        const core::Entity& e = *p.entity;
        if (e.isBrushEntity()) continue;
        const std::string path = scene::modelPathOf(e);
        if (path.empty()) continue;
        if (hidden && !hidden->empty() && hidden->count(p.ids.empty() ? scene::ObjectKey{"", e.id, ""} : scene::ObjectKey{p.ids.front(), "", ""})) continue;
        const scene::ModelStore::Slot slot = store.get(path);
        if (slot.status != scene::ModelStore::Status::Ready || !slot.model) continue;
        ModelPlacement m;
        m.model = slot.model;
        const scene::V3 origin{e.origin.x, e.origin.y, e.origin.z}, angles{e.angles.x, e.angles.y, e.angles.z};
        m.matrix = scene::modelMatrix(origin, angles, scene::modelScaleOf(e));
        m.animation = scene::animationIndex(slot.model->data, scene::modelAnimationOf(e));
        // the world box of the model's box
        const scene::Box& b = slot.model->data.bounds;
        scene::Box w{{1e300, 1e300, 1e300}, {-1e300, -1e300, -1e300}};
        for (int c = 0; c < 8; ++c) {
            const double x = c & 1 ? b.max[0] : b.min[0], y = c & 2 ? b.max[1] : b.min[1], z = c & 4 ? b.max[2] : b.min[2];
            for (size_t i = 0; i < 3; ++i) {
                const double v = m.matrix[i] * x + m.matrix[4 + i] * y + m.matrix[8 + i] * z + m.matrix[12 + i];
                w.min[i] = std::min(w.min[i], v);
                w.max[i] = std::max(w.max[i], v);
            }
        }
        m.bounds = w;
        out->placements.push_back(std::move(m));
    }
    if (out->placements.empty()) return nullptr;
    return out;
}

} // namespace native::render
