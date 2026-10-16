#include "core/csg.h"
#include "core/derived.h"
#include "core/router.h"
#include "test_util.h"
#include <doctest.h>
#include <map>

using namespace core;
using namespace testutil;

namespace {
// Volume enclosed by a closed polygon soup (divergence theorem), positive when wound outward.
double volume(const std::vector<CsgFragment>& frags) {
    double v = 0;
    for (const auto& f : frags)
        for (size_t i = 1; i + 1 < f.verts.size(); ++i) v += f.verts[0].dot(f.verts[i].cross(f.verts[i + 1])) / 6.0;
    return v;
}
double areaOfSource(const std::vector<CsgFragment>& frags, int source) {
    double a = 0;
    for (const auto& f : frags) {
        if (f.source != source) continue;
        Vec3 s;
        for (size_t i = 1; i + 1 < f.verts.size(); ++i) s = s + (f.verts[i] - f.verts[0]).cross(f.verts[i + 1] - f.verts[0]);
        a += 0.5 * s.length();
    }
    return a;
}
Brush sub(const std::string& id, Vec3 lo, Vec3 hi) {
    Brush b = Brush::makeBox(id, lo, hi);
    b.op = BrushOp::Subtract;
    return b;
}
}  // namespace

TEST_CASE("csg: a through-hole removes exactly the overlapping volume") {
    const Brush wall = Brush::makeBox("wall", {0, 0, 0}, {4, 4, 4});
    const Brush hole = sub("hole", {1, 1, -1}, {3, 3, 5});
    std::vector<CsgFragment> out;
    REQUIRE(csgSubtract(wall, {&hole}, out));
    CHECK(volume(out) == doctest::Approx(64.0 - 2 * 2 * 4));
    // surface: the wall's 6 faces minus the two 2x2 openings, plus the four 2x4 walls of the hole
    CHECK(areaOfSource(out, 0) == doctest::Approx(6 * 16.0 - 2 * 4.0));
    CHECK(areaOfSource(out, 1) == doctest::Approx(4 * 2 * 4.0));
}

TEST_CASE("csg: every fragment names the authored face it lies on") {
    const Brush wall = Brush::makeBox("wall", {0, 0, 0}, {4, 4, 4});
    const Brush hole = sub("hole", {1, 1, -1}, {3, 3, 5});
    std::vector<CsgFragment> out;
    REQUIRE(csgSubtract(wall, {&hole}, out));
    for (const auto& f : out) {
        const Brush& src = f.source == 0 ? wall : hole;
        const BrushFace* face = nullptr;
        for (const auto& bf : src.faces) if (bf.id == f.faceId) face = &bf;
        REQUIRE(face);
        const Vec3 n = src.faceNormal(*face);
        const double w = n.dot(src.vertices[face->loop[0]]);
        for (const auto& p : f.verts) CHECK(std::abs(n.dot(p) - w) < 1e-6);  // on the parent's plane
        // and it faces the same way as the parent, or opposite for cut walls (they face into the hole)
        Vec3 fn;
        for (size_t i = 1; i + 1 < f.verts.size(); ++i) fn = fn + (f.verts[i] - f.verts[0]).cross(f.verts[i + 1] - f.verts[0]);
        fn = fn.normalized();
        if (f.source == 0) CHECK(fn.dot(n) > 0.99);
        else CHECK(fn.dot(n) < -0.99);
    }
}

TEST_CASE("csg: cavity, no overlap, full cover, stacked subtractors") {
    const Brush wall = Brush::makeBox("wall", {0, 0, 0}, {4, 4, 4});
    std::vector<CsgFragment> out;

    const Brush inner = sub("in", {1, 1, 1}, {3, 3, 3});
    REQUIRE(csgSubtract(wall, {&inner}, out));
    CHECK(volume(out) == doctest::Approx(64.0 - 8.0));
    CHECK(areaOfSource(out, 0) == doctest::Approx(96.0));  // outside untouched
    CHECK(areaOfSource(out, 1) == doctest::Approx(24.0));  // the cavity lining

    const Brush far = sub("far", {10, 10, 10}, {12, 12, 12});
    REQUIRE(csgSubtract(wall, {&far}, out));
    CHECK(volume(out) == doctest::Approx(64.0));

    const Brush all = sub("all", {-1, -1, -1}, {5, 5, 5});
    REQUIRE(csgSubtract(wall, {&all}, out));
    CHECK(out.empty());

    const Brush a = sub("a", {0, 0, 0}, {2, 4, 4}), b = sub("b", {2, 0, 0}, {4, 2, 4});
    REQUIRE(csgSubtract(wall, {&a, &b}, out));
    CHECK(volume(out) == doctest::Approx(64.0 - 32.0 - 16.0));
    CHECK(areaOfSource(out, 2) > 0);  // subtractor numbering follows the order given
}

TEST_CASE("csg: a subtractor sharing a face with the target still gives a closed result") {
    const Brush wall = Brush::makeBox("wall", {0, 0, 0}, {4, 4, 4});
    const Brush notch = sub("notch", {1, 1, 3}, {3, 3, 4});  // flush with the +Z face
    std::vector<CsgFragment> out;
    REQUIRE(csgSubtract(wall, {&notch}, out));
    CHECK(volume(out) == doctest::Approx(64.0 - 4.0));
}

