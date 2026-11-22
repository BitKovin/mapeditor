// light/entity_bounce -- the bounce light of the entity probes (T10.2): a surfel gather on the CPU.
//
// The game's light volume gathers its bounce light from the lightmap atlas of the compiled map (core::gatherCpu over the bounce source S = Db + I). The editor has no such atlas on the CPU
// (its lightmaps live on the GPU and carry direct light only), so the probes here gather from SURFELS instead: the points the gather rays hit, merged per triangle and cell
// (`cellSize`), each with its own bounce-weighted direct light Db (core::evaluateDirectWithBounceCpu: the same lights, shadows and bounce weights as the lightmaps) and an indirect part I
// of its own gathered from further surfels, `depth` levels deep. What lies deeper is extrapolated as a geometric series of the measured loop gain, and a few Jacobi sweeps
// S = Db + I(S) bring the surfels to the fixed point, so the result approaches the many bounces of the compiled map (it is the same S = Db + I recursion of DESIGN §6.5).
// A ray picks up the albedo of what it hits like the Core's gather: `albedo` times the texture colour at the hit (OccluderMesh::albedo), rays that hit surfaces of classes without
// lightmaps are not special-cased (the doors themselves are small), rays that miss (the sky) bring nothing: the sky's own light is part of the direct light of the probe.
// Thread: one call at a time per object; the ray casting and the shadow rays run on core::parallelFor. Deterministic for the same inputs (no random numbers: hashed rotations of a fixed pattern).
#pragma once

#include <core/lightbake.h>
#include <core/raytrace.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace native::light {

struct BounceSettings {
    int raysPerSample = 32;      // cosine weighted rays a probe normal gathers with
    int raysPerSurfel = 16;      // the same for a surfel
    int depth = 3;               // levels of surfels that gather further (the rest are leaves with the extrapolated tail)
    int sweeps = 4;              // Jacobi sweeps S = Db + I(S)
    int surfelAreaSamples = 4;   // shadow rays per light of a surfel's direct light (a probe averages thousands of surfels: their noise cancels, the cost does not)
    int surfelSkySamples = 8;    // the same for the sky
    double albedo = 0.7;         // reflectance of every surface, times the texture colour of the hit
    double cellSize = 0.5;       // m: surfels of one triangle closer than this are one
    size_t maxSurfels = 60000;   // a work bound: rays that would hit more surfels than this are dropped
    double maxLoopGain = 0.85;   // the tail never counts more than 1 / (1 - this)
};

class EntityBounce {
public:
    // `mesh` / `bvh` / `lights` must outlive the object. `direct` carries the shadow and sample counts and the classes of the direct light of the surfels.
    EntityBounce(const core::OccluderMesh& mesh, const core::TriangleBvh& bvh, const core::LightScene& lights, const core::BakeOptions& direct, const BounceSettings& settings = {});

    // The indirect light of every sample (4 floats each, as core::gatherCpu): positions / normals of `samples`, other members unused. Surfels are kept for the next call.
    std::vector<float> gather(const core::TexelSamples& samples);

    size_t surfels() const { return surfels_.size(); }
    size_t rays() const { return rays_; }
    double loopGain() const { return gain_; }
    double castSeconds() const { return castSeconds_; }      // time in the gather rays
    double directSeconds() const { return directSeconds_; }  // time in the direct light of the surfels

private:
    struct Surfel {
        core::Vec3 p, n;
        float db[3] = {0, 0, 0};
        float s[3] = {0, 0, 0};
        uint32_t first = 0, count = 0;   // its edges in edges_ (count 0: a leaf, or not expanded yet)
        int depth = 0;
        bool expanded = false;
    };
    struct Edge {
        uint32_t surfel;
        float w[3];
    };
    struct RayHit {
        bool hit = false;
        uint32_t tri = 0;
        core::Vec3 p, n;
        float w[3] = {0, 0, 0};
    };

    void cast(const std::vector<core::Vec3>& p, const std::vector<core::Vec3>& n, const std::vector<uint32_t>& seed, int rays, std::vector<RayHit>& out) const;
    uint32_t surfelFor(const RayHit& h, int depth, std::vector<uint32_t>& fresh);
    void evaluateDirect(const std::vector<uint32_t>& ids);

    const core::OccluderMesh& mesh_;
    const core::TriangleBvh& bvh_;
    const core::LightScene& lights_;
    core::BakeOptions direct_;
    BounceSettings cfg_;
    std::vector<Surfel> surfels_;
    std::vector<Edge> edges_;
    std::unordered_map<uint64_t, uint32_t> index_;
    size_t rays_ = 0;
    double gain_ = 0;
    mutable double castSeconds_ = 0;
    double directSeconds_ = 0;
};

} // namespace native::light
