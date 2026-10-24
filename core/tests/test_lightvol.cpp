#include "core/lightvol.h"
#include "doctest.h"
#include <cmath>

using namespace core;

namespace {
Entity ent(const std::string& id, const std::string& cls, std::vector<Brush> brushes = {}) {
    Entity e;
    e.id = id;
    e.classname = cls;
    e.brushes = std::move(brushes);
    return e;
}

// V(n) of a model, sampled at the given normals.
std::vector<Vec3> sampleModel(const Probe& p, const std::vector<Vec3>& normals) {
    std::vector<Vec3> v;
    for (const Vec3& n : normals) v.push_back(probeLight(p, n));
    return v;
}

// Mean absolute difference between two probes' light over a dense set of normals.
double modelError(const Probe& a, const Probe& b) {
    const auto normals = sphereNormals(400);
    double e = 0;
    for (const Vec3& n : normals) { const Vec3 d = probeLight(a, n) - probeLight(b, n); e += std::fabs(d.x) + std::fabs(d.y) + std::fabs(d.z); }
    return e / static_cast<double>(normals.size() * 3);
}
}  // namespace

TEST_CASE("lightvol: sphereNormals are unit, spread evenly and their mean of max(0, n.l) is 1/4") {
    const auto n = sphereNormals(64);
    REQUIRE(n.size() == 64);
    Vec3 mean;
    double cosMean = 0;
    for (const Vec3& v : n) {
        CHECK(v.length() == doctest::Approx(1.0).epsilon(1e-9));
        mean = mean + v;
        cosMean += std::max(0.0, v.dot(Vec3{0.3, 0.8, -0.5}.normalized()));
    }
    CHECK(mean.length() / 64.0 < 0.02);
    CHECK(cosMean / 64.0 == doctest::Approx(0.25).epsilon(0.03));
}

TEST_CASE("lightvol fit: a single unshadowed delta light and a constant ambient are reproduced") {
    const auto normals = sphereNormals(32);
    SUBCASE("delta light") {
        Probe truth;
        truth.directed = {2.0, 1.5, 1.0};
        truth.dir = Vec3{0.4, 0.7, -0.3}.normalized();
        truth.valid = true;
        const Probe fit = fitProbe(normals, sampleModel(truth, normals));
        CHECK(fit.valid);
        CHECK(fit.dir.dot(truth.dir) > 0.995);
        CHECK(fit.directed.x == doctest::Approx(2.0).epsilon(0.08));
        CHECK(fit.directed.y == doctest::Approx(1.5).epsilon(0.08));
        CHECK(fit.ambient.x < 0.15);  // the quadrature of 32 normals is not exact
        CHECK(modelError(truth, fit) < 0.06);
    }
    SUBCASE("constant ambient") {
        Probe truth;
        truth.ambient = {0.3, 0.5, 0.7};
        const Probe fit = fitProbe(normals, sampleModel(truth, normals));
        CHECK(fit.ambient.x == doctest::Approx(0.3).epsilon(1e-9));
        CHECK(fit.ambient.z == doctest::Approx(0.7).epsilon(1e-9));
        CHECK(fit.directed.length() < 1e-9);
    }
    SUBCASE("ambient plus light") {
        Probe truth;
        truth.ambient = {0.2, 0.2, 0.25};
        truth.directed = {1.2, 1.0, 0.8};
        truth.dir = Vec3{-0.6, 0.5, 0.6}.normalized();
        const Probe fit = fitProbe(normals, sampleModel(truth, normals));
        CHECK(modelError(truth, fit) < 0.06);
    }
}

TEST_CASE("lightvol fit: a mirrored world (light below) points the direction down; a black world is valid and black") {
    const auto normals = sphereNormals(32);
    Probe truth;
    truth.directed = {1, 1, 1};
    truth.dir = {0, -1, 0};
    const Probe fit = fitProbe(normals, sampleModel(truth, normals));
    CHECK(fit.dir.y < -0.99);
    const Probe black = fitProbe(normals, std::vector<Vec3>(normals.size()));
    CHECK(black.valid);
    CHECK(black.ambient.length() == 0);
    CHECK(black.directed.length() == 0);
}

