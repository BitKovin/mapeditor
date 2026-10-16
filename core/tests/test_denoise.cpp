#include "core/denoise.h"
#include "core/lightbake.h"
#include "test_util.h"
#include <doctest.h>
#include <cmath>
#include <set>

using namespace core;
using namespace testutil;

namespace {

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

struct Room {
    Document doc;
    std::vector<LightSurface> surfaces;
    LightmapLayout layout;
    LightScene lights;
    TexelSamples samples;
    OccluderMesh mesh;
    explicit Room(std::vector<Entity> es, double tpm = 4) {
        doc.entities = std::move(es);
        surfaces = collectSurfaces(doc, {});
        LightmapSettings cfg;
        cfg.texelsPerMeter = tpm;
        layout = buildLightmapLayout(surfaces, LightingClasses::defaults(), cfg);
        lights = collectLights(doc, {});
        samples = buildTexelSamples(layout, surfaces);
        mesh = buildOccluders(surfaces, LightingClasses::defaults(), &layout);
    }
};

std::vector<Entity> closedRoom() {
    return {ent("w", "worldspawn", {Brush::makeBox("floor", {-4, -1, -4}, {4, 0, 4}), Brush::makeBox("ceil", {-4, 3, -4}, {4, 4, 4}),
                                    Brush::makeBox("w1", {-5, 0, -4}, {-4, 3, 4}), Brush::makeBox("w2", {4, 0, -4}, {5, 3, 4}),
                                    Brush::makeBox("w3", {-4, 0, 4}, {4, 3, 5}), Brush::makeBox("w4", {-4, 0, -5}, {4, 3, -4}),
                                    Brush::makeBox("pillar", {0, 0, -1}, {1, 3, 0})}),
            pointLight({-2, 2.5, 1}, 12, 40)};
}

double rms(const std::vector<float>& a, const std::vector<float>& ref) {
    double acc = 0;
    size_t n = 0;
    for (size_t i = 0; i < a.size(); i += 4)
        for (size_t c = 0; c < 3; ++c) { const double d = a[i + c] - ref[i + c]; acc += d * d; ++n; }
    return std::sqrt(acc / std::max<size_t>(n, 1));
}
}  // namespace

TEST_CASE("super sampling: sub positions lie inside the texel and are masked at chart borders") {
    Room r({ent("w", "worldspawn", {Brush::makeBox("floor", {-3.9, -1, -3.9}, {3.9, 0, 3.9})})}, 4);  // not texel aligned: border texels are partly outside
    REQUIRE(r.samples.hasSub());
    REQUIRE(r.samples.count() > 100);
    size_t full = 0, partial = 0, none = 0;
    for (size_t i = 0; i < r.samples.count(); ++i) {
        const uint32_t m = r.samples.subMask(i);
        if (m == 0xFFu) ++full; else if (m == 0) ++none; else ++partial;
        // a sub position never lies further than half a texel (plus the clamp of border texels) from the texel's centre
        for (int k = 0; k < kMaxSuperSamples; ++k) {
            if (!(m & (1u << k))) continue;
            const Vec3 centre{r.samples.sub[i * 12], r.samples.sub[i * 12 + 1], r.samples.sub[i * 12 + 2]};
            const double texel = 0.25;  // 4 texels per metre
            CHECK((r.samples.subPosition(i, k) - centre).length() <= texel * 0.75);
        }
        // the 2x2 / 4x4 grids of the GI: interior texels have all their cells
        if (m == 0xFFu) { CHECK(r.samples.gridMask(i, 2) == 0xFu); CHECK(r.samples.gridMask(i, 4) == 0xFFFFu); }
    }
    CHECK(full > r.samples.count() / 2);  // the interior
    CHECK(partial > 0);                   // the border texels
    CHECK(full + partial + none == r.samples.count());
}

