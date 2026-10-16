#include "core/lightbake.h"
#include "test_util.h"
#include <doctest.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>

using namespace core;
using namespace testutil;

namespace {
constexpr double kPi = 3.14159265358979323846;

Entity ent(const std::string& id, const std::string& cls, std::vector<Brush> brushes = {}) {
    Entity e;
    e.id = id;
    e.classname = cls;
    e.brushes = std::move(brushes);
    return e;
}
Entity pointLight(Vec3 o, double brightness, double radius, double bounce = 1) {
    Entity e = ent("l", "light_point");
    e.origin = o;
    e.keyvalues["brightness"] = std::to_string(brightness);
    e.keyvalues["radius"] = std::to_string(radius);
    e.keyvalues["bounce"] = std::to_string(bounce);
    return e;
}

// The room of the GPU gather tests: floor, ceiling, back wall and a pillar, a warm point light, a spot and a sky light.
std::vector<Entity> gatherRoomEntities() {
    // Not at (1, 2.2, 1): that point lies on the plane x = z through the pillar's vertical corner edge (0, y, 0), so every floor
    // texel on the floor's x = z diagonal had a shadow ray that grazed the edge exactly. The CPU (double) counted it as blocked, the
    // GPU (float) let it through, and the lit texel then fed both bounces. A degenerate scene, not a gather bug.
    Entity warm = pointLight({1.3, 2.2, 0.8}, 14, 40);
    warm.keyvalues["color"] = "1 0.8 0.5";
    Entity spot = ent("s", "light_spot");
    spot.origin = {-3, 2.8, -2};
    spot.angles = {70, 40, 0};
    spot.keyvalues["brightness"] = "9";
    spot.keyvalues["radius"] = "30";
    Entity sky = ent("k", "light_sky");
    sky.keyvalues["brightness"] = "0.3";
    sky.keyvalues["bounce"] = "0.5";
    return {ent("w", "worldspawn", {Brush::makeBox("floor", {-5, -1, -5}, {5, 0, 5}), Brush::makeBox("ceil", {-5, 3, -5}, {5, 4, 5}),
                                    Brush::makeBox("wall", {-5, 0, 4}, {5, 3, 5}), Brush::makeBox("pillar", {0, 0, -1}, {1, 3, 0})}),
            warm, spot, sky};
}

// Distance from p to the nearest polygon edge of any surface: tells whether a bad texel sits on a geometric edge.
double edgeDistance(const std::vector<LightSurface>& surfaces, const Vec3& p) {
    double best = 1e300;
    for (const auto& s : surfaces)
        for (size_t i = 0; i < s.verts.size(); ++i) {
            const Vec3 a = s.verts[i], b = s.verts[(i + 1) % s.verts.size()], ab = b - a;
            const double l2 = ab.dot(ab);
            const double t = l2 > 0 ? std::clamp((p - a).dot(ab) / l2, 0.0, 1.0) : 0.0;
            best = std::min(best, (p - (a + ab * t)).length());
        }
    return best;
}


struct GiScene {
    Document doc;
    std::vector<LightSurface> surfaces;
    LightmapLayout layout;
    LightScene lights;
    TexelSamples samples;
    OccluderMesh mesh;
    std::shared_ptr<SolidSet> solids;
    explicit GiScene(std::vector<Entity> es, double tpm = 2) {
        doc.entities = std::move(es);
        surfaces = collectSurfaces(doc, {});
        LightmapSettings cfg;
        cfg.texelsPerMeter = tpm;
        layout = buildLightmapLayout(surfaces, LightingClasses::defaults(), cfg);
        lights = collectLights(doc, {});
        const LightingClasses classes = LightingClasses::defaults();
        solids = std::make_shared<SolidSet>(collectSolids(doc, {}, [&](const std::string& c) { return classes.castsShadows(c); }));
        samples = buildTexelSamples(layout, surfaces, solids.get());
        mesh = buildOccluders(surfaces, LightingClasses::defaults(), &layout);
    }
    std::vector<float> run(BakeOptions o) const {
        const TriangleBvh bvh(mesh);
        return evaluateGiCpu(samples, lights, mesh, bvh, o);
    }
    size_t nearest(Vec3 p, Vec3 n) const {
        size_t best = 0;
        double bd = 1e300;
        for (size_t i = 0; i < samples.count(); ++i) {
            const Vec3 q{samples.pos[i * 4], samples.pos[i * 4 + 1], samples.pos[i * 4 + 2]}, nn{samples.nrm[i * 4], samples.nrm[i * 4 + 1], samples.nrm[i * 4 + 2]};
            if (nn.dot(n) < 0.99) continue;
            if ((q - p).length() < bd) { bd = (q - p).length(); best = i; }
        }
        return best;
    }
    Vec3 pos(size_t i) const { return {samples.pos[i * 4], samples.pos[i * 4 + 1], samples.pos[i * 4 + 2]}; }
};

double win(double d, double r) { const double w = std::max(0.0, 1.0 - std::pow(d / r, 4)); return w * w; }


// Independent reference for the floor / ceiling scene: I(p) = integral over the hemisphere of cos/pi * albedo(1) * V_ceiling(hit),
// V = direct light of the point light at (0, 2.5, 0) (brightness 10) on the ceiling underside (y = 3, 24 m wide). Numerical quadrature,
// shares no code with the baker.
double analyticCeilingBounce(const Vec3& p) {
    const Vec3 L{0, 2.5, 0};
    const int N = 800;
    double ref = 0;
    for (int a = 0; a < N; ++a) {
        const double theta = (a + 0.5) / N * (kPi / 2);
        for (int b = 0; b < N; ++b) {
            const double phi = (b + 0.5) / N * 2 * kPi;
            const Vec3 w{std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)};
            const double t = (3.0 - p.y) / w.y;
            const Vec3 q = p + w * t;
            if (std::fabs(q.x) > 12 || std::fabs(q.z) > 12) continue;
            const Vec3 d = L - q;
            const double dist = d.length();
            const double cosL = std::max(0.0, d.normalized().dot(Vec3{0, -1, 0}));
            // bounce source: the inverse-square term stops growing closer than kBounceNearDistance
            const double Vq = 10 * win(dist, 100) / (std::max(dist, kBounceNearDistance) * std::max(dist, kBounceNearDistance)) * cosL;
            ref += std::cos(theta) / kPi * Vq * std::sin(theta) * (kPi / 2 / N) * (2 * kPi / N);
        }
    }
    return ref;
}