TEST_CASE("lightvol bytes: x2 range compensation, round trip, clamp") {
    CHECK(lightvolToByte(0) == 0);
    CHECK(lightvolToByte(1.0) == 128);   // 1.0 / 2 * 255 = 127.5 -> 128
    CHECK(lightvolToByte(2.0) == 255);
    CHECK(lightvolToByte(5.0) == 255);   // beyond twice the brightness clamps
    CHECK(lightvolToByte(-1.0) == 0);
    CHECK(lightvolFromByte(255) == doctest::Approx(2.0));
    CHECK(lightvolFromByte(0) == 0.0);
    for (int b = 0; b < 256; ++b) CHECK(lightvolToByte(lightvolFromByte(static_cast<uint8_t>(b))) == b);

    Probe p;
    p.ambient = {0.25, 1.0, 1.9};
    p.directed = {0.0, 0.5, 2.0};
    p.dir = Vec3{0.3, -0.6, 0.74}.normalized();
    p.valid = true;
    const Probe q = unpackProbe(packProbe(p));
    CHECK(q.valid);
    CHECK(std::fabs(q.ambient.z - 1.9) < 2.0 / 255.0);
    CHECK(std::fabs(q.directed.y - 0.5) < 2.0 / 255.0);
    CHECK(q.dir.dot(p.dir.normalized()) > 0.998);
    Probe inv = p;
    inv.valid = false;
    CHECK(!unpackProbe(packProbe(inv)).valid);
}

TEST_CASE("lightvol bytes: every direction survives the lat / lng encoding within about a degree") {
    double worst = 1.0;
    for (const Vec3& d : sphereNormals(500)) {
        Probe p;
        p.dir = d;
        p.valid = true;
        worst = std::min(worst, unpackProbe(packProbe(p)).dir.dot(d));
    }
    CHECK(worst > std::cos(2.0 * 3.14159265358979323846 / 180.0));
}

TEST_CASE("lightvol grid: sampling interpolates trilinearly over valid probes and skips invalid ones") {
    LightvolGrid g;
    g.origin = {0, 0, 0};
    g.cell = 1;
    g.nx = g.ny = g.nz = 3;
    g.probes.assign(27, Probe{});
    for (int z = 0; z < 3; ++z)
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 3; ++x) {
                Probe& p = g.probes[g.index(x, y, z)];
                p.valid = true;
                p.ambient = {static_cast<double>(x), 0.5, 0};  // linear in x
                p.directed = {1, 1, 1};
                p.dir = {0, 1, 0};
            }
    const Probe m = g.sample({0.25, 1.0, 1.0});
    CHECK(m.valid);
    CHECK(m.ambient.x == doctest::Approx(0.25).epsilon(1e-9));
    CHECK(m.ambient.y == doctest::Approx(0.5).epsilon(1e-9));
    CHECK(g.sample({1.5, 0.3, 1.9}).ambient.x == doctest::Approx(1.5).epsilon(1e-9));
    CHECK(g.position(g.index(2, 1, 0)).x == 2.0);
    CHECK(g.position(g.index(2, 1, 0)).y == 1.0);

    // An invalid corner takes no weight: between probe x=0 (invalid) and x=1 the value is probe 1's.
    for (int z = 0; z < 3; ++z) for (int y = 0; y < 3; ++y) g.probes[g.index(0, y, z)].valid = false;
    CHECK(g.sample({0.5, 1.0, 1.0}).ambient.x == doctest::Approx(1.0).epsilon(1e-9));
    // Inside an invalid region: the nearest valid probe within 3 cells; none within reach: invalid.
    CHECK(g.sample({0.0, 1.0, 1.0}).valid);
    for (auto& p : g.probes) p.valid = false;
    CHECK(!g.sample({1, 1, 1}).valid);
}

TEST_CASE("lightvol grid: quantise round trips every probe through the bytes, packed() has 9 bytes per probe") {
    LightvolGrid g;
    g.origin = {0, 0, 0};
    g.cell = 1;
    g.nx = 2; g.ny = 1; g.nz = 1;
    g.probes.assign(2, Probe{});
    g.probes[0].valid = true;
    g.probes[0].ambient = {0.4, 0.4, 0.4};
    g.probes[0].directed = {1.1, 0.2, 0.0};
    g.probes[0].dir = {1, 0, 0};
    const auto bytes = g.packed();
    REQUIRE(bytes.size() == 2 * kPackedProbeBytes);
    CHECK(bytes[8] == kProbeValid);
    CHECK(bytes[17] == 0);  // the second probe is not valid
    g.quantise();
    CHECK(std::fabs(g.probes[0].ambient.x - 0.4) < 2.0 / 255);
    CHECK(!g.probes[1].valid);
}

