// scene/gltf -- the binary glTF 2.0 (.glb) reader (port of `parseGlb` / `parseGltf` of shell/src/model/gltf.ts) and the model of a Wavefront .obj (Core's reader).
//
// Supported: meshes of triangle lists / strips / fans, node hierarchies (TRS or matrix), skins (JOINTS_0 / WEIGHTS_0), animations (translation / rotation / scale; LINEAR, STEP and
// CUBICSPLINE), the base colour of the material (factor and texture) and the alpha mode, normalised integer attributes. Not supported: morph targets, sparse accessors, external
// buffers (use a .glb or data URIs), KHR extensions (ignored).
// Thread: any (pure).
// Spec: 07_EDITOR_PORT.md (gltf.ts), 08_MILESTONES.md T11.9a.
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "scene/model.h"

namespace core {
struct ObjModel;
}

namespace native::scene {

/// Parses a .glb file. Throws std::runtime_error with a readable message for anything that is not a usable binary glTF 2.0.
ModelData parseGlb(const uint8_t* data, size_t size);
/// The JSON of a glTF and its first binary buffer (the .glb's BIN chunk; may be null).
ModelData parseGltf(const nlohmann::json& g, const uint8_t* bin, size_t binSize);

/// The model of a Core .obj: one primitive per material, already in model space, `texture` = the material's path ("/models/x/y"), vertices not shared.
ModelData modelFromObj(const core::ObjModel& obj);

} // namespace native::scene