// floor (y = 0 top) and a ceiling slab (underside y = 3), both 24 m wide, a white-ish point light between them
std::vector<Entity> floorCeiling(double bounce = 1) {
    return {ent("w", "worldspawn", {Brush::makeBox("floor", {-12, -1, -12}, {12, 0, 12}), Brush::makeBox("ceil", {-12, 3, -12}, {12, 4, 12})}),
            pointLight({0, 2.5, 0}, 10, 100, bounce)};
}
}  // namespace

TEST_CASE("bounce cpu: one bounce off a ceiling matches numerical integration of the rendering equation") {
    GiScene sc(floorCeiling());
    BakeOptions o;
    o.bounces = 1;
    o.giSamples = 512;
    const auto direct = sc.run(BakeOptions{});
    const auto gi = sc.run(o);
    const size_t i = sc.nearest({0, 0, 0}, {0, 1, 0});
    const Vec3 p = sc.pos(i);
    const double ref = analyticCeilingBounce(p);
    const double indirect = gi[i * 4] - direct[i * 4];
    CHECK(indirect > 0.5);
    CHECK(indirect == doctest::Approx(ref).epsilon(0.04));
}

TEST_CASE("bounce cpu: no bounces, albedo, bounce power and bounce count behave linearly and monotonically") {
    GiScene sc(floorCeiling());
    BakeOptions base;
    base.giSamples = 128;
    const auto direct = sc.run(base);
    BakeOptions b1 = base;
    b1.bounces = 1;
    const auto one = sc.run(b1);
    // direct light is untouched by bounces
    BakeOptions b0 = base;
    b0.bounces = 0;
    CHECK(sc.run(b0) == direct);
    // albedo scales the indirect part exactly
    BakeOptions half = b1;
    half.albedo = 0.5;
    const auto h = sc.run(half);
    for (size_t i = 0; i < sc.samples.count(); i += 17)
        CHECK(h[i * 4] - direct[i * 4] == doctest::Approx(0.5 * (one[i * 4] - direct[i * 4])).epsilon(1e-4).scale(1e-3));
    // more bounces never reduce light, and add some
    BakeOptions b2 = base;
    b2.bounces = 2;
    const auto two = sc.run(b2);
    double gain = 0;
    for (size_t i = 0; i < sc.samples.count(); ++i) {
        CHECK(two[i * 4] >= one[i * 4] - 1e-5);
        gain += two[i * 4] - one[i * 4];
    }
    CHECK(gain > 1.0);
    // a light with bounce power 0 contributes no indirect light; power 2 doubles it
    GiScene none(floorCeiling(0));
    const auto n0 = none.run(b1);
    const auto nd = none.run(base);
    for (size_t i = 0; i < none.samples.count(); i += 13) CHECK(n0[i * 4] == nd[i * 4]);
    GiScene dbl(floorCeiling(2));
    const auto d2 = dbl.run(b1);
    const auto dd = dbl.run(base);
    for (size_t i = 0; i < dbl.samples.count(); i += 13)
        CHECK(d2[i * 4] - dd[i * 4] == doctest::Approx(2 * (one[i * 4] - direct[i * 4])).epsilon(1e-4).scale(1e-3));
}