TEST_CASE("lightvol grid: covers the surfaces, respects maxProbes, finds solid probes") {
    Document doc;
    doc.entities = {ent("w", "worldspawn", {Brush::makeBox("floor", {-5, -3, -5}, {5, 0, 5}), Brush::makeBox("ceil", {-5, 3, -5}, {5, 6, 5})})};
    const auto surfaces = collectSurfaces(doc, {});
    LightvolSettings s;
    s.gridSize = 1;
    const LightvolGrid g = buildLightvolGrid(surfaces, nullptr, s);
    CHECK(g.count() == static_cast<size_t>(g.nx) * g.ny * g.nz);
    CHECK(g.origin.x < -5);
    CHECK(g.origin.x + g.nx * g.cell > 5);
    CHECK(g.origin.y < -3);
    CHECK(g.origin.y + g.ny * g.cell > 6);
    s.maxProbes = 200;
    const LightvolGrid coarse = buildLightvolGrid(surfaces, nullptr, s);
    CHECK(coarse.count() <= 200);
    CHECK(coarse.cell > 1);
    s.maxProbes = 0;  // 0 = no limit
    s.gridSize = 0.25;
    const LightvolGrid fine = buildLightvolGrid(surfaces, nullptr, s);
    CHECK(fine.cell == doctest::Approx(0.25));
    CHECK(fine.count() > 200);

    const SolidSet solids = collectSolids(doc, {}, [](const std::string& c) { return c == "worldspawn"; });
    const std::vector<char> solid = solidProbes(g, &solids);
    size_t inFloor = 0, inAir = 0;
    for (size_t i = 0; i < g.count(); ++i) {
        const Vec3 p = g.position(i);
        if (solid[i]) { CHECK(((p.y >= -3 && p.y <= 0) || (p.y >= 3 && p.y <= 6))); ++inFloor; }
        else if (p.y > 0.2 && p.y < 2.8 && std::fabs(p.x) < 4.5 && std::fabs(p.z) < 4.5) ++inAir;
    }
    CHECK(inFloor > 0);
    CHECK(inAir > 0);
}

namespace {
// A closed-ish test room: floor and ceiling slabs and one light.
struct Room {
    Document doc;
    std::vector<LightSurface> surfaces;
    LightScene lights;
    OccluderMesh mesh;
    std::unique_ptr<TriangleBvh> bvh;
    std::shared_ptr<SolidSet> solids;
    explicit Room(std::vector<Entity> es) {
        doc.entities = std::move(es);
        surfaces = collectSurfaces(doc, {});
        lights = collectLights(doc, {});
        const LightingClasses classes = LightingClasses::defaults();
        mesh = buildOccluders(surfaces, classes);
        bvh = std::make_unique<TriangleBvh>(mesh);
        solids = std::make_shared<SolidSet>(collectSolids(doc, {}, [&](const std::string& c) { return classes.castsShadows(c); }));
    }
    LightvolSource source() const {
        LightvolSource s;
        s.lights = &lights;
        s.mesh = &mesh;
        s.bvh = bvh.get();
        return s;
    }
};
Entity directional(const Vec3& angles, double brightness) {
    Entity e = ent("sun", "light_directional");
    e.angles = angles;
    e.keyvalues["brightness"] = std::to_string(brightness);
    e.keyvalues["cast_shadows"] = "0";
    return e;
}
}  // namespace

TEST_CASE("lightvol bake: an unshadowed directional light gives a probe whose direction and strength match the light") {
    Room room({ent("w", "worldspawn", {Brush::makeBox("floor", {-3, -1, -3}, {3, 0, 3})}), directional({0, 0, 0}, 1.5)});
    LightvolSettings cfg;
    cfg.gridSize = 2;
    cfg.useGpu = false;
    cfg.bounces = false;
    LightvolGrid g = buildLightvolGrid(room.surfaces, room.solids.get(), cfg);
    std::vector<char> solid = solidProbes(g, room.solids.get()), baked;
    bakeProbesCpu(g, solid, baked, 0, g.count(), room.source(), cfg);
    // expected direction: towards the light, l = -forward; the shader convention is forward = R * (0, 0, 1)
    const Light& L = room.lights.lights.at(0);
    const Vec3 l = Vec3{-L.forward.x, -L.forward.y, -L.forward.z}.normalized();
    size_t checked = 0;
    for (size_t i = 0; i < g.count(); ++i) {
        if (solid[i]) { CHECK(!g.probes[i].valid); continue; }
        const Probe& p = g.probes[i];
        REQUIRE(p.valid);
        CHECK(p.dir.dot(l) > 0.99);
        CHECK(p.directed.x == doctest::Approx(1.5).epsilon(0.08));
        CHECK(p.ambient.x < 0.2);
        ++checked;
    }
    CHECK(checked > 10);
}

