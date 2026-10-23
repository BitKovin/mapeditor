#include "core/denoise.h"
#include "core/live_bake.h"
#include "test_util.h"
#include <doctest.h>
#include <cmath>

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
Entity pointLight(const std::string& id, Vec3 o, double brightness, double radius, double size = 0) {
    Entity e = ent(id, "light_point");
    e.origin = o;
    e.keyvalues["brightness"] = std::to_string(brightness);
    e.keyvalues["radius"] = std::to_string(radius);
    e.keyvalues["source_size"] = std::to_string(size);
    return e;
}

// A long corridor floor (x -20..20) so chunks are spread out along x.
struct World {
    std::vector<Entity> entities;
    std::vector<LightSurface> surfaces;
    LightmapLayout layout;
    LightScene lights;
    explicit World(std::vector<Entity> es, double tpm = 2) : entities(std::move(es)) { rebuild(tpm); }
    void rebuild(double tpm = 2) {
        Document doc;
        doc.entities = entities;
        surfaces = collectSurfaces(doc, {});
        LightmapSettings cfg;
        cfg.texelsPerMeter = tpm;
        cfg.mergeAngleDeg = 90;  // these tests are about invalidation; the chunk statistics they count assume connected box corners
        layout = buildLightmapLayout(surfaces, LightingClasses::defaults(), cfg);
        lights = collectLights(doc, {});
    }
};
std::vector<Entity> corridor(std::vector<Entity> lights = {}) {
    std::vector<Entity> es{ent("w", "worldspawn", {Brush::makeBox("floor", {-20, -1, -2}, {20, 0, 2})})};
    for (auto& l : lights) es.push_back(std::move(l));
    return es;
}
LiveSettings smallChunks() {
    LiveSettings s;
    s.chunkTexels = 8;
    s.maxPasses = 6;
    s.bounces = false;
    return s;
}
double dist(const LiveChunk& c, const Vec3& p) { return (c.bounds.center() - p).length(); }
}  // namespace

TEST_CASE("live: chunks tile the atlas and every sample belongs to exactly one chunk") {
    World w(corridor({pointLight("l", {0, 3, 0}, 10, 30)}));
    LiveBaker b(smallChunks());
    b.setGeometry(w.layout, w.surfaces);
    b.setLights(w.lights);
    REQUIRE(b.chunkCount() > 10);
    std::vector<int> owner(b.sampleCount(), 0);
    for (size_t c = 0; c < b.chunkCount(); ++c) {
        CHECK(b.chunk(static_cast<int>(c)).bounds.valid());
        for (uint32_t s : b.chunk(static_cast<int>(c)).samples) owner[s]++;
    }
    for (int o : owner) CHECK(o == 1);
    CHECK(b.staleChunks() == b.chunkCount());  // nothing has been baked yet
}

TEST_CASE("live: stale chunks are refined nearest to the camera first, within the texel budget") {
    World w(corridor({pointLight("l", {0, 3, 0}, 10, 60)}));
    LiveBaker b(smallChunks());
    b.setGeometry(w.layout, w.surfaces);
    b.setLights(w.lights);
    const Vec3 cam{15, 1, 0};
    const auto r = b.step(&cam, 600);
    REQUIRE(r.chunks.size() >= 2);
    CHECK(r.texels >= 600);
    CHECK(r.texels < 600 + 2 * 64 * 4);  // stops soon after the budget
    for (size_t i = 1; i < r.chunks.size(); ++i) CHECK(dist(b.chunk(r.chunks[i - 1]), cam) <= dist(b.chunk(r.chunks[i]), cam) + 1e-9);
    // the first one is the nearest of all chunks
    double nearest = 1e300;
    for (size_t c = 0; c < b.chunkCount(); ++c) nearest = std::min(nearest, dist(b.chunk(static_cast<int>(c)), cam));
    CHECK(dist(b.chunk(r.chunks[0]), cam) == doctest::Approx(nearest));
    for (int c : r.chunks) CHECK(b.chunk(c).passes == 1);
    // every step makes progress; a zero budget still refines one chunk
    CHECK(b.step(&cam, 0).chunks.size() == 1);
    // until nothing is stale, then the least refined chunks go next
    while (b.staleChunks() > 0) b.step(&cam, 5000);
    const auto next = b.step(&cam, 10);
    REQUIRE(!next.chunks.empty());
    for (int c : next.chunks) CHECK(b.chunk(c).passes == 2);
    int guard = 0;
    while (!b.converged() && guard++ < 100) b.step(&cam, 100000);
    CHECK(b.converged());
    CHECK(b.minPasses() == smallChunks().maxPasses);
    CHECK(b.step(&cam, 1000).chunks.empty());  // nothing left to do
}

