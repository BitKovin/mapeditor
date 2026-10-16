#pragma once
#include "core/math3.h"
#include <cstdint>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <vector>

namespace core {

// Stable id, assigned by whichever side creates the brush (the Shell, in
// practice -- see DESIGN.md §10). Stability matters beyond addressing: the
// lighting cache keys off (brush id, face id), see DESIGN.md §5.6.
using BrushId = std::string;

// Additive brushes add solid volume; subtractive brushes carve. Resolving
// them into final geometry is *derived* data (DESIGN.md §5.6), never stored.
enum class BrushOp { Add, Subtract };

// Which brushes a *subtractive* brush carves (DESIGN.md §5.6). Three
// independent checkboxes; ignored for additive brushes. Defaults: carve the
// brushes of its own entity and of structural entities (worldspawn,
// func_detail, func_group), leave other dynamic entities alone.
struct CarveScope {
    bool self = true;           // brushes of the entity this brush belongs to
    bool structural = true;     // brushes of structural entities
    bool nonStructural = false; // brushes of other, non-structural entities
};

// World aligned planar albedo projection of a face (the Shell's `projection`, shell/src/model/uv.ts): pick the axis the normal points along,
// project onto the other two, rotate, divide by `scale` (metres per repeat) and add `shift` (repeats). Core only reads it: the baker needs the
// albedo coordinates of a bounce ray's hit point.
struct FaceProjection {
    double scaleU = 2, scaleV = 2;
    double shiftU = 0, shiftV = 0;
    double rotate = 0;  // degrees
};
/// Albedo coordinates (repeats) of the world point `p` on a surface piece with outward unit normal `n`. MUST equal planarCoordsProjected() in the Shell.
Vec2 projectAlbedoUv(const Vec3& n, const Vec3& p, const FaceProjection& proj);
/// The affine UV map the Shell laid on a face, from its stored per-corner `uvs` (REQUIREMENTS §24 C3): u = U.xyz . p + U.w, v = V.xyz . p + V.w for any
/// point p on the face plane (least squares over all corners: exact for affine uvs, the closest affine map for corners moved with UV lock). False when
/// the face has no usable uvs (missing, wrong count, degenerate face).
struct UvMap { double u[4] = {0, 0, 0, 0}, v[4] = {0, 0, 0, 0}; };

struct BrushFace {
    // Unique within the owning brush. Editing tools keep a face's id when the
    // face survives an operation, so lighting computed for it stays valid.
    uint32_t id = 0;

    // Indices into Brush::vertices, counter-clockwise seen from outside
    // (i.e. the Newell normal points away from the solid).
    std::vector<uint32_t> loop;

    // Per-corner albedo UVs, parallel to `loop`. May be empty (= unassigned).
    // Core never generates UVs: projection/unwrapping is Shell-side (§9).
    std::vector<Vec2> uvs;

    // Albedo texture path, Quake-style ("dev/grey", "base_wall/concrete01").
    // Deliberately just a path: no material system yet (DESIGN.md §9).
    std::string texture = "dev/grey";

    // Hash of everything the Shell stores about how the texture lies on the face (the raw `projection` incl. kind / basis / shear, `uvs`, `repeat`,
    // `layout`), 0 = none of it. The Core only reads part of it (FaceProjection), but any change of it changes the face's bounce colour, so the
    // light baker compares this too (REQUIREMENTS §22 P3).
    uint64_t lookHash = 0;

    // Projection parameters (defaults when the document has none).
    FaceProjection projection;

    // The face's raw `projection`, `repeat` and `layout` as the Shell stored them (an object with those keys that are present; null when none is):
    // the texture layout port (texplan.h) reads them exactly as the Shell does (projection kind / basis / shear, repetition override, texture group).
    std::shared_ptr<const nlohmann::json> look;

    // Texel density of the face's lightmap relative to the level's `texelsPerMeter` (2 = twice as many texels per metre). Clamped to the range below when read.
    double lightmapScale = 1;
};
inline constexpr double kMinLightmapScale = 0.0625, kMaxLightmapScale = 16.0;

// A brush as *stored and transmitted*: a plain indexed polygon mesh. The
// half-edge structure that modeling tools need is a Shell-side runtime
// structure built from this on load (DESIGN.md §5.1) -- Core never edits
// meshes, it only validates, lights and exports them.
struct Brush {
    BrushId id;
    BrushOp op = BrushOp::Add;
    CarveScope carve;
    std::vector<Vec3> vertices;
    std::vector<BrushFace> faces;

    // Axis-aligned box. Test/fixture helper -- the Shell has its own
    // primitive tools. UVs are left empty.
    static Brush makeBox(const BrushId& id, const Vec3& mins, const Vec3& maxs,
                          const std::string& texture = "dev/grey");

    // Bounds of the vertices referenced by faces. False if there are none.
    bool bounds(Vec3& outMin, Vec3& outMax) const;
    void translate(const Vec3& delta);

    // Unit normal of a face (Newell). Precondition: checkStructure() passed.
    Vec3 faceNormal(const BrushFace& face) const;

    // Signed volume; positive when faces wind outward. Precondition as above.
    double signedVolume() const;

    // Structural sanity every brush must satisfy: indices in range, loops >= 3,
    // unique face ids, uv counts, and every face non-degenerate and PLANAR
    // (lightmap charts and BSP export need planar faces; the Shell splits a
    // face that stops being planar -- DESIGN.md §5.1). `epsilon` in meters.
    // Returns a human-readable problem, or nullopt if fine.
    std::optional<std::string> checkStructure(double epsilon = 1e-4) const;

    // Everything worldspawn/func_group brushes must satisfy (DESIGN.md §5.1):
    // structurally valid, closed, outward-wound, planar faces, convex.
    // `epsilon` is in meters.
    std::optional<std::string> checkConvexSolid(double epsilon = 1e-4) const;
};
/// See UvMap: the affine UV map of a face from its stored uvs.
bool faceUvMap(const Brush& brush, const BrushFace& face, UvMap& out);


}  // namespace core