TEST_CASE("lightvol bake: shadows darken the probe under a blocker, a probe in the open keeps its light") {
    Room room({ent("w", "worldspawn", {Brush::makeBox("floor", {-6, -1, -6}, {6, 0, 6}), Brush::makeBox("roof", {-1, 3, -1}, {1, 3.5, 1})}), directional({90, 0, 0}, 1.0)});
    // pitch -90 / +90: the light shines along the Y axis; check which sign comes down by the unshadowed probe far away from the roof
    for (Light& l : room.lights.lights) l.castShadows = true;
    LightvolSettings cfg;
    cfg.gridSize = 1;
    cfg.useGpu = false;
    cfg.bounces = false;
    cfg.areaSamples = 4;
    LightvolGrid g = buildLightvolGrid(room.surfaces, room.solids.get(), cfg);
    std::vector<char> solid = solidProbes(g, room.solids.get()), baked;
    bakeProbesCpu(g, solid, baked, 0, g.count(), room.source(), cfg);
    const Probe open = g.sample({4, 1.5, 4});
    const Probe under = g.sample({0, 1.5, 0});
    const Probe over = g.sample({0, 4.5, 0});
    REQUIRE(open.valid);
    REQUIRE(under.valid);
    REQUIRE(over.valid);
    const double lOpen = open.directed.x + open.ambient.x, lUnder = under.directed.x + under.ambient.x, lOver = over.directed.x + over.ambient.x;
    // the sun is either above (then the roof shades the probe under it) or below; either way exactly one side of the roof is dark
    REQUIRE(open.dir.y > 0.9);  // the sun is above
    CHECK(lOpen > 0.5);
    CHECK(lUnder < 0.25 * lOpen);
    CHECK(lOver > 0.8 * lOpen);
}

TEST_CASE("lightvol bake: indirect light from the bounce source reaches a probe; no atlas, no indirect") {
    // A floor lit by a point light; the lightmap's bounce source holds a constant S on the floor, the probe above it must pick up about S * albedo / 2.
    Room room({ent("w", "worldspawn", {Brush::makeBox("floor", {-8, -1, -8}, {8, 0, 8})})});
    LightmapSettings ls;
    ls.texelsPerMeter = 1;
    const LightmapLayout layout = buildLightmapLayout(room.surfaces, LightingClasses::defaults(), ls);
    room.mesh = buildOccluders(room.surfaces, LightingClasses::defaults(), &layout);
    room.bvh = std::make_unique<TriangleBvh>(room.mesh);
    REQUIRE(room.mesh.hasLightmap());
    const size_t texels = static_cast<size_t>(layout.pageCount) * layout.settings.atlasSize * layout.settings.atlasSize;
    std::vector<float> atlas(texels * 4, 0.f);
    for (size_t t = 0; t < texels; ++t) { atlas[t * 4] = atlas[t * 4 + 1] = atlas[t * 4 + 2] = 1.f; atlas[t * 4 + 3] = 1.f; }
    LightvolSource src = room.source();
    src.atlasS = &atlas;
    src.atlasSize = layout.settings.atlasSize;
    src.pages = layout.pageCount;

    LightvolSettings cfg;
    cfg.gridSize = 4;
    cfg.useGpu = false;
    cfg.albedo = 0.8;
    cfg.giSamples = 64;
    LightvolGrid g = buildLightvolGrid(room.surfaces, room.solids.get(), cfg);
    std::vector<char> solid = solidProbes(g, room.solids.get()), baked;
    bakeProbesCpu(g, solid, baked, 0, g.count(), src, cfg);
    const Probe p = g.sample({0, 2, 0});
    REQUIRE(p.valid);
    // V(n) from the gather for a normal pointing straight down is albedo * S * (fraction of the cosine hemisphere hitting the floor) ~ 0.8 * about 0.97,
    // for the opposite normal (up) it is ~0: the mean over the sphere is ~ 0.8 * 0.97 / 2, with the direction pointing down.
    const double mean = p.ambient.x + p.directed.x / 4.0;
    CHECK(mean == doctest::Approx(0.8 * 0.5 * 0.97).epsilon(0.08));
    CHECK(p.dir.y > 0.99);  // no direct light: light from above (fitProbeDirectFirst), the bounce from below is ambient

    cfg.bounces = false;
    LightvolGrid g2 = buildLightvolGrid(room.surfaces, room.solids.get(), cfg);
    bakeProbesCpu(g2, solid, baked, 0, g2.count(), src, cfg);
    CHECK(g2.sample({0, 2, 0}).ambient.x == 0.0);
    CHECK(g2.sample({0, 2, 0}).directed.x == 0.0);
}