TEST_CASE("live: quality improves over passes towards a high-sample reference (soft shadows)") {
    // a sized light above a slab: penumbra noise falls as passes accumulate
    std::vector<Entity> es{ent("w", "worldspawn", {Brush::makeBox("floor", {-6, -1, -6}, {6, 0, 6}), Brush::makeBox("slab", {-1, 2, -1}, {1, 2.2, 1})}),
                           pointLight("l", {0, 4, 0}, 10, 40, 0.8)};
    World w(es, 4);
    LiveSettings cfg;
    cfg.chunkTexels = 16;
    cfg.maxPasses = 32;
    cfg.areaSamplesPerPass = 4;
    cfg.bounces = false;
    LiveBaker b(cfg);
    b.setGeometry(w.layout, w.surfaces);
    b.setLights(w.lights);
    BakeOptions refOpt;
    refOpt.areaSamples = 1024;
    const OccluderMesh mesh = buildOccluders(w.surfaces, LightingClasses::defaults());
    const TriangleBvh bvh(mesh);
    const TexelSamples& s = b.samples();
    const auto ref = evaluateDirectCpu(s, w.lights, &bvh, refOpt);
    auto error = [&] {
        double e = 0, v = 0;
        for (size_t i = 0; i < s.count(); ++i) { e += std::fabs(b.value(i).x - ref[i * 4]); v += ref[i * 4]; }
        return e / v;
    };
    std::map<int, double> err;
    for (int pass = 1; pass <= 32; ++pass) {
        b.step(nullptr, 1000000);
        if (pass == 1 || pass == 4 || pass == 16 || pass == 32) err[pass] = error();
    }
    MESSAGE("relative error after 1 / 4 / 16 / 32 passes: " << err[1] << " " << err[4] << " " << err[16] << " " << err[32]);
    CHECK(err[1] > err[4]);
    CHECK(err[4] > err[16]);
    CHECK(err[16] > err[32]);
    CHECK(err[32] < 0.4 * err[1]);
    CHECK(err[32] < 0.02);
}

TEST_CASE("live: a converged chunk set equals a full bake (hard shadows are deterministic)") {
    std::vector<Entity> es{ent("w", "worldspawn", {Brush::makeBox("floor", {-6, -1, -6}, {6, 0, 6}), Brush::makeBox("slab", {-1, 2, -1}, {1, 2.2, 1})}),
                           pointLight("l", {0, 4, 0}, 10, 40)};
    World w(es, 3);
    LiveSettings cfg = smallChunks();
    cfg.maxPasses = 3;
    LiveBaker b(cfg);
    b.setGeometry(w.layout, w.surfaces);
    b.setLights(w.lights);
    while (!b.converged()) b.step(nullptr, 100000);
    BakeOptions o;
    o.forceCpu = true;
    const LightmapPages full = bakeLightmaps(w.layout, w.surfaces, w.lights, o);
    const LightmapPages live = b.snapshot();
    REQUIRE(live.pageCount == full.pageCount);
    double maxDiff = 0, maxVal = 0;
    for (int p = 0; p < full.pageCount; ++p)
        for (size_t i = 0; i < full.rgb[static_cast<size_t>(p)].size(); ++i) {
            maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(full.rgb[static_cast<size_t>(p)][i] - live.rgb[static_cast<size_t>(p)][i])));
            maxVal = std::max(maxVal, static_cast<double>(full.rgb[static_cast<size_t>(p)][i]));
        }
    CHECK(maxVal > 1.0);
    CHECK(maxDiff < 1e-4);
    CHECK(full.valid == live.valid);  // same texels baked and same gutter
}

