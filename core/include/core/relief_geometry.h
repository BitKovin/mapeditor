#pragma once
#include "core/derived.h"
#include "core/math3.h"
#include "core/raytrace.h"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

namespace core {

// The relief of a texture as geometry for the lightmap bake. The baker lights the FLAT surface pieces; a relief the Shell lays over a face reuses their
// lightmap coordinates. To give it real light, the Shell sends the displaced triangles it draws for every authored face (side) that has a relief, and
// the bake uses them (1) as occluders INSTEAD of the flat pieces of that face (a pit must not be covered by the flat face), and (2) to place every
// lightmap texel of the face where the relief really is above it: displaced position, relief normal. The texel keeps its lightmap coordinates.
struct ReliefMesh {
    std::string instance, entityId, brushInstance, brushId;
    uint32_t faceId = 0;
    FaceSide side = FaceSide::Front;
    Vec3 normal;          // unit, of the flat face (this side); the relief stands out of the plane along it
    double offset = 0;    // plane: normal . x = offset
    std::vector<float> tris;  // 9 floats per triangle, world space
    Vec3 bmin, bmax;
    double lowH = 0, highH = 0;  // extent along `normal` above the plane (lowest / highest vertex)
    uint64_t hash = 0;
    std::shared_ptr<const TriangleBvh> bvh;

    std::string key() const;
    /// Projection of `p` onto the flat plane.
    Vec3 footprint(const Vec3& p) const { return p - normal * (normal.dot(p) - offset); }
    struct Hit { Vec3 pos, normal; uint32_t triangle = 0; };
    /// Where the relief stands above the flat point `p` (cast down the normal from above the highest point; the first surface hit). The normal faces the
    /// side the ray came from. False when the relief does not cover `p`.
    bool surfaceAt(const Vec3& p, Hit& out) const;
};

std::string reliefKey(const std::string& instance, const std::string& entityId, const std::string& brushInstance, const std::string& brushId, uint32_t faceId, FaceSide side);

/// What the Shell sent (`relief_geometry` message): meshes by reliefKey.
class ReliefSet {
public:
    /// Replaces the contents. Malformed entries are skipped. Returns true when the result differs from before.
    bool assign(const nlohmann::json& message);
    void clear() { meshes_.clear(); }
    size_t size() const { return meshes_.size(); }
    /// The mesh for a surface piece when its face's plane still matches; null otherwise.
    std::shared_ptr<const ReliefMesh> find(const LightSurface& s) const;
    /// Builds a mesh directly (tests and `assign`).
    static std::shared_ptr<const ReliefMesh> make(ReliefMesh m);
    void add(std::shared_ptr<const ReliefMesh> m);

private:
    std::unordered_map<std::string, std::shared_ptr<const ReliefMesh>> meshes_;
};

/// Moves a texel sample onto the relief: `pos` / `nrm` become the displaced point and the relief normal, `sub` (12 floats, may be null) gets the same
/// treatment (centre on the relief, per-texel steps along the relief's tangent plane when the whole texel lies on one triangle). False: not covered, unchanged.
bool displaceSample(const ReliefMesh& r, const Vec3& pos, const Vec3& nrm, const float* sub, Vec3& outPos, Vec3& outNrm, float* outSub);

}  // namespace core