TEST_CASE("lightvol bake: slices bake exactly the probes asked for, baked marks them") {
    Room room({ent("w", "worldspawn", {Brush::makeBox("floor", {-3, -1, -3}, {3, 0, 3})}), directional({0, 0, 0}, 1.0)});
    LightvolSettings cfg;
    cfg.gridSize = 2;
    cfg.useGpu = false;
    cfg.bounces = false;
    LightvolGrid g = buildLightvolGrid(room.surfaces, room.solids.get(), cfg);
    std::vector<char> solid = solidProbes(g, room.solids.get()), baked;
    bakeProbesCpu(g, solid, baked, 3, 5, room.source(), cfg);
    REQUIRE(baked.size() == g.count());
    for (size_t i = 0; i < g.count(); ++i) CHECK((baked[i] != 0) == (i >= 3 && i < 8));
    for (size_t i = 0; i < g.count(); ++i) if (!baked[i]) CHECK(!g.probes[i].valid);
}

TEST_CASE("lightvol entities: non-lightmapped brush entities are sampled at the centre of their bounds, lightmapped ones are not") {
    Document doc;
    doc.entities = {ent("w", "worldspawn", {Brush::makeBox("floor", {-5, -1, -5}, {5, 0, 5})}),
                    ent("door", "func_door", {Brush::makeBox("d", {1, 0, 1}, {3, 2, 2})}),
                    ent("detail", "func_detail", {Brush::makeBox("x", {-3, 0, -3}, {-2, 1, -2})})};
    const auto surfaces = collectSurfaces(doc, {});
    LightvolGrid g;
    g.origin = {-6, -2, -6};
    g.cell = 1;
    g.nx = 13; g.ny = 6; g.nz = 13;
    g.probes.assign(static_cast<size_t>(g.nx) * g.ny * g.nz, Probe{});
    for (auto& p : g.probes) { p.valid = true; p.ambient = {0.5, 0.5, 0.5}; }
    LightingClasses classes = LightingClasses::defaults();
    const auto lit = entityLights(surfaces, classes, g);
    REQUIRE(lit.size() == 1);
    CHECK(lit[0].entityId == "door");
    CHECK(lit[0].centre.x == doctest::Approx(2.0));
    CHECK(lit[0].centre.y == doctest::Approx(1.0));
    CHECK(lit[0].centre.z == doctest::Approx(1.5));
    CHECK(lit[0].probe.valid);
    CHECK(lit[0].probe.ambient.x == doctest::Approx(0.5));
}

namespace {
LightvolGrid bakeAll(Room& room, const LightvolSource& src, LightvolSettings cfg, std::string* device = nullptr, bool* gpu = nullptr, size_t stepSize = 64) {
    LightvolGrid g = buildLightvolGrid(room.surfaces, room.solids.get(), cfg);
    ProbeBaker b(std::move(g), solidProbes(buildLightvolGrid(room.surfaces, room.solids.get(), cfg), room.solids.get()), src, cfg);
    size_t guard = 0;
    while (!b.done() && ++guard < 100000) b.step(stepSize);
    REQUIRE(b.done());
    CHECK(b.baked() == b.total());
    if (device) *device = b.device();
    if (gpu) *gpu = b.onGpu();
    return b.take();
}
}  // namespace

