// scene/point_box -- the box of a point entity as the editor shows and picks it (port of shell/src/model/entityBox.ts, pointBox.ts and the rotation maths).
//
// Owns: the Quake -> editor box conversion (the .ent file gives boxes in Quake units and axes; the editor is metric and Y-up: editor = (qx, qz, -qy) / 32),
// the class box with its 0.5 m default, the decal projector volume, rotationFromAngles (Ry(yaw) * Rx(pitch) * Rz(roll)), OrientedBox (the class box turned
// with the entity's `angles` about its origin) and its corners / bounds. Pure maths, no GPU, no Qt: the renderer draws these boxes, the picker hits them, the
// arrows start from their centres. `misc_model` bounds (the model's own box once loaded) come with models (T5).
// Thread: any (pure).
// Spec: 05_RENDERER.md section 5 and 6, 08_MILESTONES.md T3.6.
#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>

#include "core/document.h"
#include "core/entdef.h"

namespace native::scene {

using V3 = std::array<double, 3>;
using Mat3 = std::array<double, 9>;   // row-major
struct Box {
    V3 min{0, 0, 0}, max{0, 0, 0};
};
// The class box of a classname in editor space, relative to the entity origin.
using BoxOf = std::function<Box(const std::string& classname)>;

constexpr double kQuakeUnitsPerMeter = 32;

V3 quakeToEditor(const V3& v);
V3 editorToQuake(const V3& v);
// A .ent box (min xyz, max xyz in Quake units) as an editor box; a missing or unusable box gives the 16 unit (half metre) default.
Box boxFromQuake(const std::optional<std::array<double, 6>>& box);
// BoxOf over the entity definitions (null = no definitions: every class gets the default box). `defs` must outlive the function.
BoxOf classBoxes(const core::EntityDefinitions* defs);
Box boxAt(const Box& box, const V3& origin);

// angles = [pitch, yaw, roll] in degrees; Ry(yaw) * Rx(pitch) * Rz(roll) applied to a column vector (the decal convention).
Mat3 rotationFromAngles(const V3& anglesDeg);
V3 apply3(const Mat3& m, const V3& v);
V3 unrotate(const Mat3& m, const V3& v);   // the transpose of a rotation undoes it

// A box in the frame of an entity: min / max relative to `origin`, turned by `rot`.
struct OrientedBox {
    V3 origin{0, 0, 0};
    Mat3 rot{1, 0, 0, 0, 1, 0, 0, 0, 1};
    V3 min{0, 0, 0}, max{0, 0, 0};
};

bool isModernLightClass(const std::string& classname);   // light_point / light_spot / light_directional / light_rect / light_sky
// Modern lights have a gizmo of their own and decals show their projector bounds: a filled box would only hide them (both stay selectable through their box).
bool drawsBox(const core::Entity& e);
// The projector volume in the decal's own frame: width along x, depth along y (the projection axis), height along z, centred on the origin.
Box decalLocalBox(const core::Entity& e);
OrientedBox pointBox(const core::Entity& e, const BoxOf& boxOf);
V3 toWorld(const OrientedBox& b, const V3& v);
V3 toLocal(const OrientedBox& b, const V3& p);
// The eight corners in a fixed order (bottom face 0..3, top face 4..7), in the box frame / in world space.
std::array<V3, 8> cornersOf(const V3& min, const V3& max);
std::array<V3, 8> worldCorners(const OrientedBox& b);
Box boundsOfBox(const OrientedBox& b);
// The twelve edges of the eight corners as 24 corner indices (pairs).
extern const std::array<int, 24> kBoxEdges;

} // namespace native::scene
