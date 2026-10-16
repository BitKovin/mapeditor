#pragma once
#include "core/document.h"
#include "core/math3.h"
#include <array>
#include <string>
#include <vector>

namespace core {

// Decals (DESIGN.md §5.6). Authored = the projector: a point entity `decal` whose origin and angles place
// an oriented box and whose keyvalues size it:
//   texture  material path            (default "dev/grey")
//   width    metres along `right`     (default 1)
//   height   metres along `up`        (default 1)
//   depth    metres along `dir`       (default 0.5); the box is centred on the origin
// angles (degrees, pitch/yaw/roll): dir = Ry(yaw) Rx(pitch) Rz(roll) * (0,-1,0), up = ... * (0,0,-1),
// right = dir x up. Zero angles project straight down (onto floors) with the image's top (v = 0) toward -Z, as seen from above (right = +X).
//
// Derived = the target surfaces clipped to that box, with texture coordinates from the projector. Each
// fragment lies on an authored face (its parent) and belongs to the entity that owns that face.
struct DecalProjector {
    std::string id;
    Vec3 origin;
    Vec3 dir, up, right;  // unit, right-handed
    double width = 1, height = 1, depth = 0.5;
    std::string texture = "dev/grey";
};

bool isDecalEntity(const Entity& e);
DecalProjector decalFromEntity(const Entity& e);

struct SurfaceInput {
    std::vector<Vec3> verts;  // convex, CCW seen from outside
};
struct DecalPolygon {
    std::vector<Vec3> verts;
    std::vector<std::array<double, 2>> uv;
};

/// Clips one convex surface polygon to the projector. Empty when the surface faces away, is outside the box,
/// or is (nearly) parallel to the projection direction.
DecalPolygon clipToDecal(const DecalProjector& d, const SurfaceInput& surface);

}  // namespace core
