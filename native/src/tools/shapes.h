// tools/shapes -- block-tool primitives beyond the box (port of shell/src/model/shapes.ts). A shape is built inside the oriented box the person drew (a rectangle on
// a plane, extruded by a height). Solids that are not convex (an arch, stairs) are several convex brushes, as level editors traditionally do.
// Thread: any (pure).
// Spec: 08_MILESTONES.md T4.2, DESIGN sections 7 and 8, REQUIREMENTS 33 (H4 preview, H5 sphere, H6 smooth edges).
#pragma once

#include <functional>
#include <string>
#include <variant>
#include <vector>

#include "core/brush.h"
#include "tools/box_tool.h"

namespace native::tools {

enum class ShapeId { Box, Cylinder, Wedge, Arch, Stairs, Sphere };
const char* shapeName(ShapeId s);   // "box", "cylinder", ...
// Quarter turns about the height axis: which way a ramp rises, stairs climb, an arch spans.
using Turn = int;   // 0..3

struct ShapeParams {
    int sides = 12;            // cylinder
    int steps = 8;             // stairs
    int archSegments = 8;
    double archThickness = 0.5;
    int segments = 12;         // sphere: faces around (4..32) ...
    int rings = 6;             // ... and from pole to pole (2..16)
    bool smooth = true;        // cylinder sides / the whole sphere come out with their edges marked smooth
    Turn turn = 0;
};

// The drawn box: origin at its min corner, axes u (rect width), v (rect depth), n (height).
struct Frame {
    Vec3 o, u, v, n;
    double du = 0, dv = 0, h = 0;
};
Frame frameOf(const DrawPlane& plane, const Rect& rect, double height);
// The same box after `turn` quarter turns about its height axis (handedness kept).
Frame turned(const Frame& f, Turn turn);

struct ShapeOutcome {
    std::vector<core::Brush> brushes;
    std::string error;   // non-empty: nothing was built
    bool ok() const { return error.empty(); }
};

// All shapes fill the drawn box exactly. Parts come out in a fixed order; ids from `newId`.
ShapeOutcome buildShape(ShapeId shape, const DrawPlane& plane, const Rect& rect, double height, const std::string& texture, const ShapeParams& params,
                        const std::function<std::string()>& newId, Turn turn = 0);
ShapeOutcome buildInFrame(ShapeId shape, const Frame& f, const std::string& texture, const ShapeParams& params, const std::function<std::string()>& newId);
// Edges of a shape being drawn: flat line endpoints of every brush edge, each shared edge once; empty when it cannot be built.
std::vector<float> shapePreviewLines(ShapeId shape, const DrawPlane& plane, const Rect& rect, double height, const ShapeParams& params, Turn turn = 0);

// Marks every edge between two faces of `faceIds` smooth (the sides of a cylinder, a whole sphere); the brush itself when nothing changes.
core::Brush smoothBetween(const core::Brush& b, const std::vector<uint32_t>& faceIds);

} // namespace native::tools