TEST_CASE("bounce cpu: open space has no indirect light; a lit wall bleeds colour onto the floor") {
    // floor only: nothing to bounce off
    GiScene open({ent("w", "worldspawn", {Brush::makeBox("floor", {-6, -1, -6}, {6, 0, 6})}), pointLight({0, 2, 0}, 10, 50)});
    BakeOptions o;
    o.bounces = 2;
    o.giSamples = 64;
    const auto a = open.run(o);
    BakeOptions d;
    CHECK(a == open.run(d));
    // a wall lit by a coloured light: the floor next to it gets the colour (red channel dominates the indirect part)
    Entity l = pointLight({-1, 1.5, 0}, 12, 50);
    l.keyvalues["color"] = "1 0.1 0.1";
    GiScene room({ent("w", "worldspawn", {Brush::makeBox("floor", {-6, -1, -6}, {6, 0, 6}), Brush::makeBox("wall", {1.5, 0, -6}, {2.5, 4, 6})}), l});
    BakeOptions g;
    g.bounces = 1;
    g.giSamples = 256;
    const auto lit = room.run(g);
    const auto dir = room.run(BakeOptions{});
    const size_t i = room.nearest({0.8, 0, 0}, {0, 1, 0});
    const double ir = lit[i * 4] - dir[i * 4], ig = lit[i * 4 + 1] - dir[i * 4 + 1];
    CHECK(ir > 0.01);
    CHECK(ir > 5 * ig);  // red light bounces red
}

namespace {
// page texel of a sample inside a baked page set
Vec3 pageValue(const LightmapPages& P, const TexelSamples& s, size_t i) {
    const size_t per = static_cast<size_t>(P.atlas) * P.atlas, page = s.texel[i] / per, t = s.texel[i] % per;
    return {P.rgb[page][t * 3], P.rgb[page][t * 3 + 1], P.rgb[page][t * 3 + 2]};
}
}  // namespace