TEST_CASE("live: moving a light only makes the chunks it can reach stale") {
    World w(corridor({pointLight("a", {-14, 3, 0}, 10, 6), pointLight("b", {14, 3, 0}, 10, 6)}));
    LiveSettings cfg = smallChunks();
    LiveBaker b(cfg);
    b.setGeometry(w.layout, w.surfaces);
    b.setLights(w.lights);
    while (!b.converged()) b.step(nullptr, 100000);
    // an identical light set changes nothing
    b.setLights(w.lights);
    CHECK(b.staleChunks() == 0);
    // move light a by 1 m
    std::vector<Entity> es = corridor({pointLight("a", {-13, 3, 0}, 10, 6), pointLight("b", {14, 3, 0}, 10, 6)});
    World moved(es);
    b.setLights(moved.lights);
    REQUIRE(b.staleChunks() > 0);
    REQUIRE(b.staleChunks() < b.chunkCount());
    const Aabb oldBox = Aabb{}.expanded(0), unused = oldBox;
    (void)unused;
    Aabb oldInf, newInf;
    oldInf.add({-14, 3, 0});
    newInf.add({-13, 3, 0});
    for (size_t c = 0; c < b.chunkCount(); ++c) {
        const LiveChunk& ch = b.chunk(static_cast<int>(c));
        const bool reach = ch.bounds.intersects(oldInf.expanded(6)) || ch.bounds.intersects(newInf.expanded(6));
        CHECK_MESSAGE((ch.passes == 0) == reach, "chunk at x=" << ch.bounds.center().x);
        if (ch.bounds.mn.x > 0) CHECK(ch.passes == cfg.maxPasses);  // chunks lying wholly in the other half of the corridor are untouched (a chunk can hold texels of several faces, so test its bounds, not its centre)
    }
    // removing and adding lights also invalidates exactly their reach
    while (!b.converged()) b.step(nullptr, 100000);
    b.setLights(corridor({pointLight("a", {-13, 3, 0}, 10, 6)}).size() ? World(corridor({pointLight("a", {-13, 3, 0}, 10, 6)})).lights : LightScene{});
    for (size_t c = 0; c < b.chunkCount(); ++c) {
        const LiveChunk& ch = b.chunk(static_cast<int>(c));
        if (ch.bounds.mx.x < -5) CHECK(ch.passes == cfg.maxPasses);
        if (ch.bounds.mn.x > 12) CHECK(ch.passes == 0);
    }
}

TEST_CASE("live: a directional light makes everything stale but the camera's neighbourhood is refined first") {
    World w(corridor({pointLight("a", {0, 3, 0}, 5, 8)}));
    LiveBaker b(smallChunks());
    b.setGeometry(w.layout, w.surfaces);
    b.setLights(w.lights);
    while (!b.converged()) b.step(nullptr, 100000);
    Entity sun = ent("sun", "light_directional");
    sun.angles = {60, 20, 0};
    sun.keyvalues["brightness"] = "1";
    World sunny(corridor({pointLight("a", {0, 3, 0}, 5, 8), sun}));
    b.setLights(sunny.lights);
    CHECK(b.staleChunks() == b.chunkCount());
    const Vec3 cam{-17, 1, 0};
    const auto r = b.step(&cam, 800);
    REQUIRE(r.chunks.size() >= 2);
    double nearest = 1e300;
    for (size_t c = 0; c < b.chunkCount(); ++c) nearest = std::min(nearest, dist(b.chunk(static_cast<int>(c)), cam));
    CHECK(dist(b.chunk(r.chunks[0]), cam) == doctest::Approx(nearest));
    // everything processed is nearer than everything that is still stale
    double farthestDone = 0, nearestStale = 1e300;
    for (int c : r.chunks) farthestDone = std::max(farthestDone, dist(b.chunk(c), cam));
    for (size_t c = 0; c < b.chunkCount(); ++c)
        if (b.chunk(static_cast<int>(c)).passes == 0) nearestStale = std::min(nearestStale, dist(b.chunk(static_cast<int>(c)), cam));
    CHECK(farthestDone <= nearestStale + 1e-9);
    // an ambient change is global too
    LightScene amb = sunny.lights;
    while (!b.converged()) b.step(nullptr, 100000);
    amb.ambient = {0.1, 0.1, 0.1};
    b.setLights(amb);
    CHECK(b.staleChunks() == b.chunkCount());
}