TEST_CASE("lightvol ProbeBaker: ray traced shadows and GI on the GPU session agree with the CPU reference") {
    // a floor lit by a sun, a roof casting a shadow, and a bounce source of S = 1 on every texel of a lightmapped floor
    Room room({ent("w", "worldspawn", {Brush::makeBox("floor", {-8, -1, -8}, {8, 0, 8}), Brush::makeBox("roof", {-1, 3, -1}, {1, 3.5, 1})}), directional({90, 0, 0}, 1.0)});
    for (Light& l : room.lights.lights) l.castShadows = true;
    LightmapSettings ls;
    ls.texelsPerMeter = 1;
    const LightmapLayout layout = buildLightmapLayout(room.surfaces, LightingClasses::defaults(), ls);
    room.mesh = buildOccluders(room.surfaces, LightingClasses::defaults(), &layout);
    room.bvh = std::make_unique<TriangleBvh>(room.mesh);
    const size_t texels = static_cast<size_t>(layout.pageCount) * layout.settings.atlasSize * layout.settings.atlasSize;
    std::vector<float> atlas(texels * 4, 1.f);
    LightvolSource src = room.source();
    src.atlasS = &atlas;
    src.atlasSize = layout.settings.atlasSize;
    src.pages = layout.pageCount;

    LightvolSettings cfg;
    cfg.gridSize = 2;
    cfg.areaSamples = 8;
    cfg.giSamples = 64;
    cfg.albedo = 0.7;
    cfg.useGpu = true;
    bool gpu = false;
    std::string device;
    const LightvolGrid g = bakeAll(room, src, cfg, &device, &gpu);
    MESSAGE("probe evaluator: " << device << (gpu ? " (GPU session)" : " (CPU)"));
    cfg.useGpu = false;
    const LightvolGrid c = bakeAll(room, src, cfg);
    REQUIRE(g.count() == c.count());

    // compare what the probes are for: the light they give for every normal, probe by probe
    const auto normals = sphereNormals(64);
    double err = 0, mag = 0;
    size_t valid = 0;
    for (size_t i = 0; i < g.count(); ++i) {
        CHECK(g.probes[i].valid == c.probes[i].valid);
        if (!g.probes[i].valid || !c.probes[i].valid) continue;
        ++valid;
        for (const Vec3& n : normals) {
            const Vec3 a = probeLight(g.probes[i], n), b = probeLight(c.probes[i], n);
            err += std::fabs(a.x - b.x);
            mag += std::fabs(b.x);
        }
    }
    REQUIRE(valid > 20);
    CHECK(mag > 0);
    CHECK(err / mag < 0.12);  // different random draws: Monte Carlo noise only

    // GI shows up in the light a downward normal gets: a probe above the floor sees the S = 1 floor (half the sphere's mean is about
    // albedo * floor fraction / 2). The direction stays the sun's, so that light from below is ambient (without GI it is ~0, below).
    for (const LightvolGrid* gr : {&g, &c}) {
        const Probe p = gr->sample({5, 1.5, 5});
        REQUIRE(p.valid);
        CHECK(p.dir.y > 0.9);
        CHECK(probeLight(p, {0, -1, 0}).x > 0.2);
    }

    // direct light only: the shadow of the roof and the sun above are seen with either evaluator
    cfg.bounces = false;
    for (bool useGpu : {true, false}) {
        cfg.useGpu = useGpu;
        const LightvolGrid d = bakeAll(room, src, cfg);
        const Probe open = d.sample({5, 1.5, 5}), under = d.sample({0, 1.5, 0});
        REQUIRE(open.valid);
        REQUIRE(under.valid);
        CHECK(open.dir.y > 0.8);
        CHECK(open.directed.x > 0.8);
        CHECK(under.directed.x < 0.35 * open.directed.x);
        CHECK(probeLight(open, {0, -1, 0}).x < 0.05);  // nothing bounces without GI
    }
}

TEST_CASE("lightvol ProbeBaker: steps advance by the asked amount, finished grid is complete, no lights = black but valid probes") {
    Room room({ent("w", "worldspawn", {Brush::makeBox("floor", {-3, -1, -3}, {3, 0, 3})})});
    LightvolSettings cfg;
    cfg.gridSize = 2;
    cfg.useGpu = false;
    LightvolGrid g = buildLightvolGrid(room.surfaces, room.solids.get(), cfg);
    const size_t n = g.count();
    std::vector<char> solid = solidProbes(g, room.solids.get());
    ProbeBaker b(std::move(g), solid, room.source(), cfg);
    CHECK(!b.onGpu());
    CHECK(b.device() == "cpu");
    CHECK(b.step(5) == 5);
    CHECK(b.baked() == 5);
    CHECK(!b.done());
    while (!b.done()) b.step(7);
    CHECK(b.step(7) == 0);
    const LightvolGrid done = b.take();
    REQUIRE(done.count() == n);
    for (size_t i = 0; i < n; ++i) if (!solid[i]) { CHECK(done.probes[i].valid); CHECK(done.probes[i].ambient.length() == 0); }
}