TEST_CASE("bounce gpu: the hardware gather matches numerical integration of the rendering equation (no CPU involved)") {
    GiScene sc(floorCeiling());
    BakeOptions o;
    o.gpuValidation = true;
    o.giSamples = 512;
    o.bounces = 1;
    BakeStats st;
    const LightmapPages withGi = bakeLightmaps(sc.layout, sc.surfaces, sc.lights, o, &st);
    if (!st.usedGpu) { MESSAGE("no Vulkan device with ray query, skipped: " << (st.warnings.empty() ? "" : st.warnings.back())); return; }
    MESSAGE("GPU device: " << st.device);
    for (const auto& m : st.validationMessages) CHECK(m.rfind("[note]", 0) == 0);
    BakeOptions d = o;
    d.bounces = 0;
    const LightmapPages direct = bakeLightmaps(sc.layout, sc.surfaces, sc.lights, d);
    // a few floor texels at different distances from the light: the GPU's indirect light vs the quadrature
    for (Vec3 q : {Vec3{0, 0, 0}, Vec3{3, 0, 1}, Vec3{-6, 0, 4}}) {
        const size_t i = sc.nearest(q, {0, 1, 0});
        const double indirect = pageValue(withGi, sc.samples, i).x - pageValue(direct, sc.samples, i).x;
        const double ref = analyticCeilingBounce(sc.pos(i));
        MESSAGE("texel at " << sc.pos(i).x << "," << sc.pos(i).z << ": gpu " << indirect << ", analytic " << ref);
        CHECK(indirect == doctest::Approx(ref).epsilon(0.05));
    }
}

TEST_CASE("bounce gpu: hardware gather agrees with the CPU reference (2 bounces, shadows, several lights), validation clean") {
    GiScene sc(gatherRoomEntities(), 3);
    BakeOptions o;
    o.gpuValidation = true;
    o.solids = sc.solids;  // the baker skips texels buried inside the pillar / wall, exactly like sc.samples did
    o.bounces = 2;
    o.giSamples = 48;
    o.skySamples = 32;
    o.denoiseGi = false;  // compares raw Monte Carlo estimates (the denoiser would hide the noise this test measures against)
    BakeStats gst;
    const LightmapPages g = bakeLightmaps(sc.layout, sc.surfaces, sc.lights, o, &gst);
    if (!gst.usedGpu) {
        MESSAGE("no Vulkan device with ray query, GPU bounce test skipped: " << (gst.warnings.empty() ? "" : gst.warnings.back()));
        return;
    }
    MESSAGE("GPU device: " << gst.device << ", texels " << gst.texels);
    for (const auto& m : gst.validationMessages) MESSAGE(m);
    for (const auto& m : gst.validationMessages) CHECK(m.rfind("[note]", 0) == 0);
    BakeOptions c = o;
    c.forceCpu = true;
    const LightmapPages cp = bakeLightmaps(sc.layout, sc.surfaces, sc.lights, c);
    // The natural noise level: two CPU runs with the SAME settings but different random numbers. GPU and CPU trace the same rays, so
    // their difference must be far below this; it is only hit resolution at geometric edges (float vs double traversal).
    BakeOptions other = c;
    other.sampleOffset = 4096;
    const LightmapPages cp2 = bakeLightmaps(sc.layout, sc.surfaces, sc.lights, other);
    BakeOptions direct = c;
    direct.bounces = 0;
    const LightmapPages dp = bakeLightmaps(sc.layout, sc.surfaces, sc.lights, direct);

    struct Worst { double err; size_t sample; };
    std::vector<Worst> worst;
    size_t texels = 0, indirectTexels = 0, outliers = 0;
    double gpuErr = 0, noiseErr = 0;
    for (size_t i = 0; i < sc.samples.count(); ++i) {
        ++texels;
        const Vec3 a = pageValue(g, sc.samples, i), b = pageValue(cp, sc.samples, i), n = pageValue(cp2, sc.samples, i), d = pageValue(dp, sc.samples, i);
        const double e = std::max({std::fabs(a.x - b.x), std::fabs(a.y - b.y), std::fabs(a.z - b.z)}) / std::max(1.0, std::max({b.x, b.y, b.z}));
        const double ne = std::max({std::fabs(n.x - b.x), std::fabs(n.y - b.y), std::fabs(n.z - b.z)}) / std::max(1.0, std::max({b.x, b.y, b.z}));
        gpuErr += e;
        noiseErr += ne;
        if (e > 0.25) ++outliers;
        worst.push_back({e, i});
        if (b.x + b.y + b.z - d.x - d.y - d.z > 0.02) ++indirectTexels;
    }
    std::sort(worst.begin(), worst.end(), [](const Worst& x, const Worst& y) { return x.err > y.err; });
    for (size_t k = 0; k < 3 && k < worst.size(); ++k) {
        const size_t i = worst[k].sample;
        const Vec3 a = pageValue(g, sc.samples, i), b = pageValue(cp, sc.samples, i);
        MESSAGE("worst #" << k << ": err " << worst[k].err << " at world (" << sc.pos(i).x << ", " << sc.pos(i).y << ", " << sc.pos(i).z << ") normal ("
                          << sc.samples.nrm[i * 4] << ", " << sc.samples.nrm[i * 4 + 1] << ", " << sc.samples.nrm[i * 4 + 2] << "), edge distance " << edgeDistance(sc.surfaces, sc.pos(i))
                          << " m, gpu " << a.x << " cpu " << b.x);
    }
    gpuErr /= static_cast<double>(texels);
    noiseErr /= static_cast<double>(texels);
    MESSAGE("texels " << texels << ", gpu-vs-cpu mean err " << gpuErr << ", natural Monte Carlo noise (cpu vs cpu) " << noiseErr << ", outliers >25% " << outliers
                      << ", texels with indirect " << indirectTexels);
    CHECK(indirectTexels > texels / 4);                                           // bounces really light the scene
    CHECK(noiseErr > 0.01);                                                       // the noise reference is meaningful
    // Texels buried inside solids are skipped and the light is off every edge plane, so the two back ends trace the same rays through the
    // same visible geometry: they agree to float precision, thousands of times below the Monte Carlo noise. (Before buried texels were
    // skipped, coplanar ties between a pillar top and the ceiling made this ~1e-3 with outliers of several percent.)
    CHECK(gpuErr < 0.01 * noiseErr);
    CHECK(static_cast<double>(outliers) < 0.005 * static_cast<double>(texels));    // >25 % deviations: essentially none
    CHECK(worst.front().err < 0.1);                                               // and no single texel is badly off
}

