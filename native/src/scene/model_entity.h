// scene/model_entity -- entities that show a model (keys model_path / model, model_scale, model_animation; port of shell/src/model/modelEntity.ts).
//
// An entity that is not a brush entity and names a model file draws that model instead of its class box: the box of the model (rest pose bounds times the scale) is what the
// editor outlines and picks. The bounds of a loaded model reach `pointBox` through a small registry (the model store installs itself with `setModelBoundsProvider`), as the Shell does:
// a model that is still loading or cannot be read leaves the entity's class box as it is. The game's defaults of a key (the .ent's `model_path`, env_door's door) come the same way.
// Thread: any (the registry is guarded).
// Spec: 07_EDITOR_PORT.md (modelEntity.ts), 08_MILESTONES.md T11.9b.
#pragma once

#include <functional>
#include <optional>
#include <string>

#include "core/document.h"
#include "scene/point_box.h"

namespace native::scene {

using ModelBoundsProvider = std::function<std::optional<Box>(const std::string& path)>;
using KeyDefaultProvider = std::function<std::string(const std::string& classname, const std::string& key)>;

/// The model store installs itself here (null function = no models known).
void setModelBoundsProvider(ModelBoundsProvider provider);
/// The game's default of a key for an entity that does not set it (the .ent's default value); an empty result = none.
void setKeyDefaultProvider(KeyDefaultProvider provider);

/// The model file of an entity: `model_path`, else the classic `model` key (relative to the assets root, forward slashes, no leading slash); empty when it has none or is a brush entity.
std::string modelPathOf(const core::Entity& e);
/// The animation to play: a name or an index; empty = the rest pose.
std::string modelAnimationOf(const core::Entity& e);
/// model_scale (one number or x y z) times modelscale and modelscale_vec (what the scale tool and prefab instances write); 1 1 1 without any.
V3 modelScaleOf(const core::Entity& e);
/// The box of the entity's model relative to its origin (scaled, before the entity's rotation), or nullopt when it shows no (loaded) model.
std::optional<Box> entityModelBox(const core::Entity& e);

} // namespace native::scene