TEST_CASE("super sampling: a hard shadow edge is anti-aliased inside a texel") {
    std::vector<Entity> es{ent("w", "worldspawn", {Brush::makeBox("floor", {-10, -1, -10}, {10, 0, 10}), Brush::makeBox("slab", {-1, 2, -1}, {1, 2.2, 1})}),
                           pointLight({0, 4, 0}, 10, 30)};
    Room r(es, 4);
    const TriangleBvh bvh(r.mesh);
    BakeOptions o;
    o.superSamples = 1;
    const auto one = evaluateDirectCpu(r.samples, r.lights, &bvh, o);
    o.superSamples = 8;
    const auto eight = evaluateDirectCpu(r.samples, r.lights, &bvh, o);
    size_t mid1 = 0, mid8 = 0;
    // find the shadow rim by comparing: a texel whose 8-sample value differs from both 0 and its unshadowed value
    const auto unshadowed = evaluateDirectCpu(r.samples, r.lights);
    for (size_t i = 0; i < r.samples.count(); ++i) {
        if (r.samples.pos[i * 4 + 1] > 0.1f || r.samples.nrm[i * 4 + 1] < 0.9f) continue;
        const double u = unshadowed[i * 4];
        if (u <= 0) continue;
        const double a = one[i * 4] / u, b = eight[i * 4] / u;
        if (a > 0.05 && a < 0.95) ++mid1;
        if (b > 0.05 && b < 0.95) ++mid8;
    }
    CHECK(mid1 == 0);  // a point light makes a binary shadow per ray
    CHECK(mid8 >= 4);  // texels the shadow edge crosses get a fraction
}

TEST_CASE("fine GI grid: entries per texel, unique fine texels, resolve averages back") {
    Room r(closedRoom(), 4);
    for (int scale : {2, 4}) {
        const GiSamples gi = buildGiSamples(r.samples, scale);
        REQUIRE(gi.scale == scale);
        CHECK(gi.s.atlas == r.samples.atlas * scale);
        REQUIRE(gi.begin.size() == r.samples.count() + 1);
        CHECK(gi.count() > r.samples.count() * static_cast<size_t>(scale * scale) / 2);
        CHECK(gi.count() <= r.samples.count() * static_cast<size_t>(scale * scale));
        std::set<uint32_t> seen(gi.s.texel.begin(), gi.s.texel.end());
        CHECK(seen.size() == gi.count());  // no two entries share a fine texel
        for (size_t e = 0; e < gi.count(); ++e) CHECK(gi.owner[e] < r.samples.count());
        std::vector<float> v(gi.count() * 4, 0.f);
        for (size_t e = 0; e < gi.count(); ++e) v[e * 4] = static_cast<float>(gi.owner[e]);  // constant per owner
        const auto back = resolveGi(gi, v);
        for (size_t i = 0; i < r.samples.count(); ++i) REQUIRE(back[i * 4] == doctest::Approx(static_cast<double>(i)));
    }
    CHECK(buildGiSamples(r.samples, 1).count() == 0);
}

TEST_CASE("fine GI grid: gathers the same light as the texel grid (mean over the room)") {
    Room r(closedRoom(), 4);
    const TriangleBvh bvh(r.mesh);
    BakeOptions o;
    o.bounces = 1;
    o.giSamples = 64;
    o.shadows = true;
    o.superSamples = 1;
    std::vector<float> ind1, ind2, grid;
    evaluateGiCpu(r.samples, r.lights, r.mesh, bvh, o, &ind1);
    const GiSamples gi = buildGiSamples(r.samples, 2);
    evaluateGiCpu(r.samples, r.lights, r.mesh, bvh, o, &ind2, &gi, &grid);
    REQUIRE(grid.size() == gi.count() * 4);
    double m1 = 0, m2 = 0;
    for (size_t i = 0; i < r.samples.count(); ++i) { m1 += ind1[i * 4]; m2 += ind2[i * 4]; }
    CHECK(m1 > 0);
    CHECK(m2 == doctest::Approx(m1).epsilon(0.08));
}

TEST_CASE("denoise: OIDN removes the noise of a smooth image (skipped when the library is missing)") {
    const denoise::Info info = denoise::probe();
    if (!info.available) { MESSAGE("OpenImageDenoise not available, skipped: " << info.error); return; }
    const int w = 96, h = 96;
    std::vector<float> clean(static_cast<size_t>(w) * h * 3), noisy;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c) clean[(static_cast<size_t>(y) * w + x) * 3 + c] = 0.2f + 0.8f * static_cast<float>(x) / w * (0.6f + 0.2f * c);
    noisy = clean;
    uint32_t s = 12345;
    for (float& v : noisy) { s = s * 1664525u + 1013904223u; v *= 0.5f + static_cast<float>(s >> 8) / 16777216.f; }
    std::vector<float> out = noisy;
    const denoise::Result r = denoise::filterRGB(out, w, h, denoise::Quality::Fast);
    REQUIRE_MESSAGE(r.ok, r.error);
    auto err = [&](const std::vector<float>& a) { double acc = 0; for (size_t i = 0; i < a.size(); ++i) acc += (a[i] - clean[i]) * (a[i] - clean[i]); return std::sqrt(acc / a.size()); };
    CHECK(err(out) < err(noisy) * 0.5);
}