TEST_CASE("lightvol: one direction per point for every style layer (REQUIREMENTS §27 G4)") {
    Probe a, b, c;
    a.valid = b.valid = c.valid = true;
    a.ambient = {0.1, 0.1, 0.1}; a.directed = {1, 1, 1}; a.dir = {0, 1, 0};
    b.ambient = {0.2, 0, 0};     b.directed = {0.5, 0.5, 0.5}; b.dir = {1, 0, 0};
    c.valid = false; c.dir = {0, 0, 1};
    const double meanA = a.ambient.x + a.directed.x / 4, meanB = b.ambient.x + b.directed.x / 4;
    shareDirection({&a, &b, &c});
    CHECK((a.dir - b.dir).length() < 1e-12);                       // shared
    CHECK(a.dir.length() == doctest::Approx(1.0));
    CHECK(a.dir.y > a.dir.x);                                      // towards the brighter layer
    CHECK(a.ambient.x + a.directed.x / 4 == doctest::Approx(meanA));  // every layer keeps its mean light
    CHECK(b.ambient.x + b.directed.x / 4 == doctest::Approx(meanB));
    CHECK(a.directed.x <= 1.0);
    CHECK(c.dir.z == 1.0);                                         // invalid layers untouched
    // a single layer is unchanged
    Probe one = a;
    const Probe before = one;
    shareDirection({&one});
    CHECK((one.dir - before.dir).length() < 1e-12);
    CHECK(one.directed.x == doctest::Approx(before.directed.x));
    // grids: every point of every layer ends with the same direction
    std::map<int, LightvolGrid> grids;
    for (int s : {0, 3}) {
        LightvolGrid g;
        g.nx = 2; g.ny = 1; g.nz = 1;
        g.probes.resize(2);
        for (auto& p : g.probes) { p.valid = true; p.directed = {1, 1, 1}; p.dir = s == 0 ? Vec3{0, 1, 0} : Vec3{0, 0, 1}; }
        grids[s] = g;
    }
    shareProbeDirections(grids);
    for (size_t i = 0; i < 2; ++i) CHECK((grids[0].probes[i].dir - grids[3].probes[i].dir).length() < 1e-12);
}

namespace {
// A floor lit by a sun from above, a roof (2 x 2 m at 3 m) shading the floor under it, and a bounce source of S = 1 on every floor texel: under the
// roof the only light is bounce light from below.
struct ShadedRoom {
    Room room{{ent("w", "worldspawn", {Brush::makeBox("floor", {-8, -1, -8}, {8, 0, 8}), Brush::makeBox("roof", {-2, 3, -2}, {2, 3.5, 2})}), directional({90, 0, 0}, 1.0)}};
    LightmapLayout layout;
    std::vector<float> atlas;
    LightvolSource src;
    ShadedRoom() {
        for (Light& l : room.lights.lights) l.castShadows = true;
        LightmapSettings ls;
        ls.texelsPerMeter = 1;
        layout = buildLightmapLayout(room.surfaces, LightingClasses::defaults(), ls);
        room.mesh = buildOccluders(room.surfaces, LightingClasses::defaults(), &layout);
        room.bvh = std::make_unique<TriangleBvh>(room.mesh);
        atlas.assign(static_cast<size_t>(layout.pageCount) * layout.settings.atlasSize * layout.settings.atlasSize * 4, 1.f);
        src = room.source();
        src.atlasS = &atlas;
        src.atlasSize = layout.settings.atlasSize;
        src.pages = layout.pageCount;
    }
};
}  // namespace

TEST_CASE("lightvol directions: direct light decides before bounce light; with (nearly) no direct light, light comes from above") {
    const auto normals = sphereNormals(64);
    auto model = [&](const Vec3& dir, double e) { Probe p; p.valid = true; p.directed = {e, e, e}; p.dir = dir.normalized(); return sampleModel(p, normals); };
    auto add = [](std::vector<Vec3> a, const std::vector<Vec3>& b) { for (size_t i = 0; i < a.size(); ++i) a[i] = a[i] + b[i]; return a; };
    const Vec3 sun = Vec3{0.3, 1, 0.2}.normalized();

    // a sunlit probe over a bright floor: more bounce light from below than sun from above. The total light's moment would point down
    // (the engine would cast shadows upwards); the direct light keeps the direction, and its strength stays directed.
    const auto direct = model(sun, 1.0), bounce = model({0, -1, 0}, 2.0), total = add(direct, bounce);
    REQUIRE(fitProbe(normals, total).dir.y < 0);
    const ProbeMoments m = probeMoments(normals, total, &direct);
    const Probe p = fitProbeDirectFirst(m, 0.25);
    CHECK(p.dir.dot(sun) > 0.9);
    CHECK(p.dir.y > 0.5);
    CHECK(p.directed.x == doctest::Approx(1.0).epsilon(0.1));
    CHECK(p.ambient.x + p.directed.x / 4 == doctest::Approx(m.mean.x));  // the mean light is kept: the bounce is ambient
    // bounce counts with its weight: at weight 1 it is the total light again
    CHECK(fitProbeDirectFirst(m, 1.0).dir.dot(fitProbe(normals, total).dir) > 0.999);

    // in the shade, bounce light only: light from above, all of it ambient
    const ProbeMoments shade = probeMoments(normals, bounce, nullptr);
    const Probe q = fitProbeDirectFirst(shade, 0.25);
    CHECK(q.dir.y == doctest::Approx(1.0));
    CHECK(q.directed.length() < 1e-9);
    CHECK(q.ambient.x == doctest::Approx(shade.mean.x));

    // a trace of direct light (1% of the mean) does not steer it either; a fair share (30%) does
    const auto faint = model({1, 0, 0}, 0.04), fair = model({1, 0, 0}, 2.0);
    CHECK(fitProbeDirectFirst(probeMoments(normals, add(faint, bounce), &faint), 0.25).dir.y == doctest::Approx(1.0));
    CHECK(fitProbeDirectFirst(probeMoments(normals, add(fair, bounce), &fair), 0.25).dir.x > 0.9);
}

