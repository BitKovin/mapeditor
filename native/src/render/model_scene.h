// render/model_scene -- the models a scene shows: one placement per entity that names a loaded model (port of the entity selection of shell/src/render/modelRenderer.ts).
//
// Pure: from a scene snapshot (the document's entities and the prefab content) and the model store to a list of placements the renderer draws. Made on the UI thread when the
// snapshot or the store changes and handed to the render thread; the models themselves are shared with the store.
// Thread: any (pure).
// Spec: 08_MILESTONES.md T11.9c.
#pragma once

#include <memory>
#include <unordered_set>
#include <vector>

#include "scene/model.h"
#include "scene/model_store.h"
#include "scene/snapshot.h"

namespace native::render {

struct ModelPlacement {
    std::shared_ptr<const scene::LoadedModel> model;
    scene::Mat4 matrix{};      // model -> world
    int animation = -1;        // index into model->data.animations, -1 = the rest pose
    scene::Box bounds;         // the placed model's box in world space (culling)
};

struct ModelScene {
    std::vector<ModelPlacement> placements;
    bool animated() const {
        for (const ModelPlacement& p : placements)
            if (scene::isAnimated(p.model->data, p.animation)) return true;
        return false;
    }
};

/// The placements of every non-brush entity of the snapshot whose model is ready in `store` (a model that is still loading is asked for, which starts it). `hidden` = the objects of
/// the Hide state ({"", entityId, ""} an entity, {instanceId, "", ""} a prefab instance). Null when there are none.
std::shared_ptr<const ModelScene> buildModelScene(const scene::SceneSnapshot& snapshot, scene::ModelStore& store, const std::unordered_set<scene::ObjectKey, scene::ObjectKey::Hash>* hidden = nullptr);

} // namespace native::render