// ---- diagnostic (skipped by default; run with: <test exe> -ns -tc="bounce gpu diag*") ---------------------------------------
// Narrows down where the GPU gather and the CPU reference part ways, using the worst texel of the 2-bounce comparison:
//   A. 2-bounce GPU vs CPU statistics (the failing test's numbers).
//   B. Pass 2 isolated: the CPU gather run over the GPU's own pass-1 atlas ("hybrid"). GPU == hybrid means pass 2 is fine and the
//      difference comes from pass 1; GPU != hybrid means the GPU's pass-2 gather itself differs.
//   C. Pass 1, ray by ray, at that texel: GPU rays are recovered by differencing bakes with 1..N samples (the sequence is nested),
//      CPU rays by gatherCpu on a one-texel subset. Rays that differ are listed with their CPU hit.
namespace {
struct Cmp {
    size_t texels = 0, big = 0;
    double sum = 0, max = 0;
    int page = 0;
    size_t t = 0;
};
Cmp compareDiag(const LightmapPages& g, const LightmapPages& c) {
    Cmp r;
    for (int p = 0; p < g.pageCount; ++p)
        for (size_t t = 0; t < g.valid[static_cast<size_t>(p)].size(); ++t) {
            if (g.valid[static_cast<size_t>(p)][t] != 1) continue;
            ++r.texels;
            double e = 0;
            for (size_t ch = 0; ch < 3; ++ch) {
                const double a = g.rgb[static_cast<size_t>(p)][t * 3 + ch], b = c.rgb[static_cast<size_t>(p)][t * 3 + ch];
                e = std::max(e, std::fabs(a - b) / std::max(1.0, std::fabs(b)));
            }
            r.sum += e;
            if (e > 5e-2) ++r.big;
            if (e > r.max) { r.max = e; r.page = p; r.t = t; }
        }
    return r;
}
std::string describe(const char* what, const Cmp& r) {
    std::ostringstream o;
    o << what << ": texels " << r.texels << ", mean err " << r.sum / static_cast<double>(std::max<size_t>(r.texels, 1)) << ", >5% " << r.big
      << ", max err " << r.max << " at page " << r.page << " texel index " << r.t;
    return o.str();
}
std::string rgbAt(const LightmapPages& g, int page, size_t t) {
    std::ostringstream o;
    const auto& v = g.rgb[static_cast<size_t>(page)];
    o << "(" << v[t * 3] << "," << v[t * 3 + 1] << "," << v[t * 3 + 2] << ")";
    return o.str();
}
// Copies of the sampler in src/lightbake.cpp, to rebuild a gather ray's direction.
uint32_t diagPcg(uint32_t v) {
    const uint32_t state = v * 747796405u + 2891336453u;
    const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
void diagSample2(uint32_t texel, uint32_t light, uint32_t s, float& u, float& v) {
    const uint32_t h = diagPcg(texel + diagPcg(light * 7919u + 12345u));
    const float u0 = static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
    const float v0 = static_cast<float>(diagPcg(h) >> 8) * (1.0f / 16777216.0f);
    const float a = u0 + static_cast<float>(s) * 0.7548776662f, b = v0 + static_cast<float>(s) * 0.5698402909f;
    u = a - std::floor(a);
    v = b - std::floor(b);
}
}  // namespace

TEST_CASE("bounce gpu diag: where the 2-bounce GPU and CPU results part ways" * doctest::skip()) {
    GiScene sc(gatherRoomEntities(), 3);
    BakeOptions base;
    base.solids = sc.solids;
    base.skySamples = 32;
    base.giSamples = 48;
    BakeStats st;
    auto bake = [&](bool cpu, int bounces, int samples) {
        BakeOptions o = base;
        o.forceCpu = cpu;
        o.bounces = bounces;
        o.giSamples = samples;
        return bakeLightmaps(sc.layout, sc.surfaces, sc.lights, o, cpu ? nullptr : &st);
    };
    const LightmapPages g2 = bake(false, 2, 48);
    if (!st.usedGpu) { MESSAGE("no Vulkan device with ray query, diagnostic skipped"); return; }
    MESSAGE("GPU device: " << st.device);
    const LightmapPages c2 = bake(true, 2, 48);
    const Cmp a = compareDiag(g2, c2);
    MESSAGE(describe("A. 2 bounces, GPU vs CPU", a));
    const int wp = a.page;
    const size_t wt = a.t;
    const size_t per = static_cast<size_t>(g2.atlas) * static_cast<size_t>(g2.atlas);
    const size_t texelId = static_cast<size_t>(wp) * per + wt;
    size_t si = sc.samples.count();
    for (size_t i = 0; i < sc.samples.count(); ++i)
        if (sc.samples.texel[i] == texelId) si = i;
    REQUIRE(si < sc.samples.count());
    const Vec3 pos{sc.samples.pos[si * 4], sc.samples.pos[si * 4 + 1], sc.samples.pos[si * 4 + 2]};
    const Vec3 nrm{sc.samples.nrm[si * 4], sc.samples.nrm[si * 4 + 1], sc.samples.nrm[si * 4 + 2]};

    // B. pass 2 over the GPU's pass-1 atlas
    const LightmapPages g1 = bake(false, 1, 48), c1 = bake(true, 1, 48);
    MESSAGE(describe("   1 bounce, GPU vs CPU", compareDiag(g1, c1)));
    const TriangleBvh bvh(sc.mesh);
    std::vector<float> db;
    const std::vector<float> dcpu = evaluateDirectWithBounceCpu(sc.samples, sc.lights, &bvh, base, &db);
    std::vector<float> atlasGpu1(per * static_cast<size_t>(sc.samples.pageCount) * 4, 0.f), atlasCpu1 = atlasGpu1;
    for (size_t i = 0; i < sc.samples.count(); ++i) {
        const size_t id = sc.samples.texel[i], page = id / per, idx = id % per;
        for (size_t ch = 0; ch < 3; ++ch) {
            atlasGpu1[id * 4 + ch] = g1.rgb[page][idx * 3 + ch] - dcpu[i * 4 + ch] + db[i * 4 + ch];  // S1 = Db + I1
            atlasCpu1[id * 4 + ch] = c1.rgb[page][idx * 3 + ch] - dcpu[i * 4 + ch] + db[i * 4 + ch];
        }
        atlasGpu1[id * 4 + 3] = atlasCpu1[id * 4 + 3] = 1.f;
    }
    const std::vector<float> iHyb = gatherCpu(sc.samples, sc.mesh, bvh, atlasGpu1, base.albedo, base.giSamples, 0);
    const std::vector<float> iCpu = gatherCpu(sc.samples, sc.mesh, bvh, atlasCpu1, base.albedo, base.giSamples, 0);
    auto statsAgainst = [&](const std::vector<float>& ind, const LightmapPages& other) {
        Cmp r;
        r.texels = sc.samples.count();
        for (size_t i = 0; i < sc.samples.count(); ++i) {
            const size_t id = sc.samples.texel[i], page = id / per, idx = id % per;
            double e = 0;
            for (size_t ch = 0; ch < 3; ++ch)
                e = std::max(e, std::fabs(dcpu[i * 4 + ch] + ind[i * 4 + ch] - other.rgb[page][idx * 3 + ch]) / std::max(1.0, std::fabs(static_cast<double>(other.rgb[page][idx * 3 + ch]))));
            r.sum += e;
            if (e > 5e-2) ++r.big;
            if (e > r.max) { r.max = e; r.page = static_cast<int>(page); r.t = idx; }
        }
        return r;
    };
    MESSAGE(describe("B. pass 2 over the GPU atlas (hybrid) vs GPU 2-bounce", statsAgainst(iHyb, g2)));
    MESSAGE(describe("   pass 2 over the CPU atlas (= CPU 2-bounce) vs CPU 2-bounce", statsAgainst(iCpu, c2)));
    MESSAGE(describe("   hybrid vs CPU 2-bounce", statsAgainst(iHyb, c2)));
    {
        std::ostringstream o;
        o << "   target texel page " << wp << " index " << wt << " pos (" << pos.x << "," << pos.y << "," << pos.z << ") normal (" << nrm.x << "," << nrm.y << "," << nrm.z
          << ")\n      GPU 2-bounce " << rgbAt(g2, wp, wt) << "  CPU 2-bounce " << rgbAt(c2, wp, wt) << "  GPU 1-bounce " << rgbAt(g1, wp, wt) << "  CPU 1-bounce " << rgbAt(c1, wp, wt)
          << "\n      hybrid 2-bounce (" << dcpu[si * 4] + iHyb[si * 4] << "," << dcpu[si * 4 + 1] + iHyb[si * 4 + 1] << "," << dcpu[si * 4 + 2] + iHyb[si * 4 + 2] << ")";
        MESSAGE(o.str());
    }

    // C. pass 1 ray by ray at the target texel
    const LightmapPages gd = bake(false, 0, 48);
    TexelSamples one;
    one.atlas = sc.samples.atlas;
    one.pageCount = sc.samples.pageCount;
    one.pos.assign(sc.samples.pos.begin() + static_cast<std::ptrdiff_t>(si * 4), sc.samples.pos.begin() + static_cast<std::ptrdiff_t>(si * 4 + 4));
    one.nrm.assign(sc.samples.nrm.begin() + static_cast<std::ptrdiff_t>(si * 4), sc.samples.nrm.begin() + static_cast<std::ptrdiff_t>(si * 4 + 4));
    one.texel = {sc.samples.texel[si]};
    one.seed = {sc.samples.seedOf(si)};
    std::vector<float> atlasDirect(atlasGpu1.size(), 0.f);  // pass-1 source: the bounce-weighted direct light
    for (size_t i = 0; i < sc.samples.count(); ++i) {
        for (size_t ch = 0; ch < 3; ++ch) atlasDirect[sc.samples.texel[i] * 4 + ch] = db[i * 4 + ch];
        atlasDirect[sc.samples.texel[i] * 4 + 3] = 1.f;
    }
    constexpr int kRays = 48;
    double prevG[3] = {0, 0, 0}, totG[3] = {0, 0, 0}, totC[3] = {0, 0, 0};
    int differing = 0;
    std::ostringstream rows;
    for (int n = 1; n <= kRays; ++n) {
        const LightmapPages gn = bake(false, 1, n);
        double sumG[3], rayG[3], rayC[3];
        for (size_t ch = 0; ch < 3; ++ch) {
            sumG[ch] = n * (static_cast<double>(gn.rgb[static_cast<size_t>(wp)][wt * 3 + ch]) - gd.rgb[static_cast<size_t>(wp)][wt * 3 + ch]);
            rayG[ch] = sumG[ch] - prevG[ch];
            prevG[ch] = sumG[ch];
        }
        const std::vector<float> rc = gatherCpu(one, sc.mesh, bvh, atlasDirect, base.albedo, 1, static_cast<uint32_t>(n - 1));
        for (size_t ch = 0; ch < 3; ++ch) {
            rayC[ch] = rc[ch];
            totG[ch] += rayG[ch];
            totC[ch] += rayC[ch];
        }
        bool differs = false;
        for (size_t ch = 0; ch < 3; ++ch) differs = differs || std::fabs(rayG[ch] - rayC[ch]) > 2e-3 * (1.0 + std::fabs(rayC[ch]));
        if (!differs) continue;
        ++differing;
        float u, v;
        diagSample2(sc.samples.seedOf(si), 100000u, static_cast<uint32_t>(n - 1), u, v);
        const Vec3 t = std::fabs(nrm.y) < 0.99 ? nrm.cross(Vec3{0, 1, 0}).normalized() : Vec3{1, 0, 0};
        const Vec3 b = nrm.cross(t);
        const double r = std::sqrt(static_cast<double>(u)), phi = 2.0 * kPi * v;
        const Vec3 dir = t * (r * std::cos(phi)) + b * (r * std::sin(phi)) + nrm * std::sqrt(std::max(0.0, 1.0 - u));
        const Vec3 o = pos + nrm * kShadowBias;
        const auto h = bvh.closestHit(o, dir, kShadowFar);
        rows << "   ray " << n - 1 << ": dir (" << dir.x << "," << dir.y << "," << dir.z << ")  cpu (" << rayC[0] << "," << rayC[1] << "," << rayC[2] << ")  gpu (" << rayG[0] << "," << rayG[1]
             << "," << rayG[2] << ")";
        if (h.t < 0) rows << "  cpu: no hit";
        else {
            const Vec3 hp = o + dir * h.t;
            rows << "  cpu hit (" << hp.x << "," << hp.y << "," << hp.z << ") tri " << h.triangle << " bary (" << 1 - h.b1 - h.b2 << "," << h.b1 << "," << h.b2 << ")";
        }
        rows << "\n";
    }
    std::ostringstream o;
    o << "C. pass 1, " << kRays << " rays at the target texel: " << differing << " differ.  sum cpu (" << totC[0] << "," << totC[1] << "," << totC[2] << ")  sum gpu (" << totG[0] << "," << totG[1] << ","
      << totG[2] << ")\n" << rows.str();
    MESSAGE(o.str());
}

TEST_CASE("bounce cpu: a light right next to a surface never feeds more than its brightness into the bounce source") {
    // light 5 cm below the ceiling: direct light on the ceiling is huge (1/0.05^2), the bounce source is capped at the 1 m value
    GiScene sc(floorCeiling());
    sc.lights.lights[0].position = {0, 2.95, 0};
    const TriangleBvh bvh(sc.mesh);
    BakeOptions o;
    std::vector<float> db;
    const std::vector<float> d = evaluateDirectWithBounceCpu(sc.samples, sc.lights, &bvh, o, &db);
    float maxD = 0, maxDb = 0;
    for (size_t i = 0; i < sc.samples.count(); ++i) { maxD = std::max(maxD, d[i * 4]); maxDb = std::max(maxDb, db[i * 4]); }
    CHECK(maxD > 10.5f);              // the direct light itself is not capped (it exceeds the brightness here)
    CHECK(maxDb <= 10.f * 1.0001f);   // brightness 10 at the reference distance, times cos <= 1
}