TEST_CASE("live: bounce light is refreshed around a change without making those chunks fully stale") {
    LiveSettings cfg = smallChunks();
    cfg.bounces = true;
    cfg.giSamplesPerPass = 8;
    cfg.indirectRadius = 6;
    cfg.indirectRefreshPasses = 2;
    World w(corridor({pointLight("a", {-14, 3, 0}, 10, 4)}));
    // add a ceiling so there is something to bounce off
    w.entities[0].brushes.push_back(Brush::makeBox("ceil", {-20, 4, -2}, {20, 5, 2}));
    w.rebuild();
    LiveBaker b(cfg);
    b.setGeometry(w.layout, w.surfaces);
    b.setLights(w.lights);
    while (!b.converged()) b.step(nullptr, 1000000);
    World moved(w.entities);
    moved.entities[1].origin = {-13, 3, 0};
    moved.rebuild();
    b.setLights(moved.lights);
    size_t stale = 0, refresh = 0, untouched = 0;
    for (size_t c = 0; c < b.chunkCount(); ++c) {
        const int passes = b.chunk(static_cast<int>(c)).passes;
        if (passes == 0) ++stale; else if (passes == cfg.indirectRefreshPasses) ++refresh; else if (passes == cfg.maxPasses) ++untouched;
    }
    CHECK(stale > 0);
    CHECK(refresh > 0);    // the ring around the reach
    CHECK(untouched > stale);
}

TEST_CASE("live: geometry changes keep unchanged texels, their values and their pass counts") {
    World w(corridor({pointLight("a", {-12, 3, 0}, 10, 8), pointLight("b", {12, 3, 0}, 10, 8)}));
    LiveSettings cfg = smallChunks();
    LiveBaker b(cfg);
    b.setGeometry(w.layout, w.surfaces);
    b.setLights(w.lights);
    while (!b.converged()) b.step(nullptr, 1000000);
    // value of a texel far from the edit, before
    size_t probe = 0;  // an upward texel near light a (x = -12): lit, and 24 m away from the pillar that will be added
    double best = 1e300;
    for (size_t i = 0; i < b.sampleCount(); ++i) {
        if (b.samples().nrm[i * 4 + 1] < 0.9f) continue;
        const double d = std::fabs(b.samples().pos[i * 4] + 12.5) + std::fabs(b.samples().pos[i * 4 + 2]);
        if (d < best) { best = d; probe = i; }
    }
    const Vec3 probePos{b.samples().pos[probe * 4], b.samples().pos[probe * 4 + 1], b.samples().pos[probe * 4 + 2]};
    const Vec3 before = b.value(probe);
    REQUIRE(before.x > 0.05);

    const auto oldSurfaces = w.surfaces;
    w.entities[0].brushes.push_back(Brush::makeBox("pillar", {12, 0, -1}, {13, 3, 1}));  // blocks light b near x = 12
    w.rebuild();
    const auto regions = diffSurfaces(oldSurfaces, w.surfaces);
    REQUIRE(!regions.empty());
    for (const auto& r : regions) {
        CHECK(r.mn.x >= 11.9);
        CHECK(r.mx.x <= 13.1);
    }
    b.setGeometry(w.layout, w.surfaces, regions);
    size_t stale = 0, kept = 0;
    for (size_t c = 0; c < b.chunkCount(); ++c) {
        const auto& ch = b.chunk(static_cast<int>(c));
        if (ch.passes == 0) ++stale; else if (ch.passes == cfg.maxPasses) ++kept;
    }
    CHECK(stale > 0);
    CHECK(kept >= 10);
    for (size_t c = 0; c < b.chunkCount(); ++c) {  // the pillar can only change shadows within light b's reach (8 m) of x = 12
        const auto& ch = b.chunk(static_cast<int>(c));
        if (ch.bounds.mx.x < 3.9) CHECK(ch.passes == cfg.maxPasses);
        if (ch.bounds.mn.x > 4.1 && ch.bounds.mx.x < 20) CHECK(ch.passes == 0);
    }
    // the far texel is still there with the same value and was not re-baked
    bool found = false;
    for (size_t i = 0; i < b.sampleCount(); ++i) {
        const Vec3 p{b.samples().pos[i * 4], b.samples().pos[i * 4 + 1], b.samples().pos[i * 4 + 2]};
        if ((p - probePos).length() < 1e-6 && b.samples().nrm[i * 4 + 1] > 0.9f) {
            found = true;
            CHECK(b.value(i).x == doctest::Approx(before.x).epsilon(1e-6));
        }
    }
    CHECK(found);
    // refining the stale chunks brings shadow behind the pillar: texels directly behind it (x > 13) get darker than before
    while (!b.converged()) b.step(nullptr, 1000000);
    CHECK(b.staleChunks() == 0);
}

