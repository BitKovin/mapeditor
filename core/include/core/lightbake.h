#pragma once
#include "core/derived.h"
#include "core/lightmap_uv.h"
#include "core/lighting_classes.h"
#include "core/lights.h"
#include "core/raytrace.h"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace core {

// ---- texel samples: one per lightmap texel that lies on a surface (DESIGN.md §6.5) ---------------------------------------
struct TexelSamples {
    int atlas = 0;
    int pageCount = 0;
    std::vector<float> pos;          // 4 floats per sample: world position xyz, 1
    std::vector<float> nrm;          // 4 floats per sample: unit normal xyz, 0
    std::vector<uint32_t> texel;     // page * atlas * atlas + y * atlas + x
    // Optional (empty = the sample's own index): the id each sample hashes its random numbers with. A subset of samples
    // (one chunk) keeps the ids of the full set, so re-evaluating a chunk reproduces what a full bake would have drawn.
    std::vector<uint32_t> seed;
    // Optional (empty = 0): added to BakeOptions::sampleOffset per sample, so chunks that are at different refinement passes can be
    // evaluated in one dispatch.
    std::vector<uint32_t> passOffset;
    // Optional (empty = no super sampling): where the sub-texel sample positions of each texel lie, 12 floats per sample:
    //   [0..2] world position of the texel centre on the chart's plane, [3] the bits of a uint32 mask (bit k set = the sub position k of
    //   kSuperOffsets lies inside the fragment, so it may be lit), [4..6] world displacement per +1 texel in u, [8..10] per +1 texel in v,
    //   [7] / [11] the same kind of mask for the cells of the 2x2 / 4x4 grid of giGridOffset (bit = cell y * scale + x).
    // Sub position k = centre + u * kSuperOffsets[k][0] + v * kSuperOffsets[k][1]. Built by buildTexelSamples.
    std::vector<float> sub;
    size_t count() const { return texel.size(); }
    uint32_t offsetOf(size_t i) const { return passOffset.empty() ? 0u : passOffset[i]; }
    uint32_t seedOf(size_t i) const { return seed.empty() ? static_cast<uint32_t>(i) : seed[i]; }
    bool hasSub() const { return !texel.empty() && sub.size() == texel.size() * 12; }
    uint32_t subMask(size_t i) const;
    Vec3 subPosition(size_t i, int k) const;
    uint32_t gridMask(size_t i, int scale) const;        // scale 2 or 4
    Vec3 gridPosition(size_t i, int scale, int cell) const;
    // Appends sample `i` of `src` (position, normal, texel and, when src has them, the sub positions). Seeds / pass offsets are the caller's.
    void appendFrom(const TexelSamples& src, size_t i);
};

// Offset (in texels, from the texel centre) of cell `i` (0..scale-1) along one axis of the scale x scale grid the global illumination is gathered on.
inline double giGridOffset(int scale, int i) { return (i + 0.5) / scale - 0.5; }

// Sub-texel offsets (in texels, from the texel centre) of the super sampled direct light: the D3D 8x MSAA pattern (eight well spread
// positions, no two sharing a row or a column). BakeOptions::superSamples = N uses the first N. MUST match SUB[] in lightmap_direct.comp.
inline constexpr int kMaxSuperSamples = 8;
inline constexpr float kSuperOffsets[kMaxSuperSamples][2] = {
    {0.0625f, -0.1875f}, {-0.0625f, 0.1875f}, {0.3125f, 0.0625f},  {-0.1875f, -0.3125f},
    {-0.3125f, 0.3125f}, {-0.4375f, -0.0625f}, {0.1875f, 0.4375f}, {0.4375f, -0.4375f}};

// Rasterises every lightmapped surface piece into its chart: each texel whose centre lies inside a piece becomes a sample
// (first piece wins on shared edges). A piece smaller than a texel still claims the texel under its centroid.
// With `solids`, texels buried inside solid brushes (a floor under a wall) are skipped: they are invisible and their rays would start
// inside geometry. `hidden` (optional) receives how many were skipped.
TexelSamples buildTexelSamples(const LightmapLayout& layout, const std::vector<LightSurface>& surfaces, const SolidSet* solids = nullptr, size_t* hidden = nullptr);

