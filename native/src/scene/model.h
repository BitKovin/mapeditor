// scene/model -- the data of a model shown for an entity (model_path) and its posing (port of the data types and `poseModel` of shell/src/model/gltf.ts).
//
// Metres, Y up, +Z forward, right handed: the editor's own conventions. A model is a list of primitives (triangle lists with a material colour / image), a node hierarchy,
// skins and animations. `poseModel` gives the vertices of every primitive in model space at a time of an animation (skinned primitives follow their joints, the others take
// their node's world matrix); the renderer draws them with the entity's origin / angles / scale on top (`modelMatrix`).
// Thread: any (pure).
// Spec: 05_RENDERER.md section 5 (models), 08_MILESTONES.md T11.9.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "scene/point_box.h"

namespace native::scene {

enum class ModelAlpha { Opaque, Mask, Blend };

struct ModelPrimitive {
    std::vector<float> positions;   // rest pose, model space, 3 per vertex (the node's transform is NOT applied for skinned primitives, see poseModel)
    std::vector<float> normals;     // 3 per vertex
    std::vector<float> uvs;         // 2 per vertex, v grows down the image (the glTF convention)
    std::vector<uint32_t> indices;  // triangles
    std::vector<uint16_t> joints;   // 4 per vertex (empty = not skinned)
    std::vector<float> weights;     // 4 per vertex
    int node = -1;                  // the node that carries the mesh (-1: none, positions are final)
    int skin = -1;                  // index into ModelData::skins, -1 = none
    std::array<float, 4> color{1, 1, 1, 1};
    int image = -1;                 // index into ModelData::images (an image inside the file), -1 = none
    std::string texture;            // a texture path of the editor's texture cache ("/models/x/y"), empty = none (an .obj's material)
    ModelAlpha alpha = ModelAlpha::Opaque;
    float cutoff = 0.5f;
    size_t vertexCount() const { return positions.size() / 3; }
};

using Mat4 = std::array<float, 16>;   // column major, as glTF

struct ModelNode {
    int parent = -1;
    std::vector<int> children;
    std::array<float, 3> t{0, 0, 0};
    std::array<float, 4> r{0, 0, 0, 1};
    std::array<float, 3> s{1, 1, 1};
    std::optional<Mat4> matrix;
};

struct ModelSkin {
    std::vector<int> joints;
    std::vector<float> inverseBind;   // 16 floats per joint
};

enum class ChannelPath { Translation, Rotation, Scale };
enum class Interpolation { Linear, Step, CubicSpline };

struct ModelChannel {
    int node = 0;
    ChannelPath path = ChannelPath::Translation;
    Interpolation interp = Interpolation::Linear;
    std::vector<float> times, values;
};

struct ModelAnimation {
    std::string name;
    float duration = 0;
    std::vector<ModelChannel> channels;
};

struct ModelImage {
    std::string mime;
    std::vector<uint8_t> bytes;
};

struct ModelData {
    std::vector<ModelPrimitive> primitives;
    std::vector<ModelNode> nodes;
    std::vector<int> roots;
    std::vector<ModelSkin> skins;
    std::vector<ModelAnimation> animations;
    std::vector<ModelImage> images;
    Box bounds;   // the rest pose bounds in model space (what the editor shows as the entity's box)
};

struct PosedPrimitive {
    std::vector<float> positions, normals;
};

/// Index of the animation `name` names (exact, then ignoring case; a plain number is an index), -1 for none / not found.
int animationIndex(const ModelData& model, const std::string& name);
/// True when posing at different times gives different results.
bool isAnimated(const ModelData& model, int anim);
/// World matrices of every node at `time` (seconds, looped) of animation `anim` (-1 = the rest pose).
std::vector<Mat4> nodeWorlds(const ModelData& model, int anim, double time);
/// The vertices of every primitive in model space at `time` of animation `anim` (-1 = rest pose). `out` (a result for the same model) is overwritten instead of allocating.
std::vector<PosedPrimitive> poseModel(const ModelData& model, int anim, double time, std::vector<PosedPrimitive>* out = nullptr);
Box boundsOfPose(const std::vector<PosedPrimitive>& posed);

/// model -> world, column major: translate(origin) * rotate(angles) * scale (angles = pitch yaw roll in degrees, as every entity).
Mat4 modelMatrix(const V3& origin, const V3& anglesDeg, const V3& scale);
/// The vertex normals of a triangle list (area weighted, normalised).
std::vector<float> vertexNormals(const std::vector<float>& positions, const std::vector<uint32_t>& indices);

} // namespace native::scene