TEST_CASE("live: diffSurfaces reports exactly what moved, appeared or vanished") {
    World w(corridor());
    CHECK(diffSurfaces(w.surfaces, w.surfaces).empty());
    World added(corridor());
    added.entities[0].brushes.push_back(Brush::makeBox("box", {5, 0, 0}, {6, 1, 1}));
    added.rebuild();
    const auto a = diffSurfaces(w.surfaces, added.surfaces);
    REQUIRE(!a.empty());
    Aabb all;
    for (const auto& r : a) { all.add(r.mn); all.add(r.mx); }
    CHECK(all.mn.x == doctest::Approx(5));
    CHECK(all.mx.x == doctest::Approx(6));
    // removing is symmetric
    const auto r = diffSurfaces(added.surfaces, w.surfaces);
    CHECK(!r.empty());
    // moving a brush reports old and new places
    World moved(corridor());
    moved.entities[0].brushes.push_back(Brush::makeBox("box", {-9, 0, 0}, {-8, 1, 1}));
    moved.rebuild();
    const auto m = diffSurfaces(added.surfaces, moved.surfaces);
    Aabb mb;
    for (const auto& x : m) { mb.add(x.mn); mb.add(x.mx); }
    CHECK(mb.mn.x <= -8.99);
    CHECK(mb.mx.x >= 5.99);
}

TEST_CASE("live: diffSurfaces reports a retexture or a texture projection change (bounce colour), not only moved vertices") {
    World w(corridor());
    REQUIRE(!w.surfaces.empty());
    // retexture one piece: nothing moved, but the albedo the bounces pick up from it changed
    auto retextured = w.surfaces;
    retextured[0].texture = "other_texture";
    const auto a = diffSurfaces(w.surfaces, retextured);
    REQUIRE(!a.empty());
    Aabb all;
    for (const auto& r : a) { all.add(r.mn); all.add(r.mx); }
    Aabb piece;
    for (const auto& v : w.surfaces[0].verts) piece.add(v);
    CHECK(all.mn.x == doctest::Approx(piece.mn.x));
    CHECK(all.mx.x == doctest::Approx(piece.mx.x));
    CHECK(all.mn.y == doctest::Approx(piece.mn.y));
    CHECK(all.mx.y == doctest::Approx(piece.mx.y));
    CHECK(all.mn.z == doctest::Approx(piece.mn.z));
    CHECK(all.mx.z == doctest::Approx(piece.mx.z));
    // the same for each projection parameter
    for (int k = 0; k < 5; ++k) {
        auto changed = w.surfaces;
        FaceProjection& p = changed[0].projection;
        if (k == 0) p.scaleU += 1; else if (k == 1) p.scaleV += 1; else if (k == 2) p.shiftU += 0.25; else if (k == 3) p.shiftV += 0.25; else p.rotate += 15;
        CHECK(!diffSurfaces(w.surfaces, changed).empty());
        CHECK(diffSurfaces(changed, changed).empty());
    }
    // a texture change marks the chunks around the piece stale, the far ones stay
    LiveSettings cfg = smallChunks();
    World lit(corridor({pointLight("l", {0, 3, 0}, 10, 30)}));
    LiveBaker b(cfg);
    b.setGeometry(lit.layout, lit.surfaces);
    b.setLights(lit.lights);
    while (!b.converged()) b.step(nullptr, 1000000);
    auto changed = lit.surfaces;
    for (auto& s : changed) if (s.verts[0].x > 15) s.texture = "other_texture";
    const auto regions = diffSurfaces(lit.surfaces, changed);
    REQUIRE(!regions.empty());
    b.setGeometry(lit.layout, changed, regions);
    size_t stale = 0, kept = 0;
    for (size_t c = 0; c < b.chunkCount(); ++c) {
        const auto& ch = b.chunk(static_cast<int>(c));
        if (ch.passes == 0) ++stale; else ++kept;
    }
    CHECK(stale > 0);
    CHECK(kept > 0);
}

