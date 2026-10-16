#include "core/raytrace.h"
#include "test_util.h"
#include <doctest.h>
#include <cmath>
#include <random>

using namespace core;
using namespace testutil;

namespace {
Entity ent(const std::string& id, const std::string& cls, std::vector<Brush> brushes) {
    Entity e;
    e.id = id;
    e.classname = cls;
    e.brushes = std::move(brushes);
    return e;
}
// brute-force reference, independent of the BVH code
double bruteNearest(const OccluderMesh& m, const Vec3& o, const Vec3& d, double tmax) {
    double best = -1;
    for (size_t i = 0; i < m.triangleCount(); ++i) {
        const float* t = &m.tris[i * 9];
        const Vec3 a{t[0], t[1], t[2]}, b{t[3], t[4], t[5]}, c{t[6], t[7], t[8]};
        const Vec3 e1 = b - a, e2 = c - a, p = d.cross(e2);
        const double det = e1.dot(p);
        if (std::fabs(det) < 1e-14) continue;
        const Vec3 s = o - a;
        const double u = s.dot(p) / det;
        const Vec3 q = s.cross(e1);
        const double v = d.dot(q) / det;
        const double dist = e2.dot(q) / det;
        if (u < 0 || v < 0 || u + v > 1 || dist < 0 || dist > tmax) continue;
        if (best < 0 || dist < best) best = dist;
    }
    return best;
}
}  // namespace

TEST_CASE("raytrace: occluders come from shadow-casting classes only") {
    Document doc;
    doc.entities = {ent("w", "worldspawn", {Brush::makeBox("a", {0, 0, 0}, {1, 1, 1})}), ent("d", "func_door", {Brush::makeBox("b", {3, 0, 0}, {4, 1, 1})})};
    const auto surfaces = collectSurfaces(doc, {});
    const OccluderMesh m = buildOccluders(surfaces, LightingClasses::defaults());
    CHECK(m.triangleCount() == 12);  // a box: 6 quads = 12 triangles; the door is not listed
    LightingClasses c = LightingClasses::defaults();
    c.castShadows.insert("func_door");
    CHECK(buildOccluders(surfaces, c).triangleCount() == 24);
    CHECK(m.boundsMin().x == doctest::Approx(0));
    CHECK(m.boundsMax().y == doctest::Approx(1));
}

TEST_CASE("raytrace: BVH agrees with brute force on random triangles and rays") {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> U(-10, 10), S(0.1f, 2.0f);
    OccluderMesh m;
    for (int i = 0; i < 1500; ++i) {
        const float x = U(rng), y = U(rng), z = U(rng);
        for (int v = 0; v < 3; ++v) { m.tris.push_back(x + S(rng) * (v == 1)); m.tris.push_back(y + S(rng) * (v == 2)); m.tris.push_back(z + S(rng) * (v == 0)); }
    }
    const TriangleBvh bvh(m);
    CHECK(bvh.nodeCount() > 100);
    int hits = 0;
    for (int i = 0; i < 3000; ++i) {
        const Vec3 o{U(rng), U(rng), U(rng)};
        Vec3 d{U(rng), U(rng), U(rng)};
        d = d.normalized();
        const double tmax = 1 + std::fabs(U(rng)) * 2;
        const double ref = bruteNearest(m, o, d, tmax);
        const double got = bvh.nearestHit(o, d, tmax);
        CHECK(bvh.occluded(o, d, tmax) == (ref >= 0));
        CHECK((got >= 0) == (ref >= 0));
        if (ref >= 0) { CHECK(got == doctest::Approx(ref).epsilon(1e-9)); ++hits; }
    }
    CHECK(hits > 100);  // the test really exercises hits
}

TEST_CASE("raytrace: segments stop at tmax, empty mesh never occludes, axis-parallel rays work") {
    Document doc;
    doc.entities = {ent("w", "worldspawn", {Brush::makeBox("a", {-1, 1, -1}, {1, 2, 1})})};
    const TriangleBvh bvh(buildOccluders(collectSurfaces(doc, {}), LightingClasses::defaults()));
    CHECK(bvh.occluded({0, 0, 0}, {0, 1, 0}, 5));
    CHECK(!bvh.occluded({0, 0, 0}, {0, 1, 0}, 0.99));        // stops before the box
    CHECK(bvh.nearestHit({0, 0, 0}, {0, 1, 0}, 5) == doctest::Approx(1));
    CHECK(!bvh.occluded({3, 0, 0}, {0, 1, 0}, 5));           // beside it
    CHECK(bvh.occluded({0, 5, 0}, {0, -1, 0}, 5));           // from above, double sided
    const TriangleBvh none{OccluderMesh{}};
    CHECK(!none.occluded({0, 0, 0}, {0, 1, 0}, 100));
}