// Same result as buildTexelSamples(), but remembers what every surface piece contributes between calls: a piece whose geometry and chart placement did
// not change is not rasterised again (the sub-texel inside tests dominate the cost), and the pieces that did change are rasterised on all cores.
// Only the cheap assembly (first claim wins, buried texels dropped) runs every time, so a moved crate costs about one crate. Not thread safe.
class TexelSampleCache {
public:
    TexelSampleCache();
    ~TexelSampleCache();
    TexelSampleCache(const TexelSampleCache&) = delete;
    TexelSampleCache& operator=(const TexelSampleCache&) = delete;
    TexelSamples build(const LightmapLayout& layout, const std::vector<LightSurface>& surfaces, const SolidSet* solids = nullptr, size_t* hidden = nullptr);
    struct Stats { size_t surfaces = 0, reused = 0; double generateSeconds = 0, assembleSeconds = 0; };  // of the last build()
    const Stats& stats() const { return stats_; }
    void clear();

private:
    struct Entry;
    static void generate(const LightSurface& s, const LightmapFace& f, int atlas, Entry& out);
    std::unordered_map<uint64_t, std::unique_ptr<Entry>> entries_;
    uint64_t generation_ = 0;
    Stats stats_;
};

// ---- global illumination on a finer grid (DESIGN §6.5 "Denoising") --------------------------------------------------------
// The bounce light is noisy (a few dozen rays per texel), so it is gathered on a `scale` x `scale` times finer grid (2 or 4: every texel is
// split into scale^2 cells, each lit at its own position), denoised at that resolution and then averaged back to the texel. That is
// scale^2 times the rays and a denoiser that sees scale^2 times the pixels, at the lightmap's own output resolution.
// One entry per valid cell of every base sample (cells outside the fragment are skipped; a texel without any valid cell keeps one entry at its
// own sample position). Entries of one base sample are contiguous, in base sample order.
struct GiSamples {
    int scale = 1;
    TexelSamples s;                 // atlas = base atlas * scale; texel = page * (atlas * scale)^2 + y * (atlas * scale) + x of the cell
    std::vector<uint32_t> owner;    // base sample of each entry
    std::vector<uint32_t> begin;    // per base sample: its first entry; begin[base count] = total
    size_t count() const { return s.count(); }
};
// scale <= 1, or samples without sub-texel data, give an empty set (scale 1).
GiSamples buildGiSamples(const TexelSamples& base, int scale);
// Averages one value (4 floats) per entry back to one per base sample.
std::vector<float> resolveGi(const GiSamples& gi, const std::vector<float>& values4);

// ---- result ---------------------------------------------------------------------------------------------------------------
// Values are irradiance with the factor pi absorbed (see DESIGN §6.5): colour shown = albedo * value.
struct LightmapPages {
    int atlas = 0;
    int pageCount = 0;
    std::vector<std::vector<float>> rgb;      // per page: atlas * atlas * 3
    std::vector<std::vector<uint8_t>> valid;  // per page: atlas * atlas; 1 = baked, 2 = gutter (dilated)
    Vec3 at(int page, int x, int y) const {
        const size_t i = (static_cast<size_t>(y) * atlas + x) * 3;
        return {rgb[page][i], rgb[page][i + 1], rgb[page][i + 2]};
    }
};

struct BakeOptions {
    bool forceCpu = false;
    bool gpuValidation = false;  // enable the Vulkan validation layer when installed and count its messages
    bool shadows = true;         // ray traced shadows from `classes.castShadows` surfaces (lights with cast_shadows 0 never trace)
    int areaSamples = 32;        // shadow rays per texel for lights with a size (point/spot source_size, directional angle, rect)
    int skySamples = 64;         // cosine-weighted rays per texel for an occluded sky light
    // Super sampling of the direct light: every texel is lit at this many sub-texel positions (1..kMaxSuperSamples) and the results are
    // averaged, which anti-aliases shadow edges and light falloff inside a texel. Each position draws its own shadow rays (areaSamples /
    // skySamples are PER position). Only applies while shadows are traced; 1 = off. Positions outside the fragment (at chart borders) are skipped.
    int superSamples = 8;
    int bounces = 0;             // diffuse indirect bounces (0 = direct light only)
    int giScale = 2;             // the bounce light is gathered on a giScale x giScale times finer grid (1, 2 or 4), denoised there and averaged back
    int giSamples = 64;          // cosine-weighted gather rays per texel and bounce
    double albedo = 1.0;         // reflectance of every surface for bounces (white until textures supply it)
    uint32_t sampleOffset = 0;   // first sample index of the random sequences: progressive refinement draws new samples every pass
    // Denoise the indirect light (global illumination) with Intel Open Image Denoise (core/denoise.h) once the bake is done. Only the bounce
    // light is filtered (it is the noisy part: gather rays); the direct light is added back untouched, so shadow edges stay sharp. Needs
    // bounces > 0. When the library cannot be loaded the noisy result is kept and BakeStats::warnings says why.
    bool denoiseGi = true;
    int denoiseQuality = 2;      // 0 fast, 1 balanced, 2 high (core::denoise::Quality)
    LightingClasses classes = LightingClasses::defaults();  // which classes cast shadows
    std::shared_ptr<const SolidSet> solids;                 // optional: skip texels buried inside solid brushes
    AlbedoLoader albedoLoader;                              // optional: bounces pick up the texture colour of the surface they hit (see core/albedo.h)
};