TEST_CASE("lightvol directions: under a roof next to a sunlit floor the direction stays the sun's side, never from below") {
    // The engine casts stencil shadows along the probe direction: the bounce off the floor under the roof comes from below.
    ShadedRoom s;
    LightvolSettings cfg;
    cfg.gridSize = 1;
    cfg.areaSamples = 4;
    cfg.giSamples = 32;
    cfg.useGpu = false;
    LightvolGrid g = buildLightvolGrid(s.room.surfaces, s.room.solids.get(), cfg);
    std::vector<char> solid = solidProbes(g, s.room.solids.get()), baked;
    bakeProbesCpu(g, solid, baked, 0, g.count(), s.src, cfg);
    const Probe under = g.sample({0, 1.5, 0}), open = g.sample({6, 1.5, 6});
    REQUIRE(under.valid);
    REQUIRE(open.valid);
    CHECK(under.dir.y > 0.9);
    CHECK(open.dir.y > 0.9);
    CHECK(open.directed.x > 0.8);
    CHECK(under.directed.x < 0.35 * open.directed.x);  // the shadow stays a shadow: the bounce from below is ambient
    size_t air = 0;
    for (size_t i = 0; i < g.count(); ++i) {
        const Vec3 p = g.position(i);
        if (!g.probes[i].valid || p.y < 0.5 || p.y > 2.5) continue;
        CHECK(g.probes[i].dir.y > 0.5);  // nowhere in the air does the light come from below
        ++air;
    }
    CHECK(air > 100);
}

TEST_CASE("lightvol directions: where no direct light reaches at all, light comes from above (ProbeBaker)") {
    // no light at all, only bounce light from the floor (S = 1): every probe would point down
    ShadedRoom s;
    s.room.lights.lights.clear();
    LightvolSettings cfg;
    cfg.gridSize = 2;
    cfg.giSamples = 32;
    cfg.useGpu = false;
    const LightvolGrid g = bakeAll(s.room, s.src, cfg);
    size_t lit = 0;
    for (size_t i = 0; i < g.count(); ++i) {
        const Probe& p = g.probes[i];
        if (!p.valid) continue;
        CHECK(p.dir.y == doctest::Approx(1.0));
        const Vec3 at = g.position(i);
        // above the floor and away from the roof (whose lit underside does send light down) all the light comes from below: none of it is directed
        if (at.y > 0.5 && (std::fabs(at.x) > 2.5 || std::fabs(at.z) > 2.5)) CHECK(p.directed.length() < 1e-9);
        if (p.ambient.x > 0.1) ++lit;
    }
    CHECK(lit > 10);  // the bounce light is still there, as ambient
}

TEST_CASE("lightvol fill: in-wall probes take their neighbours' light ring by ring, an all-invalid grid stays as it is") {
    LightvolGrid g;
    g.origin = {0, 0, 0};
    g.cell = 1;
    g.nx = 4; g.ny = 1; g.nz = 1;
    g.probes.assign(4, Probe{});
    g.probes[0].valid = true; g.probes[0].ambient = {0.4, 0.4, 0.4}; g.probes[0].directed = {1, 1, 1}; g.probes[0].dir = {0, 1, 0};
    g.probes[3].valid = true; g.probes[3].ambient = {0.2, 0.2, 0.2}; g.probes[3].directed = {0, 0, 0}; g.probes[3].dir = {1, 0, 0};
    fillInvalidProbes(g);
    for (const Probe& p : g.probes) CHECK(p.valid);
    CHECK(g.probes[1].ambient.x == doctest::Approx(0.4));  // only probe 0 was next to it in the first ring
    CHECK(g.probes[2].ambient.x == doctest::Approx(0.2));
    CHECK(g.probes[1].dir.y == doctest::Approx(1.0));
    LightvolGrid none = g;
    for (Probe& p : none.probes) p = Probe{};
    fillInvalidProbes(none);
    for (const Probe& p : none.probes) CHECK(!p.valid);
}