TEST_CASE("raytrace: closest hit reports the triangle and barycentrics") {
    OccluderMesh m;  // two parallel triangles, the nearer one is the second
    m.tris = {0, 0, 4, 4, 0, 4, 0, 4, 4,   // z = 4
              0, 0, 2, 4, 0, 2, 0, 4, 2};  // z = 2
    const TriangleBvh bvh(m);
    const auto h = bvh.closestHit({1, 1, 0}, {0, 0, 1}, 100);
    REQUIRE(h.t >= 0);
    CHECK(h.t == doctest::Approx(2));
    CHECK(h.triangle == 1);
    CHECK(h.b1 == doctest::Approx(0.25));  // x / 4
    CHECK(h.b2 == doctest::Approx(0.25));  // y / 4
    CHECK(bvh.closestHit({1, 1, 0}, {0, 0, 1}, 1.5).t < 0);
    CHECK(bvh.closestHit({9, 9, 0}, {0, 0, 1}, 100).t < 0);
}

TEST_CASE("raytrace: closest hit agrees with brute force on random triangles") {
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> U(-10, 10), S(0.1f, 2.0f);
    OccluderMesh m;
    for (int i = 0; i < 800; ++i) {
        const float x = U(rng), y = U(rng), z = U(rng);
        for (int v = 0; v < 3; ++v) { m.tris.push_back(x + S(rng) * (v == 1)); m.tris.push_back(y + S(rng) * (v == 2)); m.tris.push_back(z + S(rng) * (v == 0)); }
    }
    const TriangleBvh bvh(m);
    int hits = 0;
    for (int i = 0; i < 2000; ++i) {
        const Vec3 o{U(rng), U(rng), U(rng)};
        const Vec3 d = Vec3{U(rng), U(rng), U(rng)}.normalized();
        const double ref = bruteNearest(m, o, d, 50);
        const auto h = bvh.closestHit(o, d, 50);
        CHECK((h.t >= 0) == (ref >= 0));
        if (ref < 0) continue;
        ++hits;
        CHECK(h.t == doctest::Approx(ref).epsilon(1e-9));
        // the reported triangle really contains the hit point with the reported barycentrics
        const float* t = &m.tris[h.triangle * 9];
        const Vec3 a{t[0], t[1], t[2]}, b{t[3], t[4], t[5]}, c{t[6], t[7], t[8]};
        const Vec3 p = a * (1 - h.b1 - h.b2) + b * h.b1 + c * h.b2;
        CHECK((p - (o + d * h.t)).length() < 1e-6);
    }
    CHECK(hits > 50);
}

TEST_CASE("raytrace: occluders built with a layout carry lightmap coordinates of their face") {
    Document doc;
    doc.entities = {ent("w", "worldspawn", {Brush::makeBox("a", {0, 0, 0}, {2, 1, 2})}), ent("d", "func_door", {Brush::makeBox("b", {5, 0, 0}, {6, 1, 1})})};
    const auto surfaces = collectSurfaces(doc, {});
    LightingClasses c = LightingClasses::defaults();
    c.castShadows.insert("func_door");  // the door casts shadows but is not lightmapped
    const LightmapLayout layout = buildLightmapLayout(surfaces, LightingClasses::defaults());
    const OccluderMesh m = buildOccluders(surfaces, c, &layout);
    REQUIRE(m.triangleCount() == 24);
    REQUIRE(m.lightmap.size() == 24 * 8);
    int withMap = 0, without = 0;
    for (size_t t = 0; t < 24; ++t) (m.lightmap[t * 8 + 7] > 0.5f ? withMap : without)++;
    CHECK(withMap == 12);
    CHECK(without == 12);
    // the stored coordinates are the face map applied to the triangle's vertices
    const float* v = &m.tris[0];
    const LightmapFace* f = layout.findForFragment(surfaces[0].instance, surfaces[0].entityId, surfaces[0].brushInstance, surfaces[0].brushId, surfaces[0].faceId, surfaces[0].verts);
    REQUIRE(f);
    const Vec2 t0 = f->texel({v[0], v[1], v[2]});
    CHECK(m.lightmap[0] == doctest::Approx(t0.u));
    CHECK(m.lightmap[1] == doctest::Approx(t0.v));
    CHECK(!buildOccluders(surfaces, c).hasLightmap());
}