struct BakeStats {
    bool usedGpu = false;
    std::string device;                           // GPU name, or "cpu"
    size_t texels = 0;
    size_t hiddenTexels = 0;                      // skipped because they lie inside solid brushes
    size_t lights = 0;
    bool denoised = false;                        // the indirect light went through OIDN
    double denoiseSeconds = 0;
    int giScale = 1;                              // grid the bounce light was gathered on
    double seconds = 0;
    std::vector<std::string> warnings;            // light model limits, GPU fallbacks
    std::vector<std::string> validationMessages;  // Vulkan validation warnings / errors (must stay empty in tests)
};

// ---- direct light evaluation -----------------------------------------------------------------------------------------------
// CPU reference: 4 floats (rgb, 0) per sample. The GPU shader (shaders/lightmap_direct.comp) must produce the same numbers.
// `occluders` may be null (no shadows). Shadow behaviour: DESIGN §6.5.
std::vector<float> evaluateDirectCpu(const TexelSamples& samples, const LightScene& lights, const TriangleBvh* occluders = nullptr,
                                     const BakeOptions& options = {});

// Direct + `options.bounces` diffuse bounces (DESIGN §6.5). `occluders` must have been built with a layout (lightmap coordinates).
// I(k+1) = gather(Db + I(k)) where Db is the direct light weighted by each light's `bounce`; result = direct + I(bounces).
// `indirectOut` (optional): receives the indirect part alone (4 floats per sample; zeros without bounces), the returned values still hold the sum.
// `gi` (optional, from buildGiSamples): the last bounce is gathered on its finer grid; `giOut` then receives that raw (noisy) grid, one value per
// entry, so the caller can denoise it before averaging (resolveGi). The returned values / indirectOut hold the plain average of it.
std::vector<float> evaluateGiCpu(const TexelSamples& samples, const LightScene& lights, const OccluderMesh& mesh, const TriangleBvh& bvh,
                                 const BakeOptions& options, std::vector<float>* indirectOut = nullptr, const GiSamples* gi = nullptr,
                                 std::vector<float>* giOut = nullptr);

// One gather bounce: I(p) = mean over `samples` cosine-weighted rays of albedo * window(hit distance, range) * S(hit), 4 floats per sample. `atlasS` holds
// S (rgb) and a validity weight (a) per atlas texel (pages * atlas * atlas * 4 floats); `mesh` must carry lightmap coordinates.
// `sAtlas` (0 = samples.atlas): the size of the atlas `atlasS` is stored at, when it differs from the samples' own (a finer GI grid).
std::vector<float> gatherCpu(const TexelSamples& samples, const OccluderMesh& mesh, const TriangleBvh& bvh, const std::vector<float>& atlasS,
                             double albedo, int giSamples, uint32_t sampleOffset, double range = 10000.0, int sAtlas = 0);

// Direct light (4 floats per sample) and, optionally, the bounce-weighted direct light `db` (see evaluateGiCpu).
std::vector<float> evaluateDirectWithBounceCpu(const TexelSamples& samples, const LightScene& lights, const TriangleBvh* occluders,
                                               const BakeOptions& options, std::vector<float>* db);

// Shared constants of the shadow model (the shader has the same numbers).
constexpr double kShadowBias = 0.01;        // m: rays start this far above the surface and stop this far before their target
constexpr double kBounceNearDistance = 1.0; // m: the bounce source of a point / spot light stops growing closer than this (a light never feeds more than
                                            // its brightness into the bounce, so a light next to a wall cannot flood the room through hot spots)
constexpr double kShadowFar = 10000.0;      // m: length of rays towards infinitely distant lights (directional, sky)
constexpr double kAoBias = 1e-3;            // m: baked ambient occlusion rays start this far above the surface (CPU and shaders/lightmap_ao.comp)

// The finished light of a texel: dark lights (negative brightness) make the evaluators' sums signed, but a lightmap never stores light below 0.
inline Vec3 clampLight(const Vec3& v) { return {v.x > 0 ? v.x : 0, v.y > 0 ? v.y : 0, v.z > 0 ? v.z : 0}; }

// Spreads sample values into pages, then fills the gutter (`padding` texels) by dilation.
LightmapPages assemblePages(const LightmapLayout& layout, const TexelSamples& samples, const std::vector<float>& values4);

// Full bake: samples -> evaluation (GPU when available and not forced off, else CPU) -> pages.
LightmapPages bakeLightmaps(const LightmapLayout& layout, const std::vector<LightSurface>& surfaces, const LightScene& lights,
                            const BakeOptions& options = {}, BakeStats* stats = nullptr);

// Debug output: a page as an sRGB PNG (values clamped to 0..1). Returns false on I/O failure.
bool writePagePng(const LightmapPages& pages, int page, const std::string& path);

}  // namespace core