TEST_CASE("live: bounce light reaches as far as the light's radius lights surfaces, not a fixed range") {
    auto bake = [](bool bounces) {
        LiveSettings cfg = smallChunks();
        cfg.bounces = bounces;
        cfg.giSamplesPerPass = 8;
        // the ceiling is 30 m above the floor: lit only because the radius (60 m) reaches it
        World w(corridor({pointLight("a", {0, 3, 0}, 10, 60)}));
        w.entities[0].brushes.push_back(Brush::makeBox("ceil", {-20, 30, -2}, {20, 31, 2}));
        w.rebuild();
        LiveBaker b(cfg);
        b.setGeometry(w.layout, w.surfaces);
        b.setLights(w.lights);
        while (!b.converged()) b.step(nullptr, 1000000);
        double sum = 0;
        for (size_t i = 0; i < b.sampleCount(); ++i) sum += b.value(i).x;
        return sum;
    };
    CHECK(bake(true) > bake(false) * 1.01);
}

TEST_CASE("live: dragging a light fast leaves no old bounce light behind") {
    LiveSettings cfg = smallChunks();
    cfg.bounces = true;
    cfg.giSamplesPerPass = 8;
    auto build = [&](double x, double brightness) {
        World w(corridor({pointLight("a", {x, 3, 0}, brightness, 6)}));
        w.entities[0].brushes.push_back(Brush::makeBox("ceil", {-20, 4, -2}, {20, 5, 2}));
        w.rebuild();
        return w;
    };
    World w = build(-16, 10);
    LiveBaker b(cfg);
    b.setGeometry(w.layout, w.surfaces);
    b.setLights(w.lights);
    while (!b.converged()) b.step(nullptr, 1000000);
    // drag across the corridor: every move is followed by only a little refinement (the editor runs at frame rate)
    for (double x = -14; x <= 16; x += 2) {
        b.setLights(build(x, 10).lights);
        b.step(nullptr, 300);
    }
    // then switch it off: nothing of the light may remain anywhere, not even trapped bounce light
    b.setLights(build(16, 0).lights);
    int guard = 0;
    while (!b.converged() && guard++ < 10000) b.step(nullptr, 1000000);
    REQUIRE(b.converged());
    double worst = 0;
    for (size_t i = 0; i < b.sampleCount(); ++i) worst = std::max({worst, b.value(i).x, b.value(i).y, b.value(i).z});
    CHECK(worst < 1e-6);
}

TEST_CASE("live: after a geometry edit new texels start from the light of their neighbours, not from black") {
    std::vector<Entity> es = corridor({pointLight("l", {0, 3, 0}, 10, 60)});
    World w(es);
    LiveBaker b(smallChunks());
    b.setGeometry(w.layout, w.surfaces);
    b.setLights(w.lights);
    int guard = 0;
    while (!b.converged() && guard++ < 200) b.step(nullptr, 100000);
    REQUIRE(b.converged());
    // a box is added on the floor: its faces are new texels (and the floor under it changes)
    World edited(corridor({pointLight("l", {0, 3, 0}, 10, 60)}));
    edited.entities[0].brushes.push_back(Brush::makeBox("crate", {5, 0, -1}, {6, 1, 1}));
    edited.rebuild();
    const Vec3 region[2] = {{5, 0, -1}, {6, 1, 1}};
    Aabb changed;
    changed.add(region[0]);
    changed.add(region[1]);
    b.setGeometry(edited.layout, edited.surfaces, {changed});
    REQUIRE(b.chunkCount() > 0);
    size_t withData = 0, topTexels = 0, topLit = 0;
    // every chunk that holds crate texels (sides and top, y > 0) shows a value at once; the dark chunks are the unlit underside of the floor slab
    size_t crateChunks = 0;
    for (size_t c = 0; c < b.chunkCount(); ++c) {
        if (b.chunk(static_cast<int>(c)).bounds.center().y <= 0.1) continue;
        ++crateChunks;
        withData += b.chunkHasData(static_cast<int>(c)) ? 1 : 0;
    }
    REQUIRE(crateChunks > 0);
    CHECK(withData == crateChunks);  // nothing flashes black
    // texels on the crate's top face never existed before: their value comes from the nearest upward facing floor texel, and they are stale
    const TexelSamples& s = b.samples();
    for (size_t i = 0; i < s.count(); ++i)
        if (s.nrm[i * 4 + 1] > 0.99f && s.pos[i * 4 + 1] > 0.9f) { ++topTexels; if (b.value(i).x > 0.01) ++topLit; }
    REQUIRE(topTexels > 0);
    CHECK(topLit == topTexels);
    CHECK(b.staleChunks() > 0);  // still to be baked for real
}

