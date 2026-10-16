#pragma once
#include "core/derived.h"
#include "core/lightbake.h"
#include "core/live_bake.h"
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace core {

// Light volumes (DESIGN §6.7): a grid of probes that lights brush entities which do not receive lightmaps (doors, lifts, ...), Quake 3 style:
// `light(n) = ambient + directed * max(0, n . dir)`. A probe is fitted to the SAME lightmap value V(n) the texels use, so lightmaps and light
// volumes agree by construction.

struct LightvolSettings {
    double gridSize = 1.0;     // m per cell
    int maxProbes = 65536;     // the cell grows until the grid fits (the GPU keeps probes x normals samples resident)
    int normals = 32;          // normals V(n) is evaluated at per probe (Fibonacci sphere)
    int areaSamples = 16;      // shadow rays per normal for lights with a size
    int skySamples = 32;       // sky rays per normal
    int giSamples = 32;        // cosine-weighted gather rays per normal and bounce source lookup
    bool bounces = true;       // add the indirect light gathered from the baker's bounce source
    double albedo = 0.7;
    double bounceRange = 20;
    bool useGpu = true;        // direct light on the Vulkan ray query path when available
    LightingClasses classes = LightingClasses::defaults();
};

struct Probe {
    Vec3 ambient;              // linear, may exceed 1 (stored with the x2 range)
    Vec3 directed;
    Vec3 dir{0, 1, 0};         // unit, towards the light
    bool valid = false;        // false: inside a solid / not baked
};

// What the engine stores (REQUIREMENTS §9: floats divided by 2 before mapping to 0..255, multiplied by 2 when read).
struct PackedProbe {
    uint8_t ambient[3] = {0, 0, 0};
    uint8_t directed[3] = {0, 0, 0};
    uint8_t lat = 0, lng = 0;  // lat = acos(dir.y) / pi * 255, lng = (atan2(dir.z, dir.x) + pi) / (2 pi) * 255
    uint8_t flags = 0;         // bit 0: valid
};
inline constexpr uint8_t kProbeValid = 1;
inline constexpr size_t kPackedProbeBytes = 9;  // ambient rgb, directed rgb, lat, lng, flags
PackedProbe packProbe(const Probe& p);
Probe unpackProbe(const PackedProbe& p);
inline double lightvolFromByte(uint8_t b) { return b / 255.0 * 2.0; }
inline uint8_t lightvolToByte(double v) {
    const double c = v < 0 ? 0 : (v > 2 ? 2 : v);
    return static_cast<uint8_t>(std::lround(c / 2.0 * 255.0));
}

// `count` unit vectors, equal-area spiral on the sphere (the quadrature the fit uses).
std::vector<Vec3> sphereNormals(int count);

// Fits ambient + directed * max(0, n . dir) to values V(n_k) (one rgb per normal) by matching the sphere mean and first moment
// (DESIGN §6.7). A single unshadowed delta light and a constant ambient are reproduced exactly.
Probe fitProbe(const std::vector<Vec3>& normals, const std::vector<Vec3>& values);
// The model evaluated for a normal.
Vec3 probeLight(const Probe& p, const Vec3& n);

// ONE light direction per point, shared by every light style layer (REQUIREMENTS §27 G4: by design, it keeps the engine's rendering simple and
// allows toon shading). The shared direction is the brightness weighted mean of the layers' own directions (luminance of `directed`); each
// layer is then re-fitted to it from its moments: directed' = directed * max(0, dir_layer . dir), ambient' = ambient + (directed - directed') / 4,
// so every layer keeps its mean light over the sphere (ambient + directed / 4) and a single layer is unchanged. Invalid layers are skipped.
void shareDirection(const std::vector<Probe*>& layers);


class LightvolGrid {
public:
    Vec3 origin;
    double cell = 1;
    int nx = 0, ny = 0, nz = 0;
    std::vector<Probe> probes;  // x fastest, then y, then z

    size_t count() const { return probes.size(); }
    size_t index(int x, int y, int z) const { return (static_cast<size_t>(z) * ny + y) * nx + x; }
    Vec3 position(size_t i) const;
    // Trilinear interpolation over the valid corners of the cell around `p`; the nearest valid probe (within 3 cells) when none of them is valid;
    // `valid` false when there is none. The direction is weighted by the brightness of each probe so dark probes do not steer it.
    Probe sample(const Vec3& p) const;
    // Round-trips every probe through the byte format: what the engine will read.
    void quantise();
    std::vector<uint8_t> packed() const;  // kPackedProbeBytes per probe
};

// shareDirection for every point of grids that share one geometry (the style layers of a light volume).
void shareProbeDirections(std::map<int, LightvolGrid>& grids);

// Axis aligned grid over the bounds of every surface (plus one cell). Probes inside a light-blocking solid are invalid, the others wait to be baked
// (valid = false until then: use `isBaked`).
LightvolGrid buildLightvolGrid(const std::vector<LightSurface>& surfaces, const SolidSet* solids, const LightvolSettings& settings);

// Everything the probes gather from. `atlasS` / `atlasSize` / `pages` are the baker's bounce source (rgb + valid per texel); empty = no bounces.
struct LightvolSource {
    const LightScene* lights = nullptr;
    const OccluderMesh* mesh = nullptr;
    const TriangleBvh* bvh = nullptr;
    const std::vector<float>* atlasS = nullptr;
    int atlasSize = 0, pages = 0;
};

// CPU reference evaluator: bakes probes [first, first + count): V(n_k) = direct + (optional) indirect per normal, then fitProbe. Probes marked `solid`
// stay invalid. `baked` (size = probe count) is set for the probes that were processed. The ProbeBaker below is what the editor runs.
void bakeProbesCpu(LightvolGrid& grid, const std::vector<char>& solid, std::vector<char>& baked, size_t first, size_t count, const LightvolSource& src,
                   const LightvolSettings& settings);

// Bakes a whole grid in steps. On a ray query device the probes' sample set stays resident on the GPU (one acceleration structure, one device) and each step is
// ray traced direct light + ray traced gather over the baker's bounce atlas (the same shaders and sample contract as the lightmap texels); without one (or on
// any GPU failure) the CPU evaluator runs the same maths on the BVH. The source's pointers must stay valid until the baker is destroyed.
class ProbeBaker {
public:
    ProbeBaker(LightvolGrid grid, std::vector<char> solid, const LightvolSource& src, const LightvolSettings& settings);
    ~ProbeBaker();
    ProbeBaker(const ProbeBaker&) = delete;
    ProbeBaker& operator=(const ProbeBaker&) = delete;
    size_t total() const;
    size_t baked() const;
    bool done() const { return baked() >= total(); }
    // Bakes the next probes: about `probes` of them (a GPU step is never smaller than a few thousand: one submission per step). Returns how many were advanced.
    size_t step(size_t probes);
    bool onGpu() const;
    const std::string& device() const;     // "cpu" or the GPU name
    const std::string& gpuError() const;   // why the GPU is not used (empty when it is, or was not asked for)
    const LightvolGrid& grid() const;
    LightvolGrid take();                   // the finished grid
private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// Which probes lie inside solids (computed once per geometry).
std::vector<char> solidProbes(const LightvolGrid& grid, const SolidSet* solids);

// A brush entity that is lit from the grid: sampled once at the centre of its brushes' bounds.
struct EntityLight {
    std::string instance, entityId;
    Vec3 centre;
    Probe probe;
};
std::vector<EntityLight> entityLights(const std::vector<LightSurface>& surfaces, const LightingClasses& classes, const LightvolGrid& grid);

}  // namespace core
