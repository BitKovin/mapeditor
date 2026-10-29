#pragma once
#include "core/derived.h"
#include "core/math3.h"
#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace core {

// Static models (REQUIREMENTS: misc_model). A `misc_model` entity names a Wavefront .obj in its `model_path` (relative to the assets root); the
// triangles are baked into the world like brush surfaces: they get lightmap charts (or vertex light), cast shadows and are drawn textured.
//
//   keys: model_path   the .obj (metres, Y up, as the editor); .mtl and textures are found beside it
//         model_scale  one number or "x y z" (times modelscale / modelscale_vec, which the transform tools and prefab instances write)
//         origin, angles (pitch yaw roll) as every entity
//         _cs 0        does not cast shadows          (default 1)
//         _vertex_lit  0 = lightmapped, default 1 = lit at the vertices (the chart is baked but not stored)
//
// Textures: a material's map_Kd in the .mtl (relative to the .mtl); when there is no .mtl, or the material has no usable map_Kd, an image named like
// the material in the .obj's folder. A model texture is named by its path RELATIVE TO THE ASSETS ROOT, without extension, with a leading '/'
// ("/models/props/crate_diffuse"): the leading '/' tells Core, Shell and the exporters not to look inside the profile's texture root.

struct ObjTriangle {
    Vec3 p[3];
    Vec2 uv[3];     // editor convention: v grows DOWN the image (the .obj's v is flipped on reading)
    Vec3 n[3];      // vertex normals (the face normal when the .obj has none)
    bool hasUv = false;
    int material = 0;  // index into ObjModel::textures
};

struct ObjModel {
    std::vector<ObjTriangle> tris;
    std::vector<std::string> materials;  // names
    std::vector<std::string> textures;   // per material: "/models/x/y" or "" (no image found: the default texture is used)
    Vec3 mn, mx;                         // bounds (model space)
    std::vector<std::string> warnings;
    bool complete() const;               // every material has an image
};

/// Reads `rel` (an .obj inside `assetsRoot`). Cached by file times; a model with missing textures is looked at again after a couple of seconds.
/// Null (and `error`) when the file is missing, outside the assets root or has no triangles.
std::shared_ptr<const ObjModel> loadObjModel(const std::filesystem::path& assetsRoot, const std::string& rel, std::string* error = nullptr);

/// Parses OBJ text (no file access; materials stay unresolved: `textures` is empty and every triangle's material indexes `materials`).
ObjModel parseObj(const std::string& text, std::vector<std::string>* mtllibs = nullptr);

/// The scale a misc_model-like entity asks for: model_scale x modelscale x modelscale_vec.
Vec3 modelScaleOf(const Entity& e);

/// The surfaces of every misc_model (document and prefab content), one convex piece (a triangle) per triangle, in world space.
/// brushId = "model", faceId = the triangle's index in the model; classname = "misc_model".
std::vector<LightSurface> collectModelSurfaces(const Document& doc, const std::vector<ResolvedEntity>& prefabs, const std::filesystem::path& assetsRoot);

}  // namespace core