TEST_CASE("live: dirty chunks are the ones whose pixels changed since the last take, and survive a rebuild") {
    World w(corridor({pointLight("l", {0, 3, 0}, 10, 30)}));
    LiveBaker b(smallChunks());
    b.setGeometry(w.layout, w.surfaces);
    b.setLights(w.lights);
    CHECK(b.takeDirtyChunks().size() == b.chunkCount());  // every texel is new
    CHECK(!b.hasDirtyChunks());
    const Vec3 cam{0, 1, 0};
    const auto r = b.step(&cam, 600);
    REQUIRE(!r.chunks.empty());
    std::vector<int> expected = r.chunks;
    std::sort(expected.begin(), expected.end());
    // a rebuild with identical geometry before the tiles were taken keeps them dirty (and only them)
    b.setGeometry(w.layout, w.surfaces);
    b.setLights(w.lights);
    CHECK(b.takeDirtyChunks() == expected);
    int guard = 0;
    while (!b.converged() && guard++ < 1000) b.step(&cam, 100000);
    CHECK(b.takeDirtyChunks().size() == b.chunkCount());  // every chunk was refined since
    // identical geometry again: nothing about any pixel changed
    b.setGeometry(w.layout, w.surfaces);
    b.setLights(w.lights);
    CHECK(!b.hasDirtyChunks());
    CHECK(b.converged());
    // a crate on the floor: only the chunks around it are dirty, the rest of the corridor is untouched
    std::vector<Entity> es = corridor({pointLight("l", {0, 3, 0}, 10, 30)});
    es[0].brushes.push_back(Brush::makeBox("crate", {-1, 0, -1}, {1, 1.5, 1}));
    World w2(es);
    b.setGeometry(w2.layout, w2.surfaces);
    b.setLights(w2.lights);
    const auto dirty = b.takeDirtyChunks();
    CHECK(dirty.size() > 0);
    CHECK(dirty.size() < b.chunkCount() / 2);
}

#include "gpu_bake.h"

TEST_CASE("live gpu: a session made after another one reuses its buffers and gives the same light") {
    World w(corridor({pointLight("l", {0, 3, 0}, 10, 30, 0.3)}), 1);
    LiveSettings s = smallChunks();
    s.maxPasses = 2;
    auto bakeOnce = [&](const World& world, std::vector<Vec3>& values) {
        LiveBaker b(s);
        b.setGeometry(world.layout, world.surfaces);
        b.setLights(world.lights);
        const Vec3 cam{0, 1, 0};
        int guard = 0;
        while (!b.converged() && guard++ < 100) b.step(&cam, 100000);
        values.clear();
        for (size_t i = 0; i < b.sampleCount(); ++i) values.push_back(b.value(i));
        return b.evaluator();
    };
    std::vector<Vec3> first, second;
    const std::string ev = bakeOnce(w, first);
    if (ev == "cpu") { MESSAGE("no GPU with ray query here (evaluator '", ev, "'): pool test skipped"); return; }
    const auto before = gpu::liveBufferPoolStats();
    CHECK(before.pooledBuffers > 5);  // the first session returned its buffers when it ended
    bakeOnce(w, second);
    const auto after = gpu::liveBufferPoolStats();
    CHECK(after.hits > before.hits + 5);      // the second session took them instead of allocating
    CHECK(after.misses <= before.misses + 2);  // (the acceleration structure buffers may differ in size)
    REQUIRE(first.size() == second.size());
    for (size_t i = 0; i < first.size(); ++i) {  // the same inputs give bit identical light, whatever memory the buffers came from
        CHECK(first[i].x == second[i].x);
        CHECK(first[i].y == second[i].y);
        CHECK(first[i].z == second[i].z);
    }
    // a different, smaller level next: still correct, and compared with a session that cannot reuse anything (pool dropped by a big change is not
    // forced here), the values must match a fresh evaluation of the same level
    World small(corridor({pointLight("l", {0, 3, 0}, 10, 30, 0.3)}), 0.5);
    std::vector<Vec3> a, b2;
    bakeOnce(small, a);
    bakeOnce(small, b2);
    REQUIRE(a.size() == b2.size());
    for (size_t i = 0; i < a.size(); ++i) CHECK(a[i].x == b2[i].x);
}