TEST_CASE("denoise: GI on the texel grid and on 2x / 4x finer grids against a high-sample reference") {
    const denoise::Info info = denoise::probe();
    if (!info.available) { MESSAGE("OpenImageDenoise not available, skipped: " << info.error); return; }
    Room r(closedRoom(), 4);
    const TriangleBvh bvh(r.mesh);
    BakeOptions ref;
    ref.bounces = 1;
    ref.giSamples = 1024;
    ref.superSamples = 1;
    std::vector<float> reference;
    evaluateGiCpu(r.samples, r.lights, r.mesh, bvh, ref, &reference);
    BakeOptions o = ref;
    o.giSamples = 16;
    std::string line = "indirect RMS error vs reference";
    double errRaw = 0, errDn = 0, err2 = 0, err4 = 0, raw2 = 0, raw4 = 0;
    {
        std::vector<float> raw;
        evaluateGiCpu(r.samples, r.lights, r.mesh, bvh, o, &raw);
        std::vector<float> dn = raw;
        const auto d = denoise::denoiseSamples(r.samples, dn, denoise::Quality::High);
        REQUIRE_MESSAGE(d.ok, d.error);
        errRaw = rms(raw, reference);
        errDn = rms(dn, reference);
    }
    for (int scale : {2, 4}) {
        const GiSamples gi = buildGiSamples(r.samples, scale);
        std::vector<float> ind, grid;
        evaluateGiCpu(r.samples, r.lights, r.mesh, bvh, o, &ind, &gi, &grid);
        std::vector<float> g = grid;
        const auto d = denoise::denoiseSamples(gi.s, g, denoise::Quality::High);
        REQUIRE_MESSAGE(d.ok, d.error);
        (scale == 2 ? err2 : err4) = rms(resolveGi(gi, g), reference);
        (scale == 2 ? raw2 : raw4) = rms(ind, reference);  // the same grid averaged without denoising: what the extra rays alone give
    }
    MESSAGE(line << ": raw " << errRaw << ", denoised " << errDn << ", 2x grid averaged only " << raw2 << " / denoised " << err2 << ", 4x grid averaged only " << raw4 << " / denoised " << err4);
    CHECK(errDn < errRaw);
    CHECK(err2 < errRaw);
    CHECK(err4 < errRaw);
}

TEST_CASE("gather noise is not correlated between neighbouring texels (no streaks for the denoiser)") {
    Room r(closedRoom(), 4);
    const TriangleBvh bvh(r.mesh);
    BakeOptions ref;
    ref.bounces = 1; ref.giSamples = 1024; ref.superSamples = 1;
    std::vector<float> reference, raw;
    evaluateGiCpu(r.samples, r.lights, r.mesh, bvh, ref, &reference);
    BakeOptions o = ref;
    o.giSamples = 16;
    evaluateGiCpu(r.samples, r.lights, r.mesh, bvh, o, &raw);
    const size_t A = static_cast<size_t>(r.samples.atlas), per = A * A;
    std::vector<int> at(per * static_cast<size_t>(r.samples.pageCount), -1);
    for (size_t i = 0; i < r.samples.count(); ++i) at[r.samples.texel[i]] = static_cast<int>(i);
    double sxy[2] = {0, 0}, sa[2] = {0, 0}, sb[2] = {0, 0};
    for (size_t i = 0; i < r.samples.count(); ++i) {
        const size_t t = r.samples.texel[i], x = t % A, y = (t / A) % A;
        for (int d = 0; d < 2; ++d) {
            const size_t n = d == 0 ? t + 1 : t + A;
            if ((d == 0 && x + 1 >= A) || (d == 1 && y + 1 >= A) || at[n] < 0) continue;
            const size_t j = static_cast<size_t>(at[n]);
            const double e0 = raw[i * 4] - reference[i * 4], e1 = raw[j * 4] - reference[j * 4];
            sxy[d] += e0 * e1; sa[d] += e0 * e0; sb[d] += e1 * e1;
        }
    }
    // lag-1 correlation of the error; the old R2 lattice start measured 0.81 along x (horizontal streaks)
    CHECK(std::fabs(sxy[0] / std::sqrt(sa[0] * sb[0])) < 0.15);
    CHECK(std::fabs(sxy[1] / std::sqrt(sa[1] * sb[1])) < 0.15);
}