TEST_CASE("csg: non-convex brushes (an L prism) are carved correctly") {
    const Brush l = lPrism("l");
    const double whole = l.signedVolume();
    REQUIRE(whole > 0);
    Vec3 lo, hi;
    REQUIRE(l.bounds(lo, hi));
    const Brush cut = sub("cut", lo - Vec3{1, 1, 1}, {lo.x + (hi.x - lo.x) / 2, hi.y + 1, hi.z + 1});
    std::vector<CsgFragment> out;
    REQUIRE(csgSubtract(l, {&cut}, out));
    CHECK(volume(out) < whole);
    CHECK(volume(out) > 0);
    for (const auto& f : out) CHECK(f.verts.size() >= 3);
}

TEST_CASE("csg: too many fragments is reported, not attempted forever") {
    const Brush wall = Brush::makeBox("wall", {0, 0, 0}, {4, 4, 4});
    const Brush hole = sub("hole", {1, 1, -1}, {3, 3, 5});
    std::vector<CsgFragment> out;
    CHECK_FALSE(csgSubtract(wall, {&hole}, out, CsgLimits{3}));
    CHECK(out.empty());
}

TEST_CASE("csg: carve scope decides which entities a subtractor affects") {
    Entity world, door, other;
    world.id = "w"; world.classname = "worldspawn";
    door.id = "d"; door.classname = "func_door";
    other.id = "o"; other.classname = "func_detail";
    CarveScope def;  // self + structural, not non-structural
    CHECK(carveScopeApplies(def, true, world));
    CHECK(carveScopeApplies(def, false, other));       // structural target
    CHECK_FALSE(carveScopeApplies(def, false, door));  // dynamic target
    CarveScope dyn{false, false, true};
    CHECK(carveScopeApplies(dyn, false, door));
    CHECK_FALSE(carveScopeApplies(dyn, true, world));
    CHECK_FALSE(carveScopeApplies(dyn, false, other));
    CHECK(carveScopeApplies(CarveScope{true, false, false}, true, door));
}

TEST_CASE("csg: brushSurfaces keeps convex faces whole and triangulates concave ones") {
    const auto box = brushSurfaces(Brush::makeBox("b", {0, 0, 0}, {1, 1, 1}));
    CHECK(box.size() == 6);
    for (const auto& p : box) CHECK(p.verts.size() == 4);
    const Brush l = lPrism("l");
    const auto pieces = brushSurfaces(l);
    CHECK(pieces.size() > l.faces.size());  // the concave L faces were split
    for (const auto& p : pieces) CHECK(p.verts.size() >= 3);
}

TEST_CASE("csg: a concave (L-shaped) cutter removes exactly its own volume") {
    Brush cutter = lPrism("l");
    const double vol = cutter.signedVolume();
    REQUIRE(vol > 0);
    Vec3 lo, hi;
    REQUIRE(cutter.bounds(lo, hi));
    cutter.op = BrushOp::Subtract;
    const Brush wall = Brush::makeBox("wall", lo - Vec3{1, 1, 1}, hi + Vec3{1, 1, 1});
    const double wallVol = wall.signedVolume();
    std::vector<CsgFragment> out;
    REQUIRE(csgSubtract(wall, {&cutter}, out));
    CHECK(volume(out) == doctest::Approx(wallVol - vol));
}

TEST_CASE("csg: a concave cutter that only partly overlaps - volumes add up from both directions") {
    Brush cutter = lPrism("l");
    Vec3 lo, hi;
    REQUIRE(cutter.bounds(lo, hi));
    const double lVol = cutter.signedVolume();
    // a wall covering the lower-x half of the cutter and reaching beyond it on every other side
    const Vec3 mid = (lo + hi) * 0.5;
    const Brush wall = Brush::makeBox("wall", lo - Vec3{1, 1, 1}, {mid.x, hi.y + 1, hi.z + 1});
    const double wallVol = wall.signedVolume();
    cutter.op = BrushOp::Subtract;
    std::vector<CsgFragment> wallMinusL;
    REQUIRE(csgSubtract(wall, {&cutter}, wallMinusL));
    // the same intersection measured the other way: L minus the wall (the wall as the cutter)
    Brush wallCut = wall;
    wallCut.op = BrushOp::Subtract;
    Brush lSolid = lPrism("l2");
    std::vector<CsgFragment> lMinusWall;
    REQUIRE(csgSubtract(lSolid, {&wallCut}, lMinusWall));
    const double overlap = lVol - volume(lMinusWall);
    CHECK(overlap > 0);
    CHECK(overlap < lVol);
    CHECK(volume(wallMinusL) == doctest::Approx(wallVol - overlap));
}

TEST_CASE("validation: the convex rule is for solids - a concave subtractor is fine in worldspawn, a concave solid is not") {
    Document d;
    Entity world;
    world.id = "w";
    world.classname = "worldspawn";
    Brush l = lPrism("l");
    l.op = BrushOp::Subtract;
    world.brushes.push_back(l);
    d.entities.push_back(world);
    CHECK_FALSE(validateDocument(d).has_value());
    d.entities[0].brushes[0].op = BrushOp::Add;
    auto err = validateDocument(d);
    REQUIRE(err.has_value());
    CHECK(err->find("convex") != std::string::npos);
}