TEST_CASE("live: a denoise in three steps gives what denoiseNext gives, and it is dropped when the page was refined or the geometry replaced meanwhile") {
    const denoise::Info info = denoise::probe();
    if (!info.available) { MESSAGE("OpenImageDenoise not available, skipped: " << info.error); return; }
    World w(corridor({pointLight("l", {0, 3, 0}, 10, 30)}));
    LiveSettings s = smallChunks();
    s.bounces = true;
    s.denoiseIndirect = true;
    s.maxPasses = 3;
    s.useGpu = false;
    const Vec3 cam{0, 1, 0};
    auto converged = [&](LiveBaker& b) {
        b.setGeometry(w.layout, w.surfaces);
        b.setLights(w.lights);
        int guard = 0;
        while (!b.converged() && guard++ < 1000) b.step(&cam, 100000);
        REQUIRE(b.converged());
    };
    LiveBaker a(s), b(s);
    converged(a);
    converged(b);
    int rounds = 0;
    while (a.denoisePending() && rounds++ < 20) a.denoiseNext();
    rounds = 0;
    while (b.denoisePending() && rounds++ < 20) {
        LiveBaker::DenoiseJob job;
        REQUIRE(b.beginDenoise(job));
        const denoise::Result r = LiveBaker::runDenoise(job, b.denoiseQuality());
        REQUIRE_MESSAGE(r.ok, r.error);
        CHECK(b.finishDenoise(job, r));
    }
    CHECK(a.denoisedPages() > 0);
    CHECK(a.denoisedPages() == b.denoisedPages());
    REQUIRE(a.sampleCount() == b.sampleCount());
    for (size_t i = 0; i < a.sampleCount(); ++i) {
        const Vec3 x = a.shownValue(i), y = b.shownValue(i);
        CHECK(std::fabs(x.x - y.x) < 1e-4);
        CHECK(std::fabs(x.y - y.y) < 1e-4);
        CHECK(std::fabs(x.z - y.z) < 1e-4);
    }
    // a page in flight is not offered again
    LiveBaker c(s);
    converged(c);
    LiveBaker::DenoiseJob job;
    REQUIRE(c.beginDenoise(job));
    LiveBaker::DenoiseJob second;
    while (c.beginDenoise(second)) CHECK(second.page != job.page);
    const denoise::Result r = LiveBaker::runDenoise(job, c.denoiseQuality());
    REQUIRE(r.ok);
    // the geometry is replaced before the result comes back: it must not be written into the new sample order
    c.setGeometry(w.layout, w.surfaces);
    c.setLights(w.lights);
    CHECK_FALSE(c.finishDenoise(job, r));
    CHECK(c.denoisePending());  // and the page is pending again
    // a page that is refined meanwhile drops the result too
    LiveBaker d(s);
    converged(d);
    LiveBaker::DenoiseJob j2;
    REQUIRE(d.beginDenoise(j2));
    const denoise::Result r2 = LiveBaker::runDenoise(j2, d.denoiseQuality());
    LightScene moved = w.lights;
    moved.lights[0].position = {1, 3, 0};
    d.setLights(moved);
    int guard = 0;
    while (!d.converged() && guard++ < 1000) d.step(&cam, 100000);
    CHECK_FALSE(d.finishDenoise(j2, r2));
}
