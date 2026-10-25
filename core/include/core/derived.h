#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include "core/document.h"
#include "core/prefab.h"
#include <nlohmann/json.hpp>
#include <vector>

namespace core {
struct BrushSmoothing;
struct FaceShading;

// The `derived_geometry` channel (DESIGN.md §5.6): everything Core computes from authored data that the Shell
// draws but never stores. Two generators today, both pluggable in the same message:
//
//   * boolean carving (csg.h): subtractive brushes carve additive ones, honouring the carve scope;
//   * decals (decal.h): projectors clipped onto the (already carved) surfaces they hit.
//
// Prefab instances take part: their entities are flattened (prefab.h) and their brushes / decals are derived
// like the document's own. Each prefab instance is its own partition: a subtractor inside it carves inside
// it AND the root document around it (per its carve scope: a door frame cuts its opening into the wall it
// stands in); a subtractor of the root document may also carve prefab content. A prefab never carves a sibling
// prefab, so prefab content only ever depends on the root document and on itself.
//
// Message:
//   {"type":"derived_geometry","rev":N,
//    "carved":[{"entityId","instance","brushId","subtractors":[ids],"subtractorInstances":[instance of each],
//               "fragments":[{"brush","instance","face","v":[x,y,z,...]}]}],   // brush+instance: the brush that owns the face
//    "skipped":[{"entityId","instance","brushId"}],
//    "decals":[{"decalId","instance","texture","fragments":[{"entityId","instance","brush","face","v":[...],"uv":[u,v,...]}]}]}
// "instance" is "" for the document's own entities, else the instance ids from the root down, joined by "/".
nlohmann::json deriveGeometry(const Document& doc, const std::vector<ResolvedEntity>& prefabs, uint64_t rev);

// Which side of an authored face a surface piece is: Front = same normal as the authored face; Back = a cut wall,
// i.e. a subtractor's face seen from inside the carved room (its normal is the authored one reversed).
enum class FaceSide { Front, Back };

struct ReliefMesh;  // relief_geometry.h

// One convex piece of final (carved) surface, with the identity lightmap charts are keyed by (DESIGN.md §5.6).
struct LightSurface {
    std::string instance;       // instance of the entity that owns the surface ("" = the document itself)
    std::string entityId;
    std::string classname;      // of that entity
    std::string brushId;        // brush that owns the authored face (a cut wall: the subtractor)
    std::string brushInstance;  // instance of that brush
    uint32_t faceId = 0;
    FaceSide side = FaceSide::Front;
    std::string texture;        // of the authored face
    FaceProjection projection;  // of the authored face (albedo coordinates of any point on the piece: projectAlbedoUv)
    uint64_t lookHash = 0;      // of the authored face (BrushFace::lookHash): any change of how its texture lies
    // The authored face's own UV map when it has stored uvs and this piece lies on its plane (REQUIREMENTS §24 C3): albedoUvAt() then uses it instead
    // of the projection, so bounce colour and transparency sample the texture where the editor shows it.
    bool hasUvMap = false;
    UvMap uvMap;
    double lightmapScale = 1;   // of the authored face: texel density of its lightmap relative to the level's
    Vec3 normal;                // unit, of this piece
    std::vector<Vec3> verts;    // convex, CCW seen from outside the resulting solid
    // The displayed relief of this piece's face (side), when the Shell sent one (relief_geometry.h). It replaces the flat pieces of the face as occluder and
    // places the face's texel samples on the relief; the lightmap coordinates stay those of the flat piece. Shared by all pieces of the face.
    std::shared_ptr<const ReliefMesh> relief;
    // Smooth edges of the owning brush (smoothing.h, REQUIREMENTS §33 H6): null when it has none. `shading` (inside `smoothing`) = this face's
    // corner normals when the piece is the front of the face and lies on its plane; texel samples and exported vertices then take their normal from it.
    std::shared_ptr<const BrushSmoothing> smoothing;
    const FaceShading* shading = nullptr;
};

/// Albedo coordinates (texture repeats) of a point on the surface: the face's stored UV map when it has one, else the projection.
Vec2 albedoUvAt(const LightSurface& s, const Vec3& p);

/// Final surface pieces of every additive brush (the same carving as deriveGeometry, nothing skipped).
std::vector<LightSurface> collectSurfaces(const Document& doc, const std::vector<ResolvedEntity>& prefabs);

// The solid volumes of the level: where a point is INSIDE matter (as opposed to on its surface). A lightmap texel that lies inside a
// solid is invisible (a face buried in another brush, e.g. the floor under a wall) and its rays would start inside geometry, so the
// baker skips it. A point is solid when it is inside a light-blocking additive brush and outside every subtractive brush.
class SolidSet {
public:
    bool inside(const Vec3& p, double margin = 0.0) const;
    bool insideExcept(const Vec3& p, const Brush* ignoredBrush, double margin = 0.0) const;
    // True only when the complete triangle lies strictly inside one convex solid volume, excluding the owning
    // brush. Unlike a point/sample majority test, this cannot discard a triangle that crosses an exposed gap.
    bool triangleInsideExcept(const Vec3& a, const Vec3& b, const Vec3& c, const Brush* ignoredBrush, double margin = 0.0) const;
    // Same answer as inside(), for a run of nearby queries: remembers the grid cell of the previous one (texels of a surface are queried one after
    // the other), which saves the hash lookup that otherwise dominates.
    struct Probe {
        int64_t cell[3] = {INT64_MIN, 0, 0};
        const std::vector<int>* volumes = nullptr;
    };
    bool inside(const Vec3& p, Probe& probe, double margin = 0.0) const;
    bool insideExcept(const Vec3& p, const Brush* ignoredBrush, Probe& probe, double margin = 0.0) const;
    size_t volumeCount() const { return volumes_.size(); }
    uint64_t fingerprint() const;  // hash of every volume (box, planes, kind): equal sets give equal values
    // The volumes of a brush stay the same until the brush changes, and finding them is expensive for a concave brush (its convex pieces): a Cache keeps
    // them from one build to the next, keyed by the brush's shape (a level of 2000 brushes took 13 s to collect on every edit).
    struct Cache;
    static std::shared_ptr<Cache> makeCache();
    void add(const Brush& b, bool subtractive, Cache* cache = nullptr);
    // add() for many brushes: the ones the cache does not know are worked out on all cores.
    void addAll(const std::vector<std::pair<const Brush*, bool>>& brushes, Cache* cache = nullptr);
    void finish();  // builds the lookup grid; call after the last add()

private:
    struct Volume {
        Vec3 mn, mx;
        std::vector<std::pair<Vec3, double>> planes;  // outward normal, offset: n . x = d on the face
        bool subtractive = false;
        const Brush* source = nullptr;  // owning authored brush (restored when cached volumes are reused)
    };
    std::vector<Volume> volumes_;
    std::unordered_map<int64_t, std::vector<int>> grid_;
    static constexpr double kCell = 8.0;
    bool contains(const Volume& v, const Vec3& p, double margin) const;
};

/// Volumes of the brushes of classes accepted by `blocks` (a classname predicate, normally LightingClasses::castsShadows). `cache`: see SolidSet::Cache;
/// brushes it has not been asked for in this build are dropped from it.
SolidSet collectSolids(const Document& doc, const std::vector<ResolvedEntity>& prefabs, const std::function<bool(const std::string&)>& blocks,
                       SolidSet::Cache* cache = nullptr);

/// One decal fragment, typed (the same clipping as deriveGeometry's `decals`): lies on an authored face (its parent, owned by `entityId` of
/// `instance`; the face of `brushId` of `brushInstance`), texture coordinates from the projector.
struct DecalFragment {
    std::string decalId, decalInstance, texture;
    std::string instance, entityId, classname, brushInstance, brushId;
    uint32_t faceId = 0;
    Vec3 normal;                 // of the surface it lies on
    std::vector<Vec3> verts;     // convex, CCW seen from outside
    std::vector<Vec2> uv;        // texture coordinates (repeats), one per vertex
};
std::vector<DecalFragment> collectDecals(const Document& doc, const std::vector<ResolvedEntity>& prefabs);

/// True if there is anything to derive (a subtractive brush or a decal, in the document or its prefabs).
bool hasDerivedWork(const Document& doc, const std::vector<ResolvedEntity>& prefabs);

/// "root/a/b" instance path -> "a/b"; "root" -> "".
std::string instanceString(const std::vector<std::string>& instancePath);

}  // namespace core
